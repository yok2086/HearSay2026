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

const char* DEVICE_ID = "mic_lr_01";
const char* MQTT_USER = "nathan";
const char* MQTT_PASS = "mqtt";

SSHForwardClient sshClient;
PubSubClient mqtt(sshClient);

String sessionId = "sess_001";
uint32_t seq = 0;
bool isRunning = false;

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
  caps.add("AUDIO_PLAYBACK");

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
      // Stream straight away - a CONFIG on cmd/<id>/set is now an optional
      // override, not a gate.
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
// CENTRE INMP441 + MAX98357A
// ==================================================

#define CENTRE_BCLK 26
#define CENTRE_WS   27
#define CENTRE_SD   4

#define AMP_DIN     25


// ==================================================
// AUDIO SETTINGS
// ==================================================

#define SAMPLE_RATE   48000
#define BUFFER_FRAMES 256

// Global buffers to avoid stack overflow
int32_t leftRightSamples[BUFFER_FRAMES * 2];
int32_t centreSamples[BUFFER_FRAMES * 2];


// ==================================================
// MAX98357A PLAYBACK SETTINGS
// ==================================================

// The embedded sample is an 8 kHz mono G.711 mu-law
// representation of the word "me".
#define ME_AUDIO_SAMPLES 3450

// Number of 32-bit I2S words in the stereo TX buffer.
// 256 words = 128 stereo frames.
#define AUDIO_TX_WORDS 256

int32_t audioTxBuffer[AUDIO_TX_WORDS];

const char ME_AUDIO_BASE64[] PROGMEM =
"e3FqY19dXl9jZmhqcf7t6Orw+fnu5N7g7uzu2s7HxMfHycW/vL7D099q+vNeUUE9Oz9FSEdGQkZNW21sa11dYm99fnNqaG787ujm5ePf3t7g5u32fnRsZF9dXl9iZGRob/zr6O78fPnl3Nff6+vlzsa/wMTGxsC6ubzH3PJh4HBcRzw5OT9GRkM/PURMYmtqXlRZYHj3+XRqa33t5ODg4N/d3d/m7fd9c2piXl1eX2BfX2dz8enufG936dnS1OXo69HEvLu/w8bBurW4vdXqW/naalI8NzQ6QkdCPjo7Q1FsZ2NRUFhr8Ov1b2ly7+Te3t/f3t7e5Ov1/nhuZ2FfYGJjYWBlbvrr7nxsbvXd1NLg6e7byL+7v8TJx763uLzN6Gdi2HpfQzo3OkJIRkI9PENNZ2ppWVFXY/vt8Hdrbfrq4uDi4+Hf3+Pq8vx8cWliXl1fYWBfX2Zz7ujwc2ty59bP0+Tn7M3Aurm+wsbAuLO2vNjvV+/ba086NjM5QUZBPTk7QlJtZmFQT1ht7+v5bWhy7+Tf4OHh3t3e5Oz1/npxaWJfX2JkY2FjbH7s7Ptubf7f1tHc6OvgysC7vsPHx7+4uLvI5HBc2vdiRjs3OEBIR0I+O0FLYmtpW1FWX//t7ntsbP7r4uDh4+Hg3+Pp7/p7b2hgXl1eX15eX2h76+r3bm3/3tLO1+Xn4ci+uLu+w8S+tbS3wN1+VtvtYkc4NDQ8Q0Q/PDg9RltpZVtOUVt27ez+bGl77OHd3t/f3t7h5+76e29pZGBgYWFfX2Nt/OzufWxu993U0d3n7N3Iv7u+wsfGvre4u8rkbl7Yfl9EOjc5QEdGQT07QUtiaWdaUFZgfu7ufGxt/Ovi3+Dh4eDh5erx/HdsZV9eXl9eXV1hbffp7Xtrbu/Zz87d5OrXxLy4vL/EwruztbnK5GZf1HdcPzczNj5EQj46OD5JYWVjVU5TX/3s73ZpbPro397e397e3uHp7/t5b2ljYGBhYmFgY2z+7Oz8bm773tXQ3Ofq38m/u77Cxsa/t7e6x+F1W9r4YUc6Nzg/RkZBPTs/SV9pZ1tQVV567+97bGx/7OLf4OHg39/j6e/6em5nYF5eXl9eXV9qfurr/G1t+dzRzdnk59zGvbi7v8PDvLS0uMbgbVvWfl1ANzM1PURCPjo4PkhfZmRXTVJdfuvsfWxs+uje3N3e3t7f4+nw/3NrZmJgYF9eXWBqfe3vfGxt+t3Uz9zm693Iv7u+wsfGvre4u8vla2HZd15COjc5QUhFQDw7QkxmaGVWT1Vh+evsfm1v9+je29vc29/j5uz4cGdhXldPTE5MSEpPXub36sy8xt/F2M3dS0j2v0tQtLO0r7usra0/R680LCs4MS43MGDDQFDAx1lG40lKQz1MW1NT2cnPysLDxsjLz9be7HpoXFZPSklJRENPVU1p1NHNysXIvdxKwtlCP8XITL23uKe5vaumfy2qRyUrQjEmPDdB0D5cvchFVr5CPU5OQURTWP7j3snHy8fDys/N1u9/dFdLTEtCQkpMSF3o4tDMxL3H3dq5QjrLYzg9tN/WrralorusoaokuLQfJjMyHiwvMd04S6/LaMeuT1XMT0ZCQ0dIRU/mfu3KyMvIyM3Y5W1cUktKTU5NZN3u1cTDycjG0NZPQtY4MUvzNj6vzryrr6CptaygyiikMyEsNCUiLytCPS+xxW26rcTZttr1XUtWQz1KT0VP7m5x19fk5N/zXmJoXlf72OrYvsTNv7vZ3t5PWTM1WC4tScAtwKnAqamhnauso6Afr8EbLCwlHCYlLzQntNJBrq28vau9v8vczkZDWT86QkY/Q01OT1hu/Pzd0s7Nx8DCyL++1+THU0RKPD0uND86Lf65M6+psqinn6Wss57MKZ8kJi8qISIoKjAkT8QttrbCu66yubq8uenwyklFTkM9PkNCRUxbdunPxb+/vrq7yM/Bcj5nPi8zNDIrMj1bLLitQKWkqKegnaSpuZkqRqAZMyslHh8nKCQixzEwqu6/s62xtbCvvc+720hiSz08Pz49Rk5TdNjSy8DAxMC/yubQ4T1LSTU0NzgvNTvpNlOoTLSlqqqmn6eqv57JJ54hKjUqJCEpKikkUFApsc7Zt7OyubWyutK+yk5qWEM/Q0E/QkxRVfjSzsm/vb/AvcHX9cxJO045MDE2Mi80SvssrLlvpKWop6CfpqyvnCC4tRs8KSUdIygoIyrOKU6vT7q0rre0sLLCxLr/WeVKP0JCPj1FR0xbeOPRy8rDwMTIxMjvftlGPks7NTU8NTU4d042qs3IpqmqqqKmqrippyGnOSI/KycgKSoqJjprKcS/V7i3tby1tbrJv8Nf7/5KR0pFQURJS01fe/DTyMjEv77ExsPOXultOkFAMzA0ODE1PMkwx6pxrKSoqKSdpqm9nDY7ohwzLScfIignJCdgMTCuT8i1sre3sLS9xLnXYtdWRUhHPz9DR0hNYnvt18zLycXCyM3KzWFo7T8/Rjo2Nj03OzrNQU2r1bqoqqqqoqirv6S7KqUoKzkrJyQrKiopREMsutxft7u5vbS4vcW8zG/Zc0tNTkZDSElJT1lf/d3XzcbHxsDCy83K4VJ+Ujo/PTcwODg5NEzHMrWv1qqlp6mln6ervZ0tYaodOisoHyUpJiUrUy07tETEtbm5t7G4vb+63ejOVE1WSUNGSEZJTVVbZe7f29LOy8rMyMjU1s1yU3VOPUM+OzU9Oz43aNE4tLfLrKmqrKilq7G7oSvFuSFDLisjKisqKTBNL0a6QcK8vb65t73BwMDo3NVWVV5NSk5OTE5VV1pieP7t29fU0MzKzcvIz97Q21JdXT8/Pzw1Nj48Ojy+PNyry7KoqamqoairyJ9cMaQiMjUqJCYrKCgqRzUwuExztMC9u7O8vr+919zNY1VnUUtNT0xMUlZVW2tneevs49nZ19HP0tLO0OTh2FlUakZCQkM7OkNDPz2/RfeuzLitra2vp62vzqVXN6kmOjkuKSwwLC0vTjY5u0Tfu8fEvrnDw8TC29vQZ1x3V09WVFFTWFpbX2xscuzr6NzY19XPztTRzNvv2PdNXlE/QUA/Nj5CRjfWzjyvusStq6qvqaitv6+oIqtEJVQuKyYvKywsPEMtzNtAucTFwre/wsHAy+PM5VlzY05SV09PVlhZXGRsa/7t7ube29va09Lb1tLk995zT19RRkRGRztES0w6zN5DssDEsa+ttaytss6vsCmqNy1SMi4sNS4wMUNCM8dpSLvOzMS8ycfDyNTc0Hpk9V1VWltUV1xbXWJqaHL8fO3m6N/a2dvT0tna09xu5PhNWFNFQUNJOkBLUDnLzD+zvMWwr6y0rauwybKrJatGKGMyLSo2LS8wPUQvz+I+ucvPxLrIx8DHz9rN7mPuZFNbXVJXXFpcYGdpa3f9fvXt6+nk3t3f2dff3tnmbupyUVtUS0VKTT5HVlQ9xuZLtsXFt7Swu6+wt+auuyyqMjNdNjAyOy82N0dCN8ZQTLze1ca/1cfGztfW1G9+7F5cZlxZXWBeYGhqanF4d/32+O3o6Obd3d/b193m2eFj8XtOVVNMQkhQPUZZWzvD2Um1w8K2tK68r6+2+q65KaYyMWU2LzE7LjQ3REE2xk5Ju+fXxb/XxsTO1dPRcffnX1xrXVhfX11hZmtpbX90dPL7/e3r7Ojj3+Th2uLt3+Rnev1VVldUSEpdRUhbeT7Tyki8w8m8u7K+urO63r2vKrFQLuQ8NTRDNTk9Q0o62GtDwOXry8XdzcjU19XV9u/jaGR1Y1xmZV9lbWlqd3ZxfP59/PH19enq6ebe4Ojd3fTr3mph+VtPVlNORFZQQlDyT0m8Vdi5yr28trXAsbW/cKtJPawrS044MT47Mjs7Rz5Ayj35xWPZxM7ew83W08/c9tztYXp4XGBtX15rZ2dqc29s/Xxy/fR8/ez19+vn7e3f4/Pp4+x+7fZgaWtXVVhaSk9qSU/9/kbG2Fq9yMbBvbfFvbi/9Ly7L7NHN+xBOTtIOD1BRkg+2k9LyXL+0Mvtz8za2tTX7ubfb336aWduaWVpbGpqbnNvcnt5dv79evf49vfv7e/s6Onr5+Ts7ebvc/r8XWFoVVFWW0tOdEtQ7u5KxdJmvMjCwby2yLq4wm61zDWvOT5pPzc/QzU+QEZEP9hEWM1e8MzW78rR3tTS3unb5nHu9WhueGdobmtqbW9vb3V4dHn9d/76ffj2+fXv8fHu7O3w6uz68O10b/hnXmtfV1liVU95WlL+31TZymPEysjHw7vLw7zIfr7JObdBQnRIPUZKPERISklH40dh2V5/09/5z9rm2dnk6d7re+32bnl8bW50b25zc3NzeXh3fX57////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////";

// ==================================================
// BASE64 DECODER
// ==================================================

int base64Value(char c) {

  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;

  return -1;
}


// ==================================================
// MU-LAW DECODER
// ==================================================

int16_t muLawDecode(uint8_t value) {

  value = ~value;

  int t =
    ((value & 0x0F) << 3) + 0x84;

  t <<=
    ((unsigned)value & 0x70) >> 4;

  if (value & 0x80) {

    return (int16_t)(
      0x84 - t
    );
  }

  return (int16_t)(
    t - 0x84
  );
}


// ==================================================
// I2S1: CENTRE MICROPHONE RX ONLY
// ==================================================

bool setupCentreMicrophoneRX() {

  i2s_config_t config = {

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


  i2s_pin_config_t pins = {

    .mck_io_num = I2S_PIN_NO_CHANGE,

    .bck_io_num = CENTRE_BCLK,

    .ws_io_num = CENTRE_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = CENTRE_SD
  };


  esp_err_t result =
    i2s_driver_install(
      CENTRE_I2S_PORT,
      &config,
      0,
      NULL
    );

  if (result != ESP_OK) {
    return false;
  }


  result =
    i2s_set_pin(
      CENTRE_I2S_PORT,
      &pins
    );

  if (result != ESP_OK) {
    i2s_driver_uninstall(CENTRE_I2S_PORT);
    return false;
  }


  i2s_zero_dma_buffer(
    CENTRE_I2S_PORT
  );

  return true;
}


// ==================================================
// I2S1: MAX98357A TX ONLY
// ==================================================

bool setupAmplifierTX() {

  i2s_config_t config = {

    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_TX
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
      AUDIO_TX_WORDS,

    .use_apll = false,

    .tx_desc_auto_clear = true,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pins = {

    .bck_io_num = CENTRE_BCLK,

    .ws_io_num = CENTRE_WS,

    .data_out_num = AMP_DIN,

    .data_in_num =
      I2S_PIN_NO_CHANGE
  };


  esp_err_t result =
    i2s_driver_install(
      CENTRE_I2S_PORT,
      &config,
      0,
      NULL
    );

  if (result != ESP_OK) {
    return false;
  }


  result =
    i2s_set_pin(
      CENTRE_I2S_PORT,
      &pins
    );

  if (result != ESP_OK) {
    i2s_driver_uninstall(CENTRE_I2S_PORT);
    return false;
  }


  i2s_zero_dma_buffer(
    CENTRE_I2S_PORT
  );

  return true;
}


// ==================================================
// PLAY "ME" THROUGH MAX98357A
// ==================================================

void playMeSound() {

  Serial.println("PLAYING ME");

  // Stop Centre microphone I2S1 temporarily.
  i2s_driver_uninstall(CENTRE_I2S_PORT);
  delay(5);


  // Reconfigure I2S1 as TX-only.
  if (!setupAmplifierTX()) {

    // Try to restore the microphone if TX setup fails.
    setupCentreMicrophoneRX();
    return;
  }


  // --------------------------------------------------
  // Decode Base64 once
  // --------------------------------------------------

  static uint8_t sampleBytes[ME_AUDIO_SAMPLES];
  static int decodedSamples = 0;
  static bool sampleLoaded = false;


  if (!sampleLoaded) {

    int outIndex = 0;
    int accumulator = 0;
    int bits = 0;

    const char *text =
      ME_AUDIO_BASE64;


    for (
      int i = 0;
      text[i] != '\0';
      i++
    ) {

      int value =
        base64Value(text[i]);

      if (value < 0) {
        continue;
      }

      accumulator =
        (accumulator << 6) |
        value;

      bits += 6;


      while (bits >= 8) {

        bits -= 8;

        if (
          outIndex <
          ME_AUDIO_SAMPLES
        ) {

          sampleBytes[outIndex++] =
            (accumulator >> bits) & 0xFF;
        }
      }
    }

    decodedSamples = outIndex;
    sampleLoaded = true;
  }


  // --------------------------------------------------
  // Upsample 8 kHz -> 48 kHz
  // --------------------------------------------------

  int txIndex = 0;

  for (
    int i = 0;
    i < decodedSamples - 1;
    i++
  ) {

    int16_t sample0 =
      muLawDecode(
        sampleBytes[i]
      );


    int16_t sample1 =
      muLawDecode(
        sampleBytes[i + 1]
      );


    // 6x linear interpolation
    for (
      int step = 0;
      step < 6;
      step++
    ) {

      int32_t interpolated =
        sample0 +
        (
          (
            (int32_t)sample1 -
            (int32_t)sample0
          ) *
          step
        ) / 6;


      // Moderate output gain
      interpolated =
        (interpolated * 2) / 1;


      if (interpolated > 32767) {
        interpolated = 32767;
      }

      if (interpolated < -32768) {
        interpolated = -32768;
      }


      int32_t i2sSample =
        interpolated << 16;


      // Stereo: same signal on both channels
      audioTxBuffer[txIndex++] =
        i2sSample;

      audioTxBuffer[txIndex++] =
        i2sSample;


      if (txIndex >= AUDIO_TX_WORDS) {

        size_t bytesWritten = 0;

        i2s_write(
          CENTRE_I2S_PORT,
          audioTxBuffer,
          sizeof(audioTxBuffer),
          &bytesWritten,
          portMAX_DELAY
        );

        txIndex = 0;
      }
    }
  }


  // --------------------------------------------------
  // Send remaining TX data
  // --------------------------------------------------

  if (txIndex > 0) {

    for (
      int i = txIndex;
      i < AUDIO_TX_WORDS;
      i++
    ) {

      audioTxBuffer[i] = 0;
    }


    size_t bytesWritten = 0;

    i2s_write(
      CENTRE_I2S_PORT,
      audioTxBuffer,
      sizeof(audioTxBuffer),
      &bytesWritten,
      portMAX_DELAY
    );
  }


  // Wait until all queued audio has actually been transmitted
  // to the MAX98357A before shutting down TX.
  delay(550);

  // Clear any remaining TX DMA data.
  i2s_zero_dma_buffer(
    CENTRE_I2S_PORT
  );


  // Return I2S1 to Centre microphone RX mode.
  i2s_driver_uninstall(
    CENTRE_I2S_PORT
  );

  delay(5);

  setupCentreMicrophoneRX();

  Serial.println("ME PLAYBACK COMPLETE");
}


// ==================================================
// SERIAL COMMAND
// ==================================================

void handleSerialCommand() {

  static String command = "";


  while (
    Serial.available() > 0
  ) {

    char c =
      Serial.read();


    // Ignore common whitespace characters except newline/
    // carriage return, which are handled below.
    if (
      c == '\n' ||
      c == '\r'
    ) {

      command.trim();

      if (
        command.equalsIgnoreCase("me")
      ) {

        playMeSound();
      }

      command = "";

      continue;
    }


    // Build the command.
    if (
      command.length() < 32
    ) {

      command += c;
    }


    // Trigger "me" immediately, even when the Serial Monitor
    // is configured as "No line ending".
    if (
      command.equalsIgnoreCase("me")
    ) {

      playMeSound();

      command = "";
    }
  }
}


// ==================================================
// SETUP LEFT + RIGHT MICROPHONES
// ==================================================

bool setupLeftRightMicrophones() {

  i2s_config_t config = {

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


  i2s_pin_config_t pins = {

    .mck_io_num = I2S_PIN_NO_CHANGE,

    .bck_io_num = MIC_BCLK,

    .ws_io_num = MIC_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = MIC_SD
  };


  esp_err_t result =
    i2s_driver_install(
      MIC_I2S_PORT,
      &config,
      0,
      NULL
    );

  if (result != ESP_OK) {
    return false;
  }


  result =
    i2s_set_pin(
      MIC_I2S_PORT,
      &pins
    );

  if (result != ESP_OK) {
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }


  i2s_zero_dma_buffer(
    MIC_I2S_PORT
  );

  return true;
}


// ==================================================
// SOUND DETECTION SETTINGS
// ==================================================

#define DISPLAY_SCALE 10000.0f

#define SOUND_THRESHOLD 4.0f
#define RELEASE_THRESHOLD 2.5f
#define COOLDOWN_MS 250


// ==================================================
// SPEECH FILTER SETTINGS
// ==================================================

#define SPEECH_START_FRAMES 3
#define MIN_SPEECH_FRAMES 5
#define QUIET_FRAMES_TO_END 4
#define MAX_EVENT_FRAMES 150


// ==================================================
// TDOA SETTINGS
// ==================================================

#define MAX_LAG 20
#define CENTRE_LAG_ZONE 1
#define MIN_CORRELATION 0.35f
#define STRONG_CORRELATION 0.70f
#define MIN_TDOA_VOTES 3


// ==================================================
// EVENT VARIABLES
// ==================================================

bool soundEventActive = false;

int loudStartFrames = 0;
int quietFrames = 0;
int eventFrames = 0;
int speechFrames = 0;

unsigned long lastDetectionTime = 0;


// ==================================================
// TDOA VOTING
// ==================================================

int leftTDOAVotes = 0;
int rightTDOAVotes = 0;
int centreTDOAVotes = 0;

int validTDOAFrames = 0;


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


  // DC offset
  for (
    int i = 0;
    i < sampleCount;
    i += 2
  ) {

    leftTotal +=
      (samples[i] >> 8);

    rightTotal +=
      (samples[i + 1] >> 8);
  }


  double leftOffset =
    leftTotal / frameCount;

  double rightOffset =
    rightTotal / frameCount;


  // AC RMS
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


  // Centre mic is on RIGHT I2S channel.
  for (
    int i = 1;
    i < sampleCount;
    i += 2
  ) {

    total +=
      (samples[i] >> 8);
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


  double leftMean = 0;
  double rightMean = 0;


  for (int i = 0; i < frameCount;i++) {

    leftMean +=
      (samples[i * 2] >> 8);

    rightMean +=
      (samples[i * 2 + 1] >> 8);
  }


  leftMean /= frameCount;
  rightMean /= frameCount;


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
        leftSample * rightSample;

      leftEnergy +=
        leftSample * leftSample;

      rightEnergy +=
        rightSample * rightSample;
    }


    if (
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


  lagResult = highestLag;
  correlationResult = highestCorrelation;


  return (
    highestCorrelation >=
    MIN_CORRELATION
  );
}


// ==================================================
// READ ALL THREE MICROPHONES
// ==================================================

bool readMicrophones(
  float &leftRMS,
  float &rightRMS,
  float &centreRMS,
  int &sampleCountLR
) {

  size_t bytesReadLR = 0;
  size_t bytesReadCentre = 0;


  // Left + Right
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


  // Centre
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
// RESET SOUND EVENT
// ==================================================

void resetSoundEvent() {

  soundEventActive = false;

  loudStartFrames = 0;
  quietFrames = 0;
  eventFrames = 0;
  speechFrames = 0;

  leftTDOAVotes = 0;
  rightTDOAVotes = 0;
  centreTDOAVotes = 0;

  validTDOAFrames = 0;
}


void publishDirectionTelemetry(const char* direction) {
  if (!isRunning || !mqtt.connected()) {
    return;
  }

  StaticJsonDocument<384> doc;
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

  char buf[384];
  size_t len = serializeJson(doc, buf);
  if (mqtt.publish(topicRaw().c_str(), (const uint8_t*)buf, len, false)) {
    Serial.println("Published direction telemetry");
  } else {
    Serial.println("Failed to publish direction telemetry");
  }
}


// ==================================================
// PROCESS COMPLETED EVENT
// ==================================================
//
// Serial output is intentionally limited to the
// confirmed Left/Right result.
// ==================================================

void processSoundEvent() {

  // Reject short events/noise.
  if (
    speechFrames <
    MIN_SPEECH_FRAMES
  ) {

    return;
  }


  // Need multiple timing measurements.
  if (
    validTDOAFrames <
    MIN_TDOA_VOTES
  ) {

    return;
  }


  const char* direction = "UNCERTAIN";

  if (
    leftTDOAVotes > rightTDOAVotes &&
    leftTDOAVotes > centreTDOAVotes
  ) {
    direction = "LEFT";
    Serial.println("SOUND DETECTED ON LEFT");
  } else if (
    rightTDOAVotes > leftTDOAVotes &&
    rightTDOAVotes > centreTDOAVotes
  ) {
    direction = "RIGHT";
    Serial.println("SOUND DETECTED ON RIGHT");
  } else if (
    centreTDOAVotes > leftTDOAVotes &&
    centreTDOAVotes > rightTDOAVotes
  ) {
    direction = "FRONT";
  }

  publishDirectionTelemetry(direction);
}


// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(500);


  // Left + Right microphone bus
  setupLeftRightMicrophones();


  // Centre microphone bus
  setupCentreMicrophoneRX();

  connectWiFi();
  xTaskCreate(mqttTask, "MqttSshTask", 24576, NULL, 5, NULL);


  delay(200);
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

    delay(20);
    return;
  }


  // ==================================================
  // NORMALISE LEVELS
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


  // Strongest of all three microphones for event start.
  float strongestLevel =
    max(
      leftLevel,
      max(
        rightLevel,
        centreLevel
      )
    );


  // Left/Right pair for TDOA.
  float strongestSide =
    max(
      leftLevel,
      rightLevel
    );


  // ==================================================
  // POSSIBLE SPEECH START
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

      leftTDOAVotes = 0;
      rightTDOAVotes = 0;
      centreTDOAVotes = 0;

      validTDOAFrames = 0;
    }
  }


  // ==================================================
  // ACTIVE SPEECH EVENT
  // ==================================================

  if (soundEventActive) {

    eventFrames++;


    // Count loud frames.
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

      quietFrames = 0;
    }


    // ==================================================
    // TDOA
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


        int voteWeight =
          (currentCorrelation >=
           STRONG_CORRELATION)
            ? 2
            : 1;


        // Positive lag = LEFT
        if (
          currentLag >
          CENTRE_LAG_ZONE
        ) {

          leftTDOAVotes +=
            voteWeight;
        }

        // Negative lag = RIGHT
        else if (
          currentLag <
          -CENTRE_LAG_ZONE
        ) {

          rightTDOAVotes +=
            voteWeight;
        }

        // Around zero = front/uncertain
        else {

          centreTDOAVotes +=
            voteWeight;
        }
      }
    }


    // ==================================================
    // END EVENT
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
  mqtt.setBufferSize(512);
  connectMQTT();

  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }
    mqtt.loop();
    handleSerialCommand();
    processMicrophone();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}


void loop() {
  // Audio processing, serial playback commands, and MQTT run in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
