#pragma once

#include "bwt_protocol.h"
#include "packet_collector.h"
#include <stdint.h>

/**
 * Initialize NimBLE stack. Call once in setup().
 */
void bleInit();

/**
 * Scan for the BWT device. Returns true if found.
 */
bool bleScan();

/**
 * Connect to the BWT device found during scan.
 * Requests MTU 512, discovers service and characteristics.
 * Returns true on success.
 */
bool bleConnect();

/**
 * Disconnect from the BWT device.
 */
void bleDisconnect();

/**
 * Check if BLE client is currently connected.
 */
bool bleIsConnected();

/**
 * Read the broadcast characteristic (F2E3) and parse into BroadcastState.
 * Returns true on success.
 */
bool bleReadBroadcast(BroadcastState &state);

/**
 * Fetch a dataset (daily or quarter-hour) from the device.
 *   address   - start address of the memory region
 *   size      - number of bytes to request
 *   collector - pre-initialized PacketCollector
 * Subscribes to F2E1 notifications, writes trigger to F2E2,
 * waits until complete or timeout, then unsubscribes.
 * Returns true if collection completed successfully.
 */
bool bleFetchDataset(uint16_t address, uint16_t size, PacketCollector &collector);
