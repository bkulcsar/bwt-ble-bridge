#pragma once

#include <stdint.h>

struct PacketCollector
{
    uint16_t expectedPackets; // ceil(expectedBytes / 18)
    uint16_t expectedBytes;   // total bytes to receive
    uint16_t receivedPackets; // counter
    uint16_t lastSeenIndex;   // highest packet index seen
    uint16_t missedPackets;   // count of gaps detected
    uint8_t *buffer;          // raw concatenated data (allocated dynamically)
    uint16_t bufferLen;       // actual bytes written to buffer
    bool complete;            // all packets received
    bool error;               // overflow or critical error
};

/**
 * Initialize (reset) a packet collector for a new fetch.
 * Allocates the internal buffer. Returns true on success.
 */
bool collectorInit(PacketCollector &col, uint16_t expectedBytes);

/**
 * Free the collector's internal buffer.
 */
void collectorFree(PacketCollector &col);

/**
 * Process an incoming notification packet.
 * data/len come from the BLE notification callback.
 */
void collectorOnPacket(PacketCollector &col, const uint8_t *data, uint16_t len);
