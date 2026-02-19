/**
 * BWT Water Meter BLE-to-MQTT Bridge
 *
 * Arduino framework firmware for ESP32 that connects to a BWT smart
 * water meter via BLE, reads consumption data, and publishes to MQTT.
 *
 * See ESP32_FIRMWARE_SPEC.md for full specification.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "bwt_protocol.h"
#include "packet_collector.h"
#include "ble_client.h"
#include "mqtt_publisher.h"
#include "utils.h"

// ─── State Machine ──────────────────────────────────────────

enum FirmwareState
{
  STATE_WIFI_CONNECT,
  STATE_MQTT_CONNECT,
  STATE_IDLE,
  STATE_BLE_SCAN,
  STATE_BLE_CONNECT,
  STATE_READ_BROADCAST,
  STATE_FETCH_QH,
  STATE_BLE_DISCONNECT,
  STATE_MQTT_PUBLISH,
};

static FirmwareState s_state = STATE_WIFI_CONNECT;
static unsigned long s_lastPoll = 0;
static unsigned long s_stateTimer = 0;
static uint8_t s_retryCount = 0;
static bool s_haDiscoverySent = false;

// ─── Poll Cycle Data ────────────────────────────────────────

static BroadcastState s_broadcast;
static ConsumptionEntry *s_qhEntries = nullptr;
static uint16_t s_qhCount = 0;
static struct tm s_readTime; // NTP time at moment of BLE read

// ─── Helpers ────────────────────────────────────────────────

static void freePollData()
{
  if (s_qhEntries)
  {
    free(s_qhEntries);
    s_qhEntries = nullptr;
  }
  s_qhCount = 0;
}

static void changeState(FirmwareState newState)
{
  s_state = newState;
  s_stateTimer = millis();
}

// ─── Setup ──────────────────────────────────────────────────

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("  BWT BLE-to-MQTT Bridge");
  Serial.println("========================================");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

  // Initialize BLE
  bleInit();

  changeState(STATE_WIFI_CONNECT);
}

// ─── Loop ───────────────────────────────────────────────────

void loop()
{
  // Always process MQTT keep-alive if connected
  if (mqttIsConnected())
  {
    mqttLoop();
  }

  switch (s_state)
  {

  // ── WiFi Connect ────────────────────────────────────────
  case STATE_WIFI_CONNECT:
  {
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - wifiStart) < 15000)
    {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.printf("[WiFi] Connected! IP: %s\n",
                    WiFi.localIP().toString().c_str());

      // Initialize NTP time sync
      configTzTime(NTP_TZ, NTP_SERVER);
      Serial.println("[NTP] Syncing time...");
      time_t now = time(nullptr);
      int ntpWait = 0;
      while (now < 1700000000 && ntpWait < 50) // wait up to 10s
      {
        delay(200);
        now = time(nullptr);
        ntpWait++;
      }
      if (now > 1700000000)
      {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        Serial.printf("[NTP] Time: %s\n", buf);
      }
      else
      {
        Serial.println("[NTP] Warning: time sync failed");
      }

      mqttInit();
      changeState(STATE_MQTT_CONNECT);
    }
    else
    {
      Serial.println("[WiFi] Connection failed, retrying in 5s...");
      delay(5000);
      // stay in STATE_WIFI_CONNECT
    }
    break;
  }

  // ── MQTT Connect ────────────────────────────────────────
  case STATE_MQTT_CONNECT:
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[WiFi] Lost connection");
      changeState(STATE_WIFI_CONNECT);
      break;
    }

    if (mqttConnect())
    {
      // Publish HA discovery on first connect
      if (!s_haDiscoverySent)
      {
        mqttPublishHADiscovery();
        s_haDiscoverySent = true;
      }
      s_retryCount = 0;
      // Trigger first poll immediately
      s_lastPoll = millis() - POLL_INTERVAL_MS;
      changeState(STATE_IDLE);
    }
    else
    {
      s_retryCount++;
      uint32_t backoff = min((uint32_t)s_retryCount * 5000, (uint32_t)30000);
      Serial.printf("[MQTT] Retry in %lu ms (attempt %u)\n",
                    (unsigned long)backoff, s_retryCount);
      delay(backoff);
    }
    break;
  }

  // ── Idle (wait for poll interval) ───────────────────────
  case STATE_IDLE:
  {
    // Check WiFi
    if (WiFi.status() != WL_CONNECTED)
    {
      changeState(STATE_WIFI_CONNECT);
      break;
    }
    // Check MQTT
    if (!mqttIsConnected())
    {
      changeState(STATE_MQTT_CONNECT);
      break;
    }

    if ((millis() - s_lastPoll) >= POLL_INTERVAL_MS)
    {
      Serial.println("\n──── Starting poll cycle ────");
      Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
      freePollData();
      changeState(STATE_BLE_SCAN);
    }
    break;
  }

  // ── BLE Scan ────────────────────────────────────────────
  case STATE_BLE_SCAN:
  {
    // Disable WiFi to free the radio for BLE — they share the same
    // antenna/radio on ESP32. Eliminates packet loss during BLE.
    Serial.println("[Main] Turning off WiFi for BLE operations...");
    mqttDisconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    Serial.printf("[Main] WiFi off, free heap: %u bytes\n", ESP.getFreeHeap());

    if (bleScan())
    {
      changeState(STATE_BLE_CONNECT);
    }
    else
    {
      Serial.println("[Main] BLE scan failed, retry next cycle");
      s_lastPoll = millis();
      // Re-enable WiFi before going idle
      changeState(STATE_BLE_DISCONNECT);
    }
    break;
  }

  // ── BLE Connect ─────────────────────────────────────────
  case STATE_BLE_CONNECT:
  {
    if (bleConnect())
    {
      changeState(STATE_READ_BROADCAST);
    }
    else
    {
      Serial.println("[Main] BLE connect failed, retry next cycle");
      bleDisconnect();
      s_lastPoll = millis();
      changeState(STATE_BLE_DISCONNECT); // re-enables WiFi
    }
    break;
  }

  // ── Read Broadcast ──────────────────────────────────────
  case STATE_READ_BROADCAST:
  {
    if (!bleIsConnected())
    {
      Serial.println("[Main] Lost BLE connection");
      changeState(STATE_BLE_DISCONNECT);
      break;
    }

    if (bleReadBroadcast(s_broadcast))
    {
      changeState(STATE_FETCH_QH);
    }
    else
    {
      Serial.println("[Main] Broadcast read failed");
      changeState(STATE_BLE_DISCONNECT);
    }
    break;
  }

  // ── Fetch Quarter-Hour Data ─────────────────────────────
  case STATE_FETCH_QH:
  {
    if (!bleIsConnected())
    {
      changeState(STATE_BLE_DISCONNECT);
      break;
    }

    uint16_t regionSize = QH_END_ADDR - QH_START_ADDR;
    uint16_t reqSize = calculateRequestSize(
        s_broadcast.quarterHoursIdx, s_broadcast.quarterHoursLooped, regionSize);

    if (reqSize == 0)
    {
      Serial.println("[Main] No QH data to fetch");
      changeState(STATE_BLE_DISCONNECT);
      break;
    }

    PacketCollector collector;
    if (!collectorInit(collector, reqSize))
    {
      Serial.println("[Main] QH collector init failed");
      changeState(STATE_BLE_DISCONNECT);
      break;
    }

    if (bleFetchDataset(QH_START_ADDR, reqSize, collector))
    {
      uint16_t numEntries = collector.bufferLen / 2;
      s_qhEntries = (ConsumptionEntry *)malloc(numEntries * sizeof(ConsumptionEntry));
      if (s_qhEntries)
      {
        s_qhCount = parseBuffer(collector.buffer, collector.bufferLen,
                                s_qhEntries, false);

        // Debug: dump first raw bytes and parsed values
        Serial.printf("[Main] QH raw hex (first 20 bytes): ");
        for (uint16_t db = 0; db < 20 && db < collector.bufferLen; db++)
          Serial.printf("%02X ", collector.buffer[db]);
        Serial.println();
        Serial.printf("[Main] QH first 5 parsed: ");
        for (uint16_t dp = 0; dp < 5 && dp < s_qhCount; dp++)
          Serial.printf("[%u]=%uL ", dp, s_qhEntries[dp].litres);
        Serial.println();

        rotateRingBuffer(s_qhEntries, s_qhCount,
                         s_broadcast.quarterHoursIdx,
                         s_broadcast.quarterHoursLooped);
        Serial.printf("[Main] QH: %u entries parsed\n", s_qhCount);
      }
    }
    else
    {
      Serial.println("[Main] QH fetch failed");
    }

    collectorFree(collector);
    changeState(STATE_BLE_DISCONNECT);
    break;
  }

  // ── BLE Disconnect ──────────────────────────────────────
  case STATE_BLE_DISCONNECT:
  {
    bleDisconnect();

    // Re-enable WiFi (was turned off before BLE scan)
    Serial.println("[Main] BLE done, re-enabling WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 15000)
    {
      delay(250);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[Main] WiFi reconnect failed, skipping publish");
      freePollData();
      s_lastPoll = millis();
      changeState(STATE_WIFI_CONNECT);
      break;
    }

    Serial.printf("[Main] WiFi reconnected, IP: %s\n",
                  WiFi.localIP().toString().c_str());

    // Re-sync NTP after WiFi reconnect (SNTP client is lost after WiFi off)
    configTzTime(NTP_TZ, NTP_SERVER);
    {
      time_t now = time(nullptr);
      int ntpWait = 0;
      while (now < 1700000000 && ntpWait < 50) // wait up to 10s
      {
        delay(200);
        now = time(nullptr);
        ntpWait++;
      }
      if (now > 1700000000)
      {
        localtime_r(&now, &s_readTime);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &s_readTime);
        Serial.printf("[NTP] Time re-synced: %s\n", buf);
      }
      else
      {
        Serial.println("[NTP] Warning: time re-sync failed, dates will be wrong");
        // Still capture whatever time we have
        localtime_r(&now, &s_readTime);
      }
    }

    // Fresh MQTT connection on clean TCP socket
    mqttInit(); // re-set server in case WiFiClient was reset
    if (!mqttConnect())
    {
      Serial.println("[Main] MQTT connect failed after BLE, retrying...");
      delay(2000);
      if (!mqttConnect())
      {
        Serial.println("[Main] MQTT connect failed twice, skipping publish");
        freePollData();
        s_lastPoll = millis();
        changeState(STATE_IDLE);
        break;
      }
    }

    changeState(STATE_MQTT_PUBLISH);
    break;
  }

  // ── MQTT Publish ────────────────────────────────────────
  case STATE_MQTT_PUBLISH:
  {
    // Verify MQTT is alive (should be — reconnected in BLE_DISCONNECT)
    if (!mqttEnsureConnected())
    {
      Serial.println("[Main] MQTT not connected, skipping publish");
      freePollData();
      s_lastPoll = millis();
      changeState(STATE_IDLE);
      break;
    }

    // Publish device status (remaining capacity, alarm, etc.)
    mqttPublishStatus(s_broadcast);

    if (s_qhEntries && s_qhCount > 0)
    {
      // Reverse QH array to newest-first order
      for (uint16_t i = 0; i < s_qhCount / 2; i++)
      {
        ConsumptionEntry tmp = s_qhEntries[i];
        s_qhEntries[i] = s_qhEntries[s_qhCount - 1 - i];
        s_qhEntries[s_qhCount - 1 - i] = tmp;
      }

      // Meter: last completed 15-min consumption.
      // Index 0 is the in-progress slot; index 1 is the last fully completed one.
      if (PUBLISH_METER && s_qhCount >= 2)
      {
        mqttPublishMeter(s_qhEntries[1].litres);
      }

      // Daily history with calendar dates (computed from QH sums)
      if (PUBLISH_DAILY_HISTORY)
      {
        mqttPublishDailyHistory(s_qhEntries, s_qhCount, s_readTime);
      }

      // Hourly history with timestamps
      if (PUBLISH_HOURLY_HISTORY)
      {
        mqttPublishHourlyHistory(s_qhEntries, s_qhCount, s_readTime);
      }
    }

    // Done — free data and go idle
    freePollData();
    s_lastPoll = millis();
    Serial.println("──── Poll cycle complete ────\n");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    changeState(STATE_IDLE);
    break;
  }

  } // end switch
}