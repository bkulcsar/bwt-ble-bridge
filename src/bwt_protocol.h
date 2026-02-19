#pragma once

#include <stdint.h>

// ─── Data Structures ────────────────────────────────────────

struct BroadcastState
{
    uint32_t remaining;       // remaining capacity in litres
    uint16_t quarterHoursIdx; // current write index in QH ring buffer
    uint16_t daysIdx;         // current write index in daily ring buffer
    uint16_t regen;           // regeneration counter
    uint32_t totalCapacity;   // total capacity in litres (raw × 1000)
    bool alarm;               // bit 0 of flags byte
    bool quarterHoursLooped;  // bit 1 of flags byte
    bool daysLooped;          // bit 2 of flags byte
    uint8_t versionA;
    uint8_t versionB;
};

struct ConsumptionEntry
{
    uint16_t litres;
    bool powerCut;
    uint8_t regen; // QH: 0-1, daily: 0-3
};

// ─── Functions ──────────────────────────────────────────────

/**
 * Parse 15-byte broadcast characteristic (F2E3) into BroadcastState.
 * Returns true on success, false if len < 15.
 */
bool parseBroadcast(const uint8_t *data, uint8_t len, BroadcastState &state);

/**
 * Build a 7-byte trigger command for writing to F2E2.
 * cmd must point to a buffer of at least 7 bytes.
 */
void buildTriggerCommand(uint16_t address, uint16_t size, uint8_t *cmd);

/**
 * Calculate the request size for a ring buffer region.
 */
uint16_t calculateRequestSize(uint16_t idx, bool looped, uint16_t regionSize);

/**
 * Parse a quarter-hour word (XR parser).
 */
ConsumptionEntry parseQuarterHour(uint16_t word);

/**
 * Parse a daily word (ZR parser).
 */
ConsumptionEntry parseDaily(uint16_t word);

/**
 * Parse a raw byte buffer into consumption entries using the given parser.
 * Returns number of entries parsed. Caller must allocate `entries` with enough space.
 */
uint16_t parseBuffer(const uint8_t *buffer, uint16_t bufferLen,
                     ConsumptionEntry *entries, bool isDaily);
