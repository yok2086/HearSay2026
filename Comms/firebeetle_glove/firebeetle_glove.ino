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

const char* DEVICE_ID = "glove_01B";
const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;
unsigned long sampleRateHz = 50;
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
// VIBRATION MOTOR (used only for the calibration steps)
// ==================================================

#define VIBRATION_PIN 26

// pulses x onMs buzz, with gapMs between pulses. Blocks (calibration only).
void vibrate(int pulses, int onMs, int gapMs = 150) {
  for (int i = 0; i < pulses; i++) {
    digitalWrite(VIBRATION_PIN, HIGH);
    delay(onMs);
    digitalWrite(VIBRATION_PIN, LOW);
    if (i < pulses - 1) delay(gapMs);
  }
}

// ==================================================
// MPU6050 SCALE FACTORS
// ==================================================

// Accelerometer range: +/- 2g
#define ACCEL_SENSITIVITY 16384.0

// Gyroscope range: +/- 250 degrees/second
#define GYRO_SENSITIVITY 131.0

#define GRAVITY 9.80665

// ==================================================
// FLEX SENSOR SETTINGS (binary output: 1 = bent, 0 = straight)
// ==================================================

// Individual bending thresholds determined from
// experimental flex-sensor testing. A finger counts as bent when its
// reading rises at least this far above its own calibrated baseline.

const int flexBendDeltaDefault[5] = {
  75,    // Flex 1 - Thumb
  140,   // Flex 2 - Index
  80,    // Flex 3 - Middle
  65,    // Flex 4 - Ring
  55     // Flex 5 - Pinky
};

// Thresholds in use. Start at the defaults; the app's guided calibration
// replaces them with each wearer's own (halfway between straight and fist).
int flexBendDelta[5] = {75, 140, 80, 65, 55};

// 100 samples x 10 ms = approximately 1 second
#define CALIBRATION_SAMPLES 100

// Starting baseline (straight/rest reading) of each flex sensor
int flexBaseline[5];


// ==================================================
// FUNCTION: CALIBRATE FLEX SENSORS
// ==================================================

// waitForUser: true at power-on (gives 2 s to straighten the fingers).
// false when the phone app asks for it - the app already showed a countdown.
void calibrateFlexSensors(bool waitForUser) {

  long total[5] = {0, 0, 0, 0, 0};

  Serial.println();
  Serial.println("Starting flex sensor calibration...");
  Serial.println("Keep all fingers in the straight/rest position.");

  // Give user time to position hand
  if (waitForUser) delay(2000);

  Serial.println("Measuring baseline...");

  // 100 samples x 10 ms = approximately 1 second
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {

    total[0] += analogRead(FLEX1_PIN);
    total[1] += analogRead(FLEX2_PIN);
    total[2] += analogRead(FLEX3_PIN);
    total[3] += analogRead(FLEX4_PIN);
    total[4] += analogRead(FLEX5_PIN);

    delay(10);
  }

  // Calculate individual average baselines
  for (int i = 0; i < 5; i++) {
    flexBaseline[i] = total[i] / CALIBRATION_SAMPLES;
  }

  Serial.println("Flex sensor calibration complete.");

  Serial.print("Flex baselines: ");

  for (int i = 0; i < 5; i++) {
    Serial.print(flexBaseline[i]);
    Serial.print(" ");
  }

  Serial.println();

  Serial.print("Flex thresholds: ");

  for (int i = 0; i < 5; i++) {
    Serial.print(flexBendDelta[i]);
    Serial.print(" ");
  }

  Serial.println();
  Serial.println("----------------------------------");
}



// ==================================================
// GUIDED CALIBRATION (phone app "Calibrate glove")
// ==================================================
//
// The app sends CALIBRATE on cmd/<id>/set with payload.step:
//   "ready"    one short buzz,  wait settle_ms, check every sensor responds
//   "straight" one short buzz,  wait settle_ms, measure straight readings
//   "fist"     two short buzzes, wait settle_ms, measure bent readings
//   "save"     one long buzz,   use the new baselines + thresholds
//   "fail"     three short buzzes, keep the previous calibration
// Each step answers CALIBRATION_ACK on cmd/<id>/ack with the readings and a
// per-finger ok flag. Nothing changes for translation until "save".

#define ADC_MIN_OK 20        // below this the sensor is probably unplugged
#define ADC_MAX_OK 4075      // above this the sensor is probably shorted
#define MAX_STILL_RANGE 250  // max spread while the hand is held still

int pendingStraight[5];
int pendingBent[5];
bool haveStraight = false;
bool haveBent = false;

// About 1 s: average and spread (max - min) of each sensor.
void measureFlex(int avg[5], int range[5]) {
  const int pins[5] = {FLEX1_PIN, FLEX2_PIN, FLEX3_PIN, FLEX4_PIN, FLEX5_PIN};
  long total[5] = {0, 0, 0, 0, 0};
  int lo[5], hi[5];
  for (int f = 0; f < 5; f++) { lo[f] = 4095; hi[f] = 0; }
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    for (int f = 0; f < 5; f++) {
      int v = analogRead(pins[f]);
      total[f] += v;
      if (v < lo[f]) lo[f] = v;
      if (v > hi[f]) hi[f] = v;
    }
    delay(10);
  }
  for (int f = 0; f < 5; f++) {
    avg[f] = total[f] / CALIBRATION_SAMPLES;
    range[f] = hi[f] - lo[f];
  }
}

bool inAdcRange(int v) { return v >= ADC_MIN_OK && v <= ADC_MAX_OK; }

void publishStepAck(const char* step, const int values[5], const bool ok[5]) {
  bool allOk = true;
  for (int f = 0; f < 5; f++) allOk = allOk && ok[f];

  StaticJsonDocument<384> doc;
  doc["type"] = "CALIBRATION_ACK";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  JsonObject payload = doc.createNestedObject("payload");
  payload["step"] = step;
  payload["status"] = allOk ? "OK" : "FAIL";
  JsonArray v = payload.createNestedArray("values");
  JsonArray k = payload.createNestedArray("ok");
  JsonArray b = payload.createNestedArray("flex_baseline");
  for (int f = 0; f < 5; f++) {
    v.add(values[f]);
    k.add(ok[f] ? 1 : 0);
    b.add(flexBaseline[f]);
  }

  char buf[384];
  serializeJson(doc, buf);
  mqtt.publish(topicCmdAck().c_str(), buf);
  Serial.printf("Published CALIBRATION_ACK step=%s status=%s\n", step, allOk ? "OK" : "FAIL");
}

void runCalibrationStep(const String& step, int settleMs) {
  int avg[5], range[5];
  bool ok[5];

  if (step == "ready") {
    haveStraight = haveBent = false;
    vibrate(1, 150);
    delay(settleMs);
    measureFlex(avg, range);
    for (int f = 0; f < 5; f++) ok[f] = inAdcRange(avg[f]) && range[f] <= 2 * MAX_STILL_RANGE;
    publishStepAck("ready", avg, ok);

  } else if (step == "straight") {
    vibrate(1, 150);
    delay(settleMs);
    measureFlex(avg, range);
    for (int f = 0; f < 5; f++) {
      ok[f] = inAdcRange(avg[f]) && range[f] <= MAX_STILL_RANGE;
      pendingStraight[f] = avg[f];
    }
    haveStraight = true;
    publishStepAck("straight", avg, ok);

  } else if (step == "fist") {
    vibrate(2, 150);
    delay(settleMs);
    measureFlex(avg, range);
    for (int f = 0; f < 5; f++) {
      // A fist must bend each finger clearly past its straight reading.
      int bend = avg[f] - pendingStraight[f];
      ok[f] = haveStraight && inAdcRange(avg[f]) && bend >= (flexBendDeltaDefault[f] * 6) / 10;
      pendingBent[f] = avg[f];
    }
    haveBent = true;
    publishStepAck("fist", avg, ok);

  } else if (step == "save") {
    for (int f = 0; f < 5; f++) ok[f] = haveStraight && haveBent;
    if (haveStraight && haveBent) {
      for (int f = 0; f < 5; f++) {
        flexBaseline[f] = pendingStraight[f];
        // Bent = halfway between this wearer's straight and fist readings,
        // kept within 0.5x - 1.5x of the tested default.
        int half = (pendingBent[f] - pendingStraight[f]) / 2;
        flexBendDelta[f] = constrain(half, flexBendDeltaDefault[f] / 2, (flexBendDeltaDefault[f] * 3) / 2);
      }
      vibrate(1, 800);
      Serial.print("Calibration saved. Baselines: ");
      for (int f = 0; f < 5; f++) { Serial.print(flexBaseline[f]); Serial.print(" "); }
      Serial.print(" Thresholds: ");
      for (int f = 0; f < 5; f++) { Serial.print(flexBendDelta[f]); Serial.print(" "); }
      Serial.println();
    } else {
      vibrate(3, 150);
    }
    publishStepAck("save", flexBendDelta, ok);

  } else if (step == "fail") {
    haveStraight = haveBent = false;
    vibrate(3, 150);
    for (int f = 0; f < 5; f++) ok[f] = false;
    publishStepAck("fail", flexBaseline, ok);
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

// Sent after a calibration requested by the app (CALIBRATE on cmd/<id>/set),
// so the app can show the new straight-finger baselines.
void publishCalibrationAck() {
  StaticJsonDocument<256> doc;
  doc["type"] = "CALIBRATION_ACK";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();
  JsonObject payload = doc.createNestedObject("payload");
  payload["status"] = "OK";
  JsonArray baseline = payload.createNestedArray("flex_baseline");
  for (int i = 0; i < 5; i++) baseline.add(flexBaseline[i]);

  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish(topicCmdAck().c_str(), buf);
  Serial.println("Published CALIBRATION_ACK");
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

  // ---- Read flex sensors and convert each to a binary state ----
  int flexValue[5];
  flexValue[0] = analogRead(FLEX1_PIN);
  flexValue[1] = analogRead(FLEX2_PIN);
  flexValue[2] = analogRead(FLEX3_PIN);
  flexValue[3] = analogRead(FLEX4_PIN);
  flexValue[4] = analogRead(FLEX5_PIN);

  int flexChange[5];   // reading minus that finger's own baseline
  int flexState[5];    // 1 = bent, 0 = straight

  for (int i = 0; i < 5; i++) {
    flexChange[i] = flexValue[i] - flexBaseline[i];
    flexState[i] = (flexChange[i] >= flexBendDelta[i]) ? 1 : 0;
  }

  // ---- Build and publish the message ----
  StaticJsonDocument<320> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  JsonObject imu = payload.createNestedObject("imu");
  imu["ax"] = r2(ax);
  imu["ay"] = r2(ay);
  imu["az"] = r2(az);
  imu["gx"] = r2(gx);
  imu["gy"] = r2(gy);
  imu["gz"] = r2(gz);

  // Binary flex states: 1 = bent, 0 = straight (thumb .. pinky)
  JsonArray flex = payload.createNestedArray("flex");
  for (int i = 0; i < 5; i++) {
    flex.add(flexState[i]);
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
  } else if (type == "CALIBRATE") {
    String step = doc["payload"]["step"] | "open_hand";
    int settleMs = constrain((int)(doc["payload"]["settle_ms"] | 2500), 0, 5000);
    Serial.print("Received CALIBRATE from app, step=");
    Serial.println(step);
    if (step == "open_hand") {
      // Older app versions: re-measure the straight baseline right away.
      calibrateFlexSensors(false);
      publishCalibrationAck();
    } else {
      // Guided calibration step (telemetry pauses for a few seconds meanwhile).
      runCalibrationStep(step, settleMs);
    }
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
      if (millis() - lastSampleMs >= intervalMs) {
        lastSampleMs = millis();
        publishTelemetry();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
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

  // ---- Vibration motor off (buzzes only during calibration steps) ----
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);

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

  // Measure each finger's straight baseline at power-on. Without this every
  // baseline is 0 and almost every reading counts as "bent". Keep the hand open
  // and fingers straight for ~3 s after switching the glove on. The phone app's
  // "Calibrate glove" button can redo this at any time.
  calibrateFlexSensors(true);

  // ---- Network ----
  connectWiFi();

  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);
}

void loop() {
  // All the work happens in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
