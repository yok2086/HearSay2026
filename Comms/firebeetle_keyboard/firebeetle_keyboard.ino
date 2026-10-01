#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_eap_client.h"
#include "esp_wpa2.h"
#include "ssh_client.h"

// ---- MQTT-level settings ----
const char* DEVICE_ID  = "glove_01";
const char* MQTT_USER  = "nathan";
const char* MQTT_PASS  = "mqtt";
// --------------------------------------------------------------------------

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "keyboard_test";
uint32_t seq = 0;
uint32_t dummyCount = 0;

String topicStatus()     { return String("sys/") + DEVICE_ID + "/status"; }
String topicCmdSet()     { return String("cmd/") + DEVICE_ID + "/set"; }
String topicCmdAck()     { return String("cmd/") + DEVICE_ID + "/ack"; }
String topicPing()       { return String("cmd/") + DEVICE_ID + "/ping"; }
String topicPong()       { return String("tele/") + DEVICE_ID + "/pong"; }
String topicDummy()      { return String("cmd/") + DEVICE_ID + "/dummy"; }
String topicDummyReply() { return String("tele/") + DEVICE_ID + "/dummy"; }

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

void replyToDummy(JsonDocument& in) {
  StaticJsonDocument<320> out;
  out["type"] = "DUMMY_REPLY";
  out["device_id"] = DEVICE_ID;
  out["session_id"] = sessionId;
  out["seq"] = seq++;
  out["timestamp_ms"] = millis();

  JsonObject p = out.createNestedObject("payload");
  long sourceSeq = in["seq"] | -1L;
  p["source_seq"] = sourceSeq;
  p["key"] = String(in["payload"]["key"] | "");
  JsonArray values = p.createNestedArray("values");
  long sum = 0;
  for (JsonVariant v : in["payload"]["values"].as<JsonArray>()) {
    long x = v.as<long>();
    values.add(x * 2 + 1);
    sum += x;
  }
  p["sum"] = sum;

  char buf[320];
  size_t len = serializeJson(out, buf);
  Serial.printf("DUMMY #%ld key '%s' -> modified, sum %ld, sending back (%u handled)\n",
                sourceSeq, p["key"].as<const char*>(), sum, (unsigned)++dummyCount);
  if (!mqtt.publish(topicDummyReply().c_str(), (const uint8_t*)buf, len, false)) {
    Serial.println("DUMMY reply publish FAILED");
  }
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
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("Failed to parse incoming message");
    return;
  }

  String type = doc["type"].as<String>();
  if (type == "DUMMY") {
    replyToDummy(doc);
  } else if (type == "CONFIG") {
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
      mqtt.subscribe(topicDummy().c_str());
      mqtt.subscribe(topicCmdSet().c_str());
      mqtt.subscribe(topicPing().c_str());
      publishHello();
      Serial.println("Ready - press keys in keyboard_relay.py on the laptop.");
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
      connectMQTT();
    }
    mqtt.loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  connectWiFi();

  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
