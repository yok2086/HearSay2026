#include <Wire.h>
#include <MPU6050.h>

MPU6050 mpu;

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
// READ SENSORS
// ==================================================

void readSensors() {

  unsigned long t = millis();   // timestamp at read time, not print time

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

  // ---- One CSV line: millis,ax,ay,az,gx,gy,gz,f1,f2,f3,f4,f5 ----
  Serial.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d\n",
                t, ax, ay, az, gx, gy, gz,
                flexValue[0], flexValue[1], flexValue[2], flexValue[3], flexValue[4]);
}


// ==================================================
// SETUP
// ==================================================

#define SAMPLE_RATE_HZ 100
const unsigned long SAMPLE_INTERVAL_MS = 1000 / SAMPLE_RATE_HZ;
unsigned long lastSampleMs = 0;

void setup() {

  Serial.begin(921600);
  delay(1000);

  // ---- Initialise MPU6050 ----
  Wire.begin(MPU_SDA, MPU_SCL);

  Serial.println("# Initialising MPU6050 ...");
  mpu.initialize();

  if (mpu.testConnection()) {
    Serial.println("# MPU6050 connected successfully.");
  } else {
    Serial.println("# MPU6050 connection failed!");
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

  Serial.println("# Five flex sensors initialised.");

  // ---- Keep the vibration motor off ----
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);

  lastSampleMs = millis();
}

void loop() {
  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs += SAMPLE_INTERVAL_MS;
    readSensors();
  }
}
