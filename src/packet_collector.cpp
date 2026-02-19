#include "packet_collector.h"
#include "config.h"
#include "utils.h"
#include <Arduino.h>
#include <string.h>

bool collectorInit(PacketCollector &col, uint16_t expectedBytes)
{
    col.expectedBytes = expectedBytes;
    col.expectedPackets = (expectedBytes + PACKET_DATA - 1) / PACKET_DATA; // ceil
    col.receivedPackets = 0;
    col.lastSeenIndex = 0;
    col.missedPackets = 0;
    col.bufferLen = 0;
    col.complete = false;
    col.error = false;

    col.buffer = (uint8_t *)malloc(expectedBytes);
    if (!col.buffer)
    {
        Serial.println("[Collector] malloc failed!");
        col.error = true;
        return false;
    }
    memset(col.buffer, 0, expectedBytes);
    return true;
}

void collectorFree(PacketCollector &col)
{
    if (col.buffer)
    {
        free(col.buffer);
        col.buffer = nullptr;
    }
    col.expectedBytes = 0;
    col.expectedPackets = 0;
    col.receivedPackets = 0;
    col.lastSeenIndex = 0;
    col.missedPackets = 0;
    col.bufferLen = 0;
    col.complete = false;
    col.error = false;
}

void collectorOnPacket(PacketCollector &col, const uint8_t *data, uint16_t len)
{
    if (col.complete || col.error)
        return;

    if (len < PACKET_HEADER)
    {
        Serial.printf("[Collector] Packet too short: %u bytes\n", len);
        col.error = true;
        return;
    }

    // Extract packet index from the 2-byte header
    uint16_t pktIndex = readUint16LE(data, 0);

    // Check for duplicate or backwards jump
    if (col.receivedPackets > 0 && pktIndex < col.lastSeenIndex)
    {
        Serial.printf("[Collector] Duplicate/backwards: got %u, last was %u (ignoring)\n",
                      pktIndex, col.lastSeenIndex);
        return; // ignore but don't error
    }

    // Detect gaps (missed packets)
    uint16_t expectedNext = col.receivedPackets > 0 ? col.lastSeenIndex + 1 : 0;
    if (pktIndex > expectedNext)
    {
        uint16_t gap = pktIndex - expectedNext;
        col.missedPackets += gap;
        Serial.printf("[Collector] Gap detected: expected %u, got %u (missed %u packets, total missed: %u)\n",
                      expectedNext, pktIndex, gap, col.missedPackets);
    }

    // Check we don't exceed expected count
    if (pktIndex >= col.expectedPackets)
    {
        Serial.printf("[Collector] Packet index %u exceeds expected count %u\n",
                      pktIndex, col.expectedPackets);
        col.error = true;
        return;
    }

    // Copy data portion at the correct offset based on packet index
    uint16_t dataLen = len - PACKET_HEADER;
    uint16_t offset = pktIndex * PACKET_DATA;

    // Don't overflow the buffer
    if (offset + dataLen > col.expectedBytes)
    {
        dataLen = col.expectedBytes - offset;
    }

    memcpy(col.buffer + offset, data + PACKET_HEADER, dataLen);
    col.bufferLen += dataLen;
    col.receivedPackets++;
    col.lastSeenIndex = pktIndex;

    // Check completion: received all packets or reached last expected index
    if (pktIndex == col.expectedPackets - 1)
    {
        col.complete = true;
        if (col.missedPackets > 0)
        {
            Serial.printf("[Collector] Complete with gaps: %u/%u packets received, %u missed (zeroed)\n",
                          col.receivedPackets, col.expectedPackets, col.missedPackets);
        }
        else
        {
            Serial.printf("[Collector] Complete: %u packets, %u bytes\n",
                          col.receivedPackets, col.bufferLen);
        }
    }
}
