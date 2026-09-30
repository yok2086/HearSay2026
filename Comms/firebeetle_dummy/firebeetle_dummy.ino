#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_eap_client.h"  
#include "esp_wpa2.h"   
#include "ssh_client.h"      

// ---- MQTT-level settings ----
const char* DEVICE_ID  = "glove_02";
const char* MQTT_USER  = "nathan";
const char* MQTT_PASS  = "mqtt";
// --------------------------------------------------------------------------

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;
unsigned long sampleRateHz = 50;
unsigned long lastSampleMs = 0;

// ---- Transmission statistics (telemetry JSON payload bytes, excluding MQTT/SSH overhead) ----
const unsigned long STATS_INTERVAL_MS = 5000;
uint32_t statsBytes = 0, statsMsgs = 0, statsFailed = 0;
uint32_t totalBytes = 0, totalMsgs = 0;
unsigned long statsStartMs = 0, runningSinceMs = 0;

// ---- Dummy-data regime cycling (no real sensors attached yet) ----
enum GestureRegime { REGIME_OPEN_HAND, REGIME_CLOSED_FIST, REGIME_SHAKING, REGIME_IDLE };
GestureRegime currentRegime = REGIME_IDLE;
unsigned long regimeStartMs = 0;
unsigned long regimeDurationMs = 3000;

String topicStatus() { return String("sys/") + DEVICE_ID + "/status"; }
String topicRaw()    { return String("tele/") + DEVICE_ID + "/raw"; }
String topicCmdSet() { return String("cmd/") + DEVICE_ID + "/set"; }
String topicCmdAck() { return String("cmd/") + DEVICE_ID + "/ack"; }
String topicPing()   { return String("cmd/") + DEVICE_ID + "/ping"; }
String topicPong()   { return String("tele/") + DEVICE_ID + "/pong"; }

void connectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.println("Scanning for WiFi networks...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("No networks found at all - check antenna/power/location.");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.printf("  [%d] SSID: \"%s\"  RSSI: %d  Encryption: %d\n",
                    i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i));
    }
  }
  Serial.println("--- end of scan ---");

  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_enterprise_enable();

  WiFi.begin(WIFI_SSID);
  Serial.print("Connecting to WiFi");

  unsigned long startAttempt = millis();
  const unsigned long WIFI_TIMEOUT_MS = 20000;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - startAttempt > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.print("Still not connected. WiFi.status() code = ");
      Serial.println(WiFi.status());
      startAttempt = millis();
    }
  }
  Serial.println(" connected");
  Serial.println(WiFi.localIP());
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

  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish(topicStatus().c_str(), buf, true);
  Serial.println("Published HELLO");
}

void publishConfigAck() {
  StaticJsonDocument<192> doc;
  doc["type"] = "CONFIG_ACK";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  doc["payload"]["status"] = "OK";

  char buf[192];
  serializeJson(doc, buf);
  mqtt.publish(topicCmdAck().c_str(), buf);
  Serial.println("Published CONFIG_ACK -> now RUNNING");
  isRunning = true;
  statsBytes = statsMsgs = statsFailed = totalBytes = totalMsgs = 0;
  statsStartMs = runningSinceMs = millis();
}

void maybeSwitchRegime() {
  if (millis() - regimeStartMs > regimeDurationMs) {
    currentRegime = (GestureRegime)((currentRegime + 1) % 4);
    regimeStartMs = millis();
    regimeDurationMs = random(2000, 5000);

    const char* names[] = {"OPEN_HAND", "CLOSED_FIST", "SHAKING", "IDLE"};
    Serial.print("Switching dummy-data regime to: ");
    Serial.println(names[currentRegime]);
  }
}

void publishTelemetry() {
  maybeSwitchRegime();

  StaticJsonDocument<320> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  JsonObject imu = payload.createNestedObject("imu");

  int gyroRange;

  static const int REGIME_FLEX[4][5] = {
    {0, 0, 0, 0, 0},   // REGIME_OPEN_HAND   - avg 0.0 -> YES
    {1, 1, 1, 1, 1},   // REGIME_CLOSED_FIST - avg 1.0 -> HELLO
    {1, 1, 1, 0, 0},   // REGIME_SHAKING     - avg 0.6, falls through to gyro -> THANK_YOU
    {1, 0, 1, 0, 0},   // REGIME_IDLE        - avg 0.4, falls through to gyro -> PLEASE
  };

  switch (currentRegime) {
    case REGIME_OPEN_HAND:   gyroRange = 10; break;
    case REGIME_CLOSED_FIST: gyroRange = 10; break;
    case REGIME_SHAKING:     gyroRange = 100; break;
    default:                 gyroRange = 5;  break;
  }

  imu["ax"] = random(-50, 50) / 100.0;
  imu["ay"] = random(-50, 50) / 100.0;
  imu["az"] = 9.81 + random(-30, 30) / 100.0;
  imu["gx"] = random(-gyroRange, gyroRange);
  imu["gy"] = random(-gyroRange, gyroRange);
  imu["gz"] = random(-gyroRange, gyroRange);

  JsonArray flex = payload.createNestedArray("flex");
  for (int i = 0; i < 5; i++) {
    flex.add(REGIME_FLEX[currentRegime][i]);
  }

  char buf[320];
  size_t len = serializeJson(doc, buf);
  if (mqtt.publish(topicRaw().c_str(), buf)) {
    statsBytes += len;
    statsMsgs++;
  } else {
    statsFailed++;   // e.g. message larger than PubSubClient's buffer, or link down
  }
}

void maybeReportStats() {
  unsigned long now = millis();
  if (now - statsStartMs < STATS_INTERVAL_MS) return;

  float secs = (now - statsStartMs) / 1000.0;
  totalBytes += statsBytes;
  totalMsgs += statsMsgs;
  float totalSecs = (now - runningSinceMs) / 1000.0;

  Serial.printf("[stats] %s last %.1fs: %.2f kbps, %.1f msg/s, %u B, %u failed | avg %.2f kbps, %u msgs, %.1f KiB\n",
                DEVICE_ID, secs, statsBytes * 8 / 1000.0 / secs, statsMsgs / secs,
                (unsigned)statsBytes, (unsigned)statsFailed, totalBytes * 8 / 1000.0 / totalSecs,
                (unsigned)totalMsgs, totalBytes / 1024.0);

  statsBytes = statsMsgs = statsFailed = 0;
  statsStartMs = now;
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  // Latency probe from Comms/stats.py: echo straight back, before any JSON
  // parsing. Copy first - payload points into PubSubClient's buffer, which
  // publish() reuses.
  if (topicPing() == topic) {
    uint8_t echo[32];
    unsigned int n = length < sizeof(echo) ? length : sizeof(echo);
    memcpy(echo, payload, n);
    mqtt.publish(topicPong().c_str(), echo, n, false);
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("Failed to parse incoming message");
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

void connectMQTT() {
  // "ultra96"/1883 here are placeholders - SSHForwardClient ignores them and
  // always routes through the hardcoded stujump -> ultra96 -> localhost:1883
  // path from secrets.h / ssh_client.h.
  mqtt.setServer("ultra96", 1883);
  mqtt.setCallback(onMessage);

  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker via SSH tunnel...");
    String willTopic = topicStatus();
    String willMsg = "{\"type\":\"STATUS\",\"device_id\":\"" + String(DEVICE_ID) + "\",\"payload\":{\"state\":\"offline\"}}";

    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                            willTopic.c_str(), 1, true, willMsg.c_str());
    if (ok) {
      Serial.println(" connected");
      mqtt.subscribe(topicCmdSet().c_str());
      mqtt.subscribe(topicPing().c_str());
      publishHello();
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 3s");
      delay(3000);
    }
  }
}

void mqttTask(void* param) {
  connectMQTT();
  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }
    mqtt.loop();

    if (isRunning) {
      unsigned long intervalMs = 1000 / sampleRateHz;
      if (millis() - lastSampleMs >= intervalMs) {
        lastSampleMs = millis();
        publishTelemetry();
      }
      maybeReportStats();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  connectWiFi();

  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
