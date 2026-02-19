# BWT BLE-to-MQTT Bridge

An ESP32 firmware that talks to **BWT Perla** water softeners over Bluetooth Low Energy, extracts consumption data, and publishes it to MQTT. Built for integration with **Home Assistant**, **Loxone**, or anything that speaks MQTT.

> **Tested on: BWT Perla Concept BIO 75.** This is currently the only confirmed model. Other BWT Perla variants likely use the same BLE protocol, but your mileage may vary — [open an issue](../../issues) if you test on a different model!

```
[BWT Perla] --(BLE)--> [ESP32] --(WiFi/MQTT)--> [Home Assistant / Loxone / ...]
```

## Why does this exist?

BWT provides a mobile app that connects to the Perla via BLE, but there's no local API, no MQTT, no Home Assistant integration. The data is locked inside the app. This project reverse-engineers the BLE protocol and bridges the data to MQTT, keeping everything local.

## What it does

Every X minutes (configurable), the ESP32:

1. **Scans** for the BWT device via BLE
2. **Reads** the broadcast characteristic (remaining capacity, alarm state, regen count, firmware version)
3. **Fetches** the quarter-hour consumption ring buffer (up to 120 days of 15-min granularity data)
4. **Disconnects** BLE, reconnects WiFi (they share the same radio on ESP32)
5. **Publishes** everything to MQTT

### MQTT Topics

| Topic              | Payload       | Description                                                                                                     |
| ------------------ | ------------- | --------------------------------------------------------------------------------------------------------------- |
| `bwt/water/status` | JSON          | Device state: remaining capacity, percentage, alarm, regen count, firmware                                      |
| `bwt/water/meter`  | Plain integer | Last completed 15-min consumption in litres. Perfect for Loxone Meter blocks (Delta mode) or HA `utility_meter` |
| `bwt/water/daily`  | JSON array    | Last X days of daily consumption with dates                                                                     |
| `bwt/water/hourly` | JSON array    | Last X hours of hourly consumption with timestamps                                                              |

All topics are **retained**, so your smart home gets the last known state immediately on connect.

### Home Assistant Auto-Discovery

The firmware publishes HA MQTT discovery messages automatically. After first boot you'll see these entities appear:

- **BWT Remaining Capacity** (litres)
- **BWT Capacity Percentage** (%)
- **BWT Alarm** (binary sensor)
- **BWT Regen Count**
- **BWT 15min Consumption** (litres)

## Hardware

You need:

- Any **ESP32** dev board (the classic ESP32-WROOM-32 works fine — ~$5)
- Your BWT Perla water softener within BLE range (~10m, less through walls)
- A WiFi network and an MQTT broker

That's it. No wiring, no soldering, no modifications to the BWT device.

## Getting Started

### 1. Clone and configure

```bash
git clone https://github.com/YOUR_USERNAME/bwt-ble-bridge.git
cd bwt-ble-bridge
cp src/config.h.example src/config.h
```

Edit `src/config.h` with your details:

```cpp
// Your WiFi
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

// Your MQTT broker
#define MQTT_HOST     "192.168.1.100"
#define MQTT_PORT     1883

// Your BWT device (leave empty to scan by name "BWTblue")
#define BWT_DEVICE_MAC "AA:BB:CC:DD:EE:FF"

// Your timezone (POSIX TZ string)
// See: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define NTP_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
```

> **Finding your BWT MAC address:** Use any BLE scanner app (nRF Connect, LightBlue) and look for a device advertising as "BWTblue". Or just leave `BWT_DEVICE_MAC` empty and it'll find it by name.

### 2. Build and flash

Using [PlatformIO](https://platformio.org/):

```bash
pio run --target upload
pio device monitor  # optional: watch serial output
```

### 3. Verify

Check your MQTT broker for messages on `bwt/water/#`. You should see the first data within ~30 seconds of boot.

## Project Structure

```
src/
├── config.h.example  # Configuration template (copy to config.h)
├── main.cpp          # State machine: WiFi → BLE → MQTT cycle
├── ble_client.cpp/h  # BLE scanning, connection, data fetching
├── bwt_protocol.cpp/h # Protocol parsing (broadcast, QH, daily formats)
├── mqtt_publisher.cpp/h # MQTT publishing, HA discovery
├── packet_collector.cpp/h # BLE notification packet reassembly
└── utils.h           # Ring buffer rotation, byte helpers
```

## How the BLE Protocol Works

The BWT Perla exposes a single BLE service with three characteristics:

| Characteristic | UUID suffix | Function                             |
| -------------- | ----------- | ------------------------------------ |
| Buffer         | `F2E1`      | Notifications — streams data packets |
| Trigger        | `F2E2`      | Write — sends read commands          |
| Broadcast      | `F2E3`      | Read — current device state          |

The device stores consumption data in two ring buffers:

- **Quarter-hour** (addr 0x0000–0x1680): 2880 entries = 120 days at 15-min resolution, 1L granularity
- **Daily** (addr 0x1900–0x2742): 1825 entries = ~5 years at daily resolution, 10L granularity

To fetch data, you write a 7-byte command to the trigger characteristic, then collect notification packets that stream back on the buffer characteristic. Each packet is 20 bytes: 2-byte sequence index + 18 bytes of data.

The full protocol analysis is documented in [BLE_PROTOCOL_ANALYSIS.md](BLE_PROTOCOL_ANALYSIS.md) — it has everything from byte-level packet structure to the original minified JavaScript function names we traced it back to.

## Known Limitations & Open Issues

### Daily/Hourly history has small discrepancies vs. the BWT app

The daily and hourly history values are computed by summing quarter-hour (QH) entries. This mostly matches the BWT app, but we've observed small differences:

- **Days at ~7-day intervals** show slightly different values (positions 7-8, 15-16, etc. from today)
- **Hourly values** sometimes differ by a few litres

We haven't fully nailed down the root cause. Theories include:

- **QH 1L floor:** The device stores 0 for any 15-min period with <1L consumption. Summing these loses the sub-litre fractions that the device might track internally for its own daily totals
- **Clock drift:** The BWT device's internal RTC may differ from NTP time, shifting which QH slots fall into which hour/day boundaries
- **Regen slot handling:** The QH format has a regeneration flag — it's unclear whether the BWT app treats these differently when computing totals

**The meter value (last completed 15-min consumption) is accurate** and works great for tracking total consumption over time with Loxone or HA utility_meter.

If you figure out the daily/hourly discrepancy, please open an issue or PR — we'd love to know!

### ESP32 BLE + WiFi radio sharing

The ESP32 has a single radio shared between BLE and WiFi. Running both simultaneously causes packet loss during BLE data transfer. The firmware handles this by turning off WiFi during BLE operations and reconnecting afterwards, including NTP time re-sync.

## Contributing

Found a bug? Have an improvement? Figured out the daily history mystery?

**Please [open an issue](../../issues)!** PRs are also welcome.

Areas where help would be especially appreciated:

- Matching daily/hourly totals exactly to the BWT app values
- Testing with different BWT Perla models/firmware versions
- ESPHome port (if someone's into that)
- Power optimization for battery-powered setups

## A Note on How This Was Built

I'm not a C++ expert. This project started because I wanted my BWT water data in Loxone, and there was simply no integration available.

The BLE protocol was reverse-engineered through a combination of:

- **Sniffing live BLE traffic** between the BWT app and the Perla using Wireshark (with an HCI log from an Android device)
- **Analyzing the BWT web app's** minified JavaScript to understand the packet structure and parsing logic
- **LLMs (large language models)** helped enormously throughout — from making sense of obfuscated JS variable names, to understanding the BLE protocol byte layout, to writing the actual ESP32 firmware in C++

Think of the code quality as "works reliably on real hardware in production" rather than "textbook embedded C++". If you see something that makes you cringe — PRs welcome. 😄

## License

MIT — do whatever you want with it. If it helps you get your BWT data into your smart home, that's all that matters.

---

_Built with an ESP32, stubbornness, and several LLMs. Running in production on a BWT Perla Concept BIO 75._
