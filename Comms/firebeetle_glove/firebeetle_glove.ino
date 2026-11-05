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
volatile unsigned long sampleRateHz = 100;

// Running totals, shown in the serial status line so you can see the real send rate
unsigned long publishOk = 0;
unsigned long publishFail = 0;
unsigned long samplesSent = 0;
volatile unsigned long samplesDropped = 0;

// ---- Sampling and sending run separately, joined by this buffer ----
#define SAMPLE_BUFFER_SAMPLES 600     // 6 s at 100 Hz: how long a stall can last before data is lost
#define BATCH_SAMPLES 10              // samples per message: one message every 100 ms at 100 Hz
#define BATCH_MAX_WAIT_MS 200
#define TELEMETRY_BUFFER_BYTES 1100   // ~130 bytes of header + up to ~90 per sample
#define MQTT_BUFFER_BYTES 1200

struct RawSample {
  uint32_t t;
  int16_t imu[6];     // ax, ay, az, gx, gy, gz as read from the MPU6050
  uint16_t flex[5];   // thumb .. pinky
};

QueueHandle_t sampleQueue;
volatile bool samplingEnabled = false;

String topicStatus() { return String("sys/") + DEVICE_ID + "/status"; }
String topicRaw()    { return String("tele/") + DEVICE_ID + "/raw"; }
String topicCalibration() { return String("sys/") + DEVICE_ID + "/calibration"; }
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

const int flexPins[5] = {
  FLEX1_PIN,
  FLEX2_PIN,
  FLEX3_PIN,
  FLEX4_PIN,
  FLEX5_PIN
};

// ==================================================
// VIBRATION MOTOR
// ==================================================

#define VIBRATION_PIN 26

#define SHORT_VIBRATION_MS 120
#define LONG_VIBRATION_MS 700

// ==================================================
// CALIBRATION SETTINGS
// ==================================================

#define CALIBRATION_SAMPLES 100
#define CALIBRATION_SAMPLE_DELAY_MS 10

// Extra spacing so each vibration cue is easier to distinguish.
#define CALIBRATION_PREP_DELAY_MS 4000
#define BETWEEN_STAGES_DELAY_MS 2000

// Straight-hand and clenched-fist readings. Sent to the Ultra96, which uses
// them to normalise the raw flex values in the telemetry.
int flexStraight[5] = {0, 0, 0, 0, 0};
int flexBent[5] = {0, 0, 0, 0, 0};

// Gyro reading (deg/s) while the hand is held still. The MPU6050's zero-rate
// offset changes at every power-up; the Ultra96 subtracts this.
float gyroOffset[3] = {0, 0, 0};

bool calibrationComplete = false;
bool calibrationPublished = false;

// ==================================================
// MPU6050 SCALE FACTORS
// ==================================================

// Accelerometer range: +/- 2g
#define ACCEL_SENSITIVITY 16384.0

// Gyroscope range: +/- 250 degrees/second
#define GYRO_SENSITIVITY 131.0

#define GRAVITY 9.80665

// ==================================================
// VIBRATION FUNCTIONS
// ==================================================

void vibrateShort() {
  digitalWrite(VIBRATION_PIN, HIGH);
  delay(SHORT_VIBRATION_MS);
  digitalWrite(VIBRATION_PIN, LOW);
}

void vibrateLong() {
  digitalWrite(VIBRATION_PIN, HIGH);
  delay(LONG_VIBRATION_MS);
  digitalWrite(VIBRATION_PIN, LOW);
}

void vibrateTwice() {
  vibrateShort();
  delay(150);
  vibrateShort();
}

// ==================================================
// READ AND AVERAGE ALL FIVE FLEX SENSORS
// ==================================================

// Calibration takes longer than the MQTT keepalive (15 s), so keep servicing
// the connection while waiting or the broker drops the glove part way through.
void calibrationWait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    mqtt.loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void recordFlexReadings(int readings[5]) {

  long total[5] = {0, 0, 0, 0, 0};

  for (int sample = 0; sample < CALIBRATION_SAMPLES; sample++) {

    for (int sensor = 0; sensor < 5; sensor++) {
      total[sensor] += analogRead(flexPins[sensor]);
    }

    calibrationWait(CALIBRATION_SAMPLE_DELAY_MS);
  }

  for (int sensor = 0; sensor < 5; sensor++) {
    readings[sensor] = total[sensor] / CALIBRATION_SAMPLES;
  }
}

void recordGyroOffset() {

  float total[3] = {0, 0, 0};

  for (int sample = 0; sample < CALIBRATION_SAMPLES; sample++) {

    int16_t axRaw, ayRaw, azRaw;
    int16_t gxRaw, gyRaw, gzRaw;
    mpu.getMotion6(&axRaw, &ayRaw, &azRaw, &gxRaw, &gyRaw, &gzRaw);

    total[0] += gxRaw / GYRO_SENSITIVITY;
    total[1] += gyRaw / GYRO_SENSITIVITY;
    total[2] += gzRaw / GYRO_SENSITIVITY;

    calibrationWait(CALIBRATION_SAMPLE_DELAY_MS);
  }

  for (int axis = 0; axis < 3; axis++) {
    gyroOffset[axis] = total[axis] / CALIBRATION_SAMPLES;
  }

  Serial.printf("Gyro offset deg/s: %.2f %.2f %.2f\n", gyroOffset[0], gyroOffset[1], gyroOffset[2]);
}

// ==================================================
// PRINT CALIBRATION READINGS
// ==================================================

void printReadings(const char *label, const int readings[5]) {

  Serial.print(label);
  Serial.print(": ");

  for (int i = 0; i < 5; i++) {
    Serial.print(readings[i]);

    if (i < 4) {
      Serial.print(" ");
    }
  }

  Serial.println();
}

// ==================================================
// CALIBRATION PROCESS
// ==================================================

// Runs once the glove is connected to the broker. No telemetry is sent until
// it succeeds.
void calibrateFlexSensors() {

  calibrationComplete = false;

  // Repeat the entire sequence until valid calibration
  // readings are obtained.

  while (!calibrationComplete) {

    // ---- Start calibration ----
    Serial.println();
    Serial.println("======================================");
    Serial.println("       FLEX SENSOR CALIBRATION");
    Serial.println("======================================");
    Serial.println("Calibration starting.");
    Serial.println("Vibration: one short pulse.");

    vibrateShort();

    // ---- Straighten fingers ----
    Serial.println();
    Serial.println("STEP 1: STRAIGHTEN ALL FINGERS");
    Serial.println("Keep all fingers straight and relaxed.");
    Serial.println("Recording straight-hand readings in 4 seconds...");

    calibrationWait(CALIBRATION_PREP_DELAY_MS);

    Serial.println("Recording straight-hand readings...");
    recordFlexReadings(flexStraight);
    printReadings("Straight readings", flexStraight);

    Serial.println("Hold still, measuring gyro offset...");
    recordGyroOffset();

    Serial.println("Vibration: one long pulse.");
    vibrateLong();
    calibrationWait(BETWEEN_STAGES_DELAY_MS);

    // ---- Clench fingers ----
    Serial.println();
    Serial.println("STEP 2: CLENCH ALL FINGERS");
    Serial.println("Make a comfortable fist with all fingers.");
    Serial.println("Vibration: two short pulses.");

    vibrateTwice();

    Serial.println("Hold the fist steady.");
    Serial.println("Recording fist readings in 4 seconds...");

    calibrationWait(CALIBRATION_PREP_DELAY_MS);

    Serial.println("Recording clenched-fist readings...");
    recordFlexReadings(flexBent);
    printReadings("Clenched readings", flexBent);

    Serial.println("Vibration: one long pulse.");
    vibrateLong();
    calibrationWait(BETWEEN_STAGES_DELAY_MS);

    // ---- Validate ----
    Serial.println();
    Serial.println("Checking calibration readings...");

    bool validCalibration = true;

    for (int i = 0; i < 5; i++) {
      Serial.printf("Sensor %d | Straight: %d | Clenched: %d | Difference: %d\n",
                    i + 1, flexStraight[i], flexBent[i], flexBent[i] - flexStraight[i]);

      // Assumption: bending increases the ADC reading.
      // If even one clench reading is lower than its
      // straight reading, restart the entire calibration.
      if (flexBent[i] < flexStraight[i]) {
        validCalibration = false;
      }
    }

    if (!validCalibration) {
      Serial.println();
      Serial.println("CALIBRATION FAILED!");
      Serial.println("At least one clench reading is lower");
      Serial.println("than its straight reading.");
      Serial.println("Check sensor wiring and finger movement.");
      Serial.println("Restarting calibration...");
      Serial.println();

      calibrationWait(1000);
      continue;
    }

    calibrationComplete = true;

    Serial.println();
    Serial.println("======================================");
    Serial.println("     CALIBRATION SUCCESSFUL!");
    Serial.println("======================================");
    printReadings("Straight readings", flexStraight);
    printReadings("Clenched readings", flexBent);
    Serial.println();
  }
}


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

// Round to 2 decimals so the JSON stays short - every byte has to go through
// two SSH hops on the ESP32, and 2 decimals is well within the sensor's noise.
static inline float r2(float v) {
  return roundf(v * 100.0f) / 100.0f;
}

// Retained, so the Ultra96 handler still gets it if it starts after the glove.
bool publishCalibration() {
  StaticJsonDocument<512> doc;
  doc["type"] = "CALIBRATION";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  JsonObject payload = doc.createNestedObject("payload");
  JsonArray straight = payload.createNestedArray("straight");
  JsonArray bent = payload.createNestedArray("bent");
  for (int i = 0; i < 5; i++) {
    straight.add(flexStraight[i]);
    bent.add(flexBent[i]);
  }
  JsonArray gyro = payload.createNestedArray("gyro_offset");
  for (int i = 0; i < 3; i++) {
    gyro.add(r2(gyroOffset[i]));
  }

  char buf[320];
  serializeJson(doc, buf);
  bool ok = mqtt.publish(topicCalibration().c_str(), buf, true);
  Serial.println(ok ? "Published CALIBRATION" : "Failed to publish CALIBRATION");
  return ok;
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

// The sampling task reads the sensors at a steady rate and only touches this
// queue, so a stalled send delays samples instead of losing them.
void sampleTask(void* param) {

  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000UL / sampleRateHz));
    if (!samplingEnabled) continue;

    RawSample sample;
    sample.t = millis();   // timestamp at read time, not send time
    mpu.getMotion6(&sample.imu[0], &sample.imu[1], &sample.imu[2],
                   &sample.imu[3], &sample.imu[4], &sample.imu[5]);
    for (int i = 0; i < 5; i++) {
      sample.flex[i] = analogRead(flexPins[i]);
    }

    // Buffer full: drop the oldest sample, the newest is the most useful.
    if (xQueueSend(sampleQueue, &sample, 0) != pdTRUE) {
      RawSample oldest;
      xQueueReceive(sampleQueue, &oldest, 0);
      xQueueSend(sampleQueue, &sample, 0);
      samplesDropped = samplesDropped + 1;
    }
  }
}

// One sample as [t,ax,ay,az,gx,gy,gz,f1,f2,f3,f4,f5]: accelerometer in m/s^2,
// gyroscope in degrees/second, raw flex readings 0-4095 (thumb .. pinky).
int formatSample(char* out, size_t size, const RawSample& s) {
  return snprintf(out, size, "[%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u]",
                  (unsigned long)s.t,
                  (s.imu[0] / ACCEL_SENSITIVITY) * GRAVITY,
                  (s.imu[1] / ACCEL_SENSITIVITY) * GRAVITY,
                  (s.imu[2] / ACCEL_SENSITIVITY) * GRAVITY,
                  s.imu[3] / GYRO_SENSITIVITY,
                  s.imu[4] / GYRO_SENSITIVITY,
                  s.imu[5] / GYRO_SENSITIVITY,
                  s.flex[0], s.flex[1], s.flex[2], s.flex[3], s.flex[4]);
}

// Sends the next batch once it is full. A batch that fails is kept and retried.
void publishTelemetry() {

  static RawSample batch[BATCH_SAMPLES];
  static int batchCount = 0;
  static unsigned long lastSendMs = 0;
  static char buf[TELEMETRY_BUFFER_BYTES];

  while (batchCount < BATCH_SAMPLES && xQueueReceive(sampleQueue, &batch[batchCount], 0) == pdTRUE) {
    batchCount++;
  }

  // A part-full batch only goes out after a while, for low CONFIG sample rates.
  if (batchCount == 0) return;
  if (batchCount < BATCH_SAMPLES && millis() - lastSendMs < BATCH_MAX_WAIT_MS) return;

  int len = snprintf(buf, sizeof(buf),
                     "{\"type\":\"TELEMETRY\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"seq\":%lu,\"timestamp_ms\":%lu,\"payload\":{\"samples\":[",
                     DEVICE_ID, sessionId.c_str(), (unsigned long)seq, (unsigned long)batch[batchCount - 1].t);
  for (int i = 0; i < batchCount; i++) {
    if (i > 0) buf[len++] = ',';
    len += formatSample(buf + len, sizeof(buf) - len, batch[i]);
  }
  len += snprintf(buf + len, sizeof(buf) - len, "]}}");

  if (mqtt.publish(topicRaw().c_str(), buf)) {
    seq++;
    publishOk++;
    samplesSent += batchCount;
    lastSendMs = millis();
  } else {
    publishFail++;
    static unsigned long lastWarnMs = 0;
    if (millis() - lastWarnMs > 1000) {
      lastWarnMs = millis();
      // These numbers say why it failed:
      //   len + ~24 > bufSize        -> message too big for the MQTT buffer
      //   connected=0                -> tunnel/MQTT session dropped
      //   heap/maxBlock very small   -> ESP32 out of memory (libssh can't allocate the packet)
      Serial.printf("Telemetry publish failed: len=%d bufSize=%u connected=%d state=%d freeHeap=%u maxBlock=%u\n",
                    len, (unsigned)mqtt.getBufferSize(),
                    (int)mqtt.connected(), mqtt.state(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
  }

  // ---- Serial status (twice a second, not every batch) ----
  static unsigned long lastPrintMs = 0;
  if (millis() - lastPrintMs >= 500) {
    lastPrintMs = millis();
    const RawSample& s = batch[batchCount - 1];
    Serial.printf("Flex: %u %u %u %u %u | sent=%lu buffered=%u dropped=%lu | pub ok=%lu fail=%lu heap=%u\n",
                  s.flex[0], s.flex[1], s.flex[2], s.flex[3], s.flex[4],
                  samplesSent, (unsigned)uxQueueMessagesWaiting(sampleQueue), samplesDropped,
                  publishOk, publishFail, (unsigned)ESP.getFreeHeap());
  }
  batchCount = 0;
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
  // a batch of samples - publish() would silently return false.
  bool bufferOk = mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  Serial.printf("MQTT buffer resize to %d: %s\n", MQTT_BUFFER_BYTES, bufferOk ? "ok" : "FAILED");

  connectMQTT();
  for (;;) {
    if (!mqtt.connected()) {
      calibrationPublished = false;
      connectMQTT();
    }
    mqtt.loop();

    // Calibrate on the first connection only. A later reconnect just re-sends
    // the stored readings, in case the broker lost them.
    if (!calibrationComplete) {
      calibrateFlexSensors();
      continue;
    }
    if (!calibrationPublished) {
      calibrationPublished = publishCalibration();
      if (!calibrationPublished) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      // Sample at the compiled-in sampleRateHz - a CONFIG on cmd/<id>/set
      // is an optional override, not a gate. Sampling carries on through a
      // later reconnect, so the buffer holds what happened during it.
      samplingEnabled = true;
    }

    publishTelemetry();
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

  // ---- Vibration motor off until calibration starts ----
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);

  // ---- Network ----
  connectWiFi();

  sampleQueue = xQueueCreate(SAMPLE_BUFFER_SAMPLES, sizeof(RawSample));
  if (sampleQueue == NULL) {
    Serial.println("Could not allocate the sample buffer!");
    while (1);
  }

  // Calibration reads the same sensors, but only before samplingEnabled is set.
  // On core 1, away from the WiFi stack, and above the send task.
  xTaskCreatePinnedToCore(sampleTask, "SampleTask", 4096, NULL, 6, NULL, 1);
  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);
}

void loop() {
  // All the work happens in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
