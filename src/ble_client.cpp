#include "ble_client.h"
#include "config.h"
#include "bwt_protocol.h"
#include "packet_collector.h"
#include "utils.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

// ─── NimBLE RC to string helper ─────────────────────────────

static const char *nimbleRCtoStr(int rc)
{
    switch (rc)
    {
    case 0:
        return "SUCCESS";
    case (0x0200 + 0):
        return "BLE_HS_EALREADY (already in progress)";
    case (0x0200 + 1):
        return "BLE_HS_EINVAL (invalid params)";
    case (0x0200 + 2):
        return "BLE_HS_EMSGSIZE (msg too large)";
    case (0x0200 + 3):
        return "BLE_HS_ENOENT (no entry/no device)";
    case (0x0200 + 4):
        return "BLE_HS_ENOMEM (out of memory)";
    case (0x0200 + 5):
        return "BLE_HS_ENOTCONN (not connected)";
    case (0x0200 + 6):
        return "BLE_HS_ENOTSUP (not supported)";
    case (0x0200 + 7):
        return "BLE_HS_EAPP (application error)";
    case (0x0200 + 8):
        return "BLE_HS_EBADDATA (bad data)";
    case (0x0200 + 9):
        return "BLE_HS_EOS (OS error)";
    case (0x0200 + 10):
        return "BLE_HS_ECONTROLLER (controller error)";
    case (0x0200 + 11):
        return "BLE_HS_ETIMEOUT (timeout)";
    case (0x0200 + 12):
        return "BLE_HS_EDONE (done)";
    case (0x0200 + 13):
        return "BLE_HS_EBUSY (busy)";
    case (0x0200 + 14):
        return "BLE_HS_EREJECT (rejected)";
    case (0x0200 + 15):
        return "BLE_HS_EUNKNOWN (unknown)";
    case (0x0200 + 16):
        return "BLE_HS_EROLE (wrong role)";
    case (0x0200 + 17):
        return "BLE_HS_ETIMEOUT_HCI (HCI timeout)";
    case (0x0200 + 18):
        return "BLE_HS_ENOMEM_EVT (no memory for event)";
    case (0x0200 + 19):
        return "BLE_HS_ENOADDR (no address)";
    case (0x0200 + 20):
        return "BLE_HS_ENOTSYNCED (not synced)";
    case (0x0200 + 21):
        return "BLE_HS_EAUTHEN (auth failure)";
    case (0x0200 + 22):
        return "BLE_HS_EAUTHOR (authorization failure)";
    case (0x0200 + 23):
        return "BLE_HS_EENCRYPT (encryption error)";
    case (0x0200 + 24):
        return "BLE_HS_EENCRYPT_KEY_SZ (key size error)";
    case (0x0200 + 25):
        return "BLE_HS_ESTORE_CAP (store capacity exceeded)";
    case (0x0200 + 26):
        return "BLE_HS_ESTORE_FAIL (store failure)";
    default:
    {
        // HCI errors are also at base 0x0200; extract HCI code
        if (rc >= 0x0200 && rc <= 0x02FF)
        {
            uint8_t hci = rc - 0x0200;
            switch (hci)
            {
            case 0x02:
                return "HCI: Unknown Connection ID";
            case 0x06:
                return "HCI: PIN or Key Missing";
            case 0x07:
                return "HCI: Memory Capacity Exceeded";
            case 0x08:
                return "HCI: Connection Timeout";
            case 0x09:
                return "HCI: Connection Limit Exceeded";
            case 0x0C:
                return "HCI: Command Disallowed";
            case 0x12:
                return "HCI: Invalid HCI Params";
            case 0x13:
                return "HCI: Remote User Terminated";
            case 0x16:
                return "HCI: Local Host Terminated";
            case 0x1A:
                return "HCI: Unsupported Param Value";
            case 0x22:
                return "HCI: Instant Passed";
            case 0x28:
                return "HCI: Controller Busy";
            case 0x3B:
                return "HCI: Unacceptable Conn Params";
            case 0x3C:
                return "HCI: Directed Advertising Timeout";
            case 0x3D:
                return "HCI: Conn Terminated (MIC Failure)";
            case 0x3E:
                return "HCI: Connection Failed to be Established";
            default:
                return "HCI: Unknown";
            }
        }
        return "UNKNOWN";
    }
    }
}

static const char *addrTypeToStr(uint8_t type)
{
    switch (type)
    {
    case 0:
        return "PUBLIC";
    case 1:
        return "RANDOM";
    case 2:
        return "RPA_PUBLIC";
    case 3:
        return "RPA_RANDOM";
    default:
        return "UNKNOWN";
    }
}

// ─── Client Callbacks (disconnect reason) ───────────────────

class ClientCallbacks : public NimBLEClientCallbacks
{
    void onConnect(NimBLEClient *pClient) override
    {
        Serial.printf("[BLE-CB] onConnect: peer=%s\n",
                      pClient->getPeerAddress().toString().c_str());
    }

    void onDisconnect(NimBLEClient *pClient) override
    {
        Serial.printf("[BLE-CB] onDisconnect: reason=%d (0x%02X)\n",
                      pClient->getLastError(), pClient->getLastError());
    }

    bool onConnParamsUpdateRequest(NimBLEClient *pClient, const ble_gap_upd_params *params) override
    {
        Serial.printf("[BLE-CB] Conn param update: itvl_min=%u, itvl_max=%u, latency=%u, timeout=%u\n",
                      params->itvl_min, params->itvl_max,
                      params->latency, params->supervision_timeout);
        return true; // accept
    }
};

static ClientCallbacks s_clientCallbacks;

// ─── Module State ───────────────────────────────────────────

static NimBLEAddress s_targetAddr{};
static uint8_t s_targetAddrType = 0;
static int s_targetRSSI = 0;
static bool s_targetFound = false;
static NimBLEClient *s_client = nullptr;
static NimBLERemoteService *s_service = nullptr;
static NimBLERemoteCharacteristic *s_charBuffer = nullptr;    // F2E1
static NimBLERemoteCharacteristic *s_charTrigger = nullptr;   // F2E2
static NimBLERemoteCharacteristic *s_charBroadcast = nullptr; // F2E3

// Pointer to the active collector (used by notification callback)
static PacketCollector *s_activeCollector = nullptr;

// ─── Notification Callback ──────────────────────────────────

static void notifyCallback(NimBLERemoteCharacteristic *pChar,
                           uint8_t *pData, size_t length, bool isNotify)
{
    if (s_activeCollector)
    {
        collectorOnPacket(*s_activeCollector, pData, (uint16_t)length);
    }
}

// ─── Scan Callback ──────────────────────────────────────────

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
public:
    bool deviceFound = false;

    void onResult(NimBLEAdvertisedDevice *advertisedDevice) override
    {
        String macFilter(BWT_DEVICE_MAC);
        if (macFilter.length() > 0)
        {
            // Match by MAC address
            std::string addr = advertisedDevice->getAddress().toString();
            if (String(addr.c_str()) == macFilter)
            {
                Serial.printf("[BLE] Found device by MAC: %s (RSSI: %d, addrType: %s)\n",
                              addr.c_str(),
                              advertisedDevice->getRSSI(),
                              addrTypeToStr(advertisedDevice->getAddress().getType()));
                // Save address & type before scan results are cleared
                s_targetAddr = advertisedDevice->getAddress();
                s_targetAddrType = advertisedDevice->getAddress().getType();
                s_targetRSSI = advertisedDevice->getRSSI();
                s_targetFound = true;
                deviceFound = true;
                NimBLEDevice::getScan()->stop();
            }
        }
        else
        {
            // Match by name prefix
            if (advertisedDevice->haveName() &&
                advertisedDevice->getName().find(BWT_DEVICE_NAME) != std::string::npos)
            {
                Serial.printf("[BLE] Found device by name: %s (%s, RSSI: %d, addrType: %s)\n",
                              advertisedDevice->getName().c_str(),
                              advertisedDevice->getAddress().toString().c_str(),
                              advertisedDevice->getRSSI(),
                              addrTypeToStr(advertisedDevice->getAddress().getType()));
                // Save address & type before scan results are cleared
                s_targetAddr = advertisedDevice->getAddress();
                s_targetAddrType = advertisedDevice->getAddress().getType();
                s_targetRSSI = advertisedDevice->getRSSI();
                s_targetFound = true;
                deviceFound = true;
                NimBLEDevice::getScan()->stop();
            }
        }
    }
};

static ScanCallbacks s_scanCallbacks;

// ─── Public Functions ───────────────────────────────────────

void bleInit()
{
    NimBLEDevice::init("bwt-bridge");
    // Set power to max for better range
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    Serial.println("[BLE] NimBLE initialized");
}

bool bleScan()
{
    s_targetFound = false;
    s_scanCallbacks.deviceFound = false;

    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&s_scanCallbacks, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);

    Serial.println("[BLE] Starting scan...");
    pScan->start(BLE_SCAN_DURATION_SEC, false);

    // Block until scan completes or device found
    unsigned long start = millis();
    while (!s_scanCallbacks.deviceFound &&
           (millis() - start) < (BLE_SCAN_DURATION_SEC * 1000 + 1000))
    {
        delay(100);
    }

    pScan->clearResults();

    if (s_scanCallbacks.deviceFound && s_targetFound)
    {
        Serial.println("[BLE] Target device found");
        return true;
    }

    Serial.println("[BLE] Target device NOT found");
    return false;
}

bool bleConnect()
{
    if (!s_targetFound)
    {
        Serial.println("[BLE] No target device to connect to");
        return false;
    }

    for (int attempt = 1; attempt <= BLE_CONNECT_RETRIES; attempt++)
    {
        // Create or reuse client
        if (s_client)
        {
            if (s_client->isConnected())
            {
                s_client->disconnect();
            }
            NimBLEDevice::deleteClient(s_client);
            s_client = nullptr;
        }

        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_clientCallbacks, false);
        s_client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS / 1000); // NimBLE uses seconds

        Serial.printf("[BLE] Connecting to %s (addrType: %s, RSSI: %d, timeout: %ds, attempt %d/%d)...\n",
                      s_targetAddr.toString().c_str(),
                      addrTypeToStr(s_targetAddrType),
                      s_targetRSSI,
                      BLE_CONNECT_TIMEOUT_MS / 1000,
                      attempt, BLE_CONNECT_RETRIES);
        Serial.printf("[BLE] NimBLE client count: %d, free heap: %u\n",
                      NimBLEDevice::getClientListSize(), ESP.getFreeHeap());

        if (!s_client->connect(s_targetAddr, s_targetAddrType))
        {
            int lastErr = s_client->getLastError();
            Serial.printf("[BLE] Connection attempt %d FAILED — RC: %d (0x%04X) = %s\n",
                          attempt, lastErr, lastErr, nimbleRCtoStr(lastErr));
            if (attempt < BLE_CONNECT_RETRIES)
            {
                uint32_t backoff = attempt * BLE_CONNECT_RETRY_DELAY_MS;
                Serial.printf("[BLE] Retrying in %lu ms...\n", (unsigned long)backoff);
                delay(backoff);
            }
            continue;
        }

        Serial.println("[BLE] Connected, discovering services...");

        // Discover the BWT service
        s_service = s_client->getService(BWT_SERVICE_UUID);
        if (!s_service)
        {
            Serial.println("[BLE] BWT service not found!");
            s_client->disconnect();
            return false;
        }

        // Get characteristics
        s_charBuffer = s_service->getCharacteristic(BWT_CHAR_BUFFER_UUID);
        s_charTrigger = s_service->getCharacteristic(BWT_CHAR_TRIGGER_UUID);
        s_charBroadcast = s_service->getCharacteristic(BWT_CHAR_BROADCAST_UUID);

        if (!s_charBuffer || !s_charTrigger || !s_charBroadcast)
        {
            Serial.println("[BLE] Missing characteristic(s)!");
            Serial.printf("  Buffer(F2E1): %s, Trigger(F2E2): %s, Broadcast(F2E3): %s\n",
                          s_charBuffer ? "OK" : "MISSING",
                          s_charTrigger ? "OK" : "MISSING",
                          s_charBroadcast ? "OK" : "MISSING");
            s_client->disconnect();
            return false;
        }

        Serial.println("[BLE] Service and characteristics discovered");
        return true;
    }

    Serial.printf("[BLE] All %d connection attempts failed\n", BLE_CONNECT_RETRIES);
    return false;
}

void bleDisconnect()
{
    s_activeCollector = nullptr;
    s_charBuffer = nullptr;
    s_charTrigger = nullptr;
    s_charBroadcast = nullptr;
    s_service = nullptr;

    if (s_client && s_client->isConnected())
    {
        s_client->disconnect();
        Serial.println("[BLE] Disconnected");
    }
}

bool bleIsConnected()
{
    return s_client && s_client->isConnected();
}

bool bleReadBroadcast(BroadcastState &state)
{
    if (!s_charBroadcast)
    {
        Serial.println("[BLE] Broadcast characteristic not available");
        return false;
    }

    NimBLEAttValue val = s_charBroadcast->readValue();
    if (val.length() < 15)
    {
        Serial.printf("[BLE] Broadcast read returned %u bytes (expected 15)\n",
                      val.length());
        return false;
    }

    bool ok = parseBroadcast(val.data(), val.length(), state);
    if (ok)
    {
        Serial.printf("[BLE] Broadcast: remaining=%lu, QH_idx=%u, days_idx=%u, "
                      "regen=%u, capacity=%lu, alarm=%d, qhLoop=%d, dLoop=%d, v=%u.%u\n",
                      (unsigned long)state.remaining,
                      state.quarterHoursIdx, state.daysIdx,
                      state.regen, (unsigned long)state.totalCapacity,
                      state.alarm, state.quarterHoursLooped, state.daysLooped,
                      state.versionA, state.versionB);
    }
    return ok;
}

bool bleFetchDataset(uint16_t address, uint16_t size, PacketCollector &collector)
{
    if (!s_charBuffer || !s_charTrigger)
    {
        Serial.println("[BLE] Characteristics not available for fetch");
        return false;
    }

    if (size == 0)
    {
        Serial.println("[BLE] Nothing to fetch (size=0)");
        return false;
    }

    // Point the notification callback at this collector
    s_activeCollector = &collector;

    // Subscribe to notifications on F2E1
    if (!s_charBuffer->subscribe(true, notifyCallback))
    {
        Serial.println("[BLE] Failed to subscribe to F2E1 notifications");
        s_activeCollector = nullptr;
        return false;
    }

    // Build and write trigger command
    uint8_t cmd[7];
    buildTriggerCommand(address, size, cmd);
    Serial.printf("[BLE] Trigger: addr=0x%04X, size=%u, expected %u packets\n",
                  address, size, collector.expectedPackets);

    if (!s_charTrigger->writeValue(cmd, 7, true))
    {
        Serial.println("[BLE] Failed to write trigger command");
        s_charBuffer->unsubscribe();
        s_activeCollector = nullptr;
        return false;
    }

    // Wait for collection to complete or timeout
    unsigned long start = millis();
    while (!collector.complete && !collector.error &&
           (millis() - start) < BLE_PACKET_TIMEOUT_MS)
    {
        delay(10);
    }

    // Unsubscribe
    s_charBuffer->unsubscribe();
    s_activeCollector = nullptr;

    if (collector.error)
    {
        Serial.println("[BLE] Packet collection error");
        return false;
    }

    if (!collector.complete)
    {
        Serial.printf("[BLE] Timeout: received %u/%u packets\n",
                      collector.receivedPackets, collector.expectedPackets);
        return false;
    }

    Serial.printf("[BLE] Dataset fetched: %u bytes in %u packets\n",
                  collector.bufferLen, collector.receivedPackets);
    return true;
}
