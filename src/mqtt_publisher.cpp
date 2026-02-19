#include "mqtt_publisher.h"
#include "config.h"
#include "bwt_protocol.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ─── Module State ───────────────────────────────────────────

static WiFiClient s_wifiClient;
static PubSubClient s_mqtt(s_wifiClient);

// ─── Helper: build topic string ─────────────────────────────

static String buildTopic(const char *suffix)
{
    return String(MQTT_TOPIC_PREFIX) + "/" + suffix;
}

// ─── Public Functions ───────────────────────────────────────

void mqttInit()
{
    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    Serial.printf("[MQTT] Configured: %s:%d, buffer=%d\n",
                  MQTT_HOST, MQTT_PORT, MQTT_BUFFER_SIZE);
}

bool mqttConnect()
{
    if (s_mqtt.connected())
        return true;

    Serial.printf("[MQTT] Connecting as '%s'...\n", MQTT_CLIENT_ID);

    bool ok;
    if (strlen(MQTT_USER) > 0)
    {
        ok = s_mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
    }
    else
    {
        ok = s_mqtt.connect(MQTT_CLIENT_ID);
    }

    if (ok)
    {
        Serial.println("[MQTT] Connected");
    }
    else
    {
        Serial.printf("[MQTT] Connection failed, rc=%d\n", s_mqtt.state());
    }
    return ok;
}

void mqttDisconnect()
{
    if (s_mqtt.connected())
    {
        s_mqtt.disconnect();
    }
    s_wifiClient.stop();
}

bool mqttForceReconnect()
{
    Serial.println("[MQTT] Force reconnecting (fresh TCP socket)...");
    s_mqtt.disconnect();
    s_wifiClient.stop();
    delay(500);

    // Re-check WiFi
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[MQTT] WiFi down, reconnecting...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 10000)
        {
            delay(250);
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("[MQTT] WiFi reconnect failed");
            return false;
        }
        Serial.printf("[MQTT] WiFi reconnected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }
    else
    {
        Serial.printf("[MQTT] WiFi OK, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }

    // Reconnect MQTT
    return mqttConnect();
}

bool mqttEnsureConnected()
{
    if (s_mqtt.connected())
    {
        s_mqtt.loop();
        return true;
    }
    return mqttForceReconnect();
}

void mqttLoop()
{
    s_mqtt.loop();
}

bool mqttIsConnected()
{
    return s_mqtt.connected();
}

// ─── Publish Status ─────────────────────────────────────────

bool mqttPublishStatus(const BroadcastState &state)
{
    JsonDocument doc;

    doc["remaining_litres"] = state.remaining;
    doc["total_capacity_litres"] = state.totalCapacity;

    if (state.totalCapacity > 0)
    {
        doc["percentage"] = (double)state.remaining / (double)state.totalCapacity;
    }
    else
    {
        doc["percentage"] = 0;
    }

    doc["alarm"] = state.alarm;
    doc["regen_count"] = state.regen;

    char fwBuf[16];
    snprintf(fwBuf, sizeof(fwBuf), "%u.%u", state.versionA, state.versionB);
    doc["firmware"] = fwBuf;

    // Real timestamp from NTP
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char tsBuf[32];
    strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    doc["timestamp"] = tsBuf;

    String payload;
    serializeJson(doc, payload);

    String topic = buildTopic("status");
    bool ok = s_mqtt.publish(topic.c_str(), payload.c_str(), true); // retained
    Serial.printf("[MQTT] Published status (%u bytes): %s\n",
                  payload.length(), ok ? "OK" : "FAIL");
    return ok;
}

// ─── Publish Meter (last 15-min consumption) ────────────────

bool mqttPublishMeter(uint16_t litres)
{
    char payload[16];
    snprintf(payload, sizeof(payload), "%u", litres);

    String topic = buildTopic("meter");
    bool ok = s_mqtt.publish(topic.c_str(), payload, true); // retained
    Serial.printf("[MQTT] Meter: %u L -> %s\n", litres, ok ? "OK" : "FAIL");
    return ok;
}

// ─── Publish Daily History ──────────────────────────────────

bool mqttPublishDailyHistory(const ConsumptionEntry *qh, uint16_t qhCount,
                             const struct tm &readTime)
{
    // How many QH slots belong to "today" including the current in-progress slot.
    // The newest QH entry (index 0) is the in-progress slot that the device is
    // currently accumulating, so we add 1 to skip past it when computing day
    // boundaries for full past days.
    // e.g. at 14:37 → 14*4 + floor(37/15) + 1 = 59 slots assigned to today
    int slotsIntoToday = readTime.tm_hour * 4 + readTime.tm_min / 15 + 1;

    int maxDays = DAILY_HISTORY_DAYS;
    if (maxDays > 119)
        maxDays = 119; // QH buffer = 2880 entries = 120 days max

    JsonDocument doc;

    char tsBuf[32];
    strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%dT%H:%M:%S", &readTime);
    doc["timestamp"] = tsBuf;

    JsonArray days = doc["days"].to<JsonArray>();
    int count = 0;

    for (int day = 0; day < maxDays; day++)
    {
        int startIdx, endIdx;
        bool complete;

        if (day == 0)
        {
            // Today (partial): indices 0 .. slotsIntoToday-1
            startIdx = 0;
            endIdx = slotsIntoToday - 1;
            complete = false;
        }
        else
        {
            // Full past day: 96 slots per day
            startIdx = slotsIntoToday + (day - 1) * 96;
            endIdx = startIdx + 95;
            complete = (endIdx < qhCount);
        }

        if (startIdx >= (int)qhCount)
            break;
        if (endIdx >= (int)qhCount)
            endIdx = qhCount - 1;

        uint32_t sum = 0;
        for (int i = startIdx; i <= endIdx; i++)
        {
            sum += qh[i].litres;
        }

        // Compute calendar date: readTime - day days
        struct tm dayTime = readTime;
        dayTime.tm_mday -= day;
        mktime(&dayTime); // normalize (handles month/year rollover)

        char dateBuf[16];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &dayTime);

        JsonObject entry = days.add<JsonObject>();
        entry["date"] = dateBuf;
        entry["litres"] = sum;
        entry["complete"] = complete;

        count++;
    }

    doc["count"] = count;

    String payload;
    serializeJson(doc, payload);

    String topic = buildTopic("daily");
    bool ok = s_mqtt.publish(topic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Daily history: %d days (%u bytes): %s\n",
                  count, payload.length(), ok ? "OK" : "FAIL");
    return ok;
}

// ─── Publish Hourly History ─────────────────────────────────

bool mqttPublishHourlyHistory(const ConsumptionEntry *qh, uint16_t qhCount,
                              const struct tm &readTime)
{
    // How many QH slots belong to the current wall-clock hour, including the
    // in-progress slot.  The newest QH entry (index 0) is the slot the device
    // is still accumulating, so +1 keeps all boundaries aligned with BWT's app.
    // e.g. at 14:37 → floor(37/15) + 1 = 3 slots assigned to the current hour
    int slotsIntoCurrentHour = readTime.tm_min / 15 + 1;

    int maxHours = HOURLY_HISTORY_HOURS;
    if (maxHours > 719)
        maxHours = 719; // QH = 2880 slots = 720 hours max

    JsonDocument doc;

    char tsBuf[32];
    strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%dT%H:%M:%S", &readTime);
    doc["timestamp"] = tsBuf;

    JsonArray hours = doc["hours"].to<JsonArray>();
    int count = 0;

    for (int hour = 0; hour < maxHours; hour++)
    {
        int startIdx, endIdx;
        bool complete;

        if (hour == 0)
        {
            // Current hour (partial)
            startIdx = 0;
            endIdx = slotsIntoCurrentHour - 1;
            complete = false;
        }
        else
        {
            // Full past hour: 4 QH slots each
            startIdx = slotsIntoCurrentHour + (hour - 1) * 4;
            endIdx = startIdx + 3;
            complete = (endIdx < (int)qhCount);
        }

        if (startIdx >= (int)qhCount)
            break;
        if (endIdx >= (int)qhCount)
            endIdx = qhCount - 1;

        uint32_t sum = 0;
        for (int i = startIdx; i <= endIdx; i++)
        {
            sum += qh[i].litres;
        }

        // Compute hour timestamp
        struct tm hourTime = readTime;
        hourTime.tm_hour -= hour;
        hourTime.tm_min = 0;
        hourTime.tm_sec = 0;
        mktime(&hourTime); // normalize

        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M", &hourTime);

        JsonObject entry = hours.add<JsonObject>();
        entry["time"] = timeBuf;
        entry["litres"] = sum;
        entry["complete"] = complete;

        count++;
    }

    doc["count"] = count;

    String payload;
    serializeJson(doc, payload);

    String topic = buildTopic("hourly");
    bool ok = s_mqtt.publish(topic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Hourly history: %d hours (%u bytes): %s\n",
                  count, payload.length(), ok ? "OK" : "FAIL");
    return ok;
}

// ─── Home Assistant Discovery ───────────────────────────────

bool mqttPublishHADiscovery()
{
    // Remaining litres sensor
    {
        JsonDocument doc;
        doc["name"] = "BWT Remaining Capacity";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ value_json.remaining_litres }}";
        doc["unit_of_measurement"] = "L";
        doc["device_class"] = "water";
        doc["unique_id"] = "bwt_water_remaining";

        JsonObject device = doc["device"].to<JsonObject>();
        device["identifiers"][0] = "bwt_water_meter";
        device["name"] = "BWT Water Meter";
        device["manufacturer"] = "BWT";
        device["model"] = "Perla";

        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish("homeassistant/sensor/bwt_water_remaining/config",
                       payload.c_str(), true);
    }

    // Percentage sensor
    {
        JsonDocument doc;
        doc["name"] = "BWT Capacity Percentage";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ (value_json.percentage * 100) | round(1) }}";
        doc["unit_of_measurement"] = "%";
        doc["unique_id"] = "bwt_water_percentage";

        JsonObject device = doc["device"].to<JsonObject>();
        device["identifiers"][0] = "bwt_water_meter";
        device["name"] = "BWT Water Meter";
        device["manufacturer"] = "BWT";
        device["model"] = "Perla";

        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish("homeassistant/sensor/bwt_water_percentage/config",
                       payload.c_str(), true);
    }

    // Alarm binary sensor
    {
        JsonDocument doc;
        doc["name"] = "BWT Alarm";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ 'ON' if value_json.alarm else 'OFF' }}";
        doc["device_class"] = "problem";
        doc["unique_id"] = "bwt_water_alarm";

        JsonObject device = doc["device"].to<JsonObject>();
        device["identifiers"][0] = "bwt_water_meter";
        device["name"] = "BWT Water Meter";
        device["manufacturer"] = "BWT";
        device["model"] = "Perla";

        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish("homeassistant/binary_sensor/bwt_water_alarm/config",
                       payload.c_str(), true);
    }

    // Regen counter sensor
    {
        JsonDocument doc;
        doc["name"] = "BWT Regen Count";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ value_json.regen_count }}";
        doc["unique_id"] = "bwt_water_regen";
        doc["icon"] = "mdi:refresh";

        JsonObject device = doc["device"].to<JsonObject>();
        device["identifiers"][0] = "bwt_water_meter";
        device["name"] = "BWT Water Meter";
        device["manufacturer"] = "BWT";
        device["model"] = "Perla";

        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish("homeassistant/sensor/bwt_water_regen/config",
                       payload.c_str(), true);
    }

    // Meter sensor (last 15-min consumption)
    {
        JsonDocument doc;
        doc["name"] = "BWT 15min Consumption";
        doc["state_topic"] = buildTopic("meter");
        doc["unit_of_measurement"] = "L";
        doc["device_class"] = "water";
        doc["state_class"] = "measurement";
        doc["unique_id"] = "bwt_water_meter_15min";
        doc["icon"] = "mdi:water-pump";

        JsonObject device = doc["device"].to<JsonObject>();
        device["identifiers"][0] = "bwt_water_meter";
        device["name"] = "BWT Water Meter";
        device["manufacturer"] = "BWT";
        device["model"] = "Perla";

        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish("homeassistant/sensor/bwt_water_meter_15min/config",
                       payload.c_str(), true);
    }

    Serial.println("[MQTT] HA Discovery messages published");
    return true;
}
