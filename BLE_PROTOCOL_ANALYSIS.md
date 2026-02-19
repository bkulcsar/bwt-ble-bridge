# BWT Smart Water Meter — BLE Protocol Reverse Engineering

## 1. BLE Service & Characteristics

| Field                      | Value                                                   |
| -------------------------- | ------------------------------------------------------- |
| **Service UUID**           | `D973F2E0-B19E-11E2-9E96-0800200C9A66`                  |
| **Buffer (notifications)** | Characteristic `F2E1` — receives streamed data packets  |
| **Buffer Trigger (write)** | Characteristic `F2E2` — write command to request data   |
| **Broadcast (read)**       | Characteristic `F2E3` — current device state / metadata |

---

## 2. Broadcast Characteristic (`F2E3`) — Device State

Reading this characteristic returns a 15-byte payload decoded as:

| Offset | Size                 | Field              | Description                                                |
| ------ | -------------------- | ------------------ | ---------------------------------------------------------- |
| 0–3    | uint32 (2×uint16 LE) | `remaining`        | Remaining capacity (low16 + high16 × 65536)                |
| 4–5    | uint16 LE            | `quarterHoursIdx`  | Current write index in the quarter-hour ring buffer        |
| 6–7    | uint16 LE            | `daysIdx`          | Current write index in the daily ring buffer               |
| 8–9    | uint16 LE            | `regen`            | Regeneration counter                                       |
| 10–11  | uint16 LE            | `totalCapacity`    | Total capacity (×1000)                                     |
| 12     | uint8 (bitfield)     | `flags`            | Bit 0: alarm, Bit 1: quarterHoursLooped, Bit 2: daysLooped |
| 13     | uint8                | `version` (part 1) | Firmware version component                                 |
| 14     | uint8                | `version` (part 2) | Firmware version component                                 |

The `quarterHoursIdx`, `daysIdx`, `quarterHoursLooped`, and `daysLooped` fields are critical — they tell the app how much data to request and whether the ring buffer has wrapped around.

---

## 3. Command Packet Construction (Write to `F2E2`)

The write command is a **7-byte Uint8Array**:

```
[ CMD_TYPE, ADDR_LO, ADDR_HI, SIZE_LO, SIZE_HI, DELAY_LO, DELAY_HI ]
```

### Byte breakdown

| Index | Function          | Encoding                              |
| ----- | ----------------- | ------------------------------------- |
| 0     | Command type      | Always `0x02` (buffer read mode)      |
| 1     | Address low byte  | `address & 0xFF`                      |
| 2     | Address high byte | `(address >> 8) & 0xFF`               |
| 3     | Size low byte     | `size & 0xFF`                         |
| 4     | Size high byte    | `(size >> 8) & 0xFF`                  |
| 5     | Delay low byte    | `delay & 0xFF` (always 20 = `0x14`)   |
| 6     | Delay high byte   | `(delay >> 8) & 0xFF` (always `0x00`) |

All multi-byte values are **little-endian uint16**.

### Source code (minified identifiers mapped):

```javascript
// jR.buffer = 2 (command type enum)
// _o(x) = x & 0xFF       (low byte)
// Ho(x) = (x >> 8) & 0xFF (high byte)

encode: function ({ address, size, delay }) {
    return new Uint8Array([
        2,              // command type = buffer read
        _o(address),    // address low byte
        Ho(address),    // address high byte
        _o(size),       // size low byte
        Ho(size),       // size high byte
        _o(delay),      // delay low byte
        Ho(delay),      // delay high byte
    ]).buffer;
}
```

---

## 4. Memory Layout

The device stores history in two separate memory regions:

| Region           | Start Address   | End Address      | Total Bytes | Total Values | Description                   |
| ---------------- | --------------- | ---------------- | ----------- | ------------ | ----------------------------- |
| **Quarter-hour** | `0x0000` (0)    | `0x1680` (5760)  | 5760        | 2880         | 15-min consumption (120 days) |
| **Daily**        | `0x1900` (6400) | `0x2742` (10050) | 3650        | 1825         | Daily consumption (~5 years)  |

Both regions are **ring buffers**. The broadcast characteristic tells you:

- The current write index (`quarterHoursIdx` / `daysIdx`)
- Whether the buffer has wrapped (`quarterHoursLooped` / `daysLooped`)

---

## 5. Decoding the Two Captured Commands

### Command A: `02 00 19 B8 0A 14 00` → **Daily History**

| Byte(s) | Hex    | Decimal | Field                              |
| ------- | ------ | ------- | ---------------------------------- |
| `02`    | 0x02   | 2       | Command type (buffer read)         |
| `00 19` | 0x1900 | 6400    | Start address = daily region start |
| `B8 0A` | 0x0AB8 | 2744    | Size in bytes                      |
| `14 00` | 0x0014 | 20      | Inter-packet delay (ms)            |

- **2744 bytes ÷ 2 = 1372 daily values** (~3.76 years of daily data)
- Size = `2 × daysIdx` (buffer not yet looped, so only filled entries are requested)
- Triggers: **153 notification packets** (2744 ÷ 18 bytes/packet = 152.4 → ceil = 153)

### Command B: `02 00 00 80 16 14 00` → **Quarter-Hour History**

| Byte(s) | Hex    | Decimal | Field                                     |
| ------- | ------ | ------- | ----------------------------------------- |
| `02`    | 0x02   | 2       | Command type (buffer read)                |
| `00 00` | 0x0000 | 0       | Start address = quarter-hour region start |
| `80 16` | 0x1680 | 5760    | Size in bytes (full region)               |
| `14 00` | 0x0014 | 20      | Inter-packet delay (ms)                   |

- **5760 bytes ÷ 2 = 2880 quarter-hour values** (exactly 120 days)
- Size = full region (`GR - YR = 5760`), meaning `quarterHoursLooped = true`
- Triggers: **320 notification packets** (5760 ÷ 18 = 320)

---

## 6. Notification Packet Structure

Each notification on characteristic `F2E1` is a **20-byte packet**:

```
[ INDEX_LO, INDEX_HI, V1_LO, V1_HI, V2_LO, V2_HI, ... V9_LO, V9_HI ]
```

| Offset | Size          | Description                      |
| ------ | ------------- | -------------------------------- |
| 0–1    | uint16 LE     | Packet sequence index (0-based)  |
| 2–19   | 9 × uint16 LE | Data values (up to 9 per packet) |

**Note:** The last packet may contain fewer than 9 meaningful values. The parser tracks remaining bytes and only reads valid data.

### Reassembly logic:

1. Collect all packets
2. Verify packet indices are sequential (0, 1, 2, ...)
3. Strip the 2-byte index header from each packet
4. Concatenate the remaining bytes
5. Parse as uint16 little-endian values
6. Apply the appropriate word parser (`XR` or `ZR`)
7. If the ring buffer has looped, rotate the array so entries are in chronological order

---

## 7. Value Parsers

### Quarter-Hour Parser (`XR`)

Each 16-bit word encodes a 15-minute consumption sample:

```
Bit:  15 14 13 12  11  10   9 8 7 6 5 4 3 2 1 0
      [  unused  ] [R] [PC] [     litres        ]
```

| Bits | Mask           | Field      | Description                    |
| ---- | -------------- | ---------- | ------------------------------ |
| 0–9  | `word & 0x3FF` | `litres`   | Consumption in litres (0–1023) |
| 10   | `word & 0x400` | `powerCut` | Power cut flag                 |
| 11   | `word & 0x800` | `regen`    | Regeneration occurred (0 or 1) |

```javascript
function XR(word) {
    return {
        litres: word & 0x3ff, // 0–1023 litres per 15 min
        powerCut: (word & (1 << 10)) > 0,
        regen: word & (1 << 11) ? 1 : 0,
    };
}
```

### Daily Parser (`ZR`)

Each 16-bit word encodes a daily consumption sample:

```
Bit:  15 14  13 12  11    10 9 8 7 6 5 4 3 2 1 0
      [  ]  [regen] [PC]  [        litres ÷ 10  ]
```

| Bits  | Mask               | Field      | Description                                              |
| ----- | ------------------ | ---------- | -------------------------------------------------------- |
| 0–10  | `word & 0x7FF`     | `litres`   | Consumption in litres ÷ 10 (multiply by 10 → 0–20,470 L) |
| 11    | `word & 0x800`     | `powerCut` | Power cut flag                                           |
| 12–13 | `(word >> 12) & 3` | `regen`    | Regeneration count (0–3)                                 |

```javascript
function ZR(word) {
    return {
        litres: 10 * (word & 0x7ff), // 0–20,470 litres per day
        powerCut: (word & (1 << 11)) > 0,
        regen: (word >> 12) & 3, // 0–3
    };
}
```

---

## 8. Complete Call Flow

The app performs the following sequence when fetching history:

```javascript
// Step 1: Read device state from broadcast characteristic (F2E3)
const state = await connection.read(broadcast);
// Returns: { remaining, quarterHoursIdx, daysIdx, regen,
//            totalCapacity, alarm, quarterHoursLooped, daysLooped, percentage, version }

// Step 2: Request daily history
//   address = 6400 (VR)
//   size = daysLooped ? 3650 : 2 * daysIdx
//   delay = 20
const dailyData = await QR(connection, state.daysIdx, state.daysLooped, onProgress);
// dailyData = [{ litres, powerCut, regen }, ...]  (reversed for newest-first)

// Step 3: Wait 100ms between requests
await sleep(100);

// Step 4: Request quarter-hour history
//   address = 0 (YR)
//   size = quarterHoursLooped ? 5760 : 2 * quarterHoursIdx
//   delay = 20
const qhData = await JR(connection, state.quarterHoursIdx, state.quarterHoursLooped, onProgress);
// qhData = [{ litres, powerCut, regen }, ...]  (reversed for newest-first)
```

---

## 9. Summary Table

| Write Command          | Start Addr | Function | Dataset          | Parser | Resolution | Max Value |
| ---------------------- | ---------- | -------- | ---------------- | ------ | ---------- | --------- |
| `02 00 19 XX XX 14 00` | 6400       | `QR()`   | **Daily**        | `ZR`   | 1 day      | 20,470 L  |
| `02 00 00 XX XX 14 00` | 0          | `JR()`   | **Quarter-hour** | `XR`   | 15 min     | 1,023 L   |

---

## 10. Matching Observed Data

### App UI values → Quarter-hour data

| Time  | App shows | Raw uint16 (hex) | Parser                        |
| ----- | --------- | ---------------- | ----------------------------- |
| 16:00 | 20 L      | `0x0014`         | `XR`: `0x0014 & 0x3FF` = 20 ✓ |
| 17:00 | 39 L      | `0x0027`         | `XR`: `0x0027 & 0x3FF` = 39 ✓ |
| 18:00 | 0 L       | `0x0000`         | `XR`: `0x0000 & 0x3FF` = 0 ✓  |

The app likely sums 4 consecutive quarter-hour entries to display as one hourly value.

### App UI values → Daily data

| Day   | App shows | Raw word  | Parser                                   |
| ----- | --------- | --------- | ---------------------------------------- |
| Day X | 294 L     | ~29 or 30 | `ZR`: `10 × 29 = 290` or `10 × 30 = 300` |
| Day Y | 295 L     | ~29 or 30 | `ZR`: close match (10 L granularity)     |
| Day Z | 329 L     | ~33       | `ZR`: `10 × 33 = 330`                    |

**Note:** Daily values have **10 L granularity** due to the `× 10` multiplier in `ZR`. The exact app totals (294, 295) may be computed by summing quarter-hour data rather than using the daily record directly.

---

## 11. How to Replicate the Protocol

### Step-by-step:

1. **Connect** to the device via BLE
2. **Discover** service `D973F2E0-B19E-11E2-9E96-0800200C9A66`
3. **Read** characteristic `F2E3` (broadcast) → parse 15 bytes to get indices and loop flags
4. **Subscribe** to notifications on `F2E1` (buffer)
5. **Write** to `F2E2` (bufferTrigger):
    - For daily: `[0x02, 0x00, 0x19, size_lo, size_hi, 0x14, 0x00]`
    - For quarter-hour: `[0x02, 0x00, 0x00, size_lo, size_hi, 0x14, 0x00]`
6. **Collect** notification packets until expected count reached
7. **Parse** packets: strip 2-byte index, read uint16 LE values
8. **Apply** word parser (`ZR` for daily, `XR` for quarter-hour)
9. **Rotate** if ring buffer looped (use `lastEntryIdx` from broadcast)

### Size calculation:

```python
# Daily
if days_looped:
    size = 3650  # full buffer
else:
    size = 2 * days_idx  # only filled entries

# Quarter-hour
if quarter_hours_looped:
    size = 5760  # full buffer
else:
    size = 2 * quarter_hours_idx
```

### Expected packet count:

```python
expected_packets = math.ceil(size / 18)
```

---

## 12. Source References (in `main.af04a786.js`)

| Item                          | Line(s)       | Minified Name                      |
| ----------------------------- | ------------- | ---------------------------------- |
| Service UUID                  | 97787         | `MR`                               |
| Characteristic definitions    | 97788–97835   | `WR`                               |
| Broadcast decode              | 97792–97815   | `WR.broadcast.decode`              |
| Buffer trigger encode         | 97822–97833   | `WR.bufferTrigger.encode`          |
| Command type enum             | 97841–97843   | `jR` (one=1, buffer=2)             |
| Buffer gatherer class         | 97875–97934   | `zR`                               |
| Memory layout constants       | 97937–97940   | `YR=0, GR=5760, VR=6400, KR=10050` |
| Daily parser                  | 97941–97943   | `ZR`                               |
| Quarter-hour parser           | 97944–97946   | `XR`                               |
| Quarter-hour request function | 97947–97949   | `JR` (exported as `nn.om0`)        |
| Daily request function        | 97950–97952   | `QR` (exported as `nn.gyJ`)        |
| App call site (fetch history) | 234830–234840 | Inside React component callback    |
| Helper: uint16 read           | 39311–39316   | `ko(array, offset, littleEndian)`  |
| Helper: uint16 array parse    | 39318–39322   | `Lo(array, littleEndian)`          |
| Helper: bit test              | 39324–39326   | `xo(value, bit)`                   |
| Helper: low byte              | 39327–39329   | `_o(value)`                        |
| Helper: high byte             | 39330–39332   | `Ho(value)`                        |
| 2^16 constant                 | 39333         | `Do = 65536`                       |
