#pragma once

#include <stdint.h>

/**
 * Read a uint16 little-endian value from a byte buffer.
 */
inline uint16_t readUint16LE(const uint8_t *buf, uint16_t offset)
{
    return (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
}

/**
 * Read a uint16 big-endian value from a byte buffer.
 * The BWT device stores ring-buffer data words in big-endian format.
 */
inline uint16_t readUint16BE(const uint8_t *buf, uint16_t offset)
{
    return ((uint16_t)buf[offset] << 8) | (uint16_t)buf[offset + 1];
}

/**
 * Rotate a ring buffer so that the entry at `splitIdx` becomes
 * the first element (oldest → newest chronological order).
 * Only rotates if `looped` is true.
 *
 * Uses in-place std::rotate via a temp buffer approach.
 */
template <typename T>
void rotateRingBuffer(T *entries, uint16_t count, uint16_t splitIdx, bool looped)
{
    if (!looped || count == 0 || splitIdx == 0 || splitIdx >= count)
        return;
    // Allocate temp buffer for the tail portion
    uint16_t tailLen = count - splitIdx;
    T *temp = new T[tailLen];
    // Copy tail (splitIdx..count) to temp
    for (uint16_t i = 0; i < tailLen; i++)
    {
        temp[i] = entries[splitIdx + i];
    }
    // Shift head (0..splitIdx) to the end
    for (int32_t i = (int32_t)splitIdx - 1; i >= 0; i--)
    {
        entries[tailLen + i] = entries[i];
    }
    // Copy temp back to beginning
    for (uint16_t i = 0; i < tailLen; i++)
    {
        entries[i] = temp[i];
    }
    delete[] temp;
}
