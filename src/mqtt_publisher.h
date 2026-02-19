#pragma once

#include "bwt_protocol.h"
#include <stdint.h>
#include <time.h>

/**
 * Initialize MQTT client (set server, buffer size, etc).
 * Call once in setup() after WiFi is connected.
 */
void mqttInit();

/**
 * Ensure MQTT is connected. Attempts reconnect if needed.
 * Returns true if connected.
 */
bool mqttConnect();

/**
 * Disconnect MQTT cleanly.
 */
void mqttDisconnect();

/**
 * Force disconnect and reconnect MQTT with a fresh TCP socket.
 * Use after BLE session when the TCP connection may be stale.
 * Returns true if reconnected successfully.
 */
bool mqttForceReconnect();

/**
 * Verify WiFi + MQTT are both up; reconnect if needed.
 * Returns true if MQTT is connected and ready.
 */
bool mqttEnsureConnected();

/**
 * Call in loop() to process MQTT keep-alive.
 */
void mqttLoop();

/**
 * Check if MQTT is connected.
 */
bool mqttIsConnected();

/**
 * Publish device status from broadcast data.
 */
bool mqttPublishStatus(const BroadcastState &state);

/**
 * Publish last completed 15-min consumption as a plain number.
 * Topic: bwt/water/meter  (retained)
 * Payload: just the litres integer, e.g. "4"
 */
bool mqttPublishMeter(uint16_t litres);

/**
 * Publish daily consumption history with dates.
 * Computed by summing QH entries per calendar day (excluding regen slots).
 * qhEntries must be in newest-first order.
 *
 * Topic: bwt/water/daily  (retained, single JSON message)
 */
bool mqttPublishDailyHistory(const ConsumptionEntry *qhEntries, uint16_t qhCount,
                             const struct tm &readTime);

/**
 * Publish hourly consumption history with timestamps.
 * Computed by summing 4 consecutive QH entries per wall-clock hour.
 * qhEntries must be in newest-first order.
 *
 * Topic: bwt/water/hourly  (retained, single JSON message)
 */
bool mqttPublishHourlyHistory(const ConsumptionEntry *qhEntries, uint16_t qhCount,
                              const struct tm &readTime);

/**
 * Publish Home Assistant auto-discovery config messages.
 */
bool mqttPublishHADiscovery();
