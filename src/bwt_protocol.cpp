#include "bwt_protocol.h"
#include "config.h"
#include "utils.h"

bool parseBroadcast(const uint8_t *data, uint8_t len, BroadcastState &state)
{
    if (len < 15)
        return false;

    uint16_t remainingLo = readUint16LE(data, 0);
    uint16_t remainingHi = readUint16LE(data, 2);
    state.remaining = (uint32_t)remainingLo + (uint32_t)remainingHi * 65536;

    state.quarterHoursIdx = readUint16LE(data, 4);
    state.daysIdx = readUint16LE(data, 6);
    state.regen = readUint16LE(data, 8);

    uint16_t totalCapRaw = readUint16LE(data, 10);
    state.totalCapacity = (uint32_t)totalCapRaw * 1000;

    uint8_t flags = data[12];
    state.alarm = (flags & 0x01) != 0;
    state.quarterHoursLooped = (flags & 0x02) != 0;
    state.daysLooped = (flags & 0x04) != 0;

    state.versionA = data[13];
    state.versionB = data[14];

    return true;
}

void buildTriggerCommand(uint16_t address, uint16_t size, uint8_t *cmd)
{
    cmd[0] = CMD_BUFFER_READ;
    cmd[1] = (uint8_t)(address & 0xFF);
    cmd[2] = (uint8_t)((address >> 8) & 0xFF);
    cmd[3] = (uint8_t)(size & 0xFF);
    cmd[4] = (uint8_t)((size >> 8) & 0xFF);
    cmd[5] = (uint8_t)(CMD_DELAY & 0xFF);
    cmd[6] = (uint8_t)((CMD_DELAY >> 8) & 0xFF);
}

uint16_t calculateRequestSize(uint16_t idx, bool looped, uint16_t regionSize)
{
    if (looped)
        return regionSize;
    return 2 * idx;
}

ConsumptionEntry parseQuarterHour(uint16_t word)
{
    ConsumptionEntry entry;
    entry.litres = word & 0x3FF;              // bits 0-9: 0–1023 litres
    entry.powerCut = (word & (1 << 10)) != 0; // bit 10
    entry.regen = (word & (1 << 11)) ? 1 : 0; // bit 11
    return entry;
}

ConsumptionEntry parseDaily(uint16_t word)
{
    ConsumptionEntry entry;
    entry.litres = 10 * (word & 0x7FF);       // bits 0-10 × 10: 0–20470 litres
    entry.powerCut = (word & (1 << 11)) != 0; // bit 11
    entry.regen = (word >> 12) & 3;           // bits 12-13: 0–3
    return entry;
}

uint16_t parseBuffer(const uint8_t *buffer, uint16_t bufferLen,
                     ConsumptionEntry *entries, bool isDaily)
{
    uint16_t numValues = bufferLen / 2;
    for (uint16_t i = 0; i < numValues; i++)
    {
        // BWT device stores ring-buffer data in big-endian byte order
        uint16_t word = readUint16BE(buffer, i * 2);
        entries[i] = isDaily ? parseDaily(word) : parseQuarterHour(word);
    }
    return numValues;
}
