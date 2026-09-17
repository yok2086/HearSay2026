#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Local network settings
// -----------------------------------------------------------------------------
const char* WIFI_SSID     = "N8";
const char* WIFI_PASSWORD = "Password";

// This is the Mac's address on the SAME Wi-Fi/hotspot as the FireBeetle.
// On the Mac, find it with: ipconfig getifaddr en0
// Do not put the Ultra96 address here: the Mac is the SSH relay endpoint.
const char* MQTT_BROKER = "172.20.10.2";  // CHANGE to the Mac's actual IP
const uint16_t MQTT_PORT = 8883;

const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";
const char* DEVICE_ID = "glove_01";

// Use true only to prove that a failure is caused by certificate validation.
// Never leave this enabled in the finished system.
const bool INSECURE_TLS_DIAGNOSTIC = true;

// CA that signed the Mosquitto server certificate.
const char* CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
MIICpDCCAYwCCQDqnAnRl99g1zANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAlI
ZWFyU2F5Q0EwHhcNMjYwOTAyMTM0MTQ2WhcNMjcwOTAyMTM0MTQ2WjAUMRIwEAYD
VQQDDAlIZWFyU2F5Q0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDN
PYZs1jdV/AJiDJcWrwj+hvlZufbb0vpVNwU/F7nbMYHFOBJe3EfTvtvHXtA6qord
yTs1agZsIIqUk5HaLdLqrmLbMsxbVU0Oushb3hLqDhvI8IrZc4o0oP4MuwqRlCG5
rxfojsIjMcKPsFWzuNw4XFXycwkD7XFbn0GxRQAKCJbjki8irScBxk8kUHu4f+in
rg/7L46ngd9Dfvd0aQik2mKYQO002j3AvBrSELysKK9CCHGZRhw3I9u6Exj0l89G
1yXJC03a1V0M2TgufQIqN2ixJq5HVboLwvTYDI5+a0pXTFVo/L0HjyuckI1DQAea
6/tJmUI5qSPDNqd+j0YVAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAMUv4AxVoJ/0
wKPob2N5AHOlBdQ58pM7dSQvg1M4t7FevQpIwS4y2Dd+fnnp7AK2fp3tMriLdEOj
x6n8oiwskYfFtAPB4RFAvTl3GQwAioXsfuE5TgtdGGafN602ts4PFQJE6X2bSe1g
FqU/xSDXa1eFq98HOKExHutXmcaGjIz/GlGB8RQ7HMG6F0npLi6U/h5jyx73lurF
szn19x1cbnut9TODN5YgXHA5NzmeNunuAoOtpKLpRykA0WrlgRb7mdUi6AtTNKWF
Id93DMRS+Hs5iImIO2BM7fwHP/IHaIY7ETyPoe/plLJU10lgf8vC0aVsIEi7vtfw
PtpMdr4ycHY=
-----END CERTIFICATE-----
)EOF";

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;
unsigned long sampleRateHz = 50;
unsigned long lastSampleMs = 0;
unsigned long lastHelloMs = 0;
unsigned long nextMqttAttemptMs = 0;
unsigned long mqttRetryMs = 2000;

String topicStatus() { return String("sys/") + DEVICE_ID + "/status"; }
String topicRaw()    { return String("tele/") + DEVICE_ID + "/raw"; }
String topicCmdSet() { return String("cmd/") + DEVICE_ID + "/set"; }
String topicCmdAck() { return String("cmd/") + DEVICE_ID + "/ack"; }

bool publishJson(const String& topic, JsonDocument& doc, bool retained = false) {
  char payload[384];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("JSON payload buffer is too small");
    return false;
  }

  bool ok = mqtt.publish(
    topic.c_str(),
    reinterpret_cast<const uint8_t*>(payload),
    static_cast<unsigned int>(length),
    retained
  );

  if (!ok) {
    Serial.print("MQTT publish failed for topic: ");
    Serial.println(topic);
  }
  return ok;
}

void publishHello() {
  StaticJsonDocument<256> doc;
  doc["type"] = "HELLO";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  JsonArray capabilities = doc.createNestedObject("payload").createNestedArray("capabilities");
  capabilities.add("IMU");
  capabilities.add("FLEX");

  if (publishJson(topicStatus(), doc, true)) {
    Serial.println("Published HELLO");
  }
}

void publishConfigAck() {
  StaticJsonDocument<192> doc;
  doc["type"] = "CONFIG_ACK";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  doc["payload"]["status"] = "OK";

  if (publishJson(topicCmdAck(), doc)) {
    Serial.println("Published CONFIG_ACK -> now RUNNING");
    isRunning = true;
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("Invalid MQTT JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String type = doc["type"] | "";
  if (type == "CONFIG") {
    unsigned long requestedRate = doc["payload"]["sample_rate_hz"] | 50UL;
    sampleRateHz = constrain(requestedRate, 1UL, 100UL);
    Serial.print("Received CONFIG, sample_rate_hz=");
    Serial.println(sampleRateHz);
    publishConfigAck();
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to hotspot \"%s\"", WIFI_SSID);
  unsigned long startedAt = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - startedAt >= 30000) {
      Serial.printf("\nWi-Fi timed out (status=%d); restarting.\n", WiFi.status());
      delay(1000);
      ESP.restart();
    }
  }

  Serial.println(" connected");
  Serial.print("FireBeetle IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("Mac SSH relay: ");
  Serial.print(MQTT_BROKER);
  Serial.print(":");
  Serial.println(MQTT_PORT);
}

void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Synchronising clock");

  unsigned long startedAt = millis();
  time_t now = time(nullptr);
  while (now < 1700000000 && millis() - startedAt < 15000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(now >= 1700000000 ? " ready" : " unavailable");
}

void configureMqtt() {
  if (INSECURE_TLS_DIAGNOSTIC) {
    Serial.println("WARNING: MQTT TLS certificate verification is disabled");
    tlsClient.setInsecure();
  } else {
    tlsClient.setCACert(CA_CERT);
  }

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);
}

void tryConnectMqtt() {
  if (mqtt.connected() || millis() < nextMqttAttemptMs) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.print("Connecting to MQTT broker...");
  String willTopic = topicStatus();
  String willPayload =
    "{\"type\":\"STATUS\",\"device_id\":\"" + String(DEVICE_ID) +
    "\",\"payload\":{\"state\":\"offline\"}}";

  bool connected = mqtt.connect(
    DEVICE_ID,
    MQTT_USER,
    MQTT_PASS,
    willTopic.c_str(),
    1,
    true,
    willPayload.c_str()
  );

  if (connected) {
    Serial.println(" connected");
    mqttRetryMs = 2000;
    isRunning = true;
    Serial.println("MQTT connected -> starting telemetry");
    lastHelloMs = 0;

    if (!mqtt.subscribe(topicCmdSet().c_str(), 1)) {
      Serial.println("Failed to subscribe to CONFIG topic");
    }
    publishHello();
  } else {
    Serial.print(" failed, rc=");
    Serial.print(mqtt.state());
    Serial.print("; retrying in ");
    Serial.print(mqttRetryMs);
    Serial.println(" ms");
    nextMqttAttemptMs = millis() + mqttRetryMs;
    mqttRetryMs = min(mqttRetryMs * 2UL, 30000UL);
  }
}

enum GestureRegime {
  REGIME_OPEN_HAND,
  REGIME_CLOSED_FIST,
  REGIME_SHAKING,
  REGIME_IDLE
};

GestureRegime currentRegime = REGIME_IDLE;
unsigned long regimeStartMs = 0;
unsigned long regimeDurationMs = 3000;

void maybeSwitchRegime() {
  if (millis() - regimeStartMs > regimeDurationMs) {
    currentRegime = static_cast<GestureRegime>((currentRegime + 1) % 4);
    regimeStartMs = millis();
    regimeDurationMs = random(2000, 5000);

    const char* names[] = {"OPEN_HAND", "CLOSED_FIST", "SHAKING", "IDLE"};
    Serial.print("Switching dummy-data regime to: ");
    Serial.println(names[currentRegime]);
  }
}

void publishTelemetry() {
  maybeSwitchRegime();

  int flexLow;
  int flexHigh;
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

  JsonObject data = doc.createNestedObject("payload");
  JsonObject imu = data.createNestedObject("imu");
  imu["ax"] = random(-50, 50) / 100.0;
  imu["ay"] = random(-50, 50) / 100.0;
  imu["az"] = 9.81 + random(-30, 30) / 100.0;
  imu["gx"] = random(-gyroRange, gyroRange) / 100.0;
  imu["gy"] = random(-gyroRange, gyroRange) / 100.0;
  imu["gz"] = random(-gyroRange, gyroRange) / 100.0;

  JsonArray flex = data.createNestedArray("flex");
  for (int i = 0; i < 5; i++) {
    flex.add(random(flexLow, flexHigh));
  }

  publishJson(topicRaw(), doc);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  connectWiFi();
  syncClock();
  configureMqtt();
  tryConnectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    isRunning = false;
    mqtt.disconnect();
    WiFi.reconnect();
    delay(250);
    return;
  }

  if (!mqtt.connected()) {
    isRunning = false;
    tryConnectMqtt();
    delay(10);
    return;
  }

  mqtt.loop();

  // Repeat HELLO until Node-RED/coordinator sends CONFIG.
  if (!isRunning && millis() - lastHelloMs >= 1000) {
    lastHelloMs = millis();
    publishHello();
  }

  if (isRunning) {
    unsigned long intervalMs = 1000UL / sampleRateHz;
    if (millis() - lastSampleMs >= intervalMs) {
      lastSampleMs = millis();
      publishTelemetry();
    }
  }
}
