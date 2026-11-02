#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <MPU6050.h>
#include "esp_eap_client.h"
#include "esp_wpa2.h"
#include "ssh_client.h"

MPU6050 mpu;

// ==================================================
// MQTT SETTINGS
// ==================================================

const char* DEVICE_ID = "glove_01A";
const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;
unsigned long sampleRateHz = 100;
unsigned long lastSampleMs = 0;

// Running totals, shown in the serial status line so you can see the real send rate
unsigned long publishOk = 0;
unsigned long publishFail = 0;

String topicStatus() { return String("sys/") + DEVICE_ID + "/status"; }
String topicRaw()    { return String("tele/") + DEVICE_ID + "/raw"; }
String topicCmdSet() { return String("cmd/") + DEVICE_ID + "/set"; }
String topicCmdAck() { return String("cmd/") + DEVICE_ID + "/ack"; }

// ==================================================
// MPU6050 PINS
// ==================================================

#define MPU_SDA 21
#define MPU_SCL 22

// ==================================================
// FLEX SENSOR PINS (all ADC1, so they keep working while WiFi is on)
// ==================================================

#define FLEX1_PIN 39   // Thumb
#define FLEX2_PIN 34   // Index
#define FLEX3_PIN 35   // Middle
#define FLEX4_PIN 32   // Ring
#define FLEX5_PIN 33   // Pinky

// ==================================================
// VIBRATION MOTOR (unused - pin just held LOW so the motor stays off)
// ==================================================

#define VIBRATION_PIN 26

// ==================================================
// MPU6050 SCALE FACTORS
// ==================================================

// Accelerometer range: +/- 2g
#define ACCEL_SENSITIVITY 16384.0

// Gyroscope range: +/- 250 degrees/second
#define GYRO_SENSITIVITY 131.0

#define GRAVITY 9.80665

// ==================================================
// WIFI (eduroam, WPA2-Enterprise)
// ==================================================

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


// ==================================================
// MQTT MESSAGES
// ==================================================

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
  Serial.println("Published CONFIG_ACK");
}

// Round to 2 decimals so the JSON stays short - every byte has to go through
// two SSH hops on the ESP32, and 2 decimals is well within the sensor's noise.
static inline float r2(float v) {
  return roundf(v * 100.0f) / 100.0f;
}

void publishTelemetry() {

  unsigned long t = millis();   // timestamp at read time, not publish time

  // ---- Read MPU6050 ----
  int16_t axRaw, ayRaw, azRaw;
  int16_t gxRaw, gyRaw, gzRaw;
  mpu.getMotion6(&axRaw, &ayRaw, &azRaw, &gxRaw, &gyRaw, &gzRaw);

  // Accelerometer -> m/s^2, gyroscope -> degrees/second
  float ax = (axRaw / ACCEL_SENSITIVITY) * GRAVITY;
  float ay = (ayRaw / ACCEL_SENSITIVITY) * GRAVITY;
  float az = (azRaw / ACCEL_SENSITIVITY) * GRAVITY;

  float gx = gxRaw / GYRO_SENSITIVITY;
  float gy = gyRaw / GYRO_SENSITIVITY;
  float gz = gzRaw / GYRO_SENSITIVITY;

  // ---- Read flex sensors (raw 12-bit ADC, 0-4095) ----
  int flexValue[5];
  flexValue[0] = analogRead(FLEX1_PIN);
  flexValue[1] = analogRead(FLEX2_PIN);
  flexValue[2] = analogRead(FLEX3_PIN);
  flexValue[3] = analogRead(FLEX4_PIN);
  flexValue[4] = analogRead(FLEX5_PIN);

  // ---- Build and publish the message ----
  StaticJsonDocument<320> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = t;

  JsonObject payload = doc.createNestedObject("payload");
  JsonObject imu = payload.createNestedObject("imu");
  imu["ax"] = r2(ax);
  imu["ay"] = r2(ay);
  imu["az"] = r2(az);
  imu["gx"] = r2(gx);
  imu["gy"] = r2(gy);
  imu["gz"] = r2(gz);

  // Raw flex readings, 0-4095 (thumb .. pinky)
  JsonArray flex = payload.createNestedArray("flex");
  for (int i = 0; i < 5; i++) {
    flex.add(flexValue[i]);
  }

  char buf[384];
  serializeJson(doc, buf);
  if (mqtt.publish(topicRaw().c_str(), buf)) {
    publishOk++;
  } else {
    publishFail++;
    static unsigned long lastWarnMs = 0;
    if (millis() - lastWarnMs > 1000) {
      lastWarnMs = millis();
      // These numbers say why it failed:
      //   len + ~24 > bufSize        -> message too big for the MQTT buffer
      //   connected=0                -> tunnel/MQTT session dropped
      //   heap/maxBlock very small   -> ESP32 out of memory (libssh can't allocate the packet)
      Serial.printf("Telemetry publish failed: len=%u bufSize=%u connected=%d state=%d freeHeap=%u maxBlock=%u\n",
                    (unsigned)strlen(buf), (unsigned)mqtt.getBufferSize(),
                    (int)mqtt.connected(), mqtt.state(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }
  }

  // ---- Serial status (twice a second, not every sample) ----
  static unsigned long lastPrintMs = 0;
  if (millis() - lastPrintMs >= 500) {
    lastPrintMs = millis();
    Serial.printf("Accel m/s^2: %.3f %.3f %.3f | Gyro deg/s: %.3f %.3f %.3f | Flex: %d %d %d %d %d | pub ok=%lu fail=%lu heap=%u\n",
                  ax, ay, az, gx, gy, gz,
                  flexValue[0], flexValue[1], flexValue[2], flexValue[3], flexValue[4],
                  publishOk, publishFail, (unsigned)ESP.getFreeHeap());
  }
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
    long rate = doc["payload"]["sample_rate_hz"] | 50;
    sampleRateHz = (unsigned long)constrain(rate, 1L, 100L);   // never 0 (loop divides by it)
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
      publishHello();
      // Stream straight away at the compiled-in sampleRateHz - a CONFIG on
      // cmd/<id>/set is now an optional override, not a gate.
      isRunning = true;
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 3s");
      delay(3000);
    }
  }
}

// Runs the SSH tunnel + MQTT on its own task with a big stack (the SSH
// handshake overflows the default Arduino task's stack).
void mqttTask(void* param) {

  // PubSubClient's default packet buffer is 256 bytes, which is too small for
  // a telemetry message (~250 bytes of JSON + topic + header) - publish()
  // would silently return false. 512 leaves comfortable headroom.
  bool bufferOk = mqtt.setBufferSize(512);
  Serial.printf("MQTT buffer resize to 512: %s\n", bufferOk ? "ok" : "FAILED");

  connectMQTT();
  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }
    mqtt.loop();

    if (isRunning) {
      unsigned long intervalMs = 1000UL / sampleRateHz;
      unsigned long now = millis();
      if (now - lastSampleMs >= intervalMs) {
        // Step by the interval (not to "now") so loop/publish jitter doesn't
        // stretch every period; resync only if we've fallen a whole period behind.
        lastSampleMs += intervalMs;
        if (now - lastSampleMs >= intervalMs) lastSampleMs = now;
        publishTelemetry();
      }
    }
    // 1 ms tick: at 100 Hz the old 5 ms sleep alone ate half of each 10 ms period.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}


// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  // ---- Initialise MPU6050 ----
  Wire.begin(MPU_SDA, MPU_SCL);

  Serial.println("Initialising MPU6050 ...");
  mpu.initialize();

  if (mpu.testConnection()) {
    Serial.println("MPU6050 connected successfully.");
  } else {
    Serial.println("MPU6050 connection failed!");
    while (1);
  }

  // Accelerometer: +/- 2g, Gyroscope: +/- 250 degrees/second
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);

  // ---- Initialise flex sensor pins ----
  pinMode(FLEX1_PIN, INPUT);
  pinMode(FLEX2_PIN, INPUT);
  pinMode(FLEX3_PIN, INPUT);
  pinMode(FLEX4_PIN, INPUT);
  pinMode(FLEX5_PIN, INPUT);

  // 12-bit ADC, range 0 - 4095
  analogReadResolution(12);

  analogSetPinAttenuation(FLEX1_PIN, ADC_11db);
  analogSetPinAttenuation(FLEX2_PIN, ADC_11db);
  analogSetPinAttenuation(FLEX3_PIN, ADC_11db);
  analogSetPinAttenuation(FLEX4_PIN, ADC_11db);
  analogSetPinAttenuation(FLEX5_PIN, ADC_11db);

  Serial.println("Five flex sensors initialised.");

  // ---- Keep the vibration motor off ----
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);

  // ---- Network ----
  connectWiFi();

  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);
}

void loop() {
  // All the work happens in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
