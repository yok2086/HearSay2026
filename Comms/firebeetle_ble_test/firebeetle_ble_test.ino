#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

// ---- Fill these in ----
const char* DEVICE_ID = "glove_01";
// ------------------------

// Must match ble_bridge.py exactly
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define TELEMETRY_CHAR_UUID    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // NOTIFY: HELLO / TELEMETRY / CONFIG_ACK
#define COMMAND_CHAR_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // WRITE:  CONFIG from laptop

BLECharacteristic* telemetryChar;
BLECharacteristic* commandChar;
BLEServer* bleServer;

bool deviceConnected = false;
bool isRunning = false;
String sessionId = "sess_001";
uint32_t seq = 0;
unsigned long sampleRateHz = 50;
unsigned long lastSampleMs = 0;

void publishConfigAck();

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    deviceConnected = true;
    Serial.println("BLE central connected");
  }
  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;
    isRunning = false;
    Serial.println("BLE central disconnected - restarting advertising");
    delay(200);
    server->getAdvertising()->start();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = String(characteristic->getValue().c_str());
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, value);
    if (err) {
      Serial.println("Failed to parse incoming BLE command");
      return;
    }

    String type = doc["type"].as<String>();
    if (type == "CONFIG") {
      sampleRateHz = doc["payload"]["sample_rate_hz"] | 50;
      Serial.print("Received CONFIG, sample_rate_hz=");
      Serial.println(sampleRateHz);
      publishConfigAck();
    }
  }
};

void notifyJson(JsonDocument& doc) {
  char buf[320];
  size_t len = serializeJson(doc, buf);
  telemetryChar->setValue((uint8_t*)buf, len);
  telemetryChar->notify();
}

void publishHello() {
  StaticJsonDocument<256> doc;
  doc["type"] = "HELLO";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  JsonArray caps = doc.createNestedObject("payload").createNestedArray("capabilities");
  caps.add("IMU");
  caps.add("FLEX");

  notifyJson(doc);
  Serial.println("Notified HELLO");
}

void publishConfigAck() {
  StaticJsonDocument<192> doc;
  doc["type"] = "CONFIG_ACK";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  doc["payload"]["status"] = "OK";

  notifyJson(doc);
  Serial.println("Notified CONFIG_ACK -> now RUNNING");
  isRunning = true;
}

enum GestureRegime { REGIME_OPEN_HAND, REGIME_CLOSED_FIST, REGIME_SHAKING, REGIME_IDLE };
GestureRegime currentRegime = REGIME_IDLE;
unsigned long regimeStartMs = 0;
unsigned long regimeDurationMs = 3000;   // how long each simulated gesture "holds"

void maybeSwitchRegime() {
  if (millis() - regimeStartMs > regimeDurationMs) {
    currentRegime = (GestureRegime)((currentRegime + 1) % 4);   // strict round-robin, no repeats
    regimeStartMs = millis();
    regimeDurationMs = random(2000, 5000);         // vary how long it holds each time

    const char* names[] = {"OPEN_HAND", "CLOSED_FIST", "SHAKING", "IDLE"};
    Serial.print("Switching dummy-data regime to: ");
    Serial.println(names[currentRegime]);
  }
}

void publishTelemetry() {
  // No real sensors attached - simulate a "held gesture" that persists for a
  // few seconds at a time (rather than independent per-sample noise), so
  // window-averaged values actually swing between the inference thresholds
  // instead of always washing out to the same midpoint value.
  maybeSwitchRegime();

  int flexLow, flexHigh;
  int gyroRange;

  switch (currentRegime) {
    case REGIME_OPEN_HAND:
      flexLow = 350; flexHigh = 450; gyroRange = 15;
      break;
    case REGIME_CLOSED_FIST:
      flexLow = 620; flexHigh = 750; gyroRange = 15;
      break;
    case REGIME_SHAKING:
      flexLow = 480; flexHigh = 580; gyroRange = 60;
      break;
    case REGIME_IDLE:
    default:
      flexLow = 500; flexHigh = 580; gyroRange = 10;
      break;
  }

  StaticJsonDocument<320> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  JsonObject imu = payload.createNestedObject("imu");
  imu["ax"] = random(-50, 50) / 100.0;
  imu["ay"] = random(-50, 50) / 100.0;
  imu["az"] = 9.81 + random(-30, 30) / 100.0;
  imu["gx"] = random(-gyroRange, gyroRange) / 100.0;
  imu["gy"] = random(-gyroRange, gyroRange) / 100.0;
  imu["gz"] = random(-gyroRange, gyroRange) / 100.0;

  JsonArray flex = payload.createNestedArray("flex");
  for (int i = 0; i < 5; i++) {
    flex.add(random(flexLow, flexHigh));
  }

  notifyJson(doc);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));   // vary the dummy-data sequence between boots

  BLEDevice::init(DEVICE_ID);
  BLEDevice::setMTU(247);   // headroom for our ~250-320 byte JSON payloads

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  telemetryChar = service->createCharacteristic(
    TELEMETRY_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  telemetryChar->addDescriptor(new BLE2902());

  commandChar = service->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  commandChar->setCallbacks(new CommandCallbacks());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started, waiting for laptop bridge to connect...");
}

void loop() {
  static bool lastConnected = false;
  static unsigned long lastHelloMs = 0;

  if (deviceConnected && !lastConnected) {
    delay(500);   // brief settle time right after connect
    lastHelloMs = 0;   // force an immediate HELLO on this fresh connection
  }
  lastConnected = deviceConnected;

  // Keep resending HELLO every second until we're RUNNING - this covers the
  // BLE race where the central hasn't finished enabling notifications (CCCD)
  // yet when the very first HELLO goes out, which would otherwise be silently
  // dropped with no way for us to know.
  if (deviceConnected && !isRunning) {
    if (millis() - lastHelloMs > 1000) {
      lastHelloMs = millis();
      publishHello();
    }
  }

  if (deviceConnected && isRunning) {
    unsigned long intervalMs = 1000 / sampleRateHz;
    if (millis() - lastSampleMs >= intervalMs) {
      lastSampleMs = millis();
      publishTelemetry();
    }
  }
}
