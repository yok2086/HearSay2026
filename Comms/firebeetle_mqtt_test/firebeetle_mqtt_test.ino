#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_eap_client.h"
#include "esp_wpa2.h"  
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ---- Fill these in ----
const char* WIFI_SSID     = "NUS_STU";
const char* EAP_IDENTITY  = "NUSSTU\\e1121325";
const char* EAP_USERNAME  = "NUSSTU\\e1121325";
const char* EAP_PASSWORD  = "NusnetSCrnjzjb123!";
const char* MQTT_BROKER   = "172.25.102.116";   // your Mac's current LAN IP - re-check with ipconfig getifaddr en0
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "nathan";
const char* MQTT_PASS     = "mqtt";
const char* DEVICE_ID     = "glove_01";

// Paste the FULL contents of ~/mqtt/mosquitto_certs/ca.crt here, including the
// -----BEGIN CERTIFICATE----- / -----END CERTIFICATE----- lines.
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
// ------------------------

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;
unsigned long sampleRateHz = 50;
unsigned long lastSampleMs = 0;

String topicStatus() { return String("sys/") + DEVICE_ID + "/status"; }
String topicRaw()    { return String("tele/") + DEVICE_ID + "/raw"; }
String topicCmdSet() { return String("cmd/") + DEVICE_ID + "/set"; }
String topicCmdAck() { return String("cmd/") + DEVICE_ID + "/ack"; }

void connectWiFi() {
  WiFi.disconnect(true);   // clear any previous WPA2-Personal config first
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
      Serial.println("Status meanings: 0=IDLE 1=NO_SSID_AVAIL 4=CONNECT_FAILED 6=DISCONNECTED");
      startAttempt = millis();   // reset timer, keep retrying, but now with periodic status reports
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
}

void publishTelemetry() {
  // Replace these with real MPU6050 / flex sensor readings
  StaticJsonDocument<320> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  JsonObject imu = payload.createNestedObject("imu");
  imu["ax"] = 0.01; imu["ay"] = 0.02; imu["az"] = 9.81;
  imu["gx"] = 0.00; imu["gy"] = 0.00; imu["gz"] = 0.00;

  JsonArray flex = payload.createNestedArray("flex");
  flex.add(512); flex.add(490); flex.add(601); flex.add(588); flex.add(470);

  char buf[320];
  serializeJson(doc, buf);
  mqtt.publish(topicRaw().c_str(), buf);  // QoS 0, not retained
}

void onMessage(char* topic, byte* payload, unsigned int length) {
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
  espClient.setCACert(CA_CERT);
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);

  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker...");
    String willTopic = topicStatus();
    String willMsg = "{\"type\":\"STATUS\",\"device_id\":\"" + String(DEVICE_ID) + "\",\"payload\":{\"state\":\"offline\"}}";

    // Last Will and Testament: broker publishes this automatically on unexpected disconnect
    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                            willTopic.c_str(), 1, true, willMsg.c_str());
    if (ok) {
      Serial.println(" connected");
      mqtt.subscribe(topicCmdSet().c_str());
      publishHello();
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 2s");
      delay(2000);
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // diagnostic only - disables brownout detector
  Serial.begin(115200);
  connectWiFi();
  connectMQTT();
}

void loop() {
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
  }
}
