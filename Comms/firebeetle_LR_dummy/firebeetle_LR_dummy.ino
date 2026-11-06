#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_eap_client.h"
#include "esp_wpa2.h"
#include "ssh_client.h"

// Dummy stand-in for firebeetle_LR: no microphones, just publishes LEFT/RIGHT
// direction TELEMETRY in the same format so the phone's deaf-user cue can be
// tested. Uses the same device ID as the real board, so don't run both at once
// (the broker would keep kicking one off) - or change DEVICE_ID here and
// soundDeviceId in HearSaySshMqttManager to match.
//
// Serial commands (115200 baud):
//   l = send LEFT now    r = send RIGHT now    a = toggle automatic sending

// ==================================================
// MQTT / DEVICE SETTINGS
// ==================================================

const char* DEVICE_ID = "mic_lr_01";
const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;

// Automatic mode: one random LEFT/RIGHT event every MIN..MAX ms
bool autoSend = true;
const unsigned long AUTO_MIN_INTERVAL_MS = 2000;
const unsigned long AUTO_MAX_INTERVAL_MS = 4000;
unsigned long nextAutoSendMs = 0;

void mqttTask(void* param);

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

  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_enterprise_enable();

  WiFi.begin(WIFI_SSID);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
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
  caps.add("MIC");
  caps.add("TDOA_LEFT_RIGHT");
  caps.add("DUMMY");

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
  Serial.println("Published CONFIG_ACK");
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  if (topicPing() == topic) {
    uint8_t echo[32];
    unsigned int n = length < sizeof(echo) ? length : sizeof(echo);
    memcpy(echo, payload, n);
    mqtt.publish(topicPong().c_str(), echo, n, false);
    return;
  }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("Failed to parse incoming MQTT message");
    return;
  }

  if (doc["type"] == "CONFIG") {
    publishConfigAck();
  }
}

void connectMQTT() {
  mqtt.setServer("ultra96", 1883);
  mqtt.setCallback(onMessage);

  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker via SSH tunnel...");
    String willMsg = "{\"type\":\"STATUS\",\"device_id\":\"" +
                     String(DEVICE_ID) +
                     "\",\"payload\":{\"state\":\"offline\"}}";

    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                           topicStatus().c_str(), 1, true, willMsg.c_str());
    if (ok) {
      Serial.println(" connected");
      mqtt.subscribe(topicCmdSet().c_str());
      mqtt.subscribe(topicPing().c_str());
      publishHello();
      isRunning = true;
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 3s");
      delay(3000);
    }
  }
}

// ==================================================
// DUMMY DIRECTION TELEMETRY
// ==================================================

// Same payload as firebeetle_LR's publishDirectionTelemetry(), with plausible
// made-up vote counts so anything reading the extra fields still works.
void publishDirectionTelemetry(bool right) {
  if (!isRunning || !mqtt.connected()) {
    return;
  }

  int winningVotes = random(6, 15);
  int losingVotes = random(0, 3);
  int frontVotes = random(0, 3);
  int speechFrames = winningVotes + losingVotes + frontVotes + random(0, 5);

  StaticJsonDocument<384> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  payload["event"] = "SPEECH";
  payload["direction"] = right ? "RIGHT" : "LEFT";
  payload["speech_frames"] = speechFrames;
  payload["event_frames"] = speechFrames + random(4, 10);
  payload["valid_tdoa_frames"] = winningVotes + losingVotes + frontVotes;
  payload["left_votes"] = right ? losingVotes : winningVotes;
  payload["front_votes"] = frontVotes;
  payload["right_votes"] = right ? winningVotes : losingVotes;

  char buf[384];
  size_t len = serializeJson(doc, buf);
  if (mqtt.publish(topicRaw().c_str(), (const uint8_t*)buf, len, false)) {
    Serial.println(right ? "Published RIGHT" : "Published LEFT");
  } else {
    Serial.println("Failed to publish direction telemetry");
  }
}

void scheduleNextAutoSend() {
  nextAutoSendMs = millis() + random(AUTO_MIN_INTERVAL_MS, AUTO_MAX_INTERVAL_MS + 1);
}

void handleSerialCommand() {
  while (Serial.available()) {
    char c = tolower(Serial.read());
    if (c == 'l') {
      publishDirectionTelemetry(false);
    } else if (c == 'r') {
      publishDirectionTelemetry(true);
    } else if (c == 'a') {
      autoSend = !autoSend;
      Serial.println(autoSend ? "Auto send ON" : "Auto send OFF");
      scheduleNextAutoSend();
    }
  }
}

void mqttTask(void* param) {
  mqtt.setBufferSize(512);
  connectMQTT();
  scheduleNextAutoSend();

  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }
    mqtt.loop();
    handleSerialCommand();

    if (autoSend && (long)(millis() - nextAutoSendMs) >= 0) {
      publishDirectionTelemetry(random(2) == 1);
      scheduleNextAutoSend();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==================================================
// SETUP / LOOP
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed(esp_random());

  connectWiFi();
  // The SSH handshake overflows the default Arduino task's stack.
  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);

  Serial.println("Dummy LR mic: l = LEFT, r = RIGHT, a = toggle auto");
}

void loop() {
  // Everything runs in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
