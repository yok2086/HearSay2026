#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include <math.h>
#include "esp_eap_client.h"
#include "esp_wpa2.h"
#include "ssh_client.h"

// ==================================================
// MQTT / DEVICE SETTINGS
// ==================================================

const char* DEVICE_ID = "mic_01";
const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;

void publishSpeechTelemetry();
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
  caps.add("SPEECH_DETECTION");
  caps.add("TDOA");

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
  isRunning = true;
  Serial.println("Published CONFIG_ACK -> now RUNNING");
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
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 3s");
      delay(3000);
    }
  }
}

// ==================================================
// I2S PORTS
// ==================================================

#define MIC_I2S_PORT     I2S_NUM_0
#define CENTRE_I2S_PORT  I2S_NUM_1


// ==================================================
// LEFT + RIGHT INMP441
// ==================================================

#define MIC_BCLK 17
#define MIC_WS   18
#define MIC_SD   16


// ==================================================
// CENTRE INMP441
// ==================================================

#define CENTRE_BCLK 26
#define CENTRE_WS   27
#define CENTRE_SD   4


// ==================================================
// AUDIO SETTINGS
// ==================================================

#define SAMPLE_RATE 48000
#define BUFFER_FRAMES 256

// Global buffers - avoids stack overflow
int32_t leftRightSamples[BUFFER_FRAMES * 2];
int32_t centreSamples[BUFFER_FRAMES * 2];


// ==================================================
// SOUND DETECTION SETTINGS
// ==================================================

#define DISPLAY_SCALE 10000.0f

// Sound must exceed this level
#define SOUND_THRESHOLD 4.0f

// Event considered quiet below this
#define RELEASE_THRESHOLD 2.5f

// Prevent immediate retriggering
#define COOLDOWN_MS 250


// ==================================================
// SPEECH FILTER SETTINGS
// ==================================================
//
// Main purpose:
// reject short clicks, bumps and isolated noise.
//
// A human word such as "hello" lasts across many
// audio blocks.
//
// A short noise spike often lasts only 1-2 blocks.
// ==================================================

// Number of consecutive loud frames required
// before we consider the signal a possible voice.
#define SPEECH_START_FRAMES 3

// Minimum number of loud frames required before
// accepting the entire event as speech.
#define MIN_SPEECH_FRAMES 5

// Require several consecutive quiet frames before
// ending the event.
//
// This prevents gaps inside "hello" from splitting
// the word into several events.
#define QUIET_FRAMES_TO_END 4

// Safety limit so an event cannot remain active forever
#define MAX_EVENT_FRAMES 150


// ==================================================
// TDOA SETTINGS
// ==================================================

#define MAX_LAG 20

// ±1 samples treated as front / uncertain
#define CENTRE_LAG_ZONE 1

// Reject weak correlation
#define MIN_CORRELATION 0.35f

// Strong TDOA readings receive more confidence
#define STRONG_CORRELATION 0.70f

// Need several timing measurements before
// accepting a direction
#define MIN_TDOA_VOTES 3


// ==================================================
// EVENT VARIABLES
// ==================================================

bool soundEventActive = false;

int loudStartFrames = 0;
int quietFrames = 0;
int eventFrames = 0;
int speechFrames = 0;

float peakLeft = 0;
float peakRight = 0;
float peakCentre = 0;

unsigned long lastDetectionTime = 0;


// ==================================================
// TDOA VOTING
// ==================================================

int leftTDOAVotes = 0;
int rightTDOAVotes = 0;
int centreTDOAVotes = 0;

int validTDOAFrames = 0;

float bestCorrelation = 0.0f;
int bestLag = 0;


// ==================================================
// SETUP LEFT + RIGHT MICROPHONES
// ==================================================

void setupLeftRightMicrophones() {

  i2s_config_t micConfig = {

    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    ),

    .sample_rate = SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_RIGHT_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len =
      BUFFER_FRAMES,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t micPins = {

    .bck_io_num = MIC_BCLK,

    .ws_io_num = MIC_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = MIC_SD
  };


  esp_err_t result =
    i2s_driver_install(
      MIC_I2S_PORT,
      &micConfig,
      0,
      NULL
    );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: Left/Right I2S installation failed."
    );

    while (1);
  }


  result =
    i2s_set_pin(
      MIC_I2S_PORT,
      &micPins
    );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: Left/Right pin configuration failed."
    );

    while (1);
  }


  i2s_zero_dma_buffer(
    MIC_I2S_PORT
  );
}


// ==================================================
// SETUP CENTRE MICROPHONE
// ==================================================

void setupCentreMicrophone() {

  i2s_config_t centreConfig = {

    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    ),

    .sample_rate = SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_RIGHT_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len =
      BUFFER_FRAMES,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t centrePins = {

    .bck_io_num = CENTRE_BCLK,

    .ws_io_num = CENTRE_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = CENTRE_SD
  };


  esp_err_t result =
    i2s_driver_install(
      CENTRE_I2S_PORT,
      &centreConfig,
      0,
      NULL
    );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: Centre I2S installation failed."
    );

    while (1);
  }


  result =
    i2s_set_pin(
      CENTRE_I2S_PORT,
      &centrePins
    );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: Centre pin configuration failed."
    );

    while (1);
  }


  i2s_zero_dma_buffer(
    CENTRE_I2S_PORT
  );
}


// ==================================================
// CALCULATE LEFT + RIGHT RMS
// ==================================================

void calculateLeftRightRMS(
  int32_t *samples,
  int sampleCount,
  float &leftRMS,
  float &rightRMS
) {

  int frameCount =
    sampleCount / 2;


  if (frameCount <= 0) {

    leftRMS = 0;
    rightRMS = 0;

    return;
  }


  double leftTotal = 0;
  double rightTotal = 0;


  // ==================================================
  // DC OFFSET
  // ==================================================

  for (
    int i = 0;
    i < sampleCount;
    i += 2
  ) {

    int32_t leftSample =
      samples[i] >> 8;

    int32_t rightSample =
      samples[i + 1] >> 8;


    leftTotal += leftSample;
    rightTotal += rightSample;
  }


  double leftOffset =
    leftTotal / frameCount;

  double rightOffset =
    rightTotal / frameCount;


  // ==================================================
  // AC RMS
  // ==================================================

  double leftSumSquares = 0;
  double rightSumSquares = 0;


  for (
    int i = 0;
    i < sampleCount;
    i += 2
  ) {

    double leftAC =
      (samples[i] >> 8) -
      leftOffset;


    double rightAC =
      (samples[i + 1] >> 8) -
      rightOffset;


    leftSumSquares +=
      leftAC * leftAC;


    rightSumSquares +=
      rightAC * rightAC;
  }


  leftRMS =
    sqrt(
      leftSumSquares /
      frameCount
    );


  rightRMS =
    sqrt(
      rightSumSquares /
      frameCount
    );
}


// ==================================================
// CALCULATE CENTRE RMS
// ==================================================

float calculateCentreRMS(
  int32_t *samples,
  int sampleCount
) {

  int frameCount =
    sampleCount / 2;


  if (frameCount <= 0) {

    return 0;
  }


  double total = 0;


  // Centre microphone is on RIGHT I2S channel

  for (
    int i = 1;
    i < sampleCount;
    i += 2
  ) {

    int32_t sample =
      samples[i] >> 8;

    total += sample;
  }


  double offset =
    total / frameCount;


  double sumSquares = 0;


  for (
    int i = 1;
    i < sampleCount;
    i += 2
  ) {

    double ac =
      (samples[i] >> 8) -
      offset;


    sumSquares +=
      ac * ac;
  }


  return sqrt(
    sumSquares /
    frameCount
  );
}


// ==================================================
// TDOA CROSS-CORRELATION
// ==================================================

bool calculateTDOA(
  int32_t *samples,
  int sampleCount,
  int &lagResult,
  float &correlationResult
) {

  int frameCount =
    sampleCount / 2;


  if (
    frameCount <
    (MAX_LAG * 2 + 20)
  ) {

    return false;
  }


  // ==================================================
  // CALCULATE MEANS
  // ==================================================

  double leftMean = 0;
  double rightMean = 0;


  for (
    int i = 0;
    i < frameCount;
    i++
  ) {

    leftMean +=
      (samples[i * 2] >> 8);


    rightMean +=
      (samples[i * 2 + 1] >> 8);
  }


  leftMean /=
    frameCount;

  rightMean /=
    frameCount;


  // ==================================================
  // SEARCH POSSIBLE LAGS
  // ==================================================

  float highestCorrelation = -2.0f;

  int highestLag = 0;


  for (
    int lag = -MAX_LAG;
    lag <= MAX_LAG;
    lag++
  ) {

    double crossSum = 0;

    double leftEnergy = 0;
    double rightEnergy = 0;

    int validSamples = 0;


    for (
      int i = MAX_LAG;
      i < frameCount - MAX_LAG;
      i++
    ) {

      int rightIndex =
        i + lag;


      if (
        rightIndex < 0 ||
        rightIndex >= frameCount
      ) {

        continue;
      }


      double leftSample =
        (samples[i * 2] >> 8) -
        leftMean;


      double rightSample =
        (samples[rightIndex * 2 + 1] >> 8) -
        rightMean;


      crossSum +=
        leftSample *
        rightSample;


      leftEnergy +=
        leftSample *
        leftSample;


      rightEnergy +=
        rightSample *
        rightSample;


      validSamples++;
    }


    if (
      validSamples == 0 ||
      leftEnergy <= 0 ||
      rightEnergy <= 0
    ) {

      continue;
    }


    double denominator =
      sqrt(
        leftEnergy *
        rightEnergy
      );


    float correlation =
      crossSum /
      denominator;


    if (
      correlation >
      highestCorrelation
    ) {

      highestCorrelation =
        correlation;

      highestLag =
        lag;
    }
  }


  lagResult =
    highestLag;

  correlationResult =
    highestCorrelation;


  if (
    highestCorrelation <
    MIN_CORRELATION
  ) {

    return false;
  }


  return true;
}


// ==================================================
// READ ALL MICROPHONES
// ==================================================

bool readMicrophones(
  float &leftRMS,
  float &rightRMS,
  float &centreRMS,
  int &sampleCountLR
) {

  size_t bytesReadLR = 0;
  size_t bytesReadCentre = 0;


  // ==================================================
  // LEFT + RIGHT
  // ==================================================

  esp_err_t resultLR =
    i2s_read(
      MIC_I2S_PORT,
      leftRightSamples,
      sizeof(leftRightSamples),
      &bytesReadLR,
      portMAX_DELAY
    );


  if (
    resultLR != ESP_OK ||
    bytesReadLR == 0
  ) {

    return false;
  }


  // ==================================================
  // CENTRE
  // ==================================================

  esp_err_t resultCentre =
    i2s_read(
      CENTRE_I2S_PORT,
      centreSamples,
      sizeof(centreSamples),
      &bytesReadCentre,
      portMAX_DELAY
    );


  if (
    resultCentre != ESP_OK ||
    bytesReadCentre == 0
  ) {

    return false;
  }


  sampleCountLR =
    bytesReadLR /
    sizeof(int32_t);


  int sampleCountCentre =
    bytesReadCentre /
    sizeof(int32_t);


  calculateLeftRightRMS(
    leftRightSamples,
    sampleCountLR,
    leftRMS,
    rightRMS
  );


  centreRMS =
    calculateCentreRMS(
      centreSamples,
      sampleCountCentre
    );


  return true;
}


// ==================================================
// RESET EVENT
// ==================================================

void resetSoundEvent() {

  soundEventActive = false;

  loudStartFrames = 0;
  quietFrames = 0;

  eventFrames = 0;
  speechFrames = 0;

  peakLeft = 0;
  peakRight = 0;
  peakCentre = 0;

  leftTDOAVotes = 0;
  rightTDOAVotes = 0;
  centreTDOAVotes = 0;

  validTDOAFrames = 0;

  bestCorrelation = 0;
  bestLag = 0;
}


void publishSpeechTelemetry() {
  if (!isRunning || !mqtt.connected()) {
    return;
  }

  const char* direction = "UNCERTAIN";
  if (validTDOAFrames >= MIN_TDOA_VOTES) {
    if (leftTDOAVotes > rightTDOAVotes && leftTDOAVotes > centreTDOAVotes) {
      direction = "LEFT";
    } else if (rightTDOAVotes > leftTDOAVotes && rightTDOAVotes > centreTDOAVotes) {
      direction = "RIGHT";
    } else if (centreTDOAVotes > leftTDOAVotes && centreTDOAVotes > rightTDOAVotes) {
      direction = "FRONT";
    }
  }

  StaticJsonDocument<512> doc;
  doc["type"] = "TELEMETRY";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  JsonObject payload = doc.createNestedObject("payload");
  payload["event"] = "SPEECH";
  payload["direction"] = direction;
  payload["speech_frames"] = speechFrames;
  payload["event_frames"] = eventFrames;
  payload["valid_tdoa_frames"] = validTDOAFrames;
  payload["left_votes"] = leftTDOAVotes;
  payload["front_votes"] = centreTDOAVotes;
  payload["right_votes"] = rightTDOAVotes;
  payload["best_lag"] = bestLag;
  payload["best_correlation"] = bestCorrelation;
  payload["peak_left"] = peakLeft;
  payload["peak_centre"] = peakCentre;
  payload["peak_right"] = peakRight;

  char buf[512];
  size_t len = serializeJson(doc, buf);
  if (mqtt.publish(topicRaw().c_str(), (const uint8_t*)buf, len, false)) {
    Serial.println("Published SPEECH telemetry");
  } else {
    Serial.println("Failed to publish SPEECH telemetry");
  }
}


// ==================================================
// PROCESS COMPLETED EVENT
// ==================================================

void processSoundEvent() {

  Serial.println();

  Serial.println(
    "===================================="
  );


  // ==================================================
  // FIRST CHECK IF EVENT LOOKS LIKE SPEECH
  // ==================================================

  if (
    speechFrames <
    MIN_SPEECH_FRAMES
  ) {

    Serial.print(
      "Rejected short noise: "
    );

    Serial.print(
      speechFrames
    );

    Serial.println(
      " loud frames"
    );


    Serial.println(
      ">>> NON-SPEECH / NOISE IGNORED <<<"
    );


    Serial.println(
      "===================================="
    );

    Serial.println();

    return;
  }


  // ==================================================
  // VALID SPEECH EVENT
  // ==================================================

  Serial.println(
    ">>> SPEECH EVENT DETECTED <<<"
  );

  publishSpeechTelemetry();


  Serial.print(
    "Speech frames = "
  );

  Serial.println(
    speechFrames
  );


  Serial.print(
    "PEAK -> LEFT = "
  );

  Serial.print(
    peakLeft,
    1
  );


  Serial.print(
    " | CENTRE = "
  );

  Serial.print(
    peakCentre,
    1
  );


  Serial.print(
    " | RIGHT = "
  );

  Serial.println(
    peakRight,
    1
  );


  Serial.print(
    "TDOA VOTES -> LEFT = "
  );

  Serial.print(
    leftTDOAVotes
  );


  Serial.print(
    " | FRONT = "
  );

  Serial.print(
    centreTDOAVotes
  );


  Serial.print(
    " | RIGHT = "
  );

  Serial.println(
    rightTDOAVotes
  );


  Serial.print(
    "BEST TDOA -> lag = "
  );

  Serial.print(
    bestLag
  );


  Serial.print(
    " | corr = "
  );

  Serial.println(
    bestCorrelation,
    3
  );


  // ==================================================
  // FINAL DIRECTION
  // ==================================================
  //
  // Your experimental results show:
  //
  // positive lag = LEFT
  // negative lag = RIGHT
  //
  // Instead of trusting one frame, use multiple
  // TDOA votes across the spoken word.
  // ==================================================

  if (
    validTDOAFrames <
    MIN_TDOA_VOTES
  ) {

    Serial.println(
      ">>> DIRECTION UNCERTAIN <<<"
    );
  }


  else if (
    leftTDOAVotes >
      rightTDOAVotes &&
    leftTDOAVotes >
      centreTDOAVotes
  ) {

    Serial.println(
      ">>> SPEECH DETECTED ON LEFT <<<"
    );
  }


  else if (
    rightTDOAVotes >
      leftTDOAVotes &&
    rightTDOAVotes >
      centreTDOAVotes
  ) {

    Serial.println(
      ">>> SPEECH DETECTED ON RIGHT <<<"
    );
  }


  else if (
    centreTDOAVotes >
      leftTDOAVotes &&
    centreTDOAVotes >
      rightTDOAVotes
  ) {

    Serial.println(
      ">>> SPEECH DETECTED FROM FRONT <<<"
    );
  }


  else {

    Serial.println(
      ">>> DIRECTION UNCERTAIN <<<"
    );
  }


  Serial.println(
    "===================================="
  );

  Serial.println();
}


// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(1000);


  Serial.println();

  Serial.println(
    "===================================="
  );

  Serial.println(
    "3-MIC SPEECH + TDOA DETECTOR"
  );

  Serial.println(
    "===================================="
  );


  Serial.println(
    "Initialising microphones..."
  );


  setupLeftRightMicrophones();

  setupCentreMicrophone();


  Serial.println(
    "Microphones ready."
  );


  Serial.println();

  Serial.print(
    "Sample rate = "
  );

  Serial.print(
    SAMPLE_RATE
  );

  Serial.println(
    " Hz"
  );


  Serial.println(
    "Short noise rejection enabled."
  );

  Serial.println(
    "TDOA Left/Right detection enabled."
  );

  Serial.println();

  connectWiFi();

  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);

  delay(1000);
}


// ==================================================
// LOOP
// ==================================================

void processMicrophone() {

  float leftRMS;
  float rightRMS;
  float centreRMS;

  int sampleCountLR = 0;


  // ==================================================
  // READ MICROPHONES
  // ==================================================

  if (
    !readMicrophones(
      leftRMS,
      rightRMS,
      centreRMS,
      sampleCountLR
    )
  ) {

    Serial.println(
      "Microphone read error."
    );

    delay(100);

    return;
  }


  // ==================================================
  // NORMALISE RMS
  // ==================================================

  float leftLevel =
    leftRMS /
    DISPLAY_SCALE;


  float rightLevel =
    rightRMS /
    DISPLAY_SCALE;


  float centreLevel =
    centreRMS /
    DISPLAY_SCALE;


  // ==================================================
  // DISPLAY LEVELS
  // ==================================================

  Serial.print(
    "LEFT = "
  );

  Serial.print(
    leftLevel,
    1
  );


  Serial.print(
    " | CENTRE = "
  );

  Serial.print(
    centreLevel,
    1
  );


  Serial.print(
    " | RIGHT = "
  );

  Serial.println(
    rightLevel,
    1
  );


  // ==================================================
  // FIND STRONGEST MICROPHONE
  // ==================================================

  float strongestLevel =
    max(
      leftLevel,
      max(
        rightLevel,
        centreLevel
      )
    );


  float strongestSide =
    max(
      leftLevel,
      rightLevel
    );


  // ==================================================
  // POSSIBLE SPEECH START
  // ==================================================
  //
  // Do NOT immediately start an event because one
  // frame exceeds 4.0.
  //
  // Require several consecutive loud frames.
  // ==================================================

  if (!soundEventActive) {

    if (
      strongestLevel >
      SOUND_THRESHOLD
    ) {

      loudStartFrames++;
    }

    else {

      loudStartFrames = 0;
    }


    // ==================================================
    // CONFIRMED START
    // ==================================================

    if (
      loudStartFrames >=
        SPEECH_START_FRAMES &&
      millis() -
        lastDetectionTime >
        COOLDOWN_MS
    ) {

      soundEventActive = true;

      eventFrames = 0;
      speechFrames = 0;
      quietFrames = 0;

      peakLeft = leftLevel;
      peakRight = rightLevel;
      peakCentre = centreLevel;

      leftTDOAVotes = 0;
      rightTDOAVotes = 0;
      centreTDOAVotes = 0;

      validTDOAFrames = 0;

      bestCorrelation = 0;
      bestLag = 0;


      Serial.println();

      Serial.println(
        "--- POSSIBLE SPEECH EVENT STARTED ---"
      );
    }
  }


  // ==================================================
  // ACTIVE SPEECH EVENT
  // ==================================================

  if (soundEventActive) {

    eventFrames++;


    // ==================================================
    // UPDATE PEAKS
    // ==================================================

    if (
      leftLevel >
      peakLeft
    ) {

      peakLeft =
        leftLevel;
    }


    if (
      rightLevel >
      peakRight
    ) {

      peakRight =
        rightLevel;
    }


    if (
      centreLevel >
      peakCentre
    ) {

      peakCentre =
        centreLevel;
    }


    // ==================================================
    // COUNT SPEECH / LOUD FRAMES
    // ==================================================

    if (
      strongestLevel >
      SOUND_THRESHOLD
    ) {

      speechFrames++;

      quietFrames = 0;
    }

    else if (
      strongestLevel <
      RELEASE_THRESHOLD
    ) {

      quietFrames++;
    }

    else {

      // Between release and trigger threshold.
      // Do not immediately terminate the event.

      quietFrames = 0;
    }


    // ==================================================
    // TDOA
    // ==================================================
    //
    // TDOA only uses Left + Right because they are
    // sampled synchronously.
    // ==================================================

    if (
      strongestSide >
      SOUND_THRESHOLD
    ) {

      int currentLag = 0;

      float currentCorrelation = 0;


      bool currentValid =
        calculateTDOA(
          leftRightSamples,
          sampleCountLR,
          currentLag,
          currentCorrelation
        );


      if (currentValid) {

        validTDOAFrames++;


        Serial.print(
          "  TDOA lag = "
        );

        Serial.print(
          currentLag
        );


        Serial.print(
          " | corr = "
        );

        Serial.println(
          currentCorrelation,
          3
        );


        // ==================================================
        // DIRECTION VOTE
        // ==================================================

        int voteWeight = 1;


        // Strong correlations receive extra weight.

        if (
          currentCorrelation >=
          STRONG_CORRELATION
        ) {

          voteWeight = 2;
        }


        // Positive = LEFT
        if (
          currentLag >
          CENTRE_LAG_ZONE
        ) {

          leftTDOAVotes +=
            voteWeight;
        }


        // Negative = RIGHT
        else if (
          currentLag <
          -CENTRE_LAG_ZONE
        ) {

          rightTDOAVotes +=
            voteWeight;
        }


        // Around zero = FRONT / uncertain
        else {

          centreTDOAVotes +=
            voteWeight;
        }


        // ==================================================
        // STORE STRONGEST CORRELATION
        // ==================================================

        if (
          currentCorrelation >
          bestCorrelation
        ) {

          bestCorrelation =
            currentCorrelation;

          bestLag =
            currentLag;
        }
      }
    }


    // ==================================================
    // END EVENT AFTER SUSTAINED QUIET
    // ==================================================

    if (
      quietFrames >=
        QUIET_FRAMES_TO_END ||
      eventFrames >=
        MAX_EVENT_FRAMES
    ) {

      processSoundEvent();

      resetSoundEvent();

      lastDetectionTime =
        millis();
    }
  }


  delay(20);
}


void mqttTask(void* param) {
  mqtt.setBufferSize(768);
  connectMQTT();

  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }

    mqtt.loop();
    processMicrophone();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}


void loop() {
  // Microphone processing and MQTT are handled by mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
