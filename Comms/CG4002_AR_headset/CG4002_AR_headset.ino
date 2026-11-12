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

// Inference results from tcn_handler.py are spoken on the speaker.
const char* TOPIC_INFERENCE = "tele/glove_pair/result";

// tcn_handler.py already merges window predictions into words and won't publish
// the same word twice within one window, so every RESULT is a real word to speak.
// Playback turns the centre mic off, so leave a short gap between phrases for it.
const unsigned long MIN_SPEECH_GAP_MS = 400;     // after one phrase ends, before the next starts
const unsigned long MAX_SIGN_AGE_MS = 5000;      // words waiting longer than this are stale, skip them

// Filled in onMessage, played from mqttTask. Playback blocks for about a second,
// so words that arrive meanwhile wait here in order instead of overwriting each other.
// onMessage and speakPendingInference both run in mqttTask, so no locking is needed.
const int SIGN_QUEUE_LEN = 6;
char signQueue[SIGN_QUEUE_LEN][32];
unsigned long signQueueMs[SIGN_QUEUE_LEN];
int signQueueHead = 0, signQueueCount = 0;
unsigned long lastSpokenMs = 0;

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

// The mic only publishes when it hears speech, so without this the coordinator's
// watchdog would mark it STALE during silence. Not retained, so the stored
// HELLO / Last Will on this topic stays as it is.
#define HEARTBEAT_MS 5000

void publishHeartbeat() {
  static unsigned long lastHeartbeatMs = 0;
  if (!isRunning || millis() - lastHeartbeatMs < HEARTBEAT_MS) return;
  lastHeartbeatMs = millis();

  StaticJsonDocument<192> doc;
  doc["type"] = "HEARTBEAT";
  doc["device_id"] = DEVICE_ID;
  doc["session_id"] = sessionId;
  doc["seq"] = seq++;
  doc["timestamp_ms"] = millis();

  char buf[192];
  serializeJson(doc, buf);
  mqtt.publish(topicStatus().c_str(), buf, false);
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

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("Failed to parse incoming MQTT message");
    return;
  }

  if (doc["type"] == "CONFIG") {
    publishConfigAck();
  } else if (doc["type"] == "RESULT" && String(TOPIC_INFERENCE) == topic) {
    const char* sign = doc["payload"]["event"] | "";
    if (sign[0] == '\0') return;
    if (signQueueCount == SIGN_QUEUE_LEN) {
      // Full: drop the oldest word rather than the newest.
      signQueueHead = (signQueueHead + 1) % SIGN_QUEUE_LEN;
      signQueueCount--;
    }
    int slot = (signQueueHead + signQueueCount) % SIGN_QUEUE_LEN;
    strncpy(signQueue[slot], sign, sizeof(signQueue[slot]) - 1);
    signQueue[slot][sizeof(signQueue[slot]) - 1] = '\0';
    signQueueMs[slot] = millis();
    signQueueCount++;
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
      mqtt.subscribe(TOPIC_INFERENCE);
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


// Generated spoken phrases: 8 kHz mono G.711 mu-law, Base64 encoded.
// Spoken phrase: Hello
#define HELLO_AUDIO_SAMPLES 8007
const char HELLO_AUDIO_BASE64[] PROGMEM =
"/////////37////////////////+fv5+///////////////+/v/+//////7///////////7//37//////////////v7///9+////"
"///+fv///37///7////+////fv///v/+fv5+/n7+ff5+/n7+fv71c/ro6e9y/XVm8HFbfO3x4OT2ZW7l8X7renXubGFmfub0+Nv4"
"a+5obPts/Pvv5mXw5Pp57+3+8WtvW/LmYv1qeebs5t3g5mpbbWpm/m3q4f1zce7m7XZ6e+9vZmr07eTlXmzf5v3+9mn+61pY+9b7"
"+PNv7HzodPvkfup5cWlZb+f79O147up8b33c33PjXU5hfHLy1d/c3m1mWvbvfPtZbXzweN7f39fpXlntVmNl/Nv07Od73+lg7v59"
"/mZc5eXrV2PfZ+7W123vdVfp6nFmWe5v3t1x9N9eU+nx3u3853nneE5r9+7f2vhze3VeXt5fdPLPfHzkau9ddGfj1eNWavvd4Vxk"
"Xm7X3F/3YN9qUtZdfM7e19ppW1Jh61Pfz/XYXE3l5PZbXdr3a/F0ZM/hbt5q4e517/RbXV3k2mpfTNrYz2ZV421cXHDkxNv88lxU"
"Z1vNz9PiTmjrYk9za3rkZVdN787z0mHr78zc3efky9fJTElUaTc4Pz98z7Kwu73qTlp9WV1aUGtMSHzr2H1T93rIcF/gztLZzO9y"
"Y/FNaXhOWVpdTmjm6NjZ2e3Oz9364v3n3l1TR/DTW17VUlJOX1n103rb0sBd4t/NwMn1RkNJTkJuY9jaelnv39XdZs3y9P11dvbl"
"4/RyfuftaldW8dDf6+3Iv73STPPb4UM4Olq6vl++sKSptl1AuUoyJCcyS/41QMO2t85YWdzNQDs8TWxnVE7n0NX8Zmfw9WtdZn3v"
"/297+e93dvrf3+9t+ebqYU9T3szh5N68urj+StnRazcwN9euye6vppyoszjzuTInGyUub0ctULWus+JPVcviNDY4U2VjSlLTyth1"
"X23ufF5aa3x8dXv1+/zv3Njtb/bd6VxIS97D3uHZtLCu1D/QyecvKSz9qLZeqZ6Tm6g8PasuIBIZI0VWJTmwp6nGRkjPyC4tLUFi"
"Y0hE08PK/Ftc7v9eUVxn/X5+/Hvo3dLe+mze3XpKQ1HExt/hwKysrEdOxsNGJyQttqHPw5+Wj52rKLy4JBoPHCXnNCRjq6ita0ZM"
"wVwsLi9TXF5CVMzG1mtbae5vWlZka/R19XTw5tjX6HL12+RfSkvtw9jc37ixrsFE2cnQOi0uUa2uZK6lmJyq2zKuPSkbHCc4biw2"
"yLKwv09P/8k/ODY8XWFSR2/bz9x3X/3xe1xaXHD3fnr43NrdfXF95XlaU3rcytPc0sC7z9xLW+9jPzhL57+30ryvqqy6y0XHUDsv"
"KzE6UztCWdy+wdLm2c/e9khJTldZUU9Uc+/q+3p86ejpfnVw+vL3b2Rlbvj9dHTr2s/S19zS09j2UVJRWFJNTFfVz8XGx7y7ucfK"
"/XftTEk7PT1GRkVNTurY09XX0c7M2utvZm5kW1RVWWVsamx37ufp9nx9+3xtZWl77urq6tvU0dXe6Ojgcl9NSk9SVE1SVdrJyb/I"
"vLq2vsjUWtlOSTw2Oj1IP0pHYdLQy9XQzsjO4f5aZWJcU09RW2psbW/35+Pr+n3+fm9lZG327e7t4dPOz9nd5tnfZVdHTE5VS0lP"
"V8fKwcDDt7ayxMJpcd1DRzI1N0BCP0pE2s7Ix9XOzMbT4V1UYFpaTk1QXm51c3Pu4+Hq+v//emphY2328/nu39DMzdrY3tPXYlRF"
"S01VRkNNT8PIwb/CtLOuxL5iZ9E+Ry4xMz5BPko/1czDwNXMzMXP3VpOWlNcTktOWWz8+Xvx5+Lj7Pd5bmViZ29+eHTu287MzdzS"
"1c7RXk9FTUtVQD1NS7/HwL6/sK+rxbtXaMw6SCsuMDxAPkk9zc29vdTKz8TM2VVMT05fT0xLUmfs6fD++ebe4PJoXmBt+XZsY/vZ"
"y8zM2tTKzMtpT0ROS05ENUxFx77JvMOur6a+u9xEuzhNLCkvNEY8TTjeyr600sfUysTUZUZISFxVTUlLX+Xc53B059jc/1tYaPLx"
"ZVtn3MjKzNnZxcbFe1BAUFBISi9DRNi5zrzIra6iscK9N7Y9QzEiLixIPE04VsTDrcnG0Na+0e5GQkJVWE1KRlrw2eD9/uXZ3ndd"
"W2l4bFtbc9fIzczXysG+yfdRQ/lHUzwvSkHAv8fCvaysn8a0cT6yL1UoIy4vSz5JONfDua7cxufgx1lLOjk+SElISE7308zP1dXR"
"1N9lUk5PUExJS13Yx8bExLy0sby/V13RRtYyL0A/uby+0bOto5vkqztGrCpqHx4tLlE/QzXJu7asWuBTSuNAOTE0O01aVmFw3snL"
"0Nrm5OLvemlnbmRXT0xTb3D7+vjEu7a3wc7LvtzNOi9FOL/E7MjEq6Wct7PVM643PCocJi5NR9s8VrfCrcNNRTo/QUM1Oj5M1c3N"
"0NjY1d1+YFdZYn7f1dHa7mBXWVFKRUNNzMC4tcW4trO4zjotPjTGxUDE0qmdmKWxuS6xTS4qGBskTkTG+Dm6vLG0fDcuNTFFQTpI"
"UtW/vMPO321oXU5LS1J81c3N0d7k6WJRRD1FWtvMwtPBs7atvVE5Oj5TvT1Q1sifm5ytrVFEtTMvIBocLetatmxJxr+6x3UvLjU1"
"U2ReXenSy8HN52taWFhYVlx04dbV1tXV3XVUQ0lVWOpkXeW4r6yp11BBPVrN4i1LT7uYlpmlqURTuSwmGhYXLcbSrcpHXMa/398u"
"KC42VM/A3Ojf8tjW4mZbWFdfaXL+8Onf2tzg/F1gd+xxbUc+Xc61razFX2hA3MLbNTJCRaKYmZ2luDy/SCYfGRkd5by2r91CQMnY"
"eVwwKy9J5L+60GdTVl/m2e1wY11gbHv56N7c4vJpVmbc0tHqTDY9aMStqK7ebllGvr1gLS84SpyVlpuhtzq8PyIcFhUa3LiwrNM+"
"PMrY7GwvKSw/fr62yfxUT1b22+b7bGFhaXP+8+nj5fB1Y13719LZbEg5ROa/rqy2b2xbUb3DTy00O3admZqfpsU+vDolHhoYH868"
"sa7bPz/N6d5xMywvQmfAusvvVk9TfN/i6P5rZmpvfe/q5Ob5aF9da9TMz+9NOjlpxq+qrctP7krLuOEyKjo6p5WWmaCqP9PXJh8Y"
"FhY1uLepuk83Zs53zj8sKjNK1La7z2VNSlfm3N3ocF9eY2z46eHf6XVfXl/uzszWY0c4P96/r621/FT6UMC+XS8uPUWimZqdpbM9"
"xFMnIBoZG0u7tqvBSzr43u7OPy8tOEvPt77PZk1JVeze2uB6Yl1faPfi3dzlcltaX/rMxs5uRzk2Y8Kxqq7FSO1c2rfROCc0OLyX"
"lpiepN9Fuy4hGhcVH8K6qqvRPDzaXs/dNiwsOUvBtb7PYUlGVP3i2NvtbGFeZHvq4OTscV9cZH7dy8vbXkc8PPXBtq61y0vm6Na5"
"1DooMzzHm5mboajPQLw5Jx8aGB7gva6qwkY5X1Hvz0EzMDlE1bu/yOdVSU1caund3+b9Z19keOjd4PRpX1xh99vTycvnT0E7O+G8"
"tK61zEjWy8211DUjLTzNmJWXnqbBOrk+Jh4XFRlVvKujtk81TkZdyUs2LzU7bLu8wM9pSkhPVWvk2tne72NcYXjm3+H/ZV1cZPvb"
"1c7L3F5IQD5OxLy1t8NfVcTPvr1JLCc7RaqYmp2nrkZ3xC4nHRoYKce8p6zHPj5OQ9XdRTc1O0HRvr/G2l1KTFJb/t/Z2+V4Xlti"
"/N7Y3ftgV1Zd/dbOyMfdVEA9PFW9t7G1xVNWvsu6vT8nJDpHo5SYm6SsPt3IKiMaFxUpwrejqcY6PEpAzNlENDE2PdC8vMDVWERH"
"TVrr1tHV5GtXVV743dfc+GBWVFp8183IyutOPz4//bu4tLvPTWPBzrrHPSkpPk+jmJueqLA/zuArJRwaGTHDuKatzTxASkbL20g4"
"NTg+0r69wNVYRUdNXOPSz9bqZFVVXvLYz9TpYFBNUm7Sx8LF6Eg7Oj1ruLOxuM1NT73Gur0+JyI4Q6iVmJqjqUxcvyskGxgVIcq8"
"pKW6QzhGO97JVjsyMzVev7u6w/RHQERMcdXMzNd8V05QYufV0dr6Wk9PWPnQyMXJ70o9PUJmu7e3vNFWTr/Cv71ILCQ3RrSZmpyl"
"qudHvTUpHxwZHnjDq6W0YzlFO1fI/kk5ODVIzcC7v9JVRkRHWenSzc/fZVROVW/bzc7Yc1FLS1jsysHBx/9EOTpDbreyt7zXVEu9"
"ur25RikfL0W4l5eaoKfKPro4Jh4ZFhpNw6qgrNw3PzZFx9xROjcwO9zCubnFZkU/P0t80snJ0H5TS0xa6NHN0ehaTEpPZtbFwsfX"
"Tz47QlbJs7e7ynRKYrvEv885KCdCXaiZnJ6orFJryi0oHxwZJe7Iqai3XUBHO+/I5VE9NzFA6ce7u8lnSkJDT/HPycvWb1JMTl7f"
"zcnO6FZIRk1sz8C+x99JOjhCbMOvsbvMaElLvb/HzDwqJD14r5iZnKOoyka/MycfGxgcQPKvpK7JSEs6TcTR8kc5LzNK6r64vM5e"
"Rj5DU+jMxsnZaU5JTV7i0MzR6V1NSk5o2cnDyN9YQjw/X869s7nI91tMWL/G1Hs6LCpM0q6cnaCprM5LxjksJh8cHzxWuKmvwWRX"
"PkzN19piRDY2QE7Uv7/G2V5KR0tZ69LMzttwVU1OXO7TzM7balFLTFjuzsfFy/JOQT5BXca8tbXE9VJYTtG7128+LigxzLqfmZ2i"
"q69O5t0uKyEdGSM8VK6psMLqTztn0tjPckQ1NztG3sXAwsx9T0lIT2bez8zP4WZSTE9d7NfP0t90WE9RW/vYzcvQ4llJREdT27+8"
"ur7YW09dWM/C+1E8Mi1CwLahnqOqr7tM01oxLiYgHiw8XrKvt8bcT0Tq29PN70k8PDxG/c/GxcznXE9LT1/s1s/S3m9YT1Bb/djN"
"zdbwXE5NVGzczMjN3F9IQURQ5r+4ubrKYklOV2HG1lJBNi8zzripnZ+krK/YWdE2LSghHSEzPMavtLvL6Uhg0NPGzWM/Ojc5Su/M"
"wsPN+VVKSVFs3c7N0+peT0xPYefVztLfb1dPUF3y1szN1/9SR0ZOds28ur3E4VFHUlNtz3VOQTw3RcG5qqKmq7G1e/t7NTArJiMq"
"OUHHub3BzvBR6dTRxs9pRz46PEpq1MjHz+tdTk1TaOLTz9TmZlRPUV3w1c3O1vZaT05UbtzOy87nXEpER1zYw7m5vsp5S0NQVWfS"
"aEk+PDhIvbSpoKSpr7PjbOk2MCslICcyOtS5u73G3VT52dbEyeFQQjo5QE72zMbJ1PNXTU1UaOHSz9PjaVVOT1v+3NTS2exkV1NX"
"ZerY0NHbeFhNS09s1Me9vcXTdk9HUWRi2fBNQT49Qcu3r6anrLO3zFrYRDcyKyYnMTdYv76+w9BgYuLpzcnTeVJEPT9IV+fPzM/Z"
"/1xTUlhs59nU2ORuW1NUW3Tf08/V425YUVNd/9vQz9TlX09LS1Xqy8K8vcndXkxGVG5p2XRIPj09R76yq6Klq7O22F7aPTUuKCMm"
"LzVqvr69wtJdc+PjyMfQ+1JBPD5FU+nQzM3W+FxRT1Vl69nT1d91XFNRWGzo2dTX4HxeV1ZdcuLX09bhbldPTlZ008fBv8bZb1NJ"
"S2dn+eBWRkBDQ+m4s6qnq7K5v1/lXzs3LionLjU+0MPCwcrsYOnv2MnM2nRQQz9ESlzl1M/R3P5eVVNYZu/c19nie15XVVpp69nT"
"1Nz2YldUWWrp2dHT3PhdT05RX+TLw7+/yuZeTkhLbG7+5lFCPkJE4bSvqKSpr7i+X+hrOzcuKCQqLzjfw8C+xdti7vziysvS5F5I"
"QUFDTWXj087R3f5dVVJYZu7d19jg/2BXU1dh/eLa2Nvle2JbW2B059vX2OB8X1ZSV2fp08jHx8vdaVdQTlnxbvRvTEJCSU/ItLCp"
"qq+5vc5b21I+Oi8qKS8zQ9TKw8LL7HructnO0NXlX0xJR0lSZezZ1Nbf92NZV1pk++Pc3OL5aFxZWmJ3/n7+fv///////////35+"
"//7//////v///////////v///35+/////v//////fv////5+//9+fv/////////+/////v///37///////7///9+/v/////////+"
"/////37+/v///////////v///n7/fv///////v9+//7+/////////37/////////fn7//v////9+//9+/v5+/v7//37/////////"
"//9+////fn7+/////v///v/////+/v////9+///+/n7///////////////7+////fv///////v///37///////9+///////////+"
"fn7///9+fv9+//////7+fv////9+///////+/////37/fv7//v///v////7///////////7//////////37//v7/////fv///v//"
"/v///v///////v//fv////7///7////+/37///7/fv//fv////9+///////+/////v///////37//37//37//v/+////fv//////"
"/v//////fv///////v9+/37////+///////+///////////////////+///////+//5+fv9+//7//////37//////v/+fv///37/"
"/37//v/+///+/////////37+//////////9+//7////+//////////7///7/////////////fv////7//v////////////////7/"
"///+/////v7//////v///v////7//37//37///7+fv/////+////fv//fv//////fv//////fv9+///////+/n7/fv9+////////"
"///+//7//////v/+/v///////v////////7////+fv/+//////9+fv7+/v/////+///+///+/v5+///+/v//fv//fv9+/////37/"
"/////////v////9+//7///////9+/////////37/////fv9+/////v/+//////9+fn7//37////+////fv//fv///v///////v/+"
"///////////////////////+//9+///+/////////////////37///9+//7/fv//fv///////v///////v/+/v7///////9+/37/"
"//7//v9+/////37///9+/////37//////v7///7+///+//////9+/35+////////fv7///7/////fv/+fv/+////////////////"
"/37//v7///////////9+/v///v///v9+/n7///////////////////7///9+//7/fv///////////////////////////////v//"
"//////9+fv///////37//v7////+fv////7///9+/////////37+///+/////////////////37//37//////n5+/v/+//9+////"
"//7/////////fv//fv///////////37//////v//////////////////////////fv7+/////////37//////////v7//37/fv/+"
"/n7///7//37/////fv///v7////+//7/////fv////7//////v//////fn7+//5+/v/////+/////37///9+//7/////fv//////"
"//9+//////7//37///9+fv/////////////+///+/v//fv//fv5+/n7//37////////+/////37//37///7///////9+/////n7+"
"/v7//37////+//7+//////////7+/////v//fv///////////n7///////////////7/////fv7+//////////7+/35+//9+/37/"
"//////////5+//7///////7//35+//////5+/35+/////37//////////v/////////+/37//v9+////fv////5+/v//////////"
"//9+/v7//v/+/35+fv///v//////fv//////////fv///37///7///////////////////9+/v5+///+////fn5+fv////////7/"
"//7/////////////////fv///////37///9+/////////v////9+/////v//fn7+//7//37///////////7//37//37//v//////"
"///+fv7/////////////fv9+/37/fv////7////+///+//////9+fv////////7///////5+///////+fv//////fv//fv///n7+"
"/v//////////fv/+/////v9+//7///7///5+//////7//37///////9+/v///////v////7//////////////37//////37/fv7+"
"/////v///37+//9+//////7//////37//37//v////////9+/v///////////v//fv//fv/////////////+//////////////7/"
"//5+//7+//9+fv/////////////+/////37/////fv////7////+//7/fv7////+//7///9+/v////9+//7//37/////////////"
"///////////+/v////////5+//////9+//7+/v/////////+/v7+//////9+///////////+///+/37//v///////////v//fv//"
"/n7+////////////fv////9+/////////////37///5+//////////5+fn7///////////9+/////////////v/////////+fv//"
"/////////37+////fv//////////////fv////5+fv//////fn7///////7///7//////////v///////////v//fv/+/v/////+"
"///////////+////fv////7/fv7///////7//////37///////////////9+////////////////fv///37+/////v///////v//"
"//////9+//9+//9+fv9+//7/////////////////fv/+/////37/fv////////7///7////////+/////v///////v7/fv//////"
"/35+fv//fv////7/fv7/fv//fv///////////37+//////9+/37///9+////fv7///9+///////+//7////////+////////////"
"//////7//v///v////9+fv///37///////////////7+////////fv///35+///+//////5+//////7//v////////7/////////"
"///+////fn7/////fv/+///////+/////v/////////+/////v/+////////////////fv/+/v7//37//37////+fv///v/////+"
"fn7/fn7//v7/fn7////+fv9+/35+fv//fv//fv///v///////////////v//////fv//fv/////+/////v//fn7//37/////////"
"///+/v////////7+/v////////7//v5+///+///////////+///+//////5+///+//////5+//////////7+////fv///////37/"
"/v/+//////9+/v/+///////+///+//7//////v///////////////v/+//9+//////7///////////7//37///////////7/////"
"//9+/37/////fv///////////v7/////fv///////v//fv//fv9+//7///7//v/+////////fv//fv/+/////////v////7+//9+"
"////fv/+/v9+fv///////v/////+fv///////37/////////////fv//fv////////9+/v//////fv////7//v///v/+/v///v//"
"//7///7//////////37///////////////////////9+//////7/fv//fv7+/v///////////////37/fv9+////fv/////////+"
"/////////////////v5+///////+/v////////9+/////////////v9+///////+fv////7+/37/fv///35+/37//////37/////"
"fv////7//v/+/37/////////fv//fv///v///35+////fv/+/37///9+//////5+////////fv////7////////////////+/37/"
"//9+//////9+fv//fv///v///////v///v7//////35+/37///9+//7///9+//7//v////9+/v7/////fv9+//////////////7/"
"////////////////////fv////////7//v///////////37//n7+/37/fv9+fv9+//////////7///9+/////37///////7/////"
"///+/37//v//fv/////////////////////+///////////+/////35+//9+//9+/////n7///7///////9+///+/////n7+fv//"
"/////37////////////+fv///v9+//7///7////+/////v////5+/v9+//7////+fv//fv//////fv/+fn5+/////n7////////+"
"/////37//v////7+/v7////+///+//7//37///7//v///v///v////////9+//////7//v////7/";

// Spoken phrase: See you later
#define SEE_YOU_LATER_AUDIO_SAMPLES 10671
const char SEE_YOU_LATER_AUDIO_BASE64[] PROGMEM =
"//////////////////////7///////7///////5+//7//////////v/+/v7//v/+//7///////////7/////////////fv/+////"
"/n7+fv9+/n7+fv5+/n7+//7///9+/n79ffx8/Hv7e/t7/H3++dzj29v32Xzd2+be3/fq7PLc5tzf4+zf5ObY3dbs69/g19Te4tdx"
"1t/l1e7f3PR31m7d3WzaceLgc+He49ze5dlz4+Jo3Ozd1/jg3Obj2NrY6PHh6u/n4dvm/dZ32ub94dzi7O333OvWfuD1ffbz4N7v"
"9OvX2/vS4t3Y+tza6djz0u3f1N/e2Nrl6ujf2tbr4enf/t550tzn7lnaaurVa9t1ZdTq1N/1ZNfTes/f4eX72uvY2/Xb5N3r5t3o"
"9ubv6tfd5eX/5O3l1O3n6u718fLW09raevHs+Nnl5uTf9vT65tv02/Vv4N3g3OLf5t3f3u3UbO3addfy49Tc49j67O3o3XPs5+Lx"
"8vTv13ve2vfp7H7c1Ot7ePHw+tvf79rb3ezd3tLh3W1s227Xcu/12dr24t/Q7vFc5G743uDdbOh80d5w2vrp0l7b3fLN7tXW3trv"
"3u7Z7NZ56tLryn3m/N7Y7e3j3/57+N7Ycs/h7O9k6+by3eL6z2vU79lo1d7b5WXW9c3a2eXU1uXb627S7O5w3u7u5n7O+P7b7OXX"
"5uXz//d93Nvq3vT119fm1e/h4dfYctHr49rZ3eb939x00O/W5ezT/OHs5H7X7eTa8OX8293l19re1erg6m7X9tL2fOr22m7O3dzl"
"6d3e39ff/eXe6Njd3tXu2vLe4N/g4+nq5OTu6GDt32rdb3Tafut36eXq49rl6Nx92/Dd29ro1HLS69nO3OBt6XPYauT45Nb79eVr"
"2OfY7fZj3OLz3HjYYdNz1vp+1Ozf0PHh2e3a5Onk6OTY/M51zdt80Or30Pvo3W/YfvHf2G7d6vPd7/zVdd7oftfv6uDf6uP44d/U"
"2+fa3dzf5+jl6d/X6vLfz23Pa+nbbNlq6+Ts+t3v1+Df6tJm1+3e13TUZ9be7Gzc3tPu7uPo4OXh7HpUYFtDRUpGQ0VNZ99UxLXG"
"ua+vsbW4trRF+MYzNjc0Ly4uOUMyR9FV38nCwcXGwMHf09B3Z2tjYF1re/zv3d/j7evsUktjQz1BQ0A/RVPJU9esvLisqquwsrSq"
"P1W2LDAzLyspKzM9KkfLQtvDvr7Cvbu/3sTLZHvza1xafe5ufdnb6W564Ew/XUA5Oj8+PT5Ou0zPprevqaSnrK+upS3PtiIxLSsl"
"JCgxMCXc7zm/vru+u7W3xsy632jZd1xeX3N69+zg39lxVeNUOEs/NjI6PD07Pq9W6qCvrKafoqitrJ4lwa8cMSooHx8lLSkhwUUy"
"sr+8vLGwuMC6t1rUzFpOdXpaWt7X9fzT0lVG0z4yPzgyLDg5SS7Wqjarnq2joZqjo7iZxCCbIx8zISAaIiMqHEXJJLa0zLSxqre2"
"trHv0L5NW3paUW34dOHO3GvQ0TpdVi8zNDUpLzFgLjifQbuapqCglp6evJeqF5YlGDodHhYhHyUZQ9IerrT5r6yqtq+vsuW+vEhn"
"11ZKbtxoY9LLbFjLZjRbPi8uNTErLjvTJrilO6CeoaGcmZ+kt5IjSJ0TMCcfGhwiIB8fwCgwp1C/rq2yta2xvs614VbS7VBc7+5s"
"d9rUZFHXUjZOOzItNzEvLkvTKayrWJ+hn6Ocm5+qrZYcsbAVPiIgGR8hIR4n3iJPrj63sLC3sq62vr64d9PMZWrmbW/p/GXhe0dg"
"WTY/OzYtNzQ2LkzCLa+p3aKgn6OdnKGpsZgfuq8XPiQiGiAiIB8oUyRIszq6sri4tK66u7u478fJbPbVbGPn5VZp5k1Cdj05PTwy"
"MTs5OTK5Q0qkv7ShpKKonKOkypzBIpwgKDokIB4qISUjRS8qtUtNr8K7u6+8vL25zNe+6f3a21997uhUYvVTP2tAPDw/OTI9P0Ax"
"umU/qLu6pqijrZ+lp9OhsR+eKSZLJyUhLSMpJz42KrpPQbDLw72zwb68v8nTwezk2O9v5G5t7WBV/FNEWENCOUg8N0BSPzi1S12p"
"xrSrqaixo6qtap5KLZ8fN0IqJiswJi4tSS82uTl4te3HvLrQvL/Hz8nLfNbWa/7ca17nb09mZkVPSkk7Rks5P1xUNbnkR63Au6+u"
"qbuprK9To88nnyYyYS0rLTkoMzBFNja9O1a3Ys6+vty9wc3Mycvt0tR7+NtyXfp0T1dmSEtKTj5DWT5Bam85vc1LsL+8tbGrv6yv"
"s1eozCqkKzRtMS0yPCs4NEM4Osc7Wr5Z18HF5r7G0MvIzuTO1vvs3GlefWdOWFxJS0xTPk1cQkrdXUW66O+0xbm8sLLDr7S/aqg8"
"R60pT001Lz45Lz05RjdN4TrW0VTSxdjcv9TRzMza28/g+ePnZWR5Xk9dWU1MVVVBY1tIWtVQZrxgybrHur+xvL6zuM/LrTDDyCz4"
"PzgyRDY2PTxLOWT6P83aedPG1tTF08/Vzt3k2OR26OxlcfpdXGtWUFhPT0ddSUpWaktjxk7JvtS6vba5vLK7veKtPPm9LWU7PDI8"
"OTc+OlpASstN38naz8zH2srWz9/f3fV963Z3+n568H5z8W5eb1pVVkxXRVFMVEZ4+ErF0d25xLe7uLW8ueKtOc3QLnczPjE8NzxC"
"O+5NU8V60cfNys/J28/u3/Fkb2heY2hqb3b6e/t7/Hz8fP19/X3+fv7//v5+/n7+fv///n7+fv7//37+//5+///+/v//fv//////"
"/////////n7//37///7//n7+fv9+/n7/fv7//v9+/n7+ff59/X54d9/t/OHibffvb1N+b0d6YFRP12tVxt7QyMq52r27y8zPtzfF"
"1jZqREo1T0M8UUNsVk7N+uvZyt7uyO7q7uloX2NoW1prcGB44u/q3drm4d3mafhwXkz2T0dvVVdX1Gzfxdy9wtG1x8fVuGpCuTxE"
"S0w7Olw6SVtG7O9w2s7LdcXO6d7Z7l5tZlhWW1pcXW108enc29bZ09vi3+RcaGVURlxYPmdYXFrP1ua5x9KwwcvHt907t0c5RU49"
"MVpEOW5aVN3Pz/69y+TMy+Bs63pVWVdQVlZYWm9+/ejX2NfVz9rW3X31+U5NVV07S+5FT37Ge8m2yrquyb+7skRMtjg3O0wxMEo3"
"POpIT9XA1ny5x9LRzd3//VhOXFFKTVhbWGH63tbb0snH1dbJ1mpzWlFNRz462T88ab/PX7Gvvaqwvq6uuS24zSwvMjwoNTQrSNQ9"
"S82y2cy6x7/J1Fvu5E1ASktLR0lOZfh468/KyM3LyMDPX/jOVEI+TD4/TDBax8JIyKasuKezrairby2qSCwmLi4nOikkTsdHR9+x"
"x7W+2cS8xExOXVJJRD1ASlVNT2LazM7UxLy8yMrOysbyN0FQWS4wPzfN2mNCqpyquZ+onqCoQSqiPygcIiIjPiIbMsPU+m+60Kir"
"x3jOvOBmRTo7S0c9O0JPaf/73Mq+vcXMvra/62FMSdhDMCREUUBeQU3GnZ23sZuimqOrLjujMigaIRwlQCMbMNLFzce86amruXrd"
"yPZvSTo4QkY/PERPX3vgz8S8vcO/trjF3uxMVOw3LChROTtPQU+2nKK4pZqjmqWrK+6rKyUaIxomOiMdNOfEybm74KutstbT219r"
"Vz03Oz9BQkZJUPrPy8jGw7+7usbt791fT0YyLzToPDtGWcixoay/qZ6loK60MvqyNS4gJh4qPy4kMkH9xrq8Zby2tb7E3Fpf8VhH"
"Q0BCR09VXm/f2tDKwMDCys/55dd3Qz05OzxVTjNDVr26rKu+uKOipam40zS53zktJSQhM0EvLDo6/si0wdfBxL++wNpkW2dWWVFM"
"Sk5XYG158+rd187O0drZ2uTfc05CRkpLRlA8PFbQvL+0vMSxpqqruL9RS75MPy8sKCs9RTg9QD94zLrHyMnS0cnL2fFtX1VdXFta"
"Xl9la3v76+Xf39zb3NzX3PZvWVFMWFdLQ0c+Sv7IwMq9w7+xqq2vvsZTZMtJQDItKi8+Pz1JP0j3zcLKw9Pa0NHY3fxgWV5dXWBe"
"YGZseHn9+/nr5+Le5d/e4Nve72JcWVdWYUpLSlV4z7/Jwby9s66xt8TJVNtoQz4zMC87PD5IQUhp7M7Ozt/Z3uLd9mpbXFlfX2Bf"
"Y2Vydvp+/nn69Oji39/e3trX2d7ucl5yX2VfVVdo29HAwMW6vLWztb3Fz17dSkY7NTQ3PDtGPkdOXe/f5/Tsb+19aWBYV1hcXV9f"
"Y2lveH7+fv///37+fv///v////5+///+////////fv////7//////////////////37/fv9+/n5+fn7//////n7//35+fv7/////"
"/////35+/v/+//7//n7///5+/nlvfezk5ezp3djV3ej06vBsW0xOUVdWU1lrzsvDxMW8u7rGyvt9/EpFOTo8RENCS03j1NHT19DO"
"zeF8YF1mXVZQVV1udmxteO7o9G5ncPfv8n7s2tHP1Nrd09znX01PUldNT0tuy8y+xb64s7fCyFfaVkE8MTY6RT1HS17IzcvT2NHP"
"3ltUS1FVTk5OWnfn6e7r4t3jfGFdYmtrZWT92s7OzdXNx87SZlVSX09ISkDezMa8wrewrLm63FbNPT0vLDM8Pz1OS8u/y8fm29Xk"
"VkhFRVROTlFY8Nzb3+bk3d3rb15cYmdgWlhg49TTztfIwMPK519a6EtJQDzV0cC9vbOrqLqzU/nVMzkoKjM9PUFQWLrAycxY3fdW"
"RT4/SltTXl/t0NDU3ufi3+LwbGBfY2ZbUU5Pbuji2d3HvbvGzHXuzUdNOjvR0r6/uLGiqLawPMNPLjIfKzU9PkpO0bLLx2RG4UhG"
"PDlGXXzv5N3JyMzX7ffu7/L+cXFxfXVZTEdHV1tdZO3LuLzDwmu+1ktLMEzTx8S6taicsqjfQbYpMyEeMzRERE/5s7ra2jlaUTpB"
"Nkbt29TT0MXE0OhsbG9mYHDs4evy8PRrUkZJT1piWW/Gu727asvGX+AyOPXXu7+7r5yorLUztDorKBwtPD83WfG5r1X1UkruOjc+"
"U3rZ+d/Ex8vdcnn9Yl1dZvLf2trlcfRvVkk/Qfte+u/Yt7C85ctUxGQpPkTArL62oZiloEtAriUmGhwvQC4vv8KsuzrQ8047MDZY"
"4Enc1cC+z+fc5m5XUlZrcfj14dfW3ftdY2dMQUdM4cpWxry0uc9G/ME2OTY/qLS/rJydoa8rrkYgHhopOjolz669tk5gv1oyMkpC"
"+0pWwcDL2NHd3VxUV2dZZWLp4tje28/T+1tPTF5APEvF3r7Qwqi2T0HcSkEwI6yjvKmflJqeKMaxGxkVIio5H0CfuM7OzMNZLirv"
"RTdQ57/EyOLB0V9WXFhVWFt1efXt2tzV4tna60hOU0pOQWXCtF7BrbvNPjln6CgopaysqKCVmK4ip04ZFR0qJzAis6LhXLi3TToz"
"N903OunA09nG0sp3XllnVVhganF56+zd2NDj2d3tSkxDTl89Wb2vb7uvuL85L2TWJCaiqqWmnpKVrx+iQRUSHCcjLB+rol/6r7ZE"
"ODE59i8627/lz8DLyXRiX11OVV1jZH7v4uTa0dfp4+tYSUpD8UtM17DE17S8vFYvPs08IbylrqOqmJieJ8quHBgZLCMvI2yhzVa4"
"sGg8OzZmOTJ1ytn8v8zH5ntpd1BVXWVibvvq3+HZz9H86H1bQ0g951ZK466237O7uuctM9JOH+6grJ+pmJaZLUWlHBYWKB8qIjqf"
"vku3rNo6PDNVOS1czthmvsbD1Pj/d1ZOV11dZm717ODg29bT6XtuZkpKQ0/cUmTBrs/Cu8HEPi5Iyi8qqKunqKWZmq8kqEgaGSEq"
"JC0lrqxMza68REU7Qk0uP+PeV83BzMnm3vNnT11bW19rdXzt69va2drO6mtaYEdJP0rOZGXBqsXBusW+Pio/xzEnp6ijo6KXmK0h"
"pT8YFx8mICoirK1Hxqq5QkY9QEUqPut7Tcy/yMfd0+Z0TlxWXVZoaPd37Ona3drc1OdlWVpOSElF0uL+z668zL3NxFItOcxGKLCp"
"qaWqm5uoJa7YHBshKyIuJbauSMqtukhQQkRHLj5xX03SxM7H2tDg/ldgV1hcX2dv+vrh4NnX0t/V7WBOVEdJRkPN0OHLrLnIvdXE"
"TCo2yEMnq6ejoqaZmasiqEkZGSEnHyokrrRDvam9RVpCQj0rQmNMSsnEzcbPy9p6V2ZVU1VjXW9w7+/f5tfW2urbdlpMT0tPSUzJ"
"y9nKrr7Iy/PMRyw6xkYvq6ylqKmenbcoqTkdHikoJCwsr9NGt63OU9pOTDsxUUpATM7d2MzPy93o/HlcXl9eZmpxffLp5N3f3dzr"
"ZGlbTUpOUOVP4b271LyxycpSVNs3K026Ntymqp6toZ+eMGiuHx8hLR8qJle5OMmsul3Iz09JNEZKOD9pelTQ1czU2Nrddm14Xmdr"
"anF7+/Tr7ujm9P30YVpYWlNcUt7R+sy8v92+yNZvPlxhNjbEyUOur6asrqilxjCtMyYpMSkpLjTCO1m0vXrKv2peSUpZO0JVVUl9"
"5Ojd3dTd4O7lcn5teG16dfj57/Tx/fpuX2RcWVNkZOlhzcTOzru/5crs5lI4U2c6O7fK6auvo6+tq6VCSbEoKywzJC0sRGQyxbre"
"3rjJb+FXa0g8UkpASHVUZ+/d3N/a2d3q4On18/V8/HpydW5iZGRbXF5pYnb20dzbxcfWz8Ps6l9abj5D9Fo+y7n7s7essbe1rNA5"
"szYvNDotLzQ6bTXowO/rvsb61PF7WUVaTkVKYFFYefPr6N3d3uLf5+rv7X38fXdpb2RhXGBZW1xuaP7rztXXw8TQ0MLr51pZbD0/"
"8Vo8zbb3sbSqrbaxq8AzsTosMTcsKzQ0cDRjver1vcB+1OZuXUFYTkNGYFFRfPbs8d3c3ufe5uzw7fz3efl1em14aWhkaF9fYnFv"
"du3V1+PJydHbxeToZ1n4Rj9ybz9ut+i9tbKtvLWyszW/9C03ODYsOjNkP0W/1mzJv+zi3GdvSFBaSUlbXlBy++/25OHj7ubq+Pby"
"+3r2evx+enz8cXV6a2doaF5fZ3dp+uPO59DGydzKyfTiU/9bPUnnTj++vuOvt6qztbWsej2zMDEzOSwxMjpeNWPJ52rGyePZ7ufv"
"TmFgVk1eW1pdYWpnbGp3bHJyeHT/ffz8+v79e356///9+PPz8O7v7unu6unk5+bm4+f87+58XnJvZl1l+uFw4MjD1by7vbvEvsHC"
"Q8x8PUJARDtAOk1MR05raGNt+e/zd/vu/nxz/XF2bXNudG91dHZ2eHd6enp9//7+/f/8fX19fn3+/fv5+ff39/b09PPz8PHx8fTz"
"9fT6+Pbw9fPu6ejn493Y2dvTz9fS19jb3/x7+WFeW1xZW1laX19gZGlrbm5wcXV0dnZ4d3h2d3Z3d3d4eHl5enp7e3t7fX19fn5+"
"///////+/v38/Pv7+vr6+vn5+Pj29vb29vb29vb19PPx7+3s6+jl4uDf3dvc3Nze3+Pufv5uZF9eXFtbWlxeX2BjZ2lqbG1vcXJz"
"dHV1dHR0dHR0dXV3d3h4eHl5ent8fX5+fv///37+//39/Pv7+vn4+Pj39/b08/Py8/Lz8/Pz8fDv7uvp5+bj39zb29jU1tfW2tzc"
"6XX1b1xcWlVWVlNWWlpbXmBjZWdpbG1ub3BxcXFwcHBwcXJzdHV1dnd3eHh6e3x9fn7//////v79/Pr5+Pf39vb19PLy8O/v7+/v"
"7+/v7u3s6+fk4eDe29jV1tTO0NLP1NrU323uZ1RMS0lNT0hr7efa4+bp9GpdXWJtevPkc2npaWZyefBp9d7qcGBt/Xn+XeffcO/s"
"8l5tcfXtaevr5fl4emx6Z+zs9vDy9Ph3dnPx5ePd5XlmWlNOSkti09DHwbu5vtXufVREOTs/SUxXfc/JztPf3fZtYF1ud3f58Xt2"
"bnhufHv68fvyfPdidHF4+e548ep4/mp2eXt47+nz9Pl6/W10/vvw7evv6+3o7HZu8utYR053bFJnybuyrrG5qqiyej++ezEnKjgw"
"LjE3Q9TW32vIus7xZ+X6VU5OTlVaWV1mev59/310cm1sbG1ucHZ6/37++vTv7uzr6+7v9nlycG9v7d/e3tTIyczO2t/R61E5Q99P"
"QT3KtLSxr7WnoKzSMrvHLyMhMi8wNDM8v7rEX8+3zfRLTUxLTUZDT2ptbHXo5+nt93Jvamptb3Z98/T1/X59dXFxfvjx7+rq6+71"
"cWttaW/9cHTmyMbRz8PAwNVoRj7OSzUvYbC4vrGvp5ynvSzGtDEiHSwuN0I0NLuutG5ovvL2RDs5Q19SR1vh2t3n5/jx/m5kZGds"
"b3V98+np5/D4/3pxaWZpbP389ezp4OHg83v2/V5ZT1hg0tR81LSyutHZ103DOioq1qzH5K6rnpmo1iWtuiscGiorSlkvOK+pr0rs"
"5E5jPDYwRvVfTvfaz9jc9215cmJdXmRvcnxw9Ork5vV8fv19a2Robfn2+u/s39/f7vT28l9dUVRk2dVv2bi0vMnn1k7FRissX67B"
"37Sto5qmwCi+rzEgGiotPl03MrurrWJX01VnQDszPmj+T2Phz9fe6HRzc2xfYGJzcH1z+u7o5e56e/v7bWNobXv1/PDx4t7g6/Py"
"92leWE1g5c746b6yub3c3VbdzywqNrWz0b+tq5qcrTgvp2koGR4tL0tILkuuqbdA2/pxSjs2NUf2XlB32M3e3/V8bnJmYV9nbXJ1"
"fvXv6ens/f7/fG1ra295fvf37Ovi5ufw7/h5ZWFRU/7X123Oure8wv77VcZPKi5Psb3Oua+omqK6LFmrPyUaJi83TD8w4a+qxkTY"
"8PJCPDc6S/NaVnPU0ODl+f9tbmZpZGtodXf++fju6+7zfX7/cW1rcHd5/vTw6Ori6Obv7Xp2Y15OV+bU3PvHube7wl9mYMI+KTDv"
"sL/FtK+jmaXIKsitOB8ZKC84Rzozza6p10fe3+0+Ojc8TH5YWmvS0t/s+n1tbGdnY2trdnL9+fbx6u70ePt+cWxscHh6+ffx6+jh"
"6Onx73p6ZFtPbd/Y6tzCu7u80FVi3soxLDvEtca+tK+enK1MLrC/Lx0dLjM9Rjc+v66uVFTk1l09OzxBVm5ZX3vR2uT2+XRza2lj"
"aGxudnr5+O7r7PT8ff52bmlrc3p99fTs7OLl5/DvfX1qXlBe4dve6Ma9urvEWF78xT4rNvW0v8K3sqWapsgs1K49IxooMzlDPTXb"
"tKvJRvHb3UE7Oz5Ka1tcZNnS3+79/HFuaGhlbWt0dP749fHt7/H+/n5wb250dHZ89vLs7eno6Orz+3x3YlhW69/c7NPCvby/+FZr"
"0eYvMkS+usa+t7Cfn7RDNLHMMR8gMjg/RjlEwbGxV1rq1F8+PD5EWGpcY3nU2+X7+nVxaWtqbW5xdXN++fH3+PXv9/h4dm9ze3d2"
"efnx7/bv8Ojr6vP1fvxqXVRm4t/g48i/vL3JVWD5yz8tOn23v8K6tqmdqckv5rFEKB0qNjtDPjjmuq3GTPvd2Ec9PUBKY11jY9/X"
"3+59/3VwaWtpcW91bnJ59fP2/PPv8vh+/n7+fv5+/n7+//7//v/////+/v9+/37////////+////////////fv//////fv/+/v//"
"/v//fv9+fv/////+fv///////////////v///////v//fv////////9+/v////////7/fv///n7+/37////+////fv///n7///7/"
"//7///7////+/v/+///////+///////////+fv/////+//9+/////37////+///+fv7+/v///////37////+//////9+/////37/"
"/37+/n7///////9+//7//////37//////v5+fv///////////37+/37//////37+fv///v7///5+fv9+//7+/////37/fv///37/"
"/v9+/////////v////////5+////fv//fv///v/////////////////+////////////fv///37//37/fv/+fv7//////////37/"
"//5+///+/////////v//////////////fv////////5+//////////////7/fv//fv////7//35+fv//fv////////7//37+////"
"//7/////fv/////////+///+//////////7///////9+/35+////fv///37//v///35+//9+/37//////////37///9+/v5+/37/"
"//5+///+////////fv////5+fv/+/////37+/////37////+/37//////////////////////////v///////////v9+//////9+"
"fv///v///////v///v///////////v/+/////37+/////v9+/v///v9+//5+/////v///////v///37+/////35+//////9+fn7/"
"/////v///v//////fn7///////////////////7+////////fv///////v///v///////////v////////7///7//n7//v/+////"
"/////37//////v///v//fv//fv9+///+///+/////////v7//v//fv///////v/////////+//5+///+/n5+///+/35+fv//////"
"/////////v9+/////n7///9+/////v9+//////7+/v7///////////////7////////////+fv///37////+//7/////fv////7/"
"/v9+//////////9+////fv///v//////fv//fv//fv7///7/////////////fv7//////v////7///9+////fv7/////fv//fv//"
"/////v9+fv////////////////9+/////37+/////////v//////fv/////////////+////fv//fv////9+//7///9+////////"
"/37//////v////////7//v/+/n7/////fv////7//////v///////////n5+//////9+//////////////7//v/+/v////9+/v//"
"//////9+/v///////////////////v//////fv///v///////////////35+/n7//////////37+/v//fv7/////////////////"
"////fv/+fv/////+////fv7+/v////7///9+//////7///7////////+///+///////////+////fv/+/37////////////+/v//"
"////////////fv//////////fv7///////7///7//////37///7//v//////fn7///////////////////////////7//////v//"
"//9+//////7//////37/fv9+//9+/v/+/n7+fv///v/+///////+//9+/35+fv///37///9+///+/35+//9+//////7//n7/////"
"/////v/////////////+//9+///+//7//37//v9+/37////+fn7//37/fv7//35+/////v////5+/37////////+////////////"
"///+//////////7//////v9+//9+/n7///7//37//////v9+//7/////fv//fv/+//9+///+/35+fv////9+//9+/////37/////"
"//////7//v7//////37///9+///+fv//fv////////5+///////////+fv///37+/////v///37/fv////////7//37//v9+fv//"
"//9+//9+/v/////+//////////5+////////////fv7+fv7/fv///v7////+//////7///////9+//7//////n7/////fv7/fv7/"
"/////v//fv///////////v//fv/////////+fv9+//////7///////////////////9+///+/37+///+/v/+////////////////"
"/37////+//////9+///+//7///////9+///////+/v7/////fv/+//////9+/////37//v///37+/37///////7//37/fv9+//9+"
"//7+/v///37/////////////fv///v///37/fv////7//v//fn7//////////v///////v9+//7/fv///////////35+////////"
"fv////7/////fv9+//////7////+//////9+//7/fv9+//7/fv9+fv//fv/+//7+///////+/////v///37//////v9+////////"
"//5+////////fv////5+/37////////+//7////////+///////+////////fv////////////7+fv////////5+/v///////37/"
"/v////7//v7//////n7///7/fv//////////fv////5+/////v///37////+////////////fv9+///+/////v////////////9+"
"/37///7/fv///////////////37//37///7////+//9+//7/fv///////n7//37//v////7//37//v7//v7//////37//37//v//"
"///+////fv//fv7+///////////+fv9+fv////7//////37//v////7/////fv7//////v///37+///+fv///37////+/v9+////"
"///////////////////////+/35+/////37/fv/////+/////v///n7///7//////35+///+/////v///37//////////37/////"
"fv////9+//9+////fv////7//////v/////+/v7//v///////////////////37+fn7/////fn7//v/+/v///v//////////////"
"///+////fv/////////+/35+fn7+////fv9+//9+/////37///////5+///+//9+///////+fn5+/////37//37/fv7/////////"
"///////////+/37/fv9+//////////////7///9+//////9+/////////v////////9+/37/fv////7//37//v9+/////v7///7/"
"/37////+/////v////9+/////35+fv////5+//7/fv//////fv/+fv9+//7//35+/v/////+/37///7/////fv7/////fv///37/"
"/////37////+/37///9+/////37//37///7+//7/////fv/////////+//7////+/n5+/////v////7+//////9+/37///9+///+"
"fv7//////v////////7//37//////v//////////fv9+/v////////////9+///+fv///37//v9+//5+//5+//////////7///9+"
"//9+///+/37/fv////7///7+//9+/v///////////v9+/37///////7////+//7+/////v////9+/////////37//////37+////"
"//7/fv////7+/v9+///+////fv///////////v////9+fv//////fv//fv5+////////fv////////////////9+///////+/35+"
"//9+//7+//7///////////9+////fv/+////fv////5+/37///////7//////////v////9+//7//37/////////fv////9+/37+"
"//9+/////v//////////fv///////v/////+fv///v///////37/////fv7+//////7//v//fv//fv////////7////+///+//9+"
"/////37+//9+fv///v9+//9+//5+//5+fv/+/////////////////////v////9+//7+fv///////////////v//fv//////////"
"fv7/fv////////////7//37//////////35+//7//37+/v////7//////v//fv///37/fv////7////+/////////v7/////////"
"/37/////fv////////7//v//////////fv///37//35+/////////v///v///37//37///9+/v7+//////9+////fv///////n7/"
"fv/////////+/37//v/+/v////////////9+fv////////////////7//////v9+//////7/fn7+//////5+/////////37+////"
"///////////+////fv9+//7/////";

// Spoken phrase: Yes
#define YES_AUDIO_SAMPLES 7255
const char YES_AUDIO_BASE64[] PROGMEM =
"/Pr49+3w7ern7ebo7/jraWxoWV5PWE9VTV1hUNjj9sHQv8K/vMW97bhJ39g5bTtGO0I/Q0xC9FZbzv3Zz9XP2M/d1uve63H2dGps"
"b2lubnNvfHd9fvv5+u/z7u7p9uvvfW/xW2VdWFVRWU5dTedcX8f1z7/IvcO5xbrVwsI8uz5GTztEOUM9Rz9ObEPe4V3N5c/b09TW"
"393afOftfHL2b3lzfXb+dfZ5/fv6/Pn593129m1jb2ZTYVhVT2NOaFVu0k3Fy9++v73DuL+9wOKyO87PMug6Qjs6Qz0+QWc/UdRK"
"2ebf1OnO19/X1ezg3uv16vH5/PT9/H36/3r8fH10e3tua3NrXmJqWFZpWk9+VmptX8lXz7/jw7+9ybm/wr1ksUdXuzFwRD1EOEk+"
"PkBgRkbPTP7Z/9Lx0tPo2dbn6t3r+uvv/fn2+/z+/f18fXx8eXR+eGtvfF9ecFpSXl1LXlhYcUzKal6638q9u7+9tcS1cri/MrI+"
"OPg1QzU+Pzs9SmM63OJJy+jZ1tzM297U2fni3nT17Hp8+/59fP3+/n7/+/79//V5b/BuXnxlV11jUVVdVHFL3dpJv9Hfv8C+x7jB"
"udDNsTTGzC/nPj48O0Y9QkLyQ07GS+HN69Pdz9ni29vs/OH8bvR7dHX9e3b8ff1+fvF9/e73ef3rZ2j7XVpaX09QVFpXRcpZTLr1"
"zr69u8G1vbXzurMttXks3Tk7Njw/OkBA3z5ZwEnXx+DU0M3d29vd8XbjcWX6eGd0/G54/n39fPbzfPDq/H3k/2jwbGBcX15OU1xe"
"ROzaP8bPc8DHusa8urrF76w/SbAuRls4PDlJOkRCWHs+x9lOxM/j1s3Z6dnj63P58mJv/Glse3Rzev5+fvjw9vjn63rl5mz0/mxc"
"WG5OSlheQkvKP2G9VcXDvLjCsrW0c6+0K7H0KXg6NDA8OzZGPNdNSbl58b7I2tDG5ePg/XxbZmdYXWZgYG1tbXh+/vru7u7h5Obb"
"2uTe1/TseGtqSVpXQUViUDvM3k27x7m4u62xuMqq3DSuOi1KNS8tPDQ9QT7EUGO5zdjGvt/dzm10WVddTk9aWVVibGdyffx7fPh7"
"evrs7OrW1tfNyc/OytvXY1/wPUNTQDZU1znIusett6mnrberqS7CwCMxMiwkLjIwUTbaulzDtr7kxMRTalNKS0BGTE1NX/l059nd"
"39/o+G1kX15ZXHVs/NTQz8jBx8jH1sxXSvI9N0RdN0m77rewr6Srrq2jZDywLiYvLSYmNC5NUUO2vM+/tchaz1hFRT1BQkNHWnRs"
"2s7Pz87R2uX9ZFdOS0tLSVF5Z93Ewr67t7y7zfbIPTVHPy43wFjFqq6eoaelnbIusUEdJCciHSwrPMdFv6y3zr66RkRDNzg1OD9P"
"VXDKyMnFxcrO1N7pfWpfWVRMSEhJREdobvrIvbu8t767xUBeXjUsTd85vq+rnqaln51bPa8nHCEmICMxL76/WbCtxU/OWjY8NDxI"
"R1TOw83Gwcza7G5eV1hgd/rh1dPZ4etjU0RESz0/+dfdxq+4srt8vV0rKmU6JryupZyfmZWaL9K5GBUaHxwoLVGhvtOprlc4TzM5"
"ODL9zOLVub/U1e1fXE9RWVtea/V16OHf3tTY4d72Zl9GQXdNRHe2ycC4wbXyLzToLh/Fs6ygoJaUnCu8zxUUGCIhNy+5nMHksrs9"
"Mzs25URCwrvS98j2eVZRWnJbX3JubG34feTv6ezV79/Ry+rjV1jsOzhvu0LNsLGu7Thq2h4ZtL2toJmNjp4gpTEODRUhJ0ctn5rc"
"XLvXMS8vPrxAVbu43fHYXPhLTV3+WmJqamZoe+7o6+zo3O/q08zo9fpY6z48Vr5LYbG1ss5CWM8pGs62vaWfkpGbKLjNExEWJChN"
"LbCavU/Jzz4zMje/W0jFu8r52l7oU01WfV5mZXdmb2ns7eXv5+Lc7+LMzP3w81tiOj55vT3UrLG030f33R8btLi2opqOj50jq0QP"
"DhQhKUoqp5nFVM3bOzMsOLtNSsa6yt7lXt1RSlN2W2dnb21ubHzr9Oz05+vd9dnM1WDm+1lTPUndyT3Irbe86FfhayEfsr64ppyS"
"k6QorTsUEhglLkwsq53EWN7uQTkuOb1VTs+/ytrrX9pdT1NtXmlfcmh4Z3x96+3z6ene5dzey9J6XtlVUkBDXsZYRa2uuMr2d9kx"
"GT6qz6mfko+TSzqpGhAPHCVCMT2bpX5efk88MinzxEd9y7/Pzlvm4FdOXFtcZ2Zobmtre3jx9+j75e3l5tzi0tduXttaT0lIac9m"
"RrCwvsnh3uQ4HTus1K+ll5SYdDisIRUUHilFOTegp9xcW1JDOSxVxFJt1sfPz2H+32RUXF1eZmlsa3Zsd3j89+/u7+jq4eDZ3dTY"
"dVveZUpKSWvZ+UKxq77E0s/sOx0tqc2zo5aRlLotqScTERklPT4so5/GX0xORTspPcFbaOfKy8ppbNpvWVdaW2ZgbG5vbHlt+Xnt"
"/Oz17+3k5t7c29r7XePwTE5PbfXkRb2sv8rMyepHJCivv8WpnJWYqS20ShkWGSY2Vyy5oLnqS0xJSS4yzO1l99rRy+ta3uxeXV1c"
"aWNqbv1tc2/8ePl+7e/t9fPk4OTd2t/cb17h40pPVf967EjAqbzJxsDfRiUis7fRqZySlZ4vxcQaFBQfLV8tY56tzks/RUovK+rU"
"++7u1srXUPDdcl9eWGZqX2b8eG5kZmZ3+//o6ebv9fvv5e3m39zp62xi4t5PU2T8Y2tRz6y7zca/00otJcay3q+imJmfP1S2IhoX"
"Hy5bNzqnq8VZPkRNPS1N0uDafunV0ldl5fpyb11eY2Fk8PZ5emxjXl9m9+Ld3eDx+XlxeOHe3t7d8fR9b+bQX09obFRNUnGtsc2+"
"uMBNMiNOrHq6pJiXnNU2ri0aFhsqT0gtrqe69zo4REUsOtPPx9vw389bT2/78/xkX2FcWmn27er+b2FcWGF36t3c4ev/b2t1/N/d"
"3+Xh9HB7797RaVZ0Z0xHVPCyuNG9uMFLNypcsl+/qZydou49tC8fGyAvaE40s669eTo3REsxP93Nw9Pt6tZfU2Nu+vdqX2FdXWj7"
"7+jsfmhfW2Bu79/Y3OX7a2Zseefa2t7f5nJ17N7T11dmc1A+RlvGrsLEs7PRPS8tvL5Ss6Kanqg54L0mHRokNs47Q6yyvkoyM0c+"
"L0vdw77P++bcVlRcbe3vaF5bWFhjfuTi4/RsXllcZ/Lh3N3o+2llZG7s3djb3+r7aHLl2tTfXn5lSj5Lb8S1ycG0uOY+NTa9ylm3"
"qJ+lrzzaxSwkHys81j9Js7jCUzk3RkQ1Td/Iwc7o7uhVUlpr6uT8cWZcV1tk9OHf6PZrXlxbZe/b2dvg+mxfXGfw3NfU3ut4aV5u"
"5tXM1WVlW0tBR1zKs7/DurvTRjcyzb1tvK2ipa1ST701KCIpNlxNPLuzwfY/PD9GNj7kzcLK0+LhWkxQXv/h3+T0Z1hUWWF95tze"
"53ReWVpheere3N7tcGJfZHbs3djY4/1oY2Jt7NzQ0O5kXVBMS1fvvb7JxMbOXkU5T8LbzLqurLHHRstcMy0tOUViQ+26wtpXSUZI"
"Qj1d2s/Oz9jldVNQWmn65N/h8WxcWltleOvh4Oj+aF5eY3Ht39zd6HtlXl9q9eLZ2dzsb19eY3rn2s/R6mVXTk9PWO2/v8fIzddm"
"STxQyNXJvLOvtcdN0144MDA7RFdK+L3F3GBPS0dFP1rc09DP1OJ1V1FYZXbn397reWBcWl9s8OXf5u5vZl1hZO3xX+ds7PXx5+hc"
"amp92nnkb/1g31956mrc//d57Vdv4+Lye3lg3W52dernY2/23Wbub23pa/TrcOhpe3Tq73Fwad1i327cXmF+8PPg7WzpV2vt3n7u"
"aOJn6VPee9v7Zvhh4VfOW9xuam/ra+jt9/di+Gf56Npfe2vfXe/96f/qa2PfaeVr+fT6cvLoaH30cPHwa+DzXOdq523jcvZl+eRn"
"6Gj1ee155WbsYvP57vrwb314eext7v3mYN1U2m156HT4de5r6mt+9Opd3H58ae5p8+Jt8/Jv92xv6O31Yt9m3Fb66+Zud/dy4GX9"
"ad1m7GnhcfpwZN507XD9bttnaPL93mZ8be/lZ/F79nv4Xd/idf1n/HbfX+b58XBv723dXeTvYuFo8v3qbWnue95i+fn4cnxr5eb6"
"cGNz6d9h9u7+aehp31748uf9eexT2nDt/vR032Rl6GTd8mjqemrpbHz+7Hft8Wz6ZvT75fp5b3Dxeuho5Gftb3njY99s/Hx6/u77"
"+HB16m7s/3F+9vh362zud3LlY+Rm431u7Wbn/Hxt5v/+dHvz/XDuduZubu5s4G3zavPs9W7/c+l6bN1h72995W71dnz0+3x37Hv9"
"8mT/3/pre3j55mn4/XrweHz773l7/Xjx7WXr/ftu8Xr09nn7fPNm5GPfffr4aG3t9PJx7eVh+2Ds8txe6HD1cGju/+P/92LmXt5k"
"7er/+2XgXeF8efdt7fT8ffpp+Hvu7P1s7ndj3mzrfO1q/Pxq423t9X1r8f//7m3qcPfva+pf4HTsanjtc91Z4n3wYOdu4fll9mjb"
"YuVe4Ph2eu1tfutf4nTgZe5qfPzw9Xbu93Ro7W/bYflz7/P3bG7iaeNo+n7maH3ydO3zfmXma+pu+e799mP1c+L8b23ieXZ5a+vz"
"5F7hYu3wXeF75WbpaO34X+Z23mnxZ/TvbOZj4nl48Wv96vdufPj/+epsfvd19G/re/Vz7XV39Hd+cd5q5WH79HnoaO167/5ud+d1"
"/Plm3XR2cfrpbOZe43PtfHHpY+Zs8n3rbe90c+99+Pr+fuRk+Gbjbej6bN5b41/hdnPbX95ieXR65G/kbOtvaf7sfPHtcnb7/Wzl"
"auZv/XzvdnjsZ+V1dfvrbu7+avvte/JvcN9+a27vcOToY/x4+/f8dXXi/W99ce9y6HF452fiX+34cOp59W7kaHvvcO73bex8/3ts"
"7Xvkaehnbt1p72jpffV6/Pxw7Hty/up79m73d/n7+vd2/f37/u1o7HV69O1zdu5m325773t07/lu5vt+//5+/n3+fv5+/n7+/37/"
"//5+/n7+fv5+/n7+//5+/37//////v/+//9+/35+////fv/////+/v///////37/////////////////fv////7//////v//fv//"
"//////////7+/////////v///37///7//35+/////v////////////7/fv9+/n7+//7//////n7+////////fv///////37/fv/+"
"/////v///37//n7//////37///////9+/n7///////7/fv//fv/+///////////////////+/////37///////9+////////////"
"/////////v9+//7//////37/fv///37/fv7///7+//////9+////////fv/+//9+/////////v///v//////////fv////7//v//"
"//////////////9+/////////v/+/35+///+////fv//fv////////////////////7//37//v/////+////////fv/////+////"
"//9+///////+//9+////fv9+/////////v9+fv///////v////////////7///////7///////////5+////fn7+////////fv//"
"/////////v7//v5+/v9+//////9+/////35+/////////////37/fv///v/////////+/////////35+//////////7/////////"
"/v///////v////9+//9+////////fn7//////////////////////37////+//9+/v9+fn7////////////+//7+fn7/////////"
"fv///v/+/37///5+//7///9+/////////////v////7//35+/v/////////+/////37//v9+fn7//37///////7/fv/////+fn7/"
"//7/////////////fv///v////////9+///////+/////35+/v5+////////fv///37///7//////////////v/+/37/////////"
"/37//////37/fv//////////////fv7///////////5+////fv///37////+///+///+////fv////9+/37////////+/37/////"
"/v/////+//////9+//////9+/37///5+//7//////v7//37//37/////////////fv/+/37/fv///37///9+fv7+/37+///+/n7/"
"/////v///37/////fn7+//9+///+//////////7////+/v////////////9+////fv////5+//7///////7///7/fv7/////fv//"
"fn7/////fv///37+////////fn7/fv////////7+//9+//5+/////////37/fv//////////fv//fv/////+//////7//v//fv9+"
"fn7/////////////fv//fn7//37//////v////////////////7+//5+//9+/37//37+fv////7//////35+/////////////37/"
"fv///n7///7////////////////+fv9+/////v///////v/////+fv////////7+////////////////fn7+/37//v////7/fv7/"
"/37//////v7///7//37+/v///////////37///9+/////37//v/+////////fv/+//9+fv9+////fv//fv9+/v/////+/v///n7/"
"/n7///9+///////+/////////37///////////7////+/////v/+//7///////7////////+////fv9+/////n7//////v//////"
"///////////+/35+/////////n7///9+/35+//9+/37/fv////////7//////////37+/////////////v///37///7/////////"
"/////37///7//////////////////v////7/fn7/////fv9+/v5+fv9+//7+//////////9+fv////9+////fv9+fv9+/////n7/"
"/v////7/fv5+////fv/////+///+//9+//7/fv//////fv9+//////7/////////////fv/////////+fv///////v7///9+////"
"fv9+//////////////7///////7//////////////////////37////+//7/fv9+//////7//////////v9+//////////5+////"
"////////////fv9+//9+////fv9+/v7+/v////////7/fv///37//////37//v7//35+//7+//////9+/37/////////fv////7/"
"//5+fv///37/fn7//////v//fv////////7////////+fv///////37//////////////////37+////////fv7///9+//7/////"
"fv////7/fv/////////+//7////+//////7///////9+//9+/37///9+///+/v////7////+/v///v//fv9+fn5+//9+//7/////"
"fv7////+//7////////+//9+/v///////////37///7//////////////37+/v/+//////////////7//////37+///+////////"
"/////////35+//9+/v9+/37///9+//7+/////////v//fv///v/+/////////v//fv//////////fv////9+///+/v/////////+"
"/////n7//////////v///////37///7//////37/fv/////+//9+//9+//7//37///////////////7/fn7///9+//7////+//7+"
"/v////7///////7//37//35+/////n7+///////////////+//////7///7/fn7////+//9+/v/////////+/37///7///////9+"
"/////37//v////////////////9+/v////7///9+//////7////+//9+//////9+/37/fv///v/+//////9+/////n7///7/fv//"
"////fv9+/////v7/fv/+/////v////////7+////fv///////////37+/////37///////9+/v9+/n7+/////////37/fv////7/"
"fv/+/////////////37//v//////////////fv/////+//////7///////7/fv7//////37///7//////n7/fv////9+fv/+///+"
"//7//////////37/fv9+/////37//37//v///37///7////////////////+/////v///37//////n7//37//v9+///+////////"
"/////////v//fv///37+/37+///////////+///+///+//7///9+//7//////v///////v///////////////////v/+/v//////"
"/////////35+////////////////fv//fn7//////v////7+/////37/fv9+//////7//n5+/37//37//v/////////+//9+///+"
"/v7/////////fv///////////v///////37//////v/+//5+/v/+////fv////9+/////////35+///////+///+/////v///n7/"
"////fv//fv//////////fv////////////7+/n5+/////n7///7+/////35+/v/+///+///+/////////////////n7/////////"
"///////+////fn7//37////+/37//v///37/////fv////7//37///////////7+//7//////37+//7+fv///37///////7+//7/"
"/////////35+////////fv///v//fv///37//////////////v/+/////v///v9+/v////////////////7//////////37///7/"
"//////////////////5+/35+//9+//////9+/37///7//////////v//fv///////v//////////fv////7//v/////////+////"
"fv///////37+fv/+/37+//////////7/fv////9+///+///////+/v///////////////37///9+//////7+//7///////9+//9+"
"//////7+//////9+//7/////////fv///v///35+/////////v///////37//v///////////////////37///////7/////////"
"/v9+/////n7+//9+//9+/////37/fv//////////fv7/fv7+///////////+////////fv5+//7////+/////37///7//////37/"
"/////37///7/////fn7///7///////7+/37//35+fv9+fv//////fv7////+/////v//fv/////////+/////v//////////////"
"/37+//////7+/////v///v///////v7//37///////////////7/fv////7//v9+//9+/37///////7/////////fv/+////////"
"/37///9+//7///////7////+fv9+//////////7//37/fv//////////////fv/+/////v///w==";

// Spoken phrase: No
#define NO_AUDIO_SAMPLES 7188
const char NO_AUDIO_BASE64[] PROGMEM =
"dmVcV1JPTEpKS0xMTlZhbnPt29XU2NnSztfc083HxcO+trS2tbW1usLh/VxAOTU1NTc0OD9ERkhOVl9WT1RaWFNVW2/07+HVzszL"
"y8rKzM/U2d3oe2VcWFROS0lKS0tLTllmbnfl2NPV2tjQ0N7d1M3IyMS9tbe3tra4vs1+/kw+ODY2Nzc2PUVISU1UXV9RUFhaVlNY"
"Y/nv6NrPzMvLy8nLztXZ3eh1YFtXUUxJSktLS01UYG5x7dvV1Nna1dDc5drSy8rIwrm3ube3t7vE5vxdQjs3Nzc5NzpESktNVl1p"
"W1FXW1lUVlxw8u7g1M3MzMzKys3T2dzk/mdeW1dPTU1OTk5PVmJvc/Te2Nbb3dnU3Ovf2c/Ozci+ury7u7q9xd17bkk+Ojs6PDs7"
"RkxOTlZcaF9TVlxbVlZca/Tx5tnPzs7OzczO1drd4vJqXlpWT0tKS0xMTE5ZaG9449jS1NnY0NHh4NjPy8vIv7e5ubi3uL3LePJQ"
"Pzk3ODg5Nj1HS0tOVl5kU09WWVVRVl/+7+rbz8zLzMvKy87V2d3neWRdW1ZPTExOTk1OVmFub/Dd19XZ29bQ2+bb1s7NzMe8ur26"
"u7q+x+r9ZkY+Ojs6PTo8SExNTlZbZVpPVVpZVFdddO7t39XOzc3NzMzP1tve5n1jW1hSTUlJS0pKS1FdaWvv29PR1tfQzdnf1tDK"
"ysnCuLe6t7i3vMX09V5AOjc4ODo2O0VJSktRV19TTFFXVVFVXf/o5drOysrKysnKzdTY3elxYVxZUk1LTE1MTE9ZZGlw5drU1tnW"
"ztHf29XOzMzKv7m8u7q6u8HTbu9NPzs6Ojw7OUJKTEtPVFxdTk9WWFVVW2rs6ODWzszMzczLztTa3eP4ZVxYU01JSUpKSUpPW2Rm"
"+NzU0NXVz8rU3NPPycnIxLm2ure4trvC7P5rQjs2ODc6NjlESElJTlJcU0pOVVRQVFx75eTaz8nJysrJyc3U2dzkemRdW1VSUU9V"
"VFRWWVdSVnRhW3Ts28vdTdzSe09NTmev9u+3tq+7uzTGxjU5Ljg5yz43zNbG1PpHfMpLVVNu1sjP5crMz91vW1tcVFBUWGBmd/zp"
"39jRz9DY1tTpXU1LSVtNQFXgytTKUP28bko9T065t0C3rqiusF04rz8yKiotScgvTdPHvchaOt5+TFJHWNW+zNDLzs3WblBRU1FQ"
"UVZhdfzt3NbU1NTT0eBeUlVPTE0/UdbK1dfRfbvNRD1Haui0UemsqqiyuTbMwTMvJisuyl447fDBvcVCPGRLXlVLUNS/ycjU39vd"
"cFNOT1ZcXV5o7t3a2tXNzM7kX15gUkJCPU/MzNnewMq3vTw5QOtqu+BBqaOfqa5KPrE6LSEgIj+8O1pq3L22zzQ9PUNuXk1PysK/"
"v9Z5dvxgV1BNUV9tePzy59jP0tvg8+/sXUQ/SEzcz+72xrm+t2k2P13cXsZE66ShoKyyPGq6NCwgICJTv0rZXvzFts46PTU8Yetg"
"WdbSxMDQdF5fYGZhWlxqev199eXa2+Xh2drca0g+SVhY3mdpyK+vvL87NljTbUXvN7ebm52osjXMwSwlGxodYrzkv11U17PHPzws"
"L0Xf4OnS7s7DyeBjU05deHxuaGVx7eft93jz2c/Z81hKS2RqT2FZ3rqts8rSPULTzlA6TTqzm5udqLI60sctJhwbHEa7xrbdS0zB"
"xldRMS02UuDQxeDu3trk/GdYXG39/Hp1e/j5e3324trffH1tcHVmRD1Y4barq797fT/QvOEzLjs+n5WWm6K1OLxIIxwWFRrmuLGs"
"1D88ydX8YS8pLUPzv7jMdVZWXe7c7XNoYGNreP706ePk7f5kXu7a1t5kQjpP3bqsrb1f7kr2u84+LTs5tZmZmqKqTWbFKyMbGRgu"
"urytu1U7X8lz20AtLDdW1bq/3V9TVWfe3e15Zl9jbHr05t/e63diWGfYz9DtTTc6asauqa3VW2pFwbtwLi06P5+VlpqgrzrATyMd"
"FxUYTrezqsZCOdfS7tw1Kio6WsS1wOBWTE5q29vl/WRdX2h57uXe4PBsX1xq2M7R9E47O2rIsqyvzlH4TM674jYtPTyrmJmco6xE"
"3NMpIhsZGTS7u6u6Vjpb0G7PRS8tNUvbub3PcFFNVu7j4el3ZmBla/3p4t7m/mFeYHXQys96Szo2X8Wyqq3DSu1X2rfPOCc1OLaW"
"lpiepWhMvCwhGhYVIr26qa3kOj/UX81pMSstPVi8tsPZWUlJXe7k2+H7a2RlbPXp5+v3a15hbfDQytJ4TD45V8e4rrDBTXv777nL"
"PyozPNGbmZqgp8pAvDgnHhoYHtS9rqzJQjvpYN3UPC8uOknLuL/OcU5IU3Pp3NzpcWVgZHbp3t/neF1XXXPdx8bVXEI4Oee9sKux"
"z0bfds224TMmNDq1lpaYn6ZrTL0sIhoXFSG/uqirzzs95VbM3jgsLThIwbW8y2hJQk9w4NXW5WtdWV1159zd5XVcVlt23MnH11xD"
"Ozv5vrSutc1I+OTZuNM7KTM9zJuZm6GnyUG9NyYfGxke276uqsJHOmxX3stEMy43QNe5vMXnUERJXPTW09nvZVtZZfTc1tztX1NP"
"W+vQw8PWVD45Ofa7sq2zykR6ztS1yjolKzx8m5WYnKO2Or1HJR4YFhhCu66js1w1SkxZw1c3LjE4Ury5vcppRkNMWfDV09rqaVlY"
"Yvnf2t74ZVpXXPPYzcfMfUs/PkDXu7izvNVK38XNudI5Jyw//Z6Ym5+puTzCXiokHBkZOcK0pa/bOkNHSMt9QTc3OkfKv7/G3FdK"
"TE9b9t7Y2eVvXFhdfd3V2epkVlRabt3NysjNakc9PkDfuLe0vNBM7rnFt8c3JCU8UZ+UmJylrjzUyiokGhcUKMe3oae+PTo/OtjW"
"TDo1Njrjwb69yntLSUhMX+7Z0dLea1pTW3ze19vsYlVTWmzjz8vMzepOQUBFWL+5urzNZVC/vsC8SSwjNEm2mZqcpavjQ7s5KiAc"
"GB1cxqulsm86RTpPzG5IOzs4Ss3DvsLUWUpJSVR73dPR2fheVVZk6NXR2vlZT05YddbHxsfQWEA7P0jPtLe3wuZL5bbAuM4yIiQ+"
"XJ6VmJymrjzP0SkkGhgVKcq3oaa7Pjs9ONjOWz43NDVjxry5wOFLREFHXd/Oy87hX1BNVXfa0NLealNNT13ozMXGy3pHPD1Hbbu2"
"ur7YWk2/vMC8SCwjMkm7mpqcpKrLP708KiIcGRxMyK2jrtg7QjpHyt1WPTo0PeXHvbzHc0tFQ0xq287M0updUE9Z+NTNzt1iTkpM"
"W9/IwMLLb0E5O0n8ubC4vdhdR8m2wrxXLSEqTc6blpqep7I8xV4oIxsYFy3buqKnukhAPDnRzfdKOzMxSOXCubvLYklAQk550snJ"
"0vdVS0xY79XOz+BlUEtOXuLNxMbTbkc9Pk/kwbO5w9xjTWK7xMrlOCopS+Krmp2fqa1uVscyKyIeGyJM7K6ostRMTDtdytv8Sjwy"
"Ok3vwrzA0WlMQ0hV99LLzNl3VUxPXOjPys3fYE1IS17cycDE11tCOjxSz72wtMLiWU1Nv7/T6TorJ0PQr5qbnaWqx0jENikiHRod"
"OlK3p67BXVY+T8bO11k+MDE9Ts27u8PdVEJBSmHZycfO6VpLSlFx2c3N1/5WS0tV9NDHxc9tSj8+SdzCubO90lxSSVzD0upSOS4w"
"5ceqnZ+kq655atowLCUgHSZCU7atuMp6XETvyNDSYUA1Nz5N0sG/xtlbSUdMX97Oy8/lXk5LUGfcy8nO41tLSE5m1sfEyONPQD1B"
"W8W5tLPA7UxNSF/D2WVHNi0yz72nnJ6iqq3sZdkvKyQfHCI5RbqttL/bZ0Ntyc/J3kk3NTc+acq/v8fkU0hGTGPczcrP419OS05g"
"5tPO0d9rVE5PXe3Sy8zVfE5FRE1syLu7u8TvTkxXVNTMYUw8NjJTvrSjoKWrsLxR1U8xLiciHyw6Ube0ucPUXEvi2dDK3k8/PDo/"
"Wd7KxcnYb1VMTVd13dLQ1+1gUk9TY+nTzc/aeFlPT1d2287Lz+VeS0RGVuLGubm9x/RMRldc8cpuSDw3M0G9s6aeo6ivs3lx4jUv"
"KSMfJTI7yLS3u8bhTG3e2sXL7UxAOjlDVePKxcrXeVRMTVZu3dHP1edkU05QXPXa09LZ8GFWU1dp5dbP0d5rVEtJUvPPwby+ydtc"
"SkpgXufZV0Y+PTx0urSopaqvtb5b3VQ2MywnJS43R8K8vsHNcFji5NHI0HpOQzw+SVrgzcrO3HBWT1FbeN7U09ruYVVRVmLq1c/P"
"2v5bUU9Xb97QzdDhYE5ISVXiyr65vcjeV0ZHXFrt2VVDPTw7+LawpaKnrbK8WtxPNDAqJCIsNEPCvL2/y3NZ3t7Nw8zvUEI6O0NR"
"6s3Jy9f8V05OVmzh08/V5WdVT1Bb/93U0tnqZ1hTV2Tu2dHS2vxaT0xPZtfIwL3D0vdUSUtjYO7eVkY/QEDqubOppquwt75e4lw5"
"Ni0pJi01P8y/v8DK7Fzo69bIzd1kTD8+RExk3M/O0t9tWVNUXHLj2dba6mpZVFRdeN/U0dXhcltVVl173tTQ1eJuVk5NVW/Txb+9"
"wtR4VElIX3Jy211EPT4/VLqwqqOnrbW562jdPTcvKCMnLzRwwL+8wdFhe/Hty8nP5F5IPz9DTGvd0M3R33JbUlFZa+nZ1djieFxU"
"U1hl7d3X193ual1YW2b13tfW2+xpWFFTXH7XycXFx9dzWU9MWfRt6nBLQUFGTsS0r6iqr7i80VvaSzw3LikpMDVKzMbAws3+b+14"
"1M3Q2vhVR0ZFSlh73tTS2eltW1ZXXnTn3Nnd6m9eWFhdberb1dbd8WZaV1po7tvU09rtZVZPUl301cfDw8bVblVNSk/+bPp9TEA/"
"R03Hsa6np6y1uslZ2k46Ni0nJi0xQdDJwcDL9HPqfNLJzNLmWkhEQ0ZRbODTz9XjcVtUU1pr69zY2+ZyXlZVWmr+fv5+/37+////"
"/v9+/37//////n7////+///+/////////v///////////37//v///////37/fv/+/37+//7/////fv////9+//9+//////9+//7/"
"/37////+//7//v////7//////v/////+//9+//9+//7////////////+/////v////////7///7///////7///9+fv9+fv//////"
"/////v////////////5+/////////////v/+///////+///+//////9+//7/////fv///37///9+/v/////////////+/////35+"
"///////////+/37///9+/////////////////////v9+//7+fn7////////+fv9+//7///7///7+//////7///5+//////9+//7/"
"//////////////7////+///////+///+//9+///////////+//7//////v////////7///9+fv///v///v/////+//9+/37//v//"
"//7//n7/fv////////9+/n7///5+/v//////////fv///////v//fv7///////////7///9+//////9+//9+/37//v////////9+"
"//9+/35+/////////v////////7///7//v//////////////fv/+fv/+////fv///37/fn7+//////5+/////v//////fn7//v//"
"fn7+/37//////////v7+//9+/v/////////////////////+//////5+//////////////9+//////7//37///////////7/////"
"fv//////fv7//////////////37////+/v////////////////9+fn7//////37///5+//////7////////////////+//9+/37/"
"//7//v/+/n7//////v9+/n7///9+/////////v///v////7/fv///37//////35+fv/+//9+//7+//7///9+/v9+/37/fv//////"
"fv///v///n7///9+//////9+/37/fv////9+/37///////////9+////////////fv///37///9+//7/fv7/////////////////"
"fv////5+////////fv////////7+//////////9+///+/v5+//////7///////9+fv//fv////5+/////////////////v7////+"
"////////fv//fv////////////7//////v/////+/////v////9+/37///////////9+//////7//////v//////fv/+//7/////"
"//7///9+/v7//////v//////fv///v9+fv7+//////////7/fv7//////35+/37/fn7+//9+/////v////7/////fv////7/////"
"/v//fn7//v///v///////37//////v///v///v7///9+/v/////////////////+///+//////////9+///+///+////fv///37/"
"///+/////////37+//////7////+/37///7//37/fv////7//////////v//fv/+/v/////////+////////////fv/////////+"
"////////fv///37///7///7///9+//9+/v///35+///+//////9+///+/n5+fv9+/37/fv///////////v9+/////////35+//5+"
"/////////v///v/////+/////37///7/////////fv////9+//////7///9+/37///7///////7+fv9+////////fv7/fv9+/37/"
"/////v//////fv//////fv/+//7+///+//7+/37/////fv7//37+//5+fv///v////////////9+////////////fv//////////"
"//9+///+//////////9+/////n7/fv9+////////fv///////37///////7/////////////fv//fv7///////9+//9+/37///7/"
"//////////////7////////+/////v////////9+/////v/+/v//fv////9+/////37//v/+///+//9+/v7//v/+////////////"
"//////////7+/////v///37///7//////////////37///7+////fv///v9+/37/fv///35+/37////////+//////////////9+"
"/v//fv//fv//////fn7////+/////////////n7///////9+/v/////+////fv/+/////////v/////////////+fv///37+/37+"
"/n5+/v/+////////fv5+/37//////////////////37/////fv///35+///+fn5+//9+//7////+////fv////7///7//////37/"
"////////fv/+//7+////////fv///////v/////////+///////////+/////v9+fv////////////9+//9+fv////7//37/////"
"///////+///////+fn7//v9+/v///////v9+///+/////////////v///v/+//////////////7//37///9+fv/////////+///+"
"////////fv////9+//////////////9+fv7/////fv///37///////9+///+/35+////////fv//fv/////////+/v9+/37///9+"
"//9+fn7/////fv////9+fn7/fv///n7//////////////////////////v9+///+//////7///7/fv//fv7///9+fv/////+////"
"///+/v/+/37//////37///7//v9+/35+/////37+///////////////+///+//////////7///////9+//7//v///v7///9+////"
"/v///37//////37+//9+/v9+fv//////////fv7//35+fv////////////////9+/////v///37//////v//fv//fn7///7/fv7/"
"///+/////////////////n7+//7/fn7/////////fv9+////fv////5+////fv/+fv5+//////9+///+/////////////v////7+"
"//9+/37////+////////fv/+/////v/////////+//7//////v/////+/////37/fv////7///9+/////////v///v7/////fv//"
"//7//////////v/////////////+fv9+/////////v9+/v//fv/////+//////5+//9+//////9+//////////7////+/////v//"
"//////7///////9+fv//fv////////////9+////////fv9+////////////////fv7////+//7+//7///////////////9+/35+"
"/////////////37///////9+/v7///7+//7////////////+//////5+//5+/v///37//35+fv///v///v/+/v7//////////37/"
"//////////5+//////9+fv//////fv7///9+//9+/v////7/////fv////////9+/37///9+////fv////7//////////n7///5+"
"//7///////////9+/////v///////////v////////////////7///7//////v//////fv7///////////////9+/////////v/+"
"////fv////////7///5+///+//7//35+//////5+/n7+//7//v///37///7///9+/////////////37//////v//////////////"
"//7+/v////////////9+///////+/////////v//fv/////+/v///37//////////v///37///////////7/fv//////////fv7/"
"/n5+fv//fv7/fv9+//////7/fv/+/////////37///////9+////fv/+//////7/////fv////9+/37/fv////////5+fv/////+"
"//////9+/v///35+/////////n5+//5+//////////////9+///+//7+fn5+////fv////9+/////////v7/////////////////"
"/////35+//9+///+fv////////////9+//7+/v///v///37/fn7/////////////////fv7+/////37+fv7//////v7/fn7/////"
"/////37+//7+/////////////////37//v9+//////////9+/37//////////////v9+/////////v///////////v9+/////v//"
"/////37+//////7///9+fv////9+/////37///7///7/////fv5+/////////////////////////v7/////fn7////////+////"
"/////////37//v//fv////////////7//37///////9+//////////7+/n7///9+//9+//7////////+/////37///////9+////"
"/////37/fv///v//fv////9+/37+fv/+fv/////+fv7///5+///////+/37//////////v9+/v///37/fn7/";

// Spoken phrase: Help
#define HELP_AUDIO_SAMPLES 7956
const char HELP_AUDIO_BASE64[] PROGMEM =
"//////7+fv7//37//37+fv//fv////////////////////7///////7///////9+///////+///////////+/////v7///7//v/+"
"//////////9+///+fv///////v///////v/////+//5+/n39e/R49Wxf2Xt07fTsY3nl7GZ862tsefzqcnl0+/dx8uzd/X37anJx"
"duj5c/PleGDn+Gp27G15bm/e/O7y8/x3fPb3fu/8anpsZex7eeTm62Nv8377am3q4XN87HFieOZzc9bc8GRmZF3s7WPm3V/1bPl7"
"btHudPxwbnTv9/3g4G5nVmr1Zezg4PRX7txe6Obg317palz3cnvk6Wtza+5daO/s6Nvd9mpe4FZ76X7XWO9qa91d2N5Vb+VU6/nX"
"7uLOTlTXYlHZ7d5m+dtpVt7sbGrWW1DL4W925lPYXd5nT8FY5Wtab/zj/uFdbebdWH3a6+R5fWpbbd95emfSflZq791Pzedk3Fz1"
"21B+zl1vzVRi3k3MSmbG02RWfXdhX97X8lLR0X5X4kZ0eHLdzevn60Xe8tx1b/FpRt9iwvhyzFxPUG/e12zG5/VE1j55v03Q1W5N"
"7+Z9W/DX82JvadBjYtPvWFrkYd/r0+jzWFTdT2fPzX1d1W5J6GJc6s3l691dXt59XOxPxEZvyGBe/dNO0ulf7uDr9FXYXGF72lJh"
"vD/+2+NJb8/e31zGflpKZvXWSMrN4FlM3kxc2cFL+U/S1ePsyes/UNZxRsPb0l9I3s9BY9jh7XbVyVJNYuZd5ctIvF1T1lZqU2fn"
"7c/mZu9dSuf02sNO6exXW+bQ219k5lNTfdvb4+La+klu21Rgds7MT3j9fWRV6t/V+nba2UX3zN5Madps+UXh69h8Vubc5WN1Ycde"
"3sZFwD9Kw0FN3MjqzG3kVE3YYPFu69/0ZWb7/Wzj8G/zd/r+4vPt4957725u8VJOd9JQ4sbIxepOcu0zL8jTwreupaS5NrlEJCQs"
"Nz1XQbGu+XrN/ENFRFjNU27Nznjy6W/5Xl5qcWFyfPt5+Xn5e/rz4/Xp4OB07mX+dk5O4dpNz8XExWdO510sN77aubapoqPkPrQu"
"ISIvNEZIUaq2XeDNWD9EQe/UTd/K0Gni93p0XF5vZ2V1+3z9/nz8e/fk4u/c2Op69VviUEZQyVtVvr66yktP0zchXLvJrKucm58z"
"1rsdGhwsLEw4wJ+9VcnOQDk8Psp9SsrD11/ZaO5eVlxzXGF4fHx5+nn1d/Pp2/vf2tpt6F3sbkVG2dJFyL26vGNE5FsjK7fOsKyf"
"mZq9L6srGRkkKzdFPqOoY+TCYTc6Ol7GRefBxmfq6Hb3VFdnZltmeP37ff9+e3755Nrc6NXR62v3WNxHPku/VU22tbDCQ0XQLhlJ"
"sMCjopORly3erxUREyIjQS6+mLBSwcQ9LzM2xu0/xbzMW9Jr32NPVWNUVmds8vbrcPNm/njl59Df3tfUaXZpW99DPlm+RO+ztLHP"
"PU7dJBrDt7OinpCQnSSuXREPFiMmRC2mmcNmvM81MDE8vE1NvrzlatNn3VtSWWBPVWdq/+zv+nZtavXo3s/O4NPR5lnkTtxPOT/C"
"4zyxrqy2Rj3VNBQsqb+fno+Njz42ohUNDRseNS1TlKNXy75ELS4s08Q8zru/W9Vp4+xTUWBVTlhibfzo9fNrdmvo7tjW0erX2OxZ"
"41vmTj1Gy+Q8ta+vuldH3jsZKqrGp6KVj5LbLqQdEA8cIzU0OZqh+t/JUzQxLG2/RN/BwPzgZ+rdXFpfWk9YWm7/5unrdGVna+7h"
"09LN4t3Y6lLl8mdaPEXfzDe8qa+26E16SRoeqrqsn5aOj6gjpC0PDhUfKzwpn5nD/M9lOC8oPLhPYsW82ORqX9FuW2RnTVBQXWXm"
"6tzseGBlan7d2tDS2H3e6GJc1G5vSUJU1FdCrq22wn1l/jMaNqrHq6KVkpZkNKgfExIcJTszNZ2j0u3uUDoxKVm/UuDJwt7lUH7b"
"dWV+a1dSUVto/eXd5PRfXVx47tnRzdTedujtYF3M8VxHQVLdUEKqqrfB4exbLRhApseonpGPlUE7qBwRDxojPi03m6LLcF5IOy4m"
"Y8Bc3MrD1d9LeNn9an5jWU9OVWh+49ni72RgXHv63NXP2d7+9PpjXs/oV05KWX1cRa+rvcPQ2Fw1HTepya+jl5SY4zSqJhYUHCc/"
"Ni+ipMNwT0lCNylGxPzd18zT11Bd3u53eGddV1NYZX3p3ujtZWNbcPnc09DY2+v1fWJhzedNTk5cYltIrqi9wcbKVTIcM6fIsaGV"
"kpbHLqkoFBEYJD05KqSfu+5EQkI5JjnF49fe0tDPUlLg6PH/YltYUVRf/OXZ5OtsZ1xmfePW1drf4HZwaGTa0VRRWWJVY0zFqLnI"
"w8LoPykjuLHOq56VmKAyz70dGBYgLlYsTaGtw1RBQkkyKlvU3NXg28/iTGDw6+3xaWNZVFVfduXd3eh0XlpdaubZz9Pb5/RpaW/1"
"0MtWUFlbSlBPxqe0xb273DspIbyt3ayelJeeN16yHxcUHStaLjiiqbpaOjpGNSdF0svF1dzS3UhMX2/j3vT2YVJOU1x+39jZ4W5e"
"W11s59rU0tz6dGlfeuDWydVSW1hMQVBgt6zCwrq9WDgqMa/Bz6yemp6vMLviIRwbKDlqLvOossJKOTxLMi5Z2MHCz9vTdkdMU2fo"
"3uXsbVhRVVpv6drd4H1eWlho7dfSztbta2FcZOTOycXxUlpMPTxV1a2wyLi0wz8xKWSsfbummpuhbTmvMh8aHixXRS60rLXVOTI9"
"Qy03eMm5wNLf2VNGSlF92trh9WFST1Jc/t7X3ORsX1pcb+XX09Pe+WleW2zZzcrPZGVdRztFc8SxwMS1uOc7NTO8vV+2p56krjvr"
"vi0kHic22D4+t7i55zkyQEY3R1PQvMLY93pXWVNUbuXi5XlhWlpdav7q3+TtbGJeaP7q3Nva4PRka3nn2t3p0eBdSENG4sbowr21"
"scA+NE9K3sxHs6CcpKrpQbk2Kh8dJUzOS8/pzbPKRTY0N0/tTuv21MnL9l9aVmr8c3Z5a3FubWh6cPz59/5+eHl+9fjt9fLy7Pz9"
"8u3d2PZeW1Zh+/5Q3de/trzaWVFD6ONBXufApqKmtLZH4NE1NCUiKENXz79gfsznz+xHOz1BTtPl1uH6+OHq7fpoX2Robv19/Xn7"
"fPLz8vl+cnNudXl5/n337+Ll6PR1bfDvdmFYTFvc0L2/y9/gXe3QT05KOuq4rqOjq7y2TOvTNjwqJCQuNWa8yMPKUl5lUXtiVUpM"
"R1fv4M/U4PNuZmxwdG5qZWlx8uXf3eDp+W5nYGBlbfru6Onu8vTv6+x3ZlpSWnbrzMvNz9zz4dLw1WpDSEZLv7Wtqau4vsNJzE89"
"PCwpKzM72MXJwdlXcVVdcl5RTUtJX3Xe09fh7W9rbW5ubGhma3Tx5t/f4Ov7bWBdXV9s/Ovl4+nr7Ozr7HJgWFFVeOLNw8rK1N/s"
"z+Dl5ENARkTPs62nqK6+ulf62To/LigoLTNSxMe/xWNtXFBtXlJKSUJNXvbV0dje729tbGtoZWBjaHru4t7d3+f5cWZhX2Fpd/Lo"
"5eXp7e7u8fZwX1tWWXje0cbLzNLe6trc/uFNQklJb7uzrauvv7/YU85IQjotKy41QdTJxsHgbnpVZGRYTUxGSVZk5NfY3eZ+d3Jz"
"b2xnZGNpcfvt5+Xl6O/8c2plY2Rncf3s5eLk5u7v8PH2e2NaV1Vk4NHGwcnK097o1e5vZkBASE3Is66qq7TBwFbk8z5AMCorLzZY"
"y8i+xvXmX1ppXE5KRUBKUWzd1tfa6/p3c3NwamVgYGVsfPHp5uTm6vD9cm1pZ2hrcf3v6OTj5+rz9Pf5fG5fWlpde9jOx8XMz9rn"
"6t1uZ1VBR01jwbawrrC+xc9Z2ldFQDIuMDU+887JwdDk6F9paVtQTUVGTFJt49vZ3Obt+H13cmlkX19ianbz6ubk5erv/m5oYmBi"
"Zm/36d/c3N/k7vd8cGhfWVZbZ+PNx8LCy8/b6+vjYVpMPkVN+7yyrqywvsLXXNpOQzwvLS4zP+nNw77O2ONlcWxbUEpCQkdMX+zc"
"1tjd4+z5eGxhXVpaXWZ27ePe3d7k7XprZF9eYWh27+Td3Nzg5/L+cWlgXFlaZvbYy8jFyM/Y5P37/FpVSkBKVeC9trKwtsTI4Gbi"
"T0g+MzAzN0Tu0cbCztPeevZ2X1ZNRkVHTFx55Nra3N/n7/1uZF5bW11kcfTn4N7f4+v9bWJdXF1ibfbl3NjY2+Dq/G9nXlpYV113"
"383Fw8HGz9rrb29nUE1FPklX2Luyr6+1wcbfaeVORT0xLzA1QP7Rw7/JzNbq5/hkWExDQkJHUmXt3NjZ29/l7nppXlpYWVxmeO3i"
"3dzd4ez/bWRfXl9mb/jo3tva3OHr/G9nYF1bW19w5NXKxsbIz9zrcmNkWE1LREZUeci6trS2vsnPdfRpTEc7NDQ3PVPkzcPHy87c"
"4OT9Z1lMR0ZHTFdp697b293h6PJ4aWBdXF1ibP3s5ODf4+n2fv7////////+//////9+fv///v7//////////////////37/////"
"fv////9+/v///v7//////v/+///+fv9+//////9+/////n7///////////9+/v////7///9+//////////7//////v///v7/////"
"/v////5+fv9+///////////+////////////fv/+////////////////////////////fv////9+///////+fv////7/////////"
"/37//n7/fv//////fv///////////////////////37///////9+///////+fv7//37///5+/n7///7///////////////7//v/+"
"/////37///7+//////7//////v///////37///////////9+///////////+//////7//v////////////7+//7//n7//////37/"
"//7+//9+fn5+//5+/v/+fv5+/37+fv7+/v////7p4d3d3t/g5+/v6+rl3t7Z2tna2NjZ29zd3+jq6+br5+Dg3ujoeOlUXO5lYuja"
"3tPW2N/g7vN76V9Y8upWYG5xZlBo5NPn/WrRys7T2s/afWj539Xe+drO0tnbzszS3eV66fdaXvjf3tvXzszO2tva3W9aX21uc+7Z"
"0M3W5+Hi6vXw5ePu83nr6fXk4uLj6Pbo4efk4d7d3+z67uDp5dzb3eXxb2xfZ2l47Xtuevj87vHh4Obq+e/m5t7b19be397i4+Tb"
"19rf9H779+/f3NbU193g4vnw7/Hu5N/c19rd393f7Onp4t3f3uDk5+rr3tfe3Nfd2dzf3+bl6d3a19TT19jb5+3n2drk5+Lk5Onk"
"2tfe5vDu6uji3t7t+X306vD0397d4+3t4OXx+nv9d/bt39rW19fd5erv7uXq6uPf2+n1/Orv+Pvu6e/x7uPg5Ovn6eju9uPe3tzc"
"2tfe6Ozt6OPi3tfX4erx8uzv6eHd2drc2tzm5+Tp5d7f29bX2Nnc3eP0+fh7++3s7ezu9erv8u/r5ujs8Ovk397g3t/e3uTj4d/m"
"5ubp7fD36uXq3d3e5/Z4ffvz7N7b2tvf6O70+ez06uzq6+7u7enk4t/l9/R8fXvw9Ovu7Ozt5t/a2tre4+rr6ero39/c39/o7fD7"
"7/Xw5eTm3+Xf5ejm6evr7fLt8Orn4ers7fXv9urn4d/g4efq8erj4Nza2trc4ero6+zr6OPi5eff39/l6Ofu8/n06+jk5OLf5eru"
"7+7p6OXf3d3j6ezy8u/v6eDg39/i5ejo5+fl4OHg3Nzc3d3e4Obl4+Tf3+Df3+bo5ubo5unn7eXc6N/b4Oft+fTv7evt6uTf29bV"
"19nY2+Dl5eTj5ubl5uTl5uXi5uns6ufm5+Pd3eDe3+Lk6ezs7fLz8ert6unl4d/e3+Df4ufq6OPh3t/c3Nze39/g4eHg39/i397d"
"3t3f3d/h5ebtfPx9/n7+fv7/////////fv7//37+fv7//v////9+//////5+fv7//v//////fv////9+////fv/////////////+"
"/37/fn7/fv7//////37///7//////////////37//37//37//////////37//////37+/////37//////////////v9+/v//////"
"/////37/////fv///37//////////////////v////9+////////fv///////////v////////7/fn7//35+////////////////"
"//9+//7+///////+/////////////37/fv////7///////////7/fv7///9+//////////7//v//fv7+//9+/37/////fv//////"
"fn7///////9+/n7//////////////////v//fv//fv///////v/+////fv/+/////////v//////fv///////v5+///////+/37/"
"/////v9+//////////7//////////////////////37//////////37/////fv/+/////////////v///////v//fv7/fv///v7/"
"/////n7//////35+//7+/35+///+//9+/37///7/////////fv/+//7////////////+//9+/////////////v////////7///9+"
"//5+/////////v///////37/fv/////+/////37+fn7/////////////////fv///////v7/////fv//////fv//fv////7//v9+"
"/v//////fv7//37+//////9+//////9+fv5+/////////////v7///7/fv//fv///v///////37///7//v///37////+////////"
"///+///////+//////5+//////////////7+fv9+///////////+fv5+//////////7/fn7///9+////fv////7/fv//////fv//"
"///////+///////////+/v////7//////v////////7///////7+/37+/////////////////////////v5+//9+fv///v/////+"
"/v//fv//fv///v7+//9+//////9+/////////////37///9+/////37///////7/fv////7//v/////////////+//9+////fv//"
"////////fn7////+fv///37///9+///+/37///9+//////7///9+/v///////////////v////////9+////fv//////fv//////"
"/////37+/v/////+//7///7//v7//v////9+//7/////fv9+/////v/+////fv7///9+/////////////////v////9+//7/////"
"//////7//////////v//fv/+//9+fv9+//////9+/////v///////37//v/+//////////9+///+//7/fv////9+//////9+/37/"
"fv/+/37+/v9+/37+/n7//37//v//////fv////9+fv/////////////////+///////+/////////v////7///9+///+////fv//"
"//7///7/fv/+/////n7//////////////v//fv7///////9+fv////7//////v/+/v5+///+/37//////37/fv//fv//////////"
"/v5+/37//////37/////////////////////////////fv7//////v///////////v////////7+//5+/v///v//////////fv//"
"///+/////////v//fv/+fv/////+////////////fv9+////////fv9+////////fv////7//v7/////fv////7////////+/37/"
"///+////////fv/////////+///+///+//9+fv///v////7//v//fv7//////////v7//v//////fv////7/fv9+//7///9+/v//"
"//////////////9+////fv///////v9+/////////37//v/+/////////37////////////+/////////v////9+////////////"
"/////37///////////9+/////37/////fv/+//7//v///v////////9+/v////7///9+fv/+//////7/////////////fv///v//"
"//////7//////////v//fv7//v/////+///+//7//////v9+////fv///v////////9+fv5+/37/fv7//////37//37///7+//5+"
"////fv///n7//v////////////9+/v////7////////+/v///////////////v///////v////////7//v////////7////+/37/"
"////fv9+//////////////9+//9+//////9+/37//////37/////fv///35+/v//////////////fn5+///////+fn7/////////"
"////////fv////////7//37///////////7+/v/+/////37/////fv////////9+///////////////////////+////////////"
"/////////////37////////+/v///37///7//35+////fv9+//9+/////////////////37//v///v///v///37/fv//////////"
"fv7//v/+///+/37///9+/////////v///////v/+//////////9+/n7//////////v9+//////7/////////fv//fv////7/////"
"/////////37//////v///////37/fv///n7/fv////////////7//v//fv7+/37//////37//////37+/v//////////fv//fv//"
"//7///////7+/v7//37+/37//////37/////fv//////fv7////+/v////////////////////5+////////fv//fv///v//////"
"///+//////7////////////////////+//7/////fv7////+/37///////9+/////////v///////////37////+/////n7////+"
"fv7////+/////37///7////+/////////37//v////5+/37////////+/////v/+fv7//////////v////////9+/v//////////"
"/37//v///////37/////fv////////////////7//37/fv7+///+/37////////////////////////+fn7+/////////37/fv//"
"////fn7//////v///v///v9+///+/n7/////////fv9+fv7/////////////fv7/////fv//fv5+///+/////37/////////fv//"
"fn7///7//////v//fv/+///+//////////////7///7//v//////fv/+/37//v/+//7//////////37+fv///37//37///////7+"
"/v9+/n7////////////////+///+fv////////7//////37/fv/+//7/////////////////////fv/+//////9+//7/fv7/////"
"////fv//fv/+///+//////7//////////v9+////////fv////7//v7//v////////////9+///+//////5+//7///7//35+/v7/"
"/v7//37///////7////+/////////37///7/////////fv//////fv//////fv///37///////7+//////7//////v//////////"
"//7//////////37///////7//////////////37///////////////////7///////9+//9+/n7+/////37////////+////fv/+"
"//////////9+////fv///////37/fv///v//////////////fv7//////37///////////7/fv7////+/////n7//v////9+fv//"
"//////7///7+/////37/fv/+fv////////////7/fv/////+/////////////////////v/+/////////v//////fv////9+////"
"//7////////+/////////////////////37//////////////37///7///5+///+////////////////////////////fv////7/"
"/35+//7///////7//////v////////////9+/37//////37+///+//7//v////////7///////5+///+/v9+fn7/fv/+///////+"
"//9+//9+/37///////////9+/37/////fv//fn7//37///7//n7////+/////v5+/////////v//////////////////////////"
"//////9+//9+//9+//////5+///////////+/////37/////fv7/////fv//////fv/+///////////+fv///v///////////v//"
"/v///v///37///9+fv///v////9+/v////////9+//////////9+/35+//7///////////9+/////37///7+//9+/37//37//37/"
"//9+////";

// Spoken phrase: Please
#define PLEASE_AUDIO_SAMPLES 8285
const char PLEASE_AUDIO_BASE64[] PROGMEM =
"fv///v/////////////+//////9+/////////////37+/////v////////////////7///9+/////35+fv///////v///////v//"
"/////n7/fv/+//7///5+/v///////35+//7//37///////7//////////37///////7////////+//9+fv/+///+////////////"
"/////35+fv/////////////////////+//7/////fv9+///+/v9+///+//7+//////7//37////////////+/37/fn7///7//37/"
"fv////7//////////v///////v////9+/37///7//////////////////v/+//////9+//9+//7///9+fv/////////+fn7/////"
"//////9+////////fv7////+/37+//////////////////////9+///+////fv7//37////+fv9+////////////////////////"
"/37///////7////////+/v/+fv/////////+/v///n7///7/fv/+/////37+//9+fv/+///+//7//37+fv5+/n7+fv5+/n7+fv7/"
"//5+/X79ffx8+3v7evfp4ejj6eXr5uzl9OddOeVCPc/v0tbM2dP8Pkfn3NlIOEnWwsXGyb7Cz+tZY2/r3ePraPnQxb7Fy83LyM7b"
"7vTe2955+/La2dvY3XBJ8ehhdc3MzMTO6XfRzN5xcGlO/clWZc7K2NLYas3TZ3t43N9t3tbOXOzu2M7X2+TM2/ByZHnO9t/Oz9rP"
"3VrM0PLj6/N4aVh17mvV4OP0ynFA6nDk1tje3MvO+eHd8srs7Ob+63fw69re6efW3OPh7dvn1HF41Orodvbezm/ibtnP/vPj1FbP"
"cWLW1/T63V3Ozl9+6l/R22fY0mjvzljh22Tv5GHq+G/359vs6XjRWtn0V/PpZntweeZo7l7X4PPh3V7z4Wxt0mbZ1mne7uLw0H7p"
"8+t0fvB47N3l4OPr2P3a3/Hn4fXX3d3d79Lv2Nr83uFq2uvpzGTT2+jN73LZa2LfWml+YfXqdNjm/dnY4c/f2s/91d701Nns52x0"
"bm9raXH76unn5drW09rg7Ofubl1NTlBUVVFXYtDMxcPGvby6xsrzd/RLRzo7PEREQktM6tnU1dbPzcva53htdGRaU1RZX2NiZ3P0"
"6uvt7+7z/nNub21ranXw5eDi3djT09voe+5tYlJJTU9aVF1b38XGvMS9u7e8ycxX2VNIPjY7PEQ8Q0RR2+bV3NPMys3f43jwe15X"
"U1dcYF5faXvy7fPy9vh8eHNzcW548ePe3t/b1dTb5Gh6aWBWSEpMYFdoZXzAw7q9vrm2tMjBWuvtQEYxODg+OzpCQOn26driysnJ"
"2N3k6uFjXVZYXl1cXGJufPt9+fr3fnd6ef58++3i29vc3tjX3uNeZV1cVUlISG1Z8/Z4vb62uru3ta7IvFpv0jxJLzM3Ojo0PTty"
"cmTYfMfHys/b2N7aaWFaWmBbWVpeaG5xcXr/+/14/v7y8e/r39vZ2d/e2eTiYFhWWFVJS0BsZnnRZL66tLK6s7eqvrzVRr47SDIs"
"OTI6Lzg4S/NM22jPwc3J2dHT1uxoZF1kXFdZW2BmaGpvdnn5efj27unm5d7a2Nfd5d/p7GFUTVFVRlA+W+psx3W/t7Cut6+5p7i+"
"vjq6Pj84JzkuNi4vNj19Q/pn4b3VxdLSyNTb/W5qaV9YWFpcX19jZ25z//7w7Obg3t7a2dfZ4+v18GFXT0dnSVJXPGxI5NFSzMuu"
"sbG+tqG9skRGsjQ7KClMNy4zTOW7Ydq2wLjfYtRgTD88QEZARVFp3dTNxsLExsrR3XJaT0pHRUVKUltu3M3AwcbAv8TPW0zqQz46"
"NtNTSuXHrq2+v6W0sd01rz01LidJOCwvU2bI/HyvvL3H3sHnSkNDQD88P05Wa+HPx8PExcfO2n1XTERAQEBETWHfzsu9uLq8xMTF"
"cztHPTw7KVDrU9LmraStwqOorrIqsGQpLx86MiQnTlhm2WqqssO2v7jQSUtNPDc3OkZETH3TysTBv8HJ0+dgU0pEREZKT13pycXE"
"vLy5wNrX1Eo7OjRPLy7vVcHex6akt7KitKc7Sa4pMiUrPCYkO/o74e61rNi0srvFbXhuQzc/PT4/RV705dDIx8nM091sVU1IRkVK"
"XH3qy728ub+6uNRcYE47MylIOilTSLa85KWeqrOer6BSMKUmKyUlNx8fNXIsac65rOmtqrzAysrrRDxMPjk/RU5QWunb3d3b3/Z2"
"YV9iamZ81s7NyMS7yNXG1WBDQEQ/KjpYMVk/ua54raCmuaS0pM8rpSwqLSc3IiI2XShNv92136+syr24wXt+Wl9DPUxGP0VNTk9W"
"YGtl+uLp28/Kyci/u8HKusbeblRqOS46QyYzRTrQNK6lz6qdn6+gsp3bJ58kJiokLRwfMj4ec75Jt8iqrcaurcLVvudYREVJODk/"
"PTtAR01RYubUz8nEvb7CvLrKzM7aYjpLRi8tRDEuQDq5QeafuK+knaylsKenH6ZIHjkkLR4hLDohNLYyy7i5r8Gtrb7Cs8xc3FhJ"
"PT9AOjpCQkRPbf3axr6/vLa6v72/yV9Q2zUvPTQoLjcxPy2zuzeepaukm56kqK+YH7+pFjckJRwdJSsgIrQsP6vMtLSqrLWzrLzt"
"uu5FTEg7ODs9Oz9MUXDYz8e+vr+9u8fZzdY8SkowMTM2LDE27TNDpEi3n6qmppympMWcux+cJSI8JCQcKCYqH1ZYJrHA5LOwrriy"
"sLXVvcRMYFlBPT8/PT9KT1Luzs3GvLu9vbrA3drMPD1NLy4vNSwvMe05NqF0vp2noqWaoqG9nKwcmykdPCEhGiUiJh1DWyKzvW+x"
"r622rq6xzLq+TWpfQDw/Pjs9Rk1ObdPQy8K+v8O/wNl4z005UDkyMDkyMTNXXy+ruWmhqKaqn6KmtaufHqraHEYnKB0nJykiMNcm"
"2LZOuLWwubSxtcbCvFpq/Uk/RUM/QElPUmTZzsvCvb3BwL3Natp3NkU8MS00My8wPswtu6hQpaKkpaCcpKm6mChapRg4KiQdHyYj"
"IyTXKTitSsGwsLS0rbK7w7XvY95NP0RCPT5FSU5c7tnOysbAwcjGxN9d2k06SDo1MDs1ODJw8DOtttqlqKaqoaKntK+fIa/HHUYp"
"KB8oJyglL2Yoa7lDura2urWwur6+vGfe6UxGS0Q/Q0lJTWd45M7FyL++v8nGx+9R4kE4PjgwLjkzNzDMUzqnu76ipaOonaKku6Sn"
"HqE0H0EkJB0oIyUhOEAmwslEsbi0ua+wuL24v2nNfElJSj8/QEZGS1147tbLycfEwcbPy9BaWHU8Pj47Mjc6Ozg9vzzWqtOvp6ip"
"qaCqq8mePzimHzczKSQlKycpKlQvNrhH27W+u7uyu73Au9rgz1pOVUxER0pJTFVgfebUzMbIxcHF1M3PWE53Ozw7PDE0Oz03PLs7"
"2KjOrqamp6mfqKrKnDo4ox04MigiJSolKClQLTa2P9ezwLu5sL26vrvb1s1XUFpKQ0pJSE1WW3zq2dfLzM3KyNrb1G1MckU/PkE7"
"Nj9CQDi/VEysxLyrrKqwpaus1aXDKaQqLkYsKScxKS0sRzkvvVdNtsnCv7bAwL/BzuDMZFpiVklPTk1NW1xi9N3a1crMzczI2eLY"
"9klfSz49Pz40PkFIM8bZPa24wqurp7CmqKrHracgp0UiXysqIzApKis5QSvJ3jy2xMa+tr3Bvb/I5cf3WnBeSlJPTU5VWF9n++bh"
"2NnP0dTWzeT13G5MY0tHP0VGOUJLTTjD5EOxvsSvsKu4q62v2K2vJag7KmovLio4LC8vP0MvyfQ/uM3QwbvHx8DHz97LcGj8Yk9c"
"V1NTXVpdZ3r+797a2dnP0djWz+l54WtJX0pHPUdEOUBUSze7Z0uuwr2urqu4qa2ubqbCKKIqL1ctKys4KjAvRzkzvUZNtuDMvrvO"
"v8DKz9POaP/vW1NjVVFYW1hfZmt68uro3dve2NTe4dXsbO1xTVtPTT9NSz5GZE09vF5Yssu9trKvvq2zt12mTzSnKEFOMy41Oy45"
"Nk84P8E++b10zsLC2sHJz9jR2Wvs9lxdaVlZX19cZ2xtee/r6t7b39vV3ejY6Gd+fkxXT08+TU4+RXFPPLpjV7DLvLaxr76ssbhY"
"pEQ2pyZGTTIuNjotOTVPNkLCO+C+Ys3Bxty/y8/Wztty4vVdZW5YXWNfXWxrb33x7u7i4Ojg2u3t3nxi/mVQWFRSQlxPRE7iSk68"
"T9W4z7u+s7q/sbjH5qky4rYpeUQ4MkI5ND48Tjhd1zzMzmDOxtTaw9bV19Dr7+B+Y3ttXmVtY2R1cG9+7vHw5d/t5d3reuL9Xmht"
"T1RSWEJTXERN511FvfNvtsq9vbazxrK2vVuqSzqsLEdYNzI8PTA+Oko7Rc899cdX18fQ6sPS2NPQ3/Da9Gz+fmBrbGplcG93cvf3"
"8/Lt6O7u6epx9PZjX3RXVlZiSlBwTU/u7UnH02K9ycTBvbfMurrAXrLpOK82Qng9OD9GNUQ/SkFG2kNizVnlzdf1ydff1dbi69zu"
"de74aH11bW96cXZ9fv768Pb27O997+1ofP9fXG1XVFZkSlJ9TlHf70zC1Xi8ycDCu7jMuLvEaa9RQLIyS1s9OEBCN0E9Uj1G20Rd"
"13dt09Pr09bS59nX7e7i8nH8fnJudndxdX54/X75/fL99/D4fvj2cGx5a11ia1VZZldcW31nYt3V8tm948i7x7/Gu8PJ1bzvRb5E"
"RU9JPz1DQUg5WE9BWGtfXufr3u7f0+bl297s7Onv/Xv7e3Vze3d2e3p+fP3++vr49fny9Pr7935ub29oXl5mXVZdd1Zf633l69DK"
"yta8u8+6vbzFxsi/7UzGS0RGR0A+Oz5POUNPTkxVX33wY97a4+bd3eDo7Ojt+3z+fXRydXd4dnh7fHx9/v39/fr4+fj29ff8/Pp8"
"cW9ybWJlaGZiX2v7emfd29vVz8nExs28wMbHyMnL41fVWktISEZHPz1NRkZJT1VhWV/z8vLz6OXh6+zr6/P5/v97eHZ2dnZ4d3p6"
"e3x+ff7+/fz5+ff39/Xz9PP3+Pj7enNwb21nZGdvaWxv9Ofd5tbMy8nIxcG9xsXDxcrP+m/pVExGRENIPz9FR0pLTE9fYWhsffTo"
"6uzs6+vu9Pv+fXp2dXR1dXV3d3l5fHp9fH7//f78/Pv69ett7vv07vft7G5pcmnrbn5qbWP2eW7i8NbW0tPMzsvExMjJx87M1/Z9"
"d2NNSkhLSUdGRUxMUFdUX2Bpd/3t/e18337o9Ol+Zf56+vvvafJianzqe/Zt73L0Zfzt8uZn6mfjY9x87+1v/Pf/evBt/GFxYm1y"
"5G/9+Nvf1c7OysfEx7/Iw8XJzNXvfvpVTEpERUhCREdCTE1SUV1jaXBv53vk8+vw6vbpePlxc/x6fnh3bn1u8m73d+xq6mb0723o"
"eO957Hjtfvj36Wzs8XVuc25q+GZscG98/nng3dTYzc/Gyca/xMXGxMzM5Ptubk9LRkVHREM/SUhOT1BUaGdi+Hrk8u/t7uL/8Pb4"
"e/1pfetvfGp5b+5sfvV7/m3yb+lv/Opr63368u/0ce395nl89n51cGt1+2xqYG3853js3djWzs/IyMbAw8TGws/L2PT3alNOR0BH"
"REVFQklMTVNTW2V2e/vs+Onz7Ovp6/v7cvh18236cXR8a+xp8nh0fnd9+vn9/nbve/X0e/r38v3u/ffydOpv9W94/2V6YnBucHHy"
"7+zg29DPz8nHw8LGxcbDys7c+fBrUktHRUZDRkFFSEpUT1VcY3R8/v3p6ezo+/nl7nh6e3fyb3N8cP90dnj8eHh7en3xbvn3/Hv7"
"9/vvffH+7XPqee3uffFwb3t9c29s+WBvZ/r44vHf2NbS1crKxMbIzcbNzdbq6nVnT1JISk1JSUhMT1NWWFljbnvx/P7v6nXk9/Dy"
"9X509mvwc338eW97/nb2cvp9ePFv8W3y9/z7b+tx5Gry8vZy9vb26mrxauhs+2p5+Wp1eP1w6W7j59nf3NnX0NDP19DT1uPfeuht"
"X1xXWFRWT1pRXFleYGlnafl08/nxc+r78vh97X7zbH538vh0cPT6cP1t9P/sa/V5detm7v7udPl9eu1o7nzqff18eOxv7W/0+XH5"
"cHb7/m91/v7/7vr26O3m6d/f3t/f3+Xf6+p46nR6ZV9pXm5dY2JnamZpd3Nw+Wzt93b7fOt17W719n7zbu1s83d+//Z6fvxx9Hv6"
"fvt37HD/cfT8/+5r52nzcvvvbONs7nlw+nLrde93/P9tfPr9ffF4e3z2c+z/7vT57u7t/uh76/N++/J5fv5sc351d3Jq831ubv53"
"+OtufH5++Px+d+71df53+Hv2+3Dsb+5wevBw73z4dPB5c/J2+P17+3z9ff5+/35+/v/+fv9+/n7+fv5+/37+fv7/////////fv9+"
"//////7//37/fn5+///+//7///7/fv7/fv/+//////9+/////////////////v/+fv////7+//////////////7//v7/////////"
"//9+/////////////////37+//7//////////////////37///7+///+fv9+/v/////////+/v9+//7//37//37//v///37////+"
"///+//////////9+//7//////////////////////v///////v////7/fv//////////////////////////fv//////////////"
"fv////////9+//////9+///+/v////////7//v9+///////////+/v/////////////////+/v7///////////////9+fv//////"
"/v9+//7//////v//////////fv/+///+/////////v///35+/////v//fv9+///+////////////////////////fv///v//////"
"/37+//////9+/////n7//37/fn7//37+//9+/////////37//37+/////////////37/fv7+fv///v///v9+/v////////7///7/"
"/37+fv////////////7///////////9+fv////7////+////////fn7////////////+////////////////fv7//v/////+//7/"
"///+//9+//9+//////7//v////5+//5+/37///7//n7/fv/+fn7///7//////37//v//////////fv///////////37/////////"
"fv7//////////////37+/////v7////////+//7////////+/////////////////v////////7//////////35+fv5+fv//fn7+"
"/////////v///////37///////////9+fv////////9+//////7//37//v///////////////v9+////fn7//////v///37+////"
"//9+//////////7//v///////v///37///////////7//v7///7//////////////////v///v//////////////////////////"
"/////////////37///9+/////v///////////37/fv7//37//v9+///+////fv9+//9+fv/+//9+/35+////////fv////////7/"
"////fv/////+///+//////7//37////+/////37/fv///v/////////////+/////////////35+/////v////7//37////+////"
"/////v////////////9+/////////v////////////7///7////////////+/v7///////////9+//////////7/fv5+///+////"
"/35+fv7///////7///////9+////////fv//fv////7///7////+/////////37//n7////////////////////////////+///+"
"fv////7/fv///37////+/37+fv///////////v//////fv///////37/fv////7//37///////7/////////////////////////"
"//9+///+fv///37///7//37//////37///7/fv//fv/+//9+/37///7//37////+/////////37///7///////////////9+////"
"fv///v//////////fn7//////////v/+////fv/+//5+//////7+//9+///+////fv7//////////37////////////+////////"
"//////9+///////////////+fv///v/+//7//37//v////7+fv/////////////////+/v////////////////////7//v//fv//"
"//////////////////9+/37///7////////+/v/+//////7////////////////////+///+/////////37+//9+fv///////v/+"
"//////////7///////////7//37//v////////7/////fv/+/37///////7////+//7///////5+fv//////fv///v////7/////"
"//9+//7//37+//9+/37//37//////////////37/fv/+/37///9+fv7//////v/+/////v7///////////7//////v//////////"
"////////fv//fv7/////////////fv///////////37//v////////9+/////////////37///9+////fn7///9+fv//fv/+////"
"//9+/35+/n7/////fv////////9+//7///////////7//////v////9+////fv////9+/v5+/////35+/////v9+//////////9+"
"///////+/v//////fv//////////fv/////+////fv////////7//37/////fv//////////fv//fv7/fv7//////v///////37/"
"fv7//////////37///////7///9+///////+/////37+/v///v7/////fn7/////fv/////+/v7//37+/37////////+////////"
"fv9+fv9+//7//////////v7+fv9+//////////////9+//7//35+/////////////////v////7//n7//v////7/////////////"
"//////////////////9+fv///////////v////////9+/////v9+fv////9+////fv///////////v///////////////v///v7/"
"/v/////////////+/v///v///v//fv9+///+//7//////////////////////37+fv/+/////////////v///37///7/////////"
"///////+/////37////////////////+//////9+/v////////7/fv/////+////fv///v////9+fv////7+//////7//////37/"
"/v//fn7//v///////n7/////////fv//////////fv/+//9+//7/fv///////n7/////////fv////7//////37+/37/////////"
"/////v/////////+/37+////fv///////////v/////+///+//7//////v//fv/+//////9+/////////37/fv//fv7/////////"
"fv//////fv////9+/////////37//v7///////////////9+////fv///v/+///////////+////fn7/fv////7/fv//////////"
"///+fv///v////7////+/v7/////fv7////+/n7+/////////////////37/////////fv///////35+/n7//////35+////////"
"//////////////7//////////37///////7///7+fv////7///////////////9+//////////9+///+///+////fv7////////+"
"fn7//v//////fv////5+//9+///////////+////fv9+/////////////v//fv///////v///37+/v7///7/fn7+///+/////v//"
"//////9+/////37//////v7/fn5+/37//37///////9+/////////////v7/////fv////7/fv/////+//////9+//////7///9+"
"///////+/////////////////////////37///////////7/fv/+///+///+///+////fv//////fv/+/n7///9+fn7///7///7/"
"////fv////7///////9+////////fv/+/n7///7//////37////+fv////7+/37+////////////fv///37/////fv///v7+/v/+"
"fn5+/37/////fv7//////37////////+//7//37//35+/v7+//////7+/35+//7//////////v7+fn7///////9+fv//fv////5+"
"//7/////fv/////+///////////////+/////37//////37/////fv/+///////+/35+///////+/37/fv////9+////fv//////"
"/////v///v7+///+/////v///////37///7///////9+//////7//////////////////v//////fv////9+//////7////////+"
"/////v//fv9+fv///37+//////////////////9+fv///////////37/////fn7/////fn7/////fv////9+//////////7+////"
"fv/////////+//////////9+//////9+//9+/37//////v///////////37+//9+/v9+/////v////////9+//7///9+//////7/"
"/////v////7//////////////////v////////////7///7////+//7//////37+///+//////9+/v////7//v///v//////////"
"///////+//////7///////9+/v////////////////////8=";

// Spoken phrase: Thank you
#define THANK_YOU_AUDIO_SAMPLES 9129
const char THANK_YOU_AUDIO_BASE64[] PROGMEM =
"fv7/////////////fv/+/37//v7+//7//////////v/+fv7+fv/+//9+/////37///7//////v7/fv9+//9+/////v/+/37/////"
"/37/fv9+////fv9+/////v/+/////37+fv5+/n7+fv5+//5+/XZ2eHD2/f3o7/L4/3j5b3B3c/j6eHzr9u73cvf6Z/79af55fer/"
"8/D68nTwd3n8a/t8anr1++738fb3/m7/dfv8dfz6d/R4/vx77Hrxafv1//Fq+3zyevh87vX8+nR2fP1p/HHz+vp98upz9Xp8+HX/"
"8Xt+cvv5d/b3/Xr2a3jwb/H5fPx4/O7zfu7wdGv1b319/vlw+nF8fuz16/5u9/d28G517mx8/vj893j69Hx3/f54e/f6+Hf3fXvx"
"/u9v/f95+3Fr7Xp76m7q7Hp3c3H2/nz9dO9yevt+7fb+9vt9df17dfl+dHpw7fJ3+P36+fFv+vx982588Gz6fXHx+fLyd3zye/l3"
"cHB9/+52b/Tu7f3zeX14fvl9dvhx/m569XfwePB59fJ79vr89X5w+G1v73f/fnzv/Xj19Xv9/X3yfXn6e/l6fXl5/Pb5ePdw/Hl6"
"+PP39vBu+m/0+Xx8dHR26376/v7weHnvc/77d/Vy9nj9+3Pwb+34/n35dHT2fPl3933++fZy/v7293x3+HZ48v56fP1+ff72d+1t"
"7/Rs7XZ39nd2+334c/x38ep7e/x7/Ppq7ft89Xl29314/vhweu5373f79fx5+Xn0/Hdx+Xt57G7u/vdz+Xx+9nh2+/R3/Px8+vRv"
"93n3/vJ7cvF2/P1v8fpx7nv7/Pt+9Xxzef78+Pxy8Pj+ePl8dP337Wz9/W/1/PL2eHTufvh+cvl3eXn58P1ze3j2/f/8/v37d+/5"
"eu12/Xdue/X5fn75c3159Ox4ef/6b/Z88e5ufXz7+Xx7ePv8dvv1fvP8evRt/fZ5fGv66fN0/n7qcff7ce5k73d76GZ6+HXseO75"
"7/B1ffv1anFv/vBv/fJ973p1/vBt8fRy+PTy7u136HJ4/GBqVFH339XV5N7jd/9uY2psYWVo7+l98nT47/H14fj+/X1ufPlidv/y"
"8utz3/5sbmdva3t47vPk7On2fP18cPx7d3pubnL/ce3td3v1/O31dfnvfnl2ePLveXDxcHb6cnv+fvf3eP799fpvcW1z+ujs7ebf"
"4d/vcudrTUlx6+DR0MXB7T5F3mbpwrKpqrtO01EsJy08UN1b07nTQz9MT1hqZ8vH3Gb9+15fXGBqa2Nv9O3u7Ph4bnBveO7k7evh"
"3eLgc/voWkVN3ujWz8q9xEg2Y9ZYzLiopKtsWckxJCQxQuJtV7i7TjpDTU9lVtu/zXJq6WNgWlpmbWBm/+7u7PZ+bWtrcPPd4ere"
"09nc72fdZD875tHcycW4tXItOsBb6rajmp25OLo/HRohNF3TPruq3TM1QEZdTV+4vONc6WRbVk1ea11Zdfjo5+T9e2RpaXrx2d/s"
"3s7Y3uNp3mM9OODL4si/tbNrKje6Wnq0oZibujO1QBsXHzNu1zi3qOEwMz5EZEVktLvnXuhbW1RNZW5ZWG569eTf53hiXGZ69OnT"
"2O/ky8/X1mreZDkt/b3dv7muq+chKa/3V66aj5KvKaxJEw8XK3LSLK+ezysrNT1dOl+rs9te9lBWSEf5fVNQY1588d3c4GRdXm/7"
"4uTW0/5tzcXazvjkdDorSrbWxrauqc4iIq7MSK+ajo+qJaz5Ew4VKWbNKbCdxywqMz1nN1qrstlebU5YR0bp5VtSX15rfufa3HtZ"
"WWX05tvc0NxeY8TB1Mze11YwKHWz6sKwqKlWHSylbWmqlI2RyyejLw8NFy3NRyekoFYoKTNOXDLFqLbcUlNNUz1L3eVdVllfb/7f"
"19x+Xllk/vHi29viY13Rv9XQ0M9fOyw/uNruuq2szComrblMtp2Rlasosc4ZEhYrW+gnwaLHMyozUeM7Wa2zyFdKTlhDQ+Xc5WBc"
"V21p7t/Z6nJaWWTv6NvU3vxYWtm7zc7JxmI7KzO6yVnAq6rCLiK0qVC7n5CToCrQrh0SESM/3SVDobY9KS1M0j9Br668cUJITkY9"
"cNfU8FxTWV1w39XY52laWGL13tzW3nRVWnfDw9DMxNtANS/fvWH1t6y7RyhEpcfWr5mVnVsvqTgbFB03fTQpr6zjNC0+31s7yK65"
"yExJTEo+Tt3X1vxfU1dX+d3S2OFmVk9WbtjR09f6Wk5Z4brBycfFWjkuMr/AWc2vrMozJbuk5L6kk5aiLGaqJBYSIz1qJTils0wt"
"L0zaPz6yrrnbRklJQDph1s7YfVpST1T11s/U42dYUVdv2NLV3PtdUVjtvcLMzMpmPTI0xsBe07SvyjooyKjfxKqXmaUyUKwqGhcm"
"PnIqOam1VTAyS95FQLixvNhLTExFPV7a09v+XlZUV/nZ0tjkZ1ZPVWzX0NPa+lpPV+u8wMrJxmE7LzHDv1rPsa3KNia/puPAppSX"
"oy1hrCYXFCU+ayc6prRNLjBM2UI/tK+620dJSkI7YNnP2XxaVFNY89jQ1eRlWFNadNrV19z6XVJa6L7Ezc3LZT4zNcjAZNa2sMo7"
"Kc6o3MarmZqlNUmtLBwYJj10LDWrtF40M0vjSD+7sbzTTU5MRT1Z3NDY7WVZUVRp3dPU3XpbUFBc5NHO2eJrVk1a3rzAy8/KWzsy"
"Nca/Z9y2sc87Ks+n0catmpumOEKsLx4ZJj5gLDGutOo4NE7dST7AsrvKTVFQRDxP483O4XZdUkxXetjP0t5yW1RVZebV0NrpcltO"
"Vv3KwM3W1ddIPjlRwtVc1LrCWTk6s7Pcwqygqb8ywcMvJCM5TkotYLbFXjxE6e1GVr+9wOlTXlRIRV3oz9bjfWZWVFp139fY4P5e"
"VlRe99jT1OX+YVVNYd7GxdHd0/BCPj7qxntYzLzWSjpcrb/Tv6imsVs6tl4vJSxFVTsvzL3OTj5Z1mpJ7MDAyF9aYFNJTGfe0Nzn"
"emVZWV1+4trb5H1mXVxl+d/c3er8a19WZejPz9vo3eZMR0l+0HtY4cbYVERbt8HWy7GtuPw/v243LDJMUD840cPeUkt93lxO58bO"
"2H3v91hOVXj47O7q9m5gY2r87+bm7P9rZWVvfOrj3+x0aXD/8/3a0NR1W15jYkZW0MroXfDT3EVAv7m+xLSprsVAyHoxKi5CPTo6"
"z75pVO3YbUpR5s79ctPY9VtncHVnb+3ucGlpaWhsePfv9/t7eXF6+O3q6+zr73dv+e3xfXbp7WZV/t90bvXQ2lpLa9pOTce5tr61"
"rK/KS8xPMCwzPTU4RtTUWt/H2Vtb6HteXnrfbWR+7HRt9u3z/Xx7c2xrbnR3e378fn1++/bz7+3u9X55eHlvbu/m8//n2ev58Njb"
"WFzwY0NG6ON9ybessLOrrLpU2VIvLC0vKy80O0U/SllMTVdWWnPv7Nza393k8/1wZ2RiX2BhY2dqbXN4e33+fn78+/fv7Onl4+Lh"
"4eXm7Pt3a15dXFhgbG/n19nPycvFvru5sK6tqquusLbUfUkzMC0qKy4uMzo8P0dLSVNYWXLt69zY3tzg7/p1aWZkYGFjY2dqbXJ6"
"fX79/n1+/fz17uzo5OPi4OPm5+99c2VcXlpZZ2183dfYy8rKwb27tq6urKqsr7G97Gg8MC8sKiwuLzY7PUJKSkxYV1767ubZ297d"
"5/P9b2hmY2BiYmNnaWxyeXl+/f9+/vv48Ozp5uPi4eDj5ebu/nZoXV5bWGRtduDW2M3Jy8S+u7ivrq2qq66wuNt5RDIwLSorLi40"
"OjxASEtJU1dYcO/r3Njd2+Hv+3FnZGJfYGFhZGdqbnV6/vz+fn18ffz27+rn5eLh4d/h5OXs/XRnW1xaVmBtdePV2M7Jy8W/vLmx"
"rq6qq66wtdH/SzQwLSorLi4yOTw/RktJT1dWZ/Lt3tfb2t3t+nJnY2NgYmNkZWdoa292fPv6+fx9eXp7//bv7Onn5uTl5OHh4+Pr"
"+XVmW1taV2Nyfd/U187KzMW/vLmwrq2qq66wttV+STMwLSorLi4yOjxASExKUldVZ/fv3Nba2d3r+nFlYWJgY2VlZmhnaWxweP74"
"9/n7fHh3eHz68e3q6Ojo5+bk4uLi4+z8cWNbXFpaanX23NbZzszNxb+8uLCvrqyusrS+6W0/NDIuLC4wMTg9P0ZNTU5ZV11z9+vb"
"293c5PB9bWVkY2RmaGlpaWprbnJ6/fn4+f18eXh6//fw7evq6urp6efl5eXo8H1vYl1eW15uee7c2tnPz83Gwb65s7OvrrK2ucn1"
"XD03NC8vMjM2PD9ESk9PVFtaZX30593e3t/r93ltZ2dnaGpramtra2xvdHr+/Pv8/3x7e3769O/t6+vr6+vq6Ofo6O34eGxhX19d"
"Z3N+6Nzc19HSzMfCvrm2tbKzt7q/2XlOPTo2MjM2Nzs/RElOUlJbXV9u+u/k3uHg5e/9c2toaGhqa2xsbWxtb3N4fP37+/1+fHt7"
"//v18e/u7e7u7e3r6enp7PR+cGdjY19lb3vw4t/d1tXRy8fCvbq5tra5vL/O8l1FPjs3Njg6PEBFSk5VVlpfYGp89uvi4+Tl7fl4"
"bmpqaWttbm9vbm5ub3J3e//7+/3//////v5+////////fv////////5+/v9+//7/////////////fv/////+/////////v////7/"
"fv7////+//////7//37///////////9+/////v////7/////////fv//////fv///37///9+fv///v//fn7/fv/////+////////"
"//9+fv///////35+/////n7//////37////////////+/v///35+//////5+//9+/v//////////////////fv/+/37/////fn7/"
"//////////////7/fv///37//37+///////+///////////////////////////////////+////////////////////////////"
"////////fv///////37//v///////v//fv/+//////////9+/v///n7///7+/////v////7+//////9+////fv9+/37+//5+/n7/"
"///+//5+/n39ff5+fu7d6uHf7mxr7unm7u3h4ePc4+Ln3HnydNre3VA919tt/sfL7NXPz+jRyd3o6tlv/eneYU3b32x1zdvr3c/g"
"/d7j6G/89/3z3+nf19fZ3dfa4tny+Ofufnjp4ODe1NnP2Nnf4N11+ubnbfvm7vfy6e3p5+fb39/08t/9+t/v7uPh4PXd3uTi6+Xs"
"7+Lp4t7e49jd6t7h5fjj5erq3drf3s/h5tDv8ube+OTZ4d/g3eb45+J07+vp7PHY39/l7d/nd/ri7eja1NXZ2dXW2dzi397f5O3d"
"6Xbg5/Xh0+Xp09787N/+/Ory+e3o6d3a3tvY4Obn5H17+WX7d23439z36Nfj7d7h+eTk7ufx39/l4ejd3d3s3dZv5+Lu+nnt8ex6"
"69jm4d3a833n8Hpx6OXf29nb2drpfN/zbevf3+nf2dzf4N7c6u/t6uvk5OLj3eXn3eHn3Nzm4Nzb6+fj5PL74+7r6Orj7e/u7PHr"
"9ure5ePq39zl7O7u6PDn3d/o2trg49/h3+bp3fDf4uXr6t/h3+ne3eXl3+jq5+z36+zq6OPg6OTc3ufq5uft5+LoeHz4eHT++m3+"
"9Hh+6+774uDq49nm59/ecu/5cFdmdEdpXlFP32lRxufWxsm737q/yNbAwTe5Sz1aR0M3XT1FS0rlSnfMc97axfXcyvfo6+5kZ2Rg"
"W15fX19raG52fvJ5/fL3fPf6/n79ff19e35+eHX/e3R273746+Ps4Nze3t3f4PPy92NZW2xCXVRKWXJ2Xcjbz7vavrnNy8a6O8vQ"
"N09ISjRLTTteWE3Z593hxNTox9rg7ORval5eWltbX2VmaHf3efjv8//1/Hp6/HJ7/P38+vvz/X35eHNvfG90/+zw5eDZ2tja2dbs"
"a+NqTE5hTTt3UUlX1dV+ur7Ur7jOvrbANLvuMj1GQi1DRjZXakjgysVhv73d1M3Pa3JpWVZZTVRaXFljen55+ezs/vr7eXVwdnF9"
"/Xv18/T1/fl4fWxsd/xw/eTZ4tvQy9jbztj9/l5RUkxBOt9FPVrFz123ssCtscSxsbkvvsYuMjZBKjY6LknXP1LLtt/SusjE0Nhk"
"6vhNRVdTTUtVX2Vsbvjn5/H47u5vb3B4bnP///Du9O7w9W18cGdfc/j4fNzSzc7Oy8TC4WbU1UdAP044Tzg12MDcRq6ktK6nt6aq"
"rS3trS0qJTYmLjMiLMV7RE24tsGy1Mi/vGxGVWFLRT9BTFZWT2H53+nm7d/p8HF1cXlvbXr07fnu+O9ycmNuZmFe+N/Z4NbFvsPN"
"ycjIzFk5U1lKKjg+Qc9tSl+inrayoKmdp60pX6cuJxwoHy87Hh5Rx81sycW/prbMXcXKeVRAOT9MSUJET1pz8+j+5uDi8/F2fnt5"
"e/198nr2e3RlXmFrcGhr587Hv8jKwbS5zXRxR95XNSUt7zhSPkdcpJqqw52enJyp4yKj5SkcHB4cPysaIuXJvtG38K+ls89ayPVx"
"TTszPUhJP0VNYe3k6e3i3eDz9Xj8dPn9ePj+/npxZ11iaWxkberRycDHycC2ucje5k5x/jgrKmM4Q0dCULGdpb+lnKObqK8nv68r"
"JBskGyw5IB5Bzr/Pub/JqK68W9bdXFJCNjdBSERCTFd56eDq4+Te6/t1cnNyfn378/D19HlpXmFhY19k/dnLxMLHv7i0v87Y+FDb"
"Py8oOk8xSz5P2aOer7abn56eqtcpqE4qHh4fHDcvHiRU1r7Hsdu7qq++fNNhXVxFNjo/R0VKS1Ju5eHm5OXj6e5tb215e3v6+/vs"
"7vJubGhnaWZka+XTz87KycC9vM/r39xZWj4yLz92NUJA/ciqo7W9oaGjoq/ILLLVMCchJB82PCcmPlHEw7TT1LK0uMzRXlhpZkQ/"
"P0VKUVdVW2/u5uTl6Pfu/3lyent0//P9+fP8e295ZmJgaHjl2dfaz8fCwMTL+9/S50hANjc4ZkgyQU+/uaerwLKfpKKrtEs6r0Y0"
"JiUhJD46KC08Sce9s+XLurm6v81eU2dnTEpERklVW1xcaXft6Obv+vl6eXf+e3r8+n759/j9e3pqam3+8OTf3tvOycnN0dfs191b"
"QD4+QUJjPTdH7ry7rLTGsaOnp7K7SES2RzoqKCQpPz8tMz1D0b+10szCw8C/yPRaXl5TWE9NTFVbYmZwc/r27+/2+Xd3dHl+/H3+"
"fPn29fd9dW9udnf48+zk3dfPzdDX2NPf2vFSQEFGSURPPjZL57u+sbbGtaanqbC9dj27Xj0vKScnOEU1NT88csu3wczFzMrDw9P8"
"X11RWllWUlZZXmZxdXn7+vb39npxbnZ3/Pj8enj++PTv9v92eHd7/vbz7eXf3dvY2tvZ0+PsYlFIS1RQRko/PFfbvsa8vcm5q6uu"
"t8PvRMNkQzcuKys6Sj4+Rz9d173CyMjV2s3L0t55ZFNZW11eY2NjZGhsdPrx7/b9dnN1/vj09Pp+/n7+////////////////////"
"//9+/v//////////////fn7+///+/////////////v///n7////+////////////////fv/////////////////////+////////"
"/n7///7+//9+/////37//37/////fv/+/v///////35+//////7///////////9+fv////9+fv////////////////////9+////"
"fv/+/n7///////////////////7///////////9+/v/+fv////////7/fv9+//////////////////9+//9+/////v9+/n7/fv9+"
"////fn7+///+/37////////////+////fv///////v9+/////////////v/+///////////+fv///37/////fv//////////////"
"/v///35+//////////////////////////9+fv/+//7+//5+///+/37///7/fv9+//////9+fv//fn5+//////9+//9+/37+////"
"//////////////7/fv////////////////9+//9+/v///37+//7//////////////35+/////v////////////////////////9+"
"///////////////+//////7//37+/v///37/fv//fn7+///////////+//9+/////v///37///7////+////fn7///5+/37/fv9+"
"/v///v7///9+/v///37///////////9+////////fv/////////+//7+/////////37//////////v7//////37/////////////"
"fn7/////////////////fv9+///+//////////9+//7////////+//////////////9+fv///37///////7+/v7///////7//v7/"
"//7/////fv7//v///v//fv9+/v//fv////7/fv9+fv////9+/v/////+/37/fv/////+fv////9+//////////////////9+////"
"//////////7///////7/////////fn5+/v///v/+/n7//////////////n7///9+/////////////37+/////////37/////////"
"/v/+//////7//v9+////////fv/////+/37///7//////v7/fv//////fv////////////////////9+//////7////////+/v//"
"//7///9+/////n7//////35+fv/////+///////+/////n7//////////v//fv///v////7/fv////////////////7///7////+"
"/////////37+//////7///////9+///+/v7/fv7/fv///37///7///////////////9+fv///v///////35+fv////7////////+"
"/////35+//7/fv7//v///37///////7///////5+///////+//////7////////+/v/////+//9+/37+/37///5+fv///37//37+"
"/37/fv///////////////////////37//37///9+//7//////37//v///v///n5+///+/////////v9+fv///37/////fv7/////"
"//////////////////9+////////fv/////+//9+//////////7///7+///+fv7+//////9+////fv////7//v///v9+fv//fv//"
"/v///////////n5+/37///////////7//////v5+//////7+//9+/v////7+//7/fv//fv7//v///////v9+//////9+/v//////"
"//////////////7+//////7+/////37+///+//7/fn7//////v//////fv//fn7//v9+//////9+fn5+//////////7/////////"
"/////37+///+/n7//////////v7//////35+fv////9+//////9+/37//////////v//////fv///v/////////////+//7//v//"
"fv////9+///////////+fv////9+////fv/////+/////37//////////////////n7//////37////+/////37/////////fv//"
"///////+/37/fv////////9+/////37///7////+fv////////9+/37+//9+///+/n7+///+/////v///37+//9+////////////"
"//7//37///////////9+fn7//v//////fv9+/37//v/////////+///////+////fn7///9+//7+///////+fv9+///+/////v//"
"//7///9+//////7///5+/////////////////////v///////////////v/+//////7////////////+//////9+//////9+////"
"//////////7///////////9+///+//////////9+/////////////////37/fv///v////////////7+/////37+////fv//////"
"//7///7///////9+////////fn7//v////////5+/v7//v7//v///37+/v///v7///9+/////37//////////v////5+fv//////"
"/////////////////37//////////v///////////v9+////fv/+/37//v//fv///v////9+/////37//v///v///37/fn7+/37/"
"////////fv/////////+/////v//////fv//fv////////////////9+/v///////////////37////////+//////9+///+fv//"
"//5+/////////37////+/////////////37//v7///////5+/////v////////7////+fv///////////////n7//////n7/////"
"//9+//////////////////7//37///9+////fv//////////fv////////////////////////7////+fv///37////+/v7+//9+"
"//////////7//v7///7///////9+//9+fv/////////////+///+fv9+//9+/37///7+//7//v///v////9+/v//fv////7+////"
"/n7//////35+//7///9+/n5+/////////////////////////n7//v//fv7////////+///+/37///7///////////9+/37+//7/"
"////fv////////5+////fv////9+fn7//////37//////37/fv/+/v/////+fv//////fv/+fv7//////v9+//5+////////////"
"/v////7///9+/v////9+//////7///9+///////////////////+//////9+/v9+/////v///37+//////////7/////////////"
"/v///37//////v9+//5+/35+/37//n7///7//////37/////////fv/+//////7//v7///9+//7//v9+/37/////////fv/+////"
"///+/37/fv///37//37////////////+/////37+/////////v/////////////////+//9+/v/////////+fv9+///////+///+"
"/v5+//9+/////n7///9+//7///7/////////////fv////9+///////+/////v9+//////9+//9+////fv/////////+///+//7/"
"/v/+/////////37////////////+/35+fv///////v9+/////37//////v//fv7//37////////////+//9+fv/+//////9+////"
"//////5+//////9+/v//////////fn7///7/fv///v///v////////////5+////fv9+/37//////////v//fv//fv//fn7/fv9+"
"//////7/////fv/////////+//7///////9+/v7//n7+//9+/////////////37///9+/////////v/////+/////////37/fv9+"
"/////37//////35+////fv7+////////fn7+fv9+////fv///////37+//7////////+fv9+/n7//v///37+/v///v/////+////"
"//9+/////////35+/////35+fn7//////n7//////37///9+fv//////////////fv////////9+////////fv9+//7/fv///37+"
"/////v9+/v//fv7//v/+////////fv/+///+/37/////////fv9+///+//9+////fv//fv////////7+/v///////35+///+fv//"
"/////////////////v///////v7///7/////////////////fv/+//////9+fv7/////////fv////7//////////v/+////fv/+"
"/////////////37+fv//////////fv//fv/////////+/37/fv/+//////9+/v7+/v7/fv//fv/+//////////////////7///9+"
"fv7///////9+/35+//7//////////////v7///////7////////+fv///n7///7/////////";

// Spoken phrase: Want
#define WANT_AUDIO_SAMPLES 7656
const char WANT_AUDIO_BASE64[] PROGMEM =
"+e3m4N7c3N3g5e38cmhgXVtZWFpaWl5gYmt5+evb1dDJxcO/vr/DxszffV1LRUM/PkBDRUpRVFxqdP/t4+Dd2NfV1NTX2t7o+m9k"
"XVpYWFlbX2Zv/Ozj3trY19ja3eXxdWRcV1NQUFJTV15ha/rs5tvS0MzFw8G+vsDDxtTyak5EQj89PkJDRU1PUVtjaXvo3trRzs7N"
"ztLY3+5yZF1ZV1dYWl1ianX36+Pe29jX19jc4ex0YVlSTUxMS05VW2bz4t3Vzc3Kw8LAvby+v8HN6m9PQj8+PDw/QUJITEtQWl5u"
"5tjRzMjJyszQ2eT7aF5bWFZXWFlcYGhz9ejf29jW1dXW2dzh8G9fVU1KR0RFSEtTY/Xf0srJxL69u7e2t7q8yORiRjs4NjU2Ojw+"
"RUtOXvHZzcbBwcLGztrzZVhTUlJUV1pcXmJlaG52e/369vr27+7m3tnSzsvKyczU4WlNRT47Oz1BR1ft2ci9uLOura2usb3ZVTsw"
"LSwsLjI4PUtq3ce8uLa2ucDPfE5CPj09QEVMWHfbzsbBwMHGz+VfTUU/Pj9CSlNs3tDIwL27uru/zOlNPTczMjM6QU7gxLqvq6in"
"qKy520YwKScmKCw0P1TRvrixr7G3wNxMPTg1NzxDTl/o1MvFwcLGzNfo/nj++H5nVktEQUFFSlJf89bIvbeztLjEfkQ3Ly4wNjtN"
"3MKzqqWioqWuykouJCEiJCozQlnOvLewrq+2wnY/NTEyN0FRa+jb08zFv7/CzN5qXWP74t/uXUxEQkRKUVlbYnnbxbm0s7bFYj82"
"MTE5P0Rr0cOyqaOgn6Kw2j4oHx8gJS07S1vPxL+2sLK5yFY5MDA0P13d09Tb3dLHv7/G2l1LS1vcycfSXkQ8PENQXltNSk13v6+r"
"q7DKRjMuLTJCRkrez72qnpqZmJ2yaS0bGBkaHyxDW+G/xsCyr7G42ToqJykxW7+3t7/W+N7Iv7/RTTk0OVXHubfA50pCSV/d3lQ7"
"MjI757Ssqq69a0hJRENMNy43Qs2pnJqbnKTKaC4dGxoaITrHtayuy9Xdc+TwQjArLTfgsqytufVBPUb3y8XYVkA+R33FvL7UVEJI"
"/b+2t889KycpPMKwrK+929O6vMfcKiAlL8GdlZaanbplXCAaFxMZMbWkn6T6OjQvQl5HODU6XbWoqrDdOS80Qt/Dws9tTkpSfNbM"
"0OlUSEZczLm0t9w8MCw0WcjAwdd2xqyurconHyEwsZmVm5+tQsM1HxwVFyu0oZ6mSyksKkfccUpNW8qyr73cQjY8Rn3RyNHcY1dP"
"X/nSztJ9WUpIUOXGurTFeT0xNVZu3F5MPq+jn55WHRkdSZqPk52kM79lHhsPESimmpmrKBoiJdG3ytPq0sG20lI+OkF38tfY2dvn"
"X05OXN/S0OFqW19hXm5h18G+xc8+QUhgUVU6N6+pnJ9PHRsfuZaPnaHYM6spIxYQHbucm6Q1GyAqR6/Ry+HLx7zcREE+TOrg+un7"
"3dzjXVdRbN/O1+JaWV34a3taZsm8vsBUOUFFW0pSLsConpmzIhkaNpuOkp+mKrT3HxsOEzCemZvCHhomLLi72M3Tv76+SD87QmbU"
"8vBu8NnZ611UVGbn2NfjblpccOp9elh3yr7Cx0o+RE1YT0o1t6ufns8gHB7wmpGapbUsrTskHBEZTp+dn2wfHyo0tsjO0s/GwMxF"
"RT9N6dlxd1zx39TsdVdYXvPb09n7X1Ff99r79VL9x73CxkI/RFFMTj43raecn0ccGh+2lY+codQwqisiFg8awpyaoDsbHyg+sNHH"
"2MfGvN1AQT5T2ddta1f03dLpdlZXXvbf1938YlVi99nz7lVlz8DGxU1ASE5UTks3tquhoOogHSDQmpKcprwurjgmHBMbY6CdoVgf"
"ISs4tszN1s7Jwc9FRkFR4tZxcFl34Nfhe11YX3Pe2NfvalRcb9nl711U28HAwug9R0VaRlgz2amknK8nGxwwn5GTpKYvyL4jIRET"
"KKecnLAoHCcrzbraydbExb9OQz9HbtHta1xe6dbY72RXVmT439jc7mdYW3vd3+1jV+HIxcbgQUpHWEpdOuqsqZ+vLR4fL6aWl6ip"
"Nd66KCcWFyitn56vLh8qLei748nWxcjEUUVETPLS6mZWWH3e1+X9XF5cePLZ3Nx7YFdd8NXT6mdNaczAwchDRUBORlw+S6upnqg1"
"HB0mrJWRoaZLO6wsJxcSHbyenKU5HiUqRrjjxs7AxcBaPkBG/srTbFdNX+XU1+NmWlZdbeTd2eT8XVpb+NjP5HRNUtzFwsFdREVG"
"TVBZPbetpqXqJB8k252Vnqy8LrBLKyAWHEKmoKLVJyUuNb/W1c3Hw8TaQEFEX9HL7mROVWnd3Nv5alxeZf3i3NvqcVlaXOPRzORn"
"SE/bwL+/VkBARE1UVj+yrKOkVyAeJMOak56qxi6tPyseFBtQo5+ifSQlLDa+4tHNwsDA4j4/QWTNyeZkTFRl3tzb72tdW2N25d/c"
"7f5eXl302s7bc1BLc8rBwNdERkFQTe5D1LCuprovIiY4qJqbrq8337svLRsbK7Smo7M1Jy8xdMpvzs6/xMdNP0BM4cjS7FhOV2/k"
"3N3tb15cX37m19zla15WYHzYzc39V0VK3r+8vvo+Pz5OUuhCv66rpsspISZPoJidr7UvvsYvKhgaLa+korcwJi8v4tJpzsi8vsVG"
"PDtI3sPJ1l5NTFh139XZ52dXU1x73NjY7WxZWl7v28/R3VtNR1HTv73Adz4+P1Bc2E/DtLGszy0nK1alnKC5vjLHvjoxHR0tuqqn"
"tzstNjRj7FXWybu9wk0+O0NzysrN5l1RUFZo3tTR3WtST1Ft3MzN1m5VTVRq3M/N0eVZSEZK2724vNI8ODhJX8zp3LO2rbw0Jio7"
"q5ycs7w7R69DOyAbI+esp6xZLjU0QuVHetG6uLjsPjg6TdTGx898WU5NUm7czc3ZaE9KTmXdzs3YeFhPVmzj1dTY529UUVBvzL7C"
"zEw7PEb8zMZ+wcW+vFAvLzfEpp+sutc6u1g5Kx8mQ7iysc9BRldHZj9AWM6/vspcU1Ng+Olvdmx0b3Frdfvt7vp1amtreP3t6+nt"
"+XludH7w9vX76ubd7G1scfnjXEdIUNC8t87bU0zabD88P96qoKSyu0HU1TYuISMv28O/y37ivtVtRzhCWdzk4WNu3NXd6mtnd/16"
"bm1u/Pf8d3NscnV4ffn49u/z+n14e/Ls7fN4cv3zbmNaW/3Z29rs59LGzG1RPEZkenXs6b2tqa66zEzNUjovKSw6W1xjbOfIvMzd"
"XlFlcGVRUlRl7u95fv/w7fZ4dnp8/f99fv55e3p5ffr4+Pf69fPx7/T59/p1ZGJoZGJkZG5wYnP1cV9w7Oja3My+urqzrK2sra6w"
"tsl0dz40Ly4tLS0tNzo7QVJaXGxu5eP/+t7c6ODb1tfc3dvf8nNuZl1YVlhYWFpfaHL67ePc2NjY19ba3+vw/2ZaWltYVFZbYWFZ"
"ZP9zX3Dr6NnbzL66urStrqytrrC2yXN8PzQvLi0tLS03OjtBUVpbamzo5H753tvk3djT1Nja2NzseW5mXFZUVVVVV1xia3vu5d3Z"
"1tXV1dXZ4Ovyd2FZWlpWUlZbYl1aa/5rX/zs4dnZx7y6urCtrqyur7K52X5dOjMvLiwuLS85OjxFV1hea3Lj7Hfu297j29bS1NjY"
"197veW1hWVRTU1JSVltfaHns4tzY09HS09PW3Ojze2VZVlhVUlJXXWJaXnz8ZWvq6NnZ0cC6uretra2trq+0vfTwTDcwLi4sLSwy"
"Ojo9SlpXYWf74vl45tnf3tfRz9HT0tPc7P5uXlVQT09NTU9UWV1p+ubc2NPPzc3Oz9HW3/N2aVxST1JRT09WXGZcYvfvbW/n5tjZ"
"08K7urmura6tr6+0vOn1UjgyLy4sLi0yOjo9SFlWXmNu63hp+t7l59rTz8/Qzs7U3OTubl1VUU5MS0tNTlBXX2315NrTz87My8zO"
"0dXa5nlkXVhQTU5RUFBWX25tZPbj7W/s4N/X28y+u7y1rq+ur7CyuM1vfj82MTAuLy8vOjw9Q1RbWWNiff1kZfPq++nd1dPT0c3O"
"1Nnc5P1mXFlUT01NTU5PU1lhb/Lk2tXQzs7Ozs/U2d3k+2heW1ZQTlJVVVZdaHpybezi73Xo5N/b3czBvr+2sbOxs7S3vdx7Zz44"
"NTQxMzI1Pz9BS1xcX2Nm93Riau/y/Ovf2NbX1NDS2Nze5/1oX1tXUk9PUFFTVltkb/bn3djU0tHQ0NLX29/m+WthXVpVUVVYWFhe"
"aXd5bvLj7Xbt4+Xd4NHGwsO8tbe1t7i6vtZ0e0Y8OTg2Nzc4QURES1xfYGdl/n5maPnx/fDm3Nra2dXV2t7g5vVvZmBcWFVUVFVW"
"WFxia3zu5d7a19bV1dbY3N/l7H5rY2BdWVZZXFxcYm16em/x5e537ubo4eXWysfIv7m7uru8vcLYdf1LPzw8Ojs6O0VISE5eZGRq"
"Z379amj/8v/16+Dd3t3a2dzg4+fweGxnYV1aWVlZWVtdYmlzfv/+////fv7/////fv///37+//7//v///v9+//9+/37/////////"
"/////37//////v9+//////////////////////////////////////7//////////////////////////37//37//37//v///v//"
"//7///7///9+fv9+//////////7///9+///////////+///////////+////fv///////v///v7//37///7//37////+/v7///5+"
"/////37//v///37/fv/+//9+fv///v///n7///7///7+//////9+/////////v///v////////9+//9+//////////9+////////"
"//9+//7/fv////////5+/////v//////fv///v7////+//////7+//7/fv/////+////////////fv7//v///////v///37//v//"
"//////7//n7/fv5+/n7+fv5+/n7/////fv5+/f/Z1tz2ePZrb1/r3uXb8/B55tba1dHS0M3Nz9bW3vNu9Pfq3tnN1NbU19zc3t7r"
"7e/g6efe5Ojs3vXv6fPv3uHm2dzs9/n9Xvbg7eHe2eHf5d/+eefi3t7S2dnZ5tbU7t7Y2d7f3eHj+fTh5e/u5N72euT9efjY3OHi"
"9d7qb/zUe+be3dzx9OLr+OTh5N3Y3dzm/fXfftnf4+Ta8e3sZ/Z4/e7ibObg9Nvd7OX64Pzt3tzYZ+b0Zvhm69zfd+7e7+vg23Zk"
"a270beffbNbT6NXJ1dXT6dzwZNnQ7X3X2vhU4c7rd2nT3G5X5utrUG/XeODbz9BsZub16XF7z9Hp3M/f/9jr331o5nrl2nN64PZy"
"1t3Z3fLW293q3u7Y5H363uzg3dzY6t7i32zvWm316u192+Pc6uPX2+De9NLd7PHi6nLg09Xd3eLi8ejpeW3oaN3p3NTa3Nzu9uPu"
"2uPm6dzn3u3h5t/k7d7W3d3f6O9q7uHg5tfm2eXs09Lb2+Zq7+/e3+jd3tzU2NbV2+Pv7uXt4tnW3+/y7eN069rU3uDa2uV99+py"
"b+vg1t3d39vvd/p7/Hz8fP19/X7+fv5+/v/////+fv7//37+fv7////+////////fv//fv///37///7+/////////v7//////v//"
"////////fn7///////9+fv7//////37///7/fv///37//////v/+//9+//////////9+//9+////////////fv////9+//////7/"
"////fv////7//////37////////////+//5+//////////////7/fv9+//7/////fv/+//////9+fv///////////////////37/"
"/37+//9+/n7///9+////fv///v////9+/37//v///////v/+//////9+/////v9+/37//////v7//////////n7//////v9+////"
"///+///////////+//9+///+//9+/37//v///n7///////7///////////////7//37+fv9+///////////////////+//////7/"
"/v/+fv////5+/37////////+/////////37//////v9+/////37/fv////////////////////////7////+/n7///7////+////"
"//////////////////9+fv//////fv7/////fv///37//35+/////////v////////9+///+fv/+/v////9+///////+///+////"
"//9+/////////v9+/37+/////v//////fv////////7///9+/////////v/////+//9+/37//////37//37//////////////v//"
"//////7///5+//////9+/////////////v//fv/////////+////////fv//fn7///7/fv///37/////fv/+///////+//////7+"
"////fv9+////////////fn7//37//v9+///+//////////////7/fv//fv//fv//////////////fv///////v////7/////////"
"/////37//////////37////////////+///////+///////+//7////////+//////9+//7//////v///////////////////v//"
"//////////9+///////////+fv7//////////////37////+fv////9+/////////n7/fv///v/+/v5+fv5+//9+fv////5+////"
"////fv7////+/37/fv/+////////fn7///9+/37//v9+//////7//v////////7///////9+/////v7/////////////fv9+////"
"//7////+////fv////9+//////9+fv////////////7///7//v///37//v9+/////35+fv/////+/////37///////////9+////"
"//////7+//////9+//////7////+fv/+/37/fv/////+/////v///37//////37//////v/+/37+fv///v7///////////////9+"
"/v//fn7///////7///7/fv////9+//////////////7//37+///+/////v9+/////v7////+//////9+////////////////////"
"/35+//7//////37////+fv7///7//////v////////7///////////7///9+///+/////n7//////v////7+//////9+///+///+"
"/////35+///////////////////+fv////9+/37+/35+/v///////v///////////////v/////////+///+////fv9+//9+////"
"//////////////7//v9+///+//7///9+/////v///v////7+/v//fv///////v//fv9+/n5+///////+///+///+//7//v/+/v9+"
"/35+/v////////////////////7+//7////+////////////fv////////7/fv///////37///////9+///+/v////7/////fv7/"
"//////7//////v////////////7////////////////+//9+//9+////////////////////////////////fv7/fv/+////////"
"///+fv///37/////fv7/fv7+///+//7////////+/v///37+/////37//////////////37/////////fv//fv///v5+///+/v//"
"//7/////////fv/+////////fv/+////fv9+fv9+//9+/v////7/fv5+///////////////+/////////////v9+//7+////fv//"
"//9+//7/////fv//////fv//fn7///////7////+////fv///v9+/////n7//v9+//////////////////9+/////v////7////+"
"//////7/fv/+///////////+//7+//9+//////////7///////7///////7//////v//////fv/////+//7///7//37//v/+/v/+"
"//////9+/////v///37/fv///v/////////+/////n7////////////+///////////+/////v////////////////////9+fv//"
"/v/////+fv///v/////////////+////fv/+fv///////////37/////fv/////////////////////+//////////////7//v//"
"/////////////////////37+/////////37/////fv///v//////fv////7///7//////35+///+////////////////////////"
"///////////+///+///////+//////7/fv////////7+//7////////////+//7///7+/////////////////////v///////v//"
"/37///7///////////7+//////9+//////////////////////9+//////////9+fv/+/////////n7//v7/////////////////"
"/37//////////v7//37//v///v///n7+//9+//7///////9+//////5+//9+//7//////////////v/+////fv///////////37/"
"/////37///9+///+/v////////9+///////+////fv7///////7////////+///+//////////9+///+/////////37////+//7+"
"///////////////+/////37+///+fv//////fv///////37///7///7+//9+///+/////37////+fv////9+fv//fn7+//////9+"
"////////////fv///////v7//////v9+/////////////////v//fv///v///v7//37/fv///v///////n7//////n7////+fv//"
"/v///37//////v///////////v////////////7////////+/v/+//7///////////7/fv/+//7//////////////n7//v7///9+"
"//////////7////+fn7///9+/////v7//v///37+/////////37/////fv9+///+//9+//9+//////////////////9+/////37+"
"/////////v7/fv/////+////////fv/////////////////+//////9+//7///9+//9+///+/v//fv///////v////7/////////"
"/////v////7//37/fv///v//////fn7/////////////////fv///v////9+///////+///+/////////////v////////9+//9+"
"fv7///7+/////////////////37//////v////////7///////9+////////////////////////////fv/+//////7//v//////"
"//////////////9+/37//////////////////////v////////7//////v/////////////+/37//////37/////fv////9+/35+"
"////fv5+fv///35+//9+/37////////+//7//v//////////////fv////////9+/////////v7//////////////v//////////"
"/////v/////+//9+/v///v///////////v////9+/////37///9+////////////fv//fn7/fv//fv9+////fv////////////5+"
"//////9+/////////////v/+fv//////////fv////9+/n7+/v///37//v////////9+//////7//37///////////7//////v9+"
"/////n7//n7//v///////////v////////7//v/+//7+fv///37///7///7///7+//////7//37//37//v//fv///37//////v//"
"//7/fn5+//////////7+//7//37+/37//////v7////+//////////7+/37//////v/////////+//////9+/////////////v/+"
"//9+///+/35+///////+//9+////fv///v7/fv/////////+////fv///v////7///9+////////fn7//v///////v//////////"
"//9+////";

// Spoken phrase: What
#define WHAT_AUDIO_SAMPLES 6641
const char WHAT_AUDIO_BASE64[] PROGMEM =
"+/Dr5+Pg39/f4ePo7vh3bGZhXlxcW1tdXl5kaGhv//bs3dbTy8bFwb/AxMbL3ftjTUZEQD9AREVJUVRZZWxw/e3r597c29fW19ja"
"3uTs+nFpY19eXV1dX2Fma3F+9Ozm4t/d3Nzc3uHn7n1sY1xYVVNSU1dZXWhue+zk4NvS0M3GxMO/vsDExtDscVNGREE+PkJERUtP"
"T1ddX2V47+nd1tTQz8/S1dri7nxrY15cW1tbXV5hZmx2++/o49/d3Nvb3N3f5/J2Z1xWUU5NTU9QVmFs+uLa2NPMzMrDwsG9vL7B"
"w83pdFNEQT89PT9BQUdLSk5UVlxt7uLXzs3My8zP1Nrk83VqYl9dXFxcXF1fZWt1+e3m4N7d3Nzd3t/g4+fs83BkXFNNS0lHSE1P"
"WG/q3dLKycW+vLu3tbe5u8XfaUk7OTY0NTk7PEJJS1h25dLJw8HBwsnQ3XNcUk5NTU9TWF5pfOzi3Nna2+DudGNbVlRVWF5q++PZ"
"0c3Jx8bGyM3Z9llJQD06Oj1BSlvk0se8uLSvra2usLrPYT8yLi0sLTA3O0VZ/s/BvLm3ub3I2GFMRUFAQkZKT1pp7tvSzcvLzdDW"
"3uf0eWxiXVZPTUtKSk1TW3Hm2M3GwL28vL/K4lA/ODQyNDpBTuTFurCrqKeorLjWSTIqJycoLDQ+UNW/uLGvsLa+2E49NzQ2OkJO"
"btfKwb29vsXTbE1DPz9DS1r21snCv7/F0HpOQT07PUBJVfnQw7q0sbO4yGE/NC4tLzY8VNO+sKijoaKlsdVBKyMhISUrNkVhyLq1"
"rq2vt8ZfPTMxMTdATmfi0snCvbu9xNdgSkNCR05aa/Pg1czGxcnYZUpAPj9ETFNbbeTNvrWysrjMUTszLzA5Pkfuzb6vp6Gfn6S2"
"9jgkHx8gJi49TWfIv7uxr7K6z0s2Ly80QF3k2NXY1MrAvb7H5U9EQUhZ+eTua19z2Ma+v8xjQzs6PktYW1BNUPvAsKyrr8dINC4s"
"Lz5ER9/OvaqfmpmYnK98LxwYGBoeKj5RbcPEv7Ctr7XMPywoKjBMy72+yetx3MS7usRqPzg7TdbBwddUQ0Rbzb26wvNGPT9O7Nh+"
"SDs5P9uzrKquw048PDs+TjwzQ1u9pZuZnJ2n0HAyHhwaGR8v8sOys8/LwsS/yU0wKiwyXrqzt8ZlSk/mysbRWkI+R3jJvr/QWD87"
"QV7Lvb7MX0dHW9PDzVA2Ly9Iuaumqr1SQlBTZ3MsJy9CrZiRlJqfx2DrJBsWERUk1K+nqN9T3+TH0T4qJi0/v62yve1LS2ra1+tc"
"TU5Z+NnS0tvyZ1pWW2Xr3NPW4nJkZOLS1lZCNTNSwa+or8xOW2/bvTknKjC7mpGVnaRaX8cnHhYQGDOwqqi3O1Td29BELSg0U8Sz"
"vNjjaPPqbFJQXn3m9GJec+fZ5GxXXP7d2PBhX+7UyND/VmVd9GU5NUXkrKOqxE9NQrTHKiciPZuNjZedW0KyJRoPCxNDo6Wp1S3S"
"weZKKSMt1764v1tjztPjUUVL6tDbaU5QXe/v/Xr8+vtt+tzQ0f1YTGzNws59TUnnz0tGOUezp6m/2zTur0kwJyWtkYyWndUzrCsc"
"EA0Xc6Gss08xu79LNCUrSLbAynNM0cLaVEFCZ87L6FxNV2z+fWpsfunp/f7p1MvbZElM47++0FY/TcntSD4yx6iisMg9OK7NMCoe"
"+JSKjpmpKq9PGhMKDyyhpa/KLdixVjkkJTW7uMzZSOS/yl1CPVLWx9toTVFjfP1ta3nv7Pd37d3O3HZNTu7EwdBbQ03O9E1DN8ir"
"prXLPTyz2jUtItSYjpOdsC2wRB0WDRU3pau02jHLt1U7KCo7vL7O3U7cxNJXRUNe0czsXE9bb+t2am706ept/NzN0u1OSv3Fwc1b"
"QU7NdkxANr+ppLPOODyv4jMqH8WUi5GbtCytORsSDBM8oKmz7C/BtVI3JSg7ub7N303WwNRSRUNnzs5vVE1c8+p5b3vy8nBu6dHT"
"4FlOa8zH0GlGTdT5T0k5y62ntc0+ObfPOC8kZJuPlJ6tLrZYHxkPFjKnq7fRNNS0ZD4qKjnBvtLYUd/E0lhJRmPV0G9YUV/t6/xu"
"fu/2amnpz9PfU053x8TPX0FN0W1OQzjEqqW0zDk5s9g2LSHfmI2SnK4tsEcdFg0UNqSqtdgyyLJYOiYoOry+0txP2MDSVUdFaNHP"
"alVPXu3re2988Pdua+rT1uJXUHPLyNJeRE/UdFdHPMetqbnQOj245zovJs+bkJifuDCyQSAZEBlAp6667TfDuFc7Ki1AvsXY51PV"
"xdtVSUt709ZlWVNp8vFxcX7t+HFv3dDX8k9U4cbJ2k1CY9NVWTtJtaiqv2Yv8LhDOCgsqJOPm6FBTLImHRERH7mmtr1ARrDJRi4o"
"MPy61dRcbsbJbUxHWNzP9VpTXu/s/G988/xub+fX2vNWW+DLzuVNSHXcW10+XressMhQM9TCQzwrOKiXl6GqOt2/KCAWGCuzrb7I"
"QGq02kYvLTrWv+DdXevJz19OTWTd2m5bXHDr7f11+fpuaH7d199eWPvNy9ZWRFTbZmdFRL6tq73lM0e9VT8wK7malJ2j3TqyMSIZ"
"FR/cqrnBWkO2vUw2KzNVvtbob2zKyPtPS1ji1/BbWWPx7vxvfvX8cHLr2934Wl/lztDmTklq5Wx0RGC9r7THVzXry0xFMDutnJum"
"rUB1vy0mGxwtvLHGzExwtthHNTE/3cZ08GTgydFfUE9s3txuXl1t7fV7bv7z73Nu/dzd4GFZb9PN02RGTen961REzrauvNI6Pshq"
"STsuzKGZn6jIOrg/KB4ZI1uuvcr8S7u9TjsvOVjE3mdyb8vI71JOWeja6V5eYP3u+HFzfvH3dm754+DmZV502dLXZUtR8fvfW0zU"
"vLa/2D1F011WPzjHp5+oru5Hvj0tIyAu27jQ2e/+vMxFOzpM9M9eXOrZzdleVF/26PNsZnd5/nN7fvp6fv3v+/v96Oj4bGvr1dhk"
"Tk9Z2tdcede9ur5RPVVRbF08a7Cjpq2/RMhRLigjLlq+1nXPz8HLQDs/W2z8W1jVzdf1XV327ndibHnw/G9v+u/5dHL77/Rzefjm"
"5/1s9+LoblVOcdri3/XTxb/jSkxKa+tNY76urbPBUtldNy8tOFfO6GjPys3cSERQaGJZYnbW1Oxsb3z1dWppffX8dnV6+fn//v/+"
"fv///37/fv5+/v//fv//fv////////9+////fv/+/37///9+/////////37///9+/37////+/////37///////////////7+////"
"fv///////37///////7//v///v/+/////v5+//////////7//37//////37+/37///////////////7+///+fv//////////////"
"fv////9+//9+/////////v9+/////////37///7///////9+fv////7/fv//fv7//37//v////7///9+/37////+///////////+"
"///////////////+/37///////7///7/////////fv9+//////////7////////////+/v///////37//v/+///+///+/37//v//"
"////fv9+fv/////////+/37//n7+/v///v////9+/////37///9+/v/+fv7//37+//7//////v/+fv59/Hz7ed3V2+xy8m1xYP7c"
"6Nvs8//v1tnW0tLSzc3P1dbc7XD98+/e3M3S19TW29ze3eft8OPk7N7k5u7e7fbo8PPg3+fb2un1+/Zgct/u49/Z39/j4O9v6OTd"
"4NPX2tfl2tLp49jZ3eHd4OHx+uTj7u/m3ux05vR3/tzZ49/43+Z5b9Xu8dzf2u335ub85uLj39jd2+P0e9783dzl5Nvn++Rn+nr9"
"9uBx8N313trs5Pjl7vrd39Vu8udj+Gj43d36/N3t7OTa9WNra/Zw9Nxt3dDl28rQ2NHl3uVi4M/ic9rY61d6zOX6YtjY/Vf4429V"
"XdX859vTzvdf6PHqfm3VzuXf0dl52+Xk7WLo/O7Y+W/j7G3b2dzY/Nfa3Ore7dvdfv7i5ujb39Xq3+Pdb/NfYe/x5XHc4t3l59nZ"
"4tz019ft7ujjb+bV0t3c4t/v6en3aOdq5OHk09rb3eV74PLc4OXr3OXf6eXj49/v4dfa39zp6Wr3497o2OLc3vXW0tnc32v27+Ld"
"6d7e3tXX19Xa4O3x5uvm2tbc7u/y4Xv03NTb49ra4Pz76n5q7uXX3N3f3Od1+Hr8fP19/n7+fv7///9+/n7+fv5+/v/+//9+/v//"
"//9+/v9+//////9+////fv7//////////35+//7///9+fv9+//////////////9+////fv9+/////n7//////////////37//37/"
"/v///v///v///////////v/+fv9+fv7//////37///7//v///v5+/v/////////////+////////fn7//37/////fv///////37/"
"/////37//37//////////v9+///+//////////7//////v///n7/fn7///7//n7///9+////fn7///////////7//n7//35+////"
"//7/fv/+//////////7//////v///////v////////9+///////+///////+//7/fv/+fv//fn7/fv///37/fv/+///////+/37+"
"//////////7//37//////v//fv///////////////37////+fn7/////fv//////fv////////////9+/37+/v///v//fv//fn5+"
"////////////fv/////////+/////v////7///9+/////////////37//////////////37//37///9+/////////v/////+/v//"
"///+//////7+fn5+///////+//9+/37///////7//v/+fv7//v/////////+fv////9+/////n7+///+///+//////9+/n7//v//"
"fv////9+/////v///v/+/////v///////37////////+//7//n7///7//v////9+///+//////////////9+///+/v/////+////"
"/////35+/37/fv///37+/v///35+//5+fv//////fn7///////7+/////////////v/+/////////v/+/////v9+////////////"
"fv7+/////v////////7/fv/+//////////9+/37+//5+//7////+//7//v//fn7+//9+fv///////////37+///+////fv//////"
"/37////+///+////fv//////fv///35+///+////////////////fv/////////+/////v9+/////////37/////fv7/////////"
"/37//35+///////+/37///////////9+/37/fv////7/////////fv7//v///37+/v5+//////9+////////fv////7/////fn7/"
"///+fv////9+/35+//////////7////+/////37/////fv/+/v//fv//////fv////9+/37///////9+///+///+/n7/////////"
"//7//v///37/////fv///////v9+//5+//7/fv7////////+////fn7////////+/////////v7///////7///////9+/////v//"
"/35+/////v7///////////7/fv//fn7//////n7//v//////////////fv///////////v9+//////7//////////37+////////"
"fv/////+/v9+///+//////////////5+/////////v//////fv7//37///9+fv7///7+/v///v/+//////////7///9+fv9+//9+"
"/////37//v//////fv//fn7//v7//////v/////////////+fv/+////fv/+//////7/fv7/////////////////fv/////+fn7/"
"//9+/v7//////n7////+/v////////9+fv///////v///////////v//fv9+//9+/37////+/37/fv///37/////////////////"
"///+//5+/v/////+///+/v7//////n7+////////fv/+/37////+//7///9+/////v///37/fv///37////////////+fv9+///+"
"/v//fv///////37//v/+/37//v////9+fv//fv///////v///////////37+/v5+/v9+/v//fv////////7///////////9+////"
"//////////9+///+//////////9+fv//////////fv/////+///+//9+fv/+/////////v///////v////7//35+////////////"
"/n7/////////////////////fv////9+//9+fv/+/////////////v/////+fv///35+//9+/////////35+///+///////+////"
"/37///7/fv7+////fv////////7//v////////////////7//////n7///7/////fv/+/v///v5+fv/+/////v9+/v//fv//////"
"///+/////////v///////v///////////v7/fv9+//9+////fv///////////v///37/fv//fv//fv9+/v////////////9+////"
"//////7///9+//////7///9+//7///////9+//////7/////fv/+//9+/v5+//7////+///+fn7//////////////////////n7+"
"///+//9+//7//v7/fv///37//////37//37//v////7///7////+/v////////////7//////////v///////////////////37/"
"/////////37//v9+///+/v/////////////+/////////37//v///////////////37+//7//37/////////fv//////fv7/////"
"//////9+/v////5+////fv////7/fv///37//35+fn7//37/////////////////fn7//////v/////////+////////////////"
"/v/+fv///////37///////////7+//7/////fv9+//9+//////5+/////v////7///7///9+//7//////v9+////////fv9+////"
"/////v///////////37////////+/37//////v/////////////+/////37//37////////////+///+//////////9+//7////+"
"fv///v5+fv///////////v///////37//v///////////v////7+////fv5+/37//////////////v///////35+////////////"
"//9+//////////7+/37/fv7//v/////+///////////////////////+/37//////v9+//////9+///+/35+/35+////////////"
"//////5+///+//7////////////+//5+//7//////////v9+/37//v7//////////////37///////9+/n5+//7//37//v9+//7/"
"/////37//////v///////////////////v/+/////////35+///////////////+///+//7/fv9+/37///////////////7+////"
"//7/fv7//////37///7+/v////9+/v///37/////fv7//////////v//fv////////7+///////+/v////////9+/////37//v9+"
"/37///////////////9+///+//7//v///37///7//n7//////v/+fv////7//v///v////9+/////37+//7/////fv/+////////"
"//7///7+//////9+/37///9+////////fv7+////////////////fv//fv///v7//////////v9+/////n7///9+/37//v7+//9+"
"///+//9+//////7/fv//fv/+/37//v7//////v9+///////+//9+////////////fv////////////7///7//v/+///+///+////"
"/37////////+/37///9+/v9+/////////v9+//9+///+//7//////v////////9+////////fv7////////+//7/////////////"
"fv///v//fv////////////9+/37////+//7///////////////////7///////7+///////+/////////v///////////////v9+"
"//9+//////////7//v/+/////v/////////+fn7//v////7//37+fv////9+//9+//9+/n7///7///9+fn7///9+/v///////37/"
"//7////+/v//fv////////7///9+////fv////////7//////n7//37+//////7/////////////fv////7//37///////////7/"
"/37///////7///7//////v///v5+/////////v7///9+fv9+/v7///7//37////////////+/////37///9+fv7//////////v//"
"/////////v////9+//////////7+//////////7//////n7///7///9+/37//////////v9+//7///7//////35+/v//fv//////"
"/////v/+/////////v////9+//////7//v///////v/+/////37//v/+//9+/////////////////v/+/////37///9+///+fv//"
"/////v////7///7+//7//////////37/////fv/+/37///////////8=";

// Spoken phrase: Again
#define AGAIN_AUDIO_SAMPLES 8051
const char AGAIN_AUDIO_BASE64[] PROGMEM =
"+/Xu6+z+cWprcPfu7N/X1tnmW1dZYU1XatK4tcF6XldYZD45xayjqbRbWcY7KyInOMC229rT2uNJMzFC79vI2dvU0n5VU09ZZm1w"
"7efm8P9pZmZre/Pn5+TxemllZHru5tvPz9LjVFFVXEZUaMWursBnWFBWXDI3tKGbo7BB3cwtIBsiObC039TO2Og7LC1G49HE3NLO"
"0GBPTExVZXHv39/qdWRbW2J16NzZ3et0X1xi+evWysbMz1VKS1pCRF/Uq6St31VMSmA3Jcmek5afzzqzMh0TFSLDpMTayM3VRyci"
"LXjbvcfUyMfcSUdBR1Zu9dvY3v9lV1Nba+Xa1dzpcVxXXvrlz8PCyM1NRkZXPUBdzqafqd5RR0ZhMyDGm4+RnMk1ry8aDxEexaHA"
"2sXL1EgmHytw3bzF1MbG2klIQEdWbvfb2d79ZVhUWmrj2NLa53BcV17959DEwcjNTkZHVz5BX9GooKrcU0pIYzcjzJ2RlJ7HN7Iz"
"HBIUIMmkwtzHzdZKKCItb96+xtbJyNtMSkNJWHD73dvhfGhaWF5u597Y4OxuX1tp/ObTyMnN2k1MTltATmbGqqi2bldMTmUyLbef"
"mJ2oTlS9Lh8YHi63rtzZztniQSwsPe3fxNjXz854UU9LU2J78d/l7HNrXWNo8eXd4+54aGFo/+zZzc3V3FdSVGVKVXHQs6+78WZb"
"W/M/Nr+pnqWucE/CNyofJjTMu3Prz9HuSDg2RP5c8eXQztXpXlxeW1xnc/L08nx4b3R7+/b1+3t3ev3v5N/h5+5saV9WWWnW083M"
"0cvPcUZGYeXjzMa4s7vGXuNdQT47R0hLT0tRTUxOT1JVWVteX2JkZWdnaGlqa2tsbW5vcnN2eXt++/n28/Dv7uzr6uno5+fm5eTi"
"4d/d2tfU0M7Ny8rIycjGxcfIyc3X7nNaTklIR0VGR0lJSUtMTU9TV1pcXmBhY2VmZmdoaGprbG1ub3J0d3p+/Pn28/Hv7u3r6uno"
"5+bm5eTi4d/d29jV0c7NzMrJycjGxcbIyMzU6XpeT0pISEVGR0hISUtMTU9SVllcXl9hYmRlZmdoaGprbG1ub3J0d3p9/fn29PHv"
"7u3r6uno6Ofm5eTi4d/e29nV0c7NzMrJycjHxcbIyMzT5XxfUEtISEZGR0hJSUpMTU9SVllbXl9hY2RlZmdoaWprbG1vb3N2eHt+"
"/fn29PDv7u3r6+no6Ofl5ePi397c2dbSz83My8nJycfGxcjIys/e+mhUTEhIRkVHSElJSkxNTlFVWFtdX2FiZGVmZ2hpamtsbW5w"
"cnV4e//7+PXz8O7t7Orp6Obm5eTj4uDe3NnX08/NzMvJyMnHxsXHyMnO2vJvWE1JSEdFRkhJSEpLTU5PU1dZXF5gYmNkZWdnaWpr"
"bG5vcXN2eXz++/j18/Du7evq6ejn5uXk4+Lf3tvZ1tLPzczLycnJx8bFyMjKz976aFRMSEhGRUdISUlKTE1OUVRYWl1fYWJkZWZn"
"aGlqa2xub3Bzd3l8/fr39PHv7e3r6uno5+bl5ePi397b2NbRz83MysnIycbGxcjIy9Dh/WRSTEdJRUZFSEZNQfPdQEtXWUJcZE1R"
"XmVOWlpWUsW/VdrG70zd001dd2ZVa+ZiaODW3fbU1e/W2tTl1c7Oz83EzMjAw8vAxcfLxs/Y0HzbWVBeP0M/S0Y8T0hESFNKR1ZP"
"UFJOaVlWa2tZ8dbe8ObyXmFuz3D19tRYVvFqfGRzUOxO0M3nysbH5sPK287a2dZ0xb7Gxr+7zMvM2+RqXk5OTktOTUtKTUxOR0hI"
"S0tPTEtUUF9eZv/u493j2mn1cWpcWG1h7F1qbPf/3eDp4e3dbt/z6uHhz9bIzdDIyMvKx8zN1MvO1Nfd1+HqeWpbVU9OS0lJSUhI"
"SktMTU9RVFZZXF5gZGdqbG5xc3V3eXp8fX5+//79/fv6+ff08+/t6+nm4t/d29jW1NHOzczLysnKysrLztDU2eVs7GY+PkU6LzI9"
"NC8/Sj9J8tn+1MrNz9bS3uTwfXZqZ2djZWtma3r+fu/n6Obg4OLi6u/pcGFeW1NITkpJTkhr1NLPtaqvsp6esaqgqTtvuSofKTAe"
"HjAvKzhScOzWxMa+z9PE221z6V5SXltUWFlXXF9eZHt+/eXd3tzV1tbP0tnMy+3ezORIT+M7N0BDPj1sy7Wur5+XpaSam2k3rSwZ"
"HCkdFygvMkNcxrK9xb6x3k3N/EZBWktETFBRV1xlbf759vH7fXRxdXF78uXf3dXRzsvRz7/O7PbATjVSTDMtTGdUw7ClmaOgmZnG"
"MawrGBkkHhoqM3nA6rupt/7TuEc+T05JRFFPdFxZduZ68uHc5OPn7vhpaV9lWV5o/l9o4d5b4c7Ly8q9vrVIXcI+JS5tMSy5rJyZ"
"n5OQmii6xxMQFh8YJSvao73Vp6tfOtdCOj43anNdT8rVaunh9fL1cXpuaW59/vXo8PPy72zyb3l7bkXx3E9PxLrbusq5uEArSl0f"
"JbK2oqGbkJCrJqYqEBAbISQ5MqGe8NKwzzQyOT7QO1e+vfblz2v+T1RfclptYm9eeGbm9N/s4uzY5uLMyen2bVHtOzlWt0Porq+u"
"0jpNziEVxbK0npuNjJYgrtoODA8fIUcpqJW8Ub3INy4uM7xMQL+4y+/NX99SSlJtVF5uZl9saX366u7i6+zf2u3Uzdlj5lNqVDtG"
"y8c8u7CyuVxA3VEbIa3Eq6OXj4+vJqMlDw8ZIy4+L52d83XDfDczL0m+QF7BveHa5P7kT09aY1VmZnNmbG7//X7p6ePv5ODS39jM"
"z192alRdOz/xvDvOq7Cz3Edx6B4YtrS1n5mNjZoirE4ODREfJ0coqJe8WMzZOzEsM7pPRci7y97WY9NcTFJsUlxgcWNsYHZ5/P3u"
"5e3m8+Dd2O3Q0e5Z4lZjSD9UxXNAsK+3w1te2TUaOqvNqaGTkJVFPqsaEREeJkIxSZqoZnvXTTkzLdrJQ+/Ewd/TZOLqVU9hXlpk"
"a3Bob2T7cfB84u/l7ufi197dzdVfaORPVz5F8sRA8qmxuc9j32shGreyvaOajo+bJrFyEQ4RHypNKK6Zt1xtfD85Ky+9bkvYv8jP"
"21zXck9PZFdfYm1keGZ0d/z88Ov26e7k49rjz9VzXNlZUUdHacxZR66vvsjq2+YxHEqs1KyhlJOYP0muGxMSHilMMEadqu5aY01A"
"NCvqykx1z8bPz1zp311RXFpdZWhob3hr/XPx/Ob/5eni5Nrd1c/wWOTvSkpDXNzWPryoucDQ0NtHHiCrvbqjl4+RpSerOBIPFCEx"
"Syirm7tnTk9EPSkywHBZ6cnJyOtc2vlVU1pZY2JpZnZsb3V++PLv9ezr5+be3NrW7V/y4kxMS1/v3EjOqrrIzMzZTycgt7fMqp2U"
"lZ8tvNcZFBYiL10syJ60501MSkcvLs/eX/3YzcrcWODuYlldXGhlbG50a3hu/Xzx9O7v8+rn5d3Y2NjvXeXiR0tOdPrjRr6nu8fH"
"wttDISGtucemmZCToiqyUBURFCAyWii5nbPhRENGSCstzudz/93NxdxW5fNfWVZXZmhqa3xt/Wx8fO/59fL16ezs4tnb4Ptk5eJK"
"TFrycPROw6u/zsbB20UoJrK80aqdlZilLrt2GhYXJDVtLMmhtdhFQUhQLy7j5PZ5+9XI2lfz8mteWFprbWtse3B+b3t+8vT67/fn"
"6+ri09jg7Wns2kpIXutrdFjSqbbRv7vJRiwfxq53rZ6TlZs7XLUdFRIdLHoyO6Gqw0w5Pk86KUvW6d1v5srJWmL1a2FbU19ubWt0"
"cXZycXn69vTz8vfu7vLn2dzk5/jt2F1JX35iVF9hsrDTxbq9XzomOK7ix6mbmJu3MrI3GxgbKUhlLrinuuA8OURRMDXd3c7efuHM"
"61ltZ2dnXF1rbnN7eGxxamt0/vTp6vHs9n708ere1uPi4+na4ktWfWZKUFXFrMHJt7PIQS0nvbdUsZ+WmZ88bLkgGRUfMdc4Paeu"
"v0cyNkw/LUrWy8fi+dfOXVtoYGNcV15pcHjw+HNjXV5icvbn4N/l9Xpv/fnu5drd6Obk4dtgU3xvTkZZfbe517y0u1Y7LEizVsyr"
"nZygxTi1Nx8bHi5j8jO7rr3vODM/VjY87NTD0Pj60/ddZF9pZ1xbZWp29u7zeWVgX2Zu7OPd3uT8cW528ejh2Nvo6d7i3HFU83dM"
"P1Nxu7bXu6+1ZTstPrJY5Kydmp+6NLU/HxscK1rPMsStu9s2LjhXOTt617/I7GrY6l9jW2VrXlhfZnby7fN7Z2FeZGzv6OPl6vp5"
"c3f16OLc4uvm4e3zYFbzc1JL9te8u97CusJOOzNKtevMraCepbw8vUUmHx8rQmw037S7xFI/RWlBOUtU4N55aNzY8ftsbHdsYWRn"
"b3F9e/18+33+e/t+fXh7d3z+9+3o4eHn+O7v+nVfW97b6XnPyr2+XfTU5EU7Ok6vt72qoZ2jrUXa2iohHSQsPy8zx728yn100tdB"
"RU1UZ1dPWO14a2hmaW9mZWhvd/z7+n16d3Z3evz59/f5/Hp7efr17+ro7Ovl5uTm9d/Q1+Dn2d3OYEZUYFlKTlm/rbayqqalrLxK"
"z1QuKCQpLjctM2Ldz9ji1sbJZ3Z4ff9gVVlsYV1fX2JoYmRmbGtxcXd4dnZ2e3l++/r59ff39u/v6Ofj39/g4d3e3N/s5djpbFte"
"V2VMP01i937Zy7apra6pp6atuFjiczMrJigqLysqOkVOXF322Mze3dvp3Nzo89zb2dfc2tjk+W5pXVZSUlNSVFhdZG746d/a1dPT"
"0tLU2+Tt+mtbV1lXU1JXXWJZXXd6YWbt69rZz7+5ubWsrKysra6zvv7wRjQvLSwrLSwzOTk9TFlYZ2rv3/h+4dnh3tjT0dbY1tjl"
"/HBoXFVSUlFRU1hdZG705t7Z1NLR0tLT2uTu/GtbVVdWUlBVW2NaW3X8ZmTt6dvX0sC6uLetrKyrra6xu+ryTTYvLSwrLCswODg7"
"R1dVX2d84PR26Nje3tfRz9LU09Te8HhqXVVPT05OTlJXXGNz7N/a1dDOzs7P0Nbe7v5sXFJSU1BOUFZdXlhn+Hdhfufh1tjJvLi5"
"sautq62tr7XJcH0+Mi4tLCwsLDY5OT9PV1hlZ+3oc37e2uHb1M7O0NHP097tfmtbUk9OTUxMT1NYXWv24tvW0M3MzM7P0Nbh9nRm"
"WU9PUE9NT1ZdYVpn7/dnfuLh1djMvbm5tKytrKyur7TEcvZANC8uLCwtLDU5OT5OV1ZiYvTqb3Hj3OXd1c/Nz8/Oz9rl8HRfVU9O"
"TUtLTU9UWWFy7N7X0s7My8zNz9DY5P1tYFZOTk9OTU9XX2JbbO34aPbh3tTXyby5ubGrrautrq+2zHNvPDIuLiwsLC03OTpAUVVZ"
"Y2Xs72163t3k29TOzs/PztHc6fZuXVNPTk1LTE5RVltmfebc1tHOzMvNzs/T3O13alxSTk9PTk5TW2NdXvrtb2zl49nW0sC6ubet"
"rKysrq6yvO3xTTYwLi0rLSwxOTk8SFdUXmFz53ps7dvi39jQzc/Pzs7W4O1+ZVhQT01MS0xOUldea/Ti2tTPzczLzc7P1N7vcmdb"
"Uk5PUE9OVFtmX1/663ds6OTa19bFvLq6r62ura+vs7rc/Vw6My8vLS4tMDo7PEZYV1thaOx7Z3ji5erd1s/Q0c/N0drg6XhgWFNQ"
"TUxMTU9RVl1p/+rd1tHPzs3MztHV2eH5amBbVE5OUlJSVV1oc2Zy5+d3fePl29zWxb69vLGwsK+ys7e/7vRVOzUyMi8xLzY+PUBM"
"XFpfYGj1bWFt7PP35dvV1dXQztHX297qdmRdWVRPTk5PT1FWW2Nx8OPc19PQ0M/Q0dXa3+b0bGBcWlVQUldZWFtmcv1vfeTlfv3j"
"5t7f2snBwb61tLSztre6xPvyVD04NjUzNTM6QkFFUWJeZWNt9mthbu/8/Ozf2dna1tLV2tze5vhuZmBbVlRTU1JUV1tfZ3fu5N7Z"
"1tTT1NTW2d3k6vVvY15cWVVUWVtaXGVw/nd16uT1dunm5uDhz8bExLu3uLe5ubzC5H5pQzw5OTc5NzpFRUdPYWJmZ2j4d2Vp9/Z+"
"8+jd3N3b1tfb3t/k7nlsZ2BbWFhXVlZYW19lbn7///9+/37/fv///37///////7//////////////////////v/////////+/37/"
"/////////n7///9+//9+fv/////+fv9+//////9+/v///n7/////////////////////fv///37//n7/////fv////////////7/"
"/////37/////fv//////fv//////fv//fv//fv///////v/+/////37+//7/fv///v/+/////v///v7/fv///v///////////v//"
"fv///////v7+//////7//////v//fv//fv////7/////////////////fn7+//7////////////+//5+fv///37///9+fv//////"
"/37////////+/37+/37//////37////+//9+fv////7//////////37///7///7//35+///+/37///7//37///////7//n7//37+"
"//////////7///////7///9+////fv///37/////////////fv///n7///9+//////////7+//////////9+//7//////////v//"
"/////////v////////7+//9+/35+/////37///////////////9+//9+/37///7//37//////v/+/37///9+////////fv/+//9+"
"/v////////7//////v9+///////+//////////////7////+/////////v////////9+fv/////+//7////+/////v/////////+"
"/////35+//7//v9+fv////9+//7////+//////////9+fn5+/////37/////fv7///////////////////////7//////37/////"
"////fv//fv/+/37//////v7//37/////////fv/////+/////37///7//v///37//v////7//////37///////9+///+///+////"
"///+/37+fv//////////////////////fv/+//////7//v/+////////////////////////////fv7//////////n7/fv/////+"
"/v7+//////////7+/35+fv/+fv//////fv7/////fv///////37///7//v//fv9+//7///7///7+//////////9+//9+///+/v//"
"/v////9+/v7///////////5+////fv//////fv///////37//////37//37//37///9+////fv////9+///+//9+/v////9+////"
"/37/////fv////////9+fv//////fv////////////7///5+/////v////////////9+//7//////37///5+////fv///35+/37/"
"/37/fv///37/////fv////////9+///////////////+//////9+////fv////9+fv//fv//////fv7//37////+//////7///9+"
"/37/fv////9+///+/////37/////fv//////////fv////9+/37///////////9+fv/////+/////v///35+/37///9+////fv//"
"/////////////37//37//////v/////+//////////////////////////7+//9+///////////+/v7///////7/fv///35+/v//"
"/////37//////////37/fv///////////v////////7//35+/v/+/v//fv7//v/+///+fn7////+/v7//v/+/////////////v//"
"/v7+/////////////37//////////35+////fv////////9+//7/////fv///37+//7//v7////////+/v9+///////+/////v7/"
"/n5+/v///////////////////////////////////37//n7///////7//////////////37//37//////////////v/////+//9+"
"/////37///////////7////+///////+////fv/+fv////////////7////////////+//7/fv9+/////////////v///v//////"
"/v//////////////////fv////7///7///////////////////7/fv////9+/////////////37//////v///////37/////////"
"//////7//v////////9+fv/+////fv///////37///7//37///7//////37//v////////7////+/////////37+/////35+////"
"//7///////////////////9+/////////v//////fv///35+/n7//v////////////////////9+/////v//////fv//fv////7/"
"///////////+/////////v/////////////////+/v///v//fv////////7/fv//fv/+fv/+////////fv/////+//9+////////"
"////fv///37/////////////////fv////////9+//7///////////9+//9+/37////+///+//////////////////9+////fv//"
"fv9+/v//////fv9+//////9+/////v9+/35+//////9+/////v9+/37///9+///+//7//37+/37//v//////fv/+//9+//7////+"
"//7/////fv9+////////////////fv////5+/v///v///35+/v///////37//v///37////////////+///+fn7//v7+fv7+/v9+"
"///////+//////7///5+/n7//v///////////////////v/////+/v7/fv//fv5+/////v//fv7//37/////fn7/////////////"
"//7+/v7//////35+fv///37///9+/37//////35+//7+/////////v//fv7/////fn7/fv///////////////v////7+/////37/"
"//////9+fv/+//7///7+/////v9+//9+//7/fv////9+fv9+/v///37/////fv////////9+////fv///v9+fv//fn7///////7/"
"/v/////+//////////////9+//7//v/+///////////+/n7//v5+//////9+//5+/37////+///////////+/35+////////////"
"/////37+/37//37//37////+/////////37//v9+//9+///////+/////////37//v/+//////////////9+/v7/////////////"
"///////+////fv/+/n5+//9+/////////37////+fv///37//////v//fv/+/v///////////////37////+///+//7///////7/"
"/v////////7//v////9+////fv///////v////////9+////fv9+fv//////fv/////+//7//////////////v/////+//9+fv//"
"//9+//////7////////+/37////+//9+/v7////+/v7///7////////+/v////7/////fv//////////////////fv////5+//9+"
"//7+fv////////7//////v9+/////v///37/fv///v7///////7//v9+/v///////////////////v9+/////v9+/v7/////////"
"///+//5+///////+/v///////////////////v/+//////7//////////v///v//fn5+///////////+////////////////////"
"///+/n7+///////////+//////////7////+//7+//9+/v5+///+//9+//7/////////fv///////////37//v////7/////////"
"/v//////////fv//////////////fv/+//9+/v/////+//5+/////37////////+////fv////9+//////9+//////5+////////"
"/////37/fv////9+//////7/////////fv//fv/+////fn7///9+/////////////////////v/////+/////////////37/////"
"/35+///////+/v///////////v//fv////5+////////fv/+//////7/////fv7//v/////+fv9+fv9+///+/////37//37/////"
"/////37///////9+/v/+//////////////7/fn7//35+//9+/////////////////////n7+////fv5+///////+fv9+////////"
"//////////7///9+/v9+//7/fn7///////////9+fv7///////7///7+///////////////////+//9+/37+/37///5+//7//37+"
"/////////37///////9+/////////37+/////////v//fv///37////+///+//////////////9+/35+/////37///////7///7/"
"/////n7//////37/////fv///37//v///v/+///////////////+/////////////37///7+//7///5+////fv////////7//37/"
"fv///////v9+/////v//fv////7//37+/n4=";

// Spoken phrase: Eat food
#define EAT_FOOD_AUDIO_SAMPLES 9328
const char EAT_FOOD_AUDIO_BASE64[] PROGMEM =
"eu/a4+ja419zWVBIP1I+OlNcOvK8UL2ztrC2qa28vqZPOa0uLUYyKC00Lzcv+1s7vcLov7fIx77Nze/e6lZebFpe+nz82d7t29pa"
"aF1KRzxMPTVIZzhQtE+/rbSssaWos8Ci2C2nLidBLiYnMCszKl9vMry9676zvsW6xcXq2NdVX/9aXfR5+9fi/NfeTWlUQj86RzYz"
"RmIxeq9DtKmuqayfp668nUouoiQoOiohIywoLiXdTS+ywN24r7u+t76/+8rWT2/6Vl/scfzU4W3X3ERhUDw7Nz8wMD5tLlyqP7Gk"
"rKWonaSpvJpgJ50gIzcmHx4pJSsf4k8prr7YtK6zvLS6u3LFzkp39VBX53tq2c92cslRP+o7NzQ6My0xSksprLc/n6akpp2coq6p"
"mByxrBU4JiIZHyMmHyW1JkaodLmuq7S1sbLH7bhcSdxfSFvgdGXUye5hxG847j8wMDYwKi46fiW6pjOgnqOhnJifo7aQIzuaEiwo"
"HxkZIiAfHLcsK6Tmv66qrraur7z4t9pF4+9KTuzoYv7Lzl9xxkk5czgvLjguLC5OVSems0ucoZ+jmZufraeXGKe6FD8gIBgfICId"
"KcMf+KxHtK+st7GussfEt15t0GlTZvPzbPTc1l9T10s2TjkxLTgvLy5b7Cqor36eoZ+km5yfraiZGqfKFkEgIBkgICEeK/4g4rI+"
"tLKwuLCut7+7um7NzmVr32du5vdf4P1HWGA1Pjw3LTY1NS9Cvi63puGln5+jnpuhp7iXJeWnFzomIhsfIyAfJlglPLA7v7G5t7at"
"uru8t+3Kxm9w1Hdf7t5cXeRdP2FJNz07Ny47OD0uzdI0qa/FoqSgqJ6fo7monxyiRBxLJSQdKSIkIjQ+JcfLObK8vryytr+7u7/x"
"v9Vx5tZpb/zkaFh3ekRLXDo+OkIyOT1INEe2N7+pzquopKion6euyZopaqcaSC8oICkpJSkrTSo/uTXGt9C+ubfIur/A1sbMcNfb"
"Z/HiYXPmWlfyTEZUQkA6Sjo4QFo7P7M+z6rKr6ynq6+jqrHhnC5FpB1IOCskLS4nLi5LLUC7NMy588O7vM+6wsfTxc/91Nlm8d5v"
"Xup4UV5wRkxNSD0/UDo+U2k0wMc+r7q/r6+ou6qrrWKqryCgMSneLiwrPCgxMD87MMBFQrZ12MC82cC+zMzKyO3W0fF03fJfae5Y"
"T2dSRk1LSTxYSD5M2UBWtkjEs8i0uau3uKyxycqlKLy9JN46MSxALi85OEUx9mo5wd1hycHbzb7SzMvJ3dfP4f3f62difV1PW1hJ"
"S09PPlZXQ0/XUU+5Zc+2xri+r7fAr7bJ2Kkx1rkpeEQ2MUU1Mj86RjddZjzN51nQyOfVw9jQzs3b2tTe+uTpaGZ8XFReWEtSUU5G"
"XlFJZOxVbL5dyrjOu7u1ur2yu8PZrj9ZuC9ORjw0Ojw2OjhQPDzVSFLY4eTeydLO1MjP287Q5N7d6nX5e2FdZFlOWFdIUFRLTlNh"
"Um53zHfTt9TBtLy6u7S7vci+vDm+XzdOPD40ODk9NzhaPz7xVXNv3s/X187H2NDM0NnY2t3r6/T9fv7/fv5+/n7+ff5+/n3+fv9+"
"/v/+fv9+///+///+//9+///+//7//37/////fv////7+//7//////////////v7+//////5+///+/37///9+/37/fv/////+/n7/"
"fv9+/35+fv///v9+/v9+//9+///////////+//7//37//v//////////////fv//////fv7//v/////+/37////+/////////v//"
"fv//fv////7//////v////////7///7/////fv//fv////7+/v//////////fn7+/////37/fv///v//////fn7//35+//7//37/"
"/37//37////////+/////37///7//v//fv/////////////////+////////fv////5+/37+fv///v/+//7///9+//////7///9+"
"/////v///////////v9+fn7/fv/////+/37//////////////v////////////////////9+//////9+/////v//fv///37/////"
"/////v///////37///7/fv9+///////////+////fv////9+/37+//////7//37///5+/n7+fv5+/n7+fv9+/v///37+/NjW3fl5"
"+GtvX+jf5Nz18Hjk1trU0dPQzc7P19ff9W7y+Ojf2M3V1tTX3Nze3uvt79/q5t/k6eve9+7p9O7e4uXZ3e34+H5e7+Ht4d7Z4t/l"
"33x85+He3dLa2Nrl1dbu3djZ39/d4OT68uHm7+7j3vl95H559dfd4OTz3utu9dV3497d3PLz4e325OHk3dnd3Od+8t/+2ODj5Nr2"
"6+5o9nj87eNr4+Hy2t7r5vrgfuve29pm4/po+mbp3N917N7v6t/bcWRrbvRs5eFt1dXo1MnW1dTp3PZm19Hv/dbb/lTdz+xza9Ld"
"bFjj7WlQfNh439vP0Wlp5fXpbv3P0urbz+L72O3fdmrmeePab37h+3bW3tjf79bc3ure7dfnffne7t/e29nq3uHha/BZcffo7/zb"
"49zr4dfc397x0t7s8uHtdd/S1d3d4ePx6Ol1b+lp3Ovb1dnd3PHy5O3a4+bo3Ofe7eDn3+Xs3dbe3d/o8Wrt4ODl2OfY6OrT09vb"
"6Wvu797g6N3e3NTY1tbc5PDu5e3g2dbf7/Ts5HPp2tTf39ra53316m9x69/W3dzf2/F5+3z8fPx9/X79ff5+/n7+//7//37//37+"
"//9+/n7+fv/////+/////v//fv///v/////+fv////////7//////v/////+/37+////fv9+//7//////////v///37/fv7/////"
"//9+/37/fv5+/nzr4OTe3tze3N3i3d3e4+Tj4Ofo5uDq6eDf39zY4d7k5dnk7Ojx2+744/Le5ure4d3e2t7l2OHf4uTb4u7g6+Xm"
"79/07eTn4eHq3N/j4eLg69/o5+3u6PLo7PHl3eXd3vLh3eXh4eXh7PHm4+3k9enh5Ojq3efh4d7h3ubt3+Pk7+rr9uvs8uHo5+Xh"
"3+Pd39/f5uzk6O3u+fLo7ezl3tvm49jd4N7k7Oft6vLx9fHieODi5uDc3OPf3uTkfvru8O3y6nDk79vq6+Dk3Ojd4ePo5O7g7u3s"
"8d7r6uju5Obt7+ns4O3p293b59rc5d/k3uHz53fx5/blbHre7t7i4t/c2OTW49ni3t7q5+re7d7v69/v6+155Prv7fPu5OHd3OnV"
"2dHb29Lg3Nz66+3762ps8fzs4+Ld7eHh5+Nved/159rn2Nzr1tTd1+Lc3O7rfun3d911b+Dw6drs5tne3tbg6dvl5mztXN/Qet7o"
"2dTW29TU3+7p6ux18XHp+/7r99rc1+zceuvZ1d/m3+3d5vRf2nZn6fry2df41tzg2Nzf2/X54/HqderZ6t7m5N349N7w9uRx/PP+"
"9+B37dHX5d7b3M/k2t7n4Ol0/t925e/34d90393m6OHY/N/X2t3M3uvc3OnfdlxiRFd+4Nja28vM0NXMx9nW7e723HTs4f7h5334"
"3erg6+bv6uPf4fXk3dnq5f3t4f7kbvHn+HzicW/W9tjd7t3W3Onb5+Ls6eHr3f/m6PTy9e/62evx6t7n49ro3dze797i5tXp4ePe"
"3tjT49/S4u7e+eTa3/bc59va2+Tn3+njeuh7/ejt/nj62tvg2Nnk1tjf3unj7tbz99nf5dnX7N/q9+vn7nbd4Xjh3+XU3Wrb0t7p"
"7OTe1ufv4+h96O925e3u7ebl4t7i6OXo3djb1trY2Nne5uzj5vnr7Ofp393e2d3g3d7p5e5793dwb3vq3t/o39TO195yYextWD5F"
"TlFkVFb6u7C2vq+wqq661UPATDosKy8vPjMvPerT0+POzsC92/Nr3X5cU0xTYGddXW737e71+v//fHVvaW794Nne4dbLydXdYWDk"
"YUo1Q0lOXE1P3K+qs7eoq6Krs1JEtzsxIycpLDsrKjzVzs/dxsy1uth6dNRmWklFTFxdVlds7+Xn7vH4fnZybWdodefY2d/ay8bO"
"3vhV525RNzlPSF9OT1y2qa2+qqqlpbPJNLhtMygjKig9Lycv8c7M4cnPu7LL417U8F9PREdUX1hUX/rn3+nt9/p3cGliXm3v1NDX"
"2sm/wtffVGXfUjosSEVPUUdLw6Ojt66gpZypsDD+sDApHCQfLjciIkHLx9DOxcirt89d4tFiWUA9RVhVTk9n8d3f6PX5e3puZl9n"
"9trO09rPv7/M4npO3GdGLTRXQV9HS1msn6u/o6Sfn63SK6x9LiAeJCI/LB8q38rF3cHatavB4FnO9GVNPT1MV1BNVm7n2+Hr9fV0"
"cmleWWLr1MnN0sq6ucfj+UvebD4oK2I8V0FGTqqao7+fnZyZqLwhq70pHRgfGzgsGh5lyb3Wvda2orbPUszlaE04NEFQS0VNYenW"
"2t/m4f5oY2dbWV/p1cfK0Mq4uMbo7Uvo8T0pKXg9UkFGTKuao8einZ6ZqbofrbQqHxkfGzkwGh1ayb3Vvty5orPMTsvkb1E6Mz5O"
"SkNJXfHV293k2u5uWV9dWFNk28q+x8q+r7XI635E0lMxHyvvNVQ6Q1uelaa/mZuXmKVeHp5+JRgYGxo/JhUf18O70bRvqqCx11PF"
"cGNGNC89SEc+Sl3l09bb3djrbFtfXVpWa93MwcnKv7K4yd/sSddXNCUtdTVPPUVao5qqwJydnJuo3SKj4igcGx4cPSsaIeTGvdC0"
"5q+kstBUymtbSDkxPUhIQUpc7tfY3N3c5WtbWltVU17jzb7Cxb2vsL7P10553TQnI1o1PEI7R6+Xnruhl52VoaogxKcnHxUeFic2"
"HBg7xrrHtrnIoKm4UtrZUUk6LzE9Qz8/T27X0M/X0tjgYVtVWVJSWvjSxcHHv7axu8jUcFDSQS8lOUcyTjxI6J+crrWan5udp1sn"
"o0QnGx0dHDoqGyXZw77KsNWup7HOV85dT0E3MDk/RD9JWOfPzM7Oz9DecldNTUxMSVN50MS9v7y0rbW/zNtM3E4uJCpQLUE9P1Km"
"maS3m5mdl6SvIK26JB0XHhYqLxwbRsm6ya+8vaKquVfPfE5HOy4vOT49P0td3s7Iy8rLzdv2XlRMTk1MTlztz8e/w763srrK0tlX"
"7kwvKi5mMj4/Rm+snai8oJugm6iwJ7u1KiMbIhorOCIePOW+xbS+z6mttercdVNVSTc0OkBERUxSedrNz83Rz9jfbltQTk5MTU5l"
"3cvIwMK7tbG80dbRXmNBLSkv+i86PE3TqJyqu52anpuotSWxvCohGx8YLDcfHTtfvr+vxM6prLHR0l9QWk44NTc+QkdNUGrdzszM"
"z8/b4GhfVFNPTk9b9tjRzcXBu7m7z93R1lVPNy8uSVYwP0DYv6WkubadoJ+krmIuq00uIiAfHzo2IihCW7+9sOXDr7G5y9dRT19U"
"PTw7QUZSV1xt39TP0dXa5vVmXVJQTU9Y/N/W1Me/u7m7x+PQyv9HPC8vNOo2Mj1avq+fq7+nm6Cdq7EvW600LB4gGyU/LR8uPX69"
"tLRmubGwusLmS05oTT88Oz9GVllhb9/Wz9DU3ef7amFXVVJYYvnl3drMw7+/xMzs1s7vRj02NzlsQjJAVr65p6zCsZ+joqy1SDmu"
"SDQmJCAjPTsoLDxHx7yy3su5uLm+zVlOXl5KRz9BRVJcYWrz5tjV19zl7HZmXlZTUltp/+Xe2MvAvsHIy+XSzfJAOjY4OVlDLj5T"
"u7ioqr6xnp+gqbReM69VNCYhHx82PCgqOT3NvK7Lzrq6ubvEaE1UXktMREFDTllia/Pr39nZ3OLp+29mYVxdX21v8ujd1cnGyM/O"
"193P308+Pj4/RFw2OkvMurarub+poKantr443rs8NCYmICtBOCw4OlDLuLfaw8K/v77QY1FdVFFTTEpMV15qee3v5OTg5+nveWlm"
"YF9iZ25v7eLYz8jJzdTN19XUbEM8PkE/R0gwPVa9urGsvbmjn6SnuMY0wsY6LyQiHyw/NCw7N1bKtLjOv8TCvr3PalFZTlJVT0tN"
"VV1rfO3t6urm6unw/XBta2tsbW5xfe3g2dPP09fZ0NnY41tGQUhKREpENkZqvb64ssK6p6SorLzMOsfMPjUpJyQvQzoyPztQ07q7"
"zcPKy8bBz/BcXFBWW1ZTVVleaXr19Ozu7u7t7/h+e21tbm9tbXJ4/ufe2dbR1NjYztTb6FlHQEpMREJENUBgv7y9sr+8qaGnqbrB"
"Pv6+QjoqJyIpPD8zPjxB5L+2xcDGzsvBytppXVFPWVdXVlpcZGt+/e729Pf59/f4+3t5eXh7eHZ2cXz88Ofh3tza29zX0uDpY1JJ"
"TFVOREg/PV3Wvsm8vsa1qKqtt8R1RMFmRDUsKSk3Rz0/ST5c2r3Bx8XQ183L1eRsYFJZXFxcYGFlanR7/PX1+Pz6/Pr8+Xp7e3t5"
"enhzcXn69urm4eDc29vb0+DwZlJJSVJPR0hGPVfdvcC8uMO5q6isscLRQc3VRTotKykxPzw1PjtK/snF2c7OzsnEzdjv63Zye2pg"
"XV5hZmxxb3RyeH76/P18fn79/H59ffz28e/u7uvp5ubk6PVwbWBdY1pRSk1PVWvmXmziyb28tL3DurGztcDMVlvKV0c4NTM3P0A2"
"OD5CV1/5XWPd0dHP1Nvb2tbf6fN9dHNwbWpqamxvcXN1dXl7ff9+/vv59fPx7u3r6unp6+73/nZva2VYVVhaXlxfWlzpzdDMzMa+"
"urG3vry3uLa+y2RVzWBIOzc4OT49NDM9P0hGSUtPcen+dOzk3N3d5evp6e75e3Vyb29ubW5ucHNzd3h8fv79+vn19vXy8fDw8/b4"
"+vx7dG1rb3VudXBy/Orh5ufc19fO1c/O0MrMzM3Ozs/T2+r8c2BXUU9OTUxMTlFTVVdbXmBiZWlsbW5ub29ubm5ubm9wcnN0dXV1"
"dXZ2d3l6e3x+fv////38+/r49vX09PTz8/Px8O/u7e3t7e3u7u3s6+nm4t/e3NjU0c/OzMrKy8zNz9Xh9XllV09NTEtKSUtOT1FT"
"V1tdX2FlaWtsbG1ubm1sbW1ubm9xc3RzdHR1dXZ3eXp7fX7////+/fv6+ff29fX19PTz8vHv7u7t7e3t7e7u7Ovr6eXh393b19TR"
"z87LycrLzM3P1uT5dmFVT01MS0pJTE5PUVRYXF5fYmZpa2xtbm5tbWxsbW1ub3Fyc3N0dHR0dnd5enx9fn7/fv79/Pr5+Pb19PX1"
"9PPz8vDv7u3t7e3t7u7u7Ozq6OTg3t3a1tPQz83LycvMzM7W3vxybWdfXVxHVVhZXFpgXWhnb2VqZHVv6H3qXlLu6O1uem5sbt1d"
"Z+BzdXHvdHlz/vFveHfr7fpqeXj0+P7u5uxbePH57N13/G189/bo6PF++n3yc+rv2+3u/uXp4d7a19/a593f2fDj+XlsYGtiWVtl"
"V15hW2VlX3Bsbmh2af7uX//6dXVud3J5+3R3+3d2am3gd2/1bu9vZu/y9Hb0/Xx4fG7u6Hzsa+917Xz4buP7+ffv6Wb4dd355mzu"
"ful17+Tf6Pvg5d3q3NrP5t7p3eHi/vptbmJZYGVhV15WclliY3h7b2FifG1+cPP9eGZr/PJ1bnH79m5vfOx2Zf136/Ztefv+eHD2"
"53xzcvP37Wn17vn4cvDufnTs/Ot5de7p9+/+++/+++7o6+p45eXo6Obc29vd3Nzc4urj6PNycGRkWlhgYF1aYF9mX2Jpe2xscXd4"
"cG1t9n54dHZ5cm57ffp5dX17e3h1//j9fHP9+v7////+/37//////v////9+/37//////////37//////////37///7///7////+"
"/////////37//37////+/n7+///+/v///37///9+//5+////fv///v///////v7///////7+////fn7/fv////7+//////7/////"
"fn7///////7/////////fv/////////+fv7///////7//v///37///9+///+//////////////9+fv////////////7//37///9+"
"///////+//////////////////////9+/v/////+//////7//v/////+////////fv/+//9+/v///v//fv////5+//////7///7/"
"////fv/////////+/v/////////////+//7/fv////9+////fv////////7/////fv////9+/////////////////v//////////"
"/v//fn7/////////fv/+/////////////////////////n7///9+fv////7/////fv///v/+/////////37//37+////////////"
"////fn5+//9+/37/fv7////+/////37///7////+//9+////////fv///////////v7/fv///////////n7/fv/+///////////+"
"/37///9+//7//v9+fv7///9+/////37+/v9+//9+/37////////+/v//////fn5+///////+/v/////+////fv7///5+///+////"
"//////9+///////+fv//////fn7+//9+fn7//37///////////7//////////37////+///////+/v/////+////////////////"
"/////v////////7////+///+/v//fv/+///+fv////7//37//////37//////////////////v//fn7////////+/37//////37/"
"///+/37///////7///////////////7//37//v///////////////////v9+fv/+/37//////////v/+/////37+//7/////////"
"/////////////////////////////////v//fv///37///9+/////////////////////////v////7////+fv///v/+////////"
"///+//7////+///////+//////////7///////7/fv7//////37/fv7///////5+//7//v///v///37+//9+/v////7//37//v//"
"///////+//////////////////7///7///5+/v/////+//////9+/v////////////////9+//7//////////////n7+fn7/////"
"///+/v////////7/////fv//fv7/fv///v////5+/v///////////////////v/////////+fv/+//////9+/////37//37//35+"
"/37///7///////////7///7+//9+/v////////5+fv////9+//7/fn7+/////////////////37//v7///7///5+/////////n7/"
"//7///////9+/v/////////////////////////////+/////37/fv////////7///9+//7+//////7+fv9+fv///////////37/"
"fv////////////9+/v//fv///////v///v/+/////v/+fv////7////+///+////fv///v//////////////////////////////"
"/////////////37///7/fv////////9+/v/////+//7+fv//fv7//////////v////9+fv5+////fv7+/37///7///9+////////"
"////fv///v////////9+/////v/////////+/37////+/////n7//v///v9+/////////v/////////+/37/fv///37//v/////+"
"/////////////v//fv///v/+/////////v/+////fv///////////////37///////9+///////+/////n7///////9+////////"
"///+//7///9+///+///////+//////7///9+fv/////////////////+/37////////+///+//7///////////////9+////fv7/"
"/v//////fv////////////////9+fv///37/fv9+/////37///////7//v7+fv//fv////////////////9+//7///9+/37/////"
"fv/+///////////+fv///v7///9+//9+///+/37//v///37//37+//////////9+/////n7//////////////////////37//v9+"
"//9+////fv////9+/v9+////////fn5+//9+/////v///////////////////37///9+/v7+/////v////9+//////9+//////7+"
"/////37/////////fv//fv///////37+//9+/////////////v/+/v///37////////+///+/37+////////fn5+////////fn7/"
"/v///v7////////////////////+/////37///9+//////7//////////v/+//9+//////7//v///v9+/v////////9+/v//////"
"/////n7///7///////////9+fv5+fv///37+///+//////////////////9+////////fv////////////////////9+fv////7+"
"//7//v7//37/fv////7///7///7///7+/v9+fn7///////////9+fv///////v/+/n7//////v7////+////fv///37/////////"
"////fv9+///+//////9+fv///37/////fv////////7///////7///////7///7/////fv/////////+fn7/fv7////////+////"
"/37///////7//v///37////////////////+fv///v7/fv9+/v///37////+//7//v/+/37//37//////37//////v//////////"
"/////////v////9+fv///////////v/+///////+////fv///////37//////v7//////37////+//////////9+/////35+/n7/"
"/////v///////////35+///////////+/////////v/+////fv//fv9+//9+/////////37+/////////////////35+/////37/"
"fv7////+fn5+////fv///v7///////////7////////////+////////fv////7///////9+//////7//////v/////////+////"
"//9+////////////fv/+/v///////v7///////7////+//7+/37/fv///n7//37//37/////fv9+/v///v///v7//v/+//9+////"
"///+fv7//37//////////////////v9+//////7///9+fv9+/v9+/v/////+////////fv/////////+/////////////v///v//"
"/////v//fv7///7/////////fv/////+/37//////////////v///v/////+/v//////////fv/+/35+//9+/////v////////7/"
"fv////9+/////////v/////////////////+/////37+/37//////////35+/////37//37//////37//////////////35+/v//"
"/v////////7+//7///////////9+fn7///////////////7+///+fv/////////+fv///////////37///////9+/////37+//7/"
"fn7/fv///////v////////7////////////+/v7/fv//fv//fn7////+///////+fv//fv9+//7//////35+//7/////fv////9+"
"/v///37/fv///v///37/////fn5+//////7/////////fv//////fv9+fv//////////fv9+/v///////////v/+//9+/////v//"
"fv////9+fv//fv//////fn7///5+//9+//9+///+///+/35+//5+//7+/n7//////v////7+///+//////9+///////+fv////7/"
"/////////37+//9+/////37////+///+/v7/fv///////v//fv/////+//7///9+//7////+//5+/37//////////37/////////"
"/////////v/+//9+///+//9+/v/////+/37//37+/v//fn7//v7/fv////9+//7//v///37///7///5+///+///+//7/fv//////"
"//7+fv///////v///v/+//9+///+/v///37///9+////fv///v///37+fv9+/////////////35+/v///////37////+//9+/v//"
"/35+///+/v9+//9+/////v///37///5+////////////fv///37+//7//////n7+fv////7+/////////37/fv//////////fv//"
"/////////////v9+/////v/+fv//////fv5+/w==";

// Spoken phrase: More
#define MORE_AUDIO_SAMPLES 7364
const char MORE_AUDIO_BASE64[] PROGMEM =
"e29oYF1bXF5hY2ZpcP7s5ujv+Pjt4t3e7uru2MzEwcTFxsK9urzA0dxr+PBbTz87OT1DRkRDP0NLWW1qa1tbX258fnFpZm377ebk"
"4+He3Nze5Oz1fXNqYl5cXF5gYWJlb/3p5+z9ffjh2dXe6urjzMO9vsHFxL63t7rG3Ppe3XVcRDo2Nz5EREA9O0FKYmtpXFFWXnn1"
"9nFoaX7s4d7e397c3N7k7Pd9cWhgXFxcXl5eXmZz7eftd2125tbP0ubn7M3Aubm9wcS/trG1u9btVuzaaE45NDE4QEQ/PDc5QFFu"
"Zl9OTVVr7uj2bGZv7+Ld3d7e3d3e4+r0fnRrZV9fX2BfX15lcPDo7nZqb+3Yz9Dj6PHUxLu5vsLHw7qztrvQ61ps1nFaPTczOEFH"
"Qz86OkFObmlnU09WZvLq8HFpbvPm39/f4d7e3uLq8v14bWVeXFxdXl1cXmh96Oj5amp929DM1+Xq38W8tbm9wsO8srC1wOBvU9bm"
"ZEM1MTE7QkI9OTU7RFxrZVlMT1p96+p8aWd96d7c3d7e3Nze5Oz3fXNqY15eXmBfX15lcu/p8XFqcOjXz9Lm5+7Owbq6vsPHwbiz"
"t73Y7lfu2mtROzYzOkJHQT45O0NSbmZiT09Xa/Hr9m5ocPHk3t7f397e3+Xr9X5xaGFdXFxdXFxcZXTr5/JtaXTg0czQ4+Xpyb22"
"t7y/w720sLS82fFS4NxmSzcyMDhAQz46NTlAU2xiXExNVW3t6PdtZ3Ts39vb3N3c3d7k6/h7bWdhXl5eXl1dYWz66u55am3x2dDO"
"3eXt2cW8uL3AxsS8tLW5yuViXtV4XT83NDY/RkM/Ojk/SmZnZVVOU1/66+14a2v46N7d3d7e3t/j6vH+cWhhXVxcXFtbW2Rz7On1"
"bWh139HMz+Hj6Mq+tre8v8K9tK+zu9fsU+faZk03Mi83P0I+OzU4P09rYV1MTVRp7un2bWdy7eDb29zd3N3e4+r2fG9pY19eX19e"
"XV9peuvr+W1re97Tztfl5+PJv7m7vsPFvra0t8De+lbc52NJOTU0PENFPzw4PUVZamRcTlBadO7r/W1qe+zg3d3e3t7e4Oft+nlr"
"ZV5cXFxcW1peavjo7Xhobe7Wzczc4OrSwLm1ur7Cv7iwsrfL41xm1GxWOzMvND1CPzw3Nz5KZ2JiT0xRYPLn7HhpbfHj29ra3N3d"
"3+Pq9nltZmJfXl5cW1xlcu7r+WtpdOHUzdPk5ejLv7m6vsLFv7a0t7/c9lfd6GJIOTU0PENFPzw3PEFPWltfVl5lcm1uamVmbm50"
"c25ubm9vcnV9/fPw7u7s7Ovr5ufk3t3d3d7zaGRlXtPHu7WwsLi3ub20rKqmp6i+/UEoJyYoKzM9OTY5Ly83PU51zdDT2nZbWldW"
"WVtbW15fZmlucnR3/n339/Xv6+fg3tra2djU1dna+FZOSUZW38K+srW+u76/sqmjoqClyXYxHyEhJSw6QTw9PDA6RlHNv7/GznxQ"
"TExJTlFRUldaXmlucXV5d3759vfx8+rh2dfRz87NxMC/wMhxRD02NlHTzLC3xMHCva6emZqWndBeHxUZGBwsPUVJSDowT2DMr7O8"
"w1U7OTg7RFFTW1xcYHV6/HNqZmt+7+fs8nd5fuvk3+jd2c2+t7i3v05ANi09ave7vdZa6sm5nZaYkZrWZRwQFxYbOUt5x2BBPnHd"
"uq22xds2LzMyQmX3395qc3jx6OtvZVxmfuPa3+5saHbw9O5PTFRcxa6tqazVSTQrM1Pf0MpPKk1rvJGOjoibaTwMChANHsdnq6xS"
"XWFevK2xv1owJSktO9TDwcLgaGxr5ej3X1VWbevY2eVyaWrr7upbRUVHX7uvq6m37D8zMETb5dvfLDL0TJ2PlIuQzeoZChIQFl7d"
"v6nWUWxY07OxvN4+KyouNWbJxsTPa25n8ebqb11YZPrd1t9+Y2H+4+nwSD9CR8+uq6WpwlA3LjNi1/DOPyVLTLyPjo6InGc4CwoQ"
"DR/Ec6utVGBcXb2ussFZMCUqLTzQwcDB4F9paebe62BXU1/s2NjeemBl9+bj9klCQ0jQs66prcVWOjI3Ztb910QoSlTElJKSi55l"
"QQ8NFRAgymmurWFsYVXEsra/ZTYpKy492MHAxOFdW2jh2NhzVE9Ra9LNzt5dUlp+19bpSDw8RNOuqqaqw0s2MDRxy/7bQyQ/XsiS"
"jpCJmV1ODgoSDRvPYq2o9HFdSsqwsrriNyYnKjbavrq82VVPU+LPzNxYSUlP4MzK0XpQTlrlz9PoRjw9R86vrKmuz0c4NDvfzn7j"
"PidLXr6Uk5SMn2FGEA4WECDVYq6s7flaTM61tr3fOikqLDnXvru93VJMTvDPy9VkS0dO88rEyupPR015zMrRVTo5PV+0qqinulY6"
"MjNRyt1uXCgu3F2cjpOMj87cGwoRDxNB/cGkv3N+Smi4sbbGSysmKS1Pwrq4xV1MSV3WyMvnT0RFWdnIx9RnTUpZ3cvN3kg7PEPX"
"sKyprctKOTU78MnpfEooPObXmZKXjppl9xcOFxIbaly3qc7uZknxura5zEcsKisyY8G6ustYSEdc2cbJ3lVFRFXexL/McUpCTebH"
"wc5XODQ5UbqpqKe0ZTs0NUXIy11nMSV2+a2PkpGMqF8+Dg4VDiD4T6qq19RVQta4tbjMPCkoJzLuvLKzylM+PE7Wvr3KXj86QmrJ"
"vr/YU0A/TtrAvcxiOjM7TcKsqqyz/D85PEbTxmdLQCg6xcebk5qRml/RHxAaFBc7RMunusjTSErPv7q81zktKio75LuxtstUPDtG"
"48C8weNGPD1N0Ly5v3xAOTtSyLm3xlE3LTNNwqulqrDcPDY9Sdu+6j4+KinAv6KPlpWQuXg9ERUXDyA9P6mqu7xuPlPOvri7ZjEr"
"JCo/z7OstMxHNTVC27u1u9dFODhE37+6vtlJOzpG3722u91EMS88brmqq6+8Sj1AR+vAzVI8MypBurSblp6Yn0zJLBceGRcsOGyr"
"rrjKaEFLzMHDwEkvLSoyZr6xr75sPDY6T8u7usN1QTs9T826uL74Pzc4SM64s7riPC0vOu+vqKutyT9IP06/wHxLMCos3LKklJie"
"ma9HxiIaHhUYKjbJq6u23Go9R8PBysc6KysrOc61r7PLSTY2Pvq/uLzOTzw6QGjJvb7LXUE7P1vJu7nDXzszNkTHsq6uuFtVSUPN"
"v+tzPSwuQL2tnZmioqVS3mYiJB8ZHi5DvK6swn5ePU/CzNjwNi4vN1DBtbbB+kE6PlDXwb7H501AQEzvx72/z1xCPEFbzLy5wmw/"
"NDRDzretrbd9XUlExbvc5D0mKzjFqZuWnqSkZlfDKSMhGBglOr+tprJgXz890cLe9j0vLTdOzbm3w+lMPkBN7czEyNZkTEZKW+HO"
"ys7iXk1LT2nYyMXM71E+PUn5xLS0utB8TkvJv9PWSCktOOStnpqgqqvoSL46KigdGiI3ybWqrnZXS0Flyd1eQDk1O1zVx8LM+lpO"
"T1Vt5djX2uxoWlddcuDX1d77XVdXXXnc0c/S5FtFRUpdyLW3uMPtT1nJx8/NPCYuNtunm5mfqa1STb03KiQbGSA5vLOprl1NTUtt"
"z+tLNzk5QPTLyszYcF1WWltmfuTe2+T9Zl5hc+vg4e5wYV9kcO3j4ODe63ZaU01R5cO/ubvVcV3s2tDKWS4yNkWxoJyfqq/VQMBY"
"LyofHSAxxLa0rttKUGb15WpPOThES3XTz93r/PlrbGVfX3Lt4uLreWdlbPzq4uj/Zl5fbuzc3OPu9W3+fWJRUmbVxrezxM76W2HW"
"yu8yMzM1uqGbnKOtvj3MzzIpHxsdKd6vuK/CTk540eBPRjoyP1Xn2dbg/W7o6XRjWVheeuff5u15bm11e/jz7vV5a2Zjb+7c2dvm"
"fGNp+nJnYlxh3cW4vcDKX0932t1JOj0xWa2hn6Kst1VMv0gxJyAfJz23t769+GFp1NBVPT45P1fm2PB3++/m2ulqV1RZZfzm5urx"
"/Hlwcnf++e7v9Htyamdnd+na09LdcFtZZW7r62FU9s+7ube7/kxcbWlYPT8vQ6+inp6os3pCvlMxJh4fJDa5ssLC59rk1c9VNjY5"
"RF7v2Pxgb97Z1+psWFVaY27/+PDs7/ZyamptfO/s6/J6cG5vfHz07N/b2eL3XlhbduLqZ2n53cC6tcJ1WFtMYVhJPjTYrqaipK+/"
"R/HJPi8kIicvX7S/yt7dydfcaT81OkN1+vP+aGvo2NjqY1xaX2h1dnx+8O/0fW9qbG/67enq8HtybnT8+fTw593X2uVeVVdq7uLz"
"+WDhvba0wd9cSEB1VUo4OsCspJ+jsMdBzOQ3KR8iKTXUs8XMe8u/0XNKODQ7S970b2Fp79vY3HdbWVxmbG9xffju8/5ya2pvdv31"
"7uvu+Hlvcnz68fHs5d3b4HFdWmB35+nwamvIvLe9z3RQQVdsTkM39rarpaSuvFBdxkMyJSQpMU26vc7c6MHM6FZANztFfOBxbWV+"
"5tze72FcXmdsb292fvLx9X5vbWxudf307Ozr7vx0bnD98+3s7enf3N34YFtfb+nn72xezLy1uMjuVD9Kb0xGNE64qqOfqbZ7R8BO"
"MyUfJCw+vrbLz/DAw9xdQDU2Plzae29kcujd3exiWl1nb29sbHX57+72dm1rbG5ye/fz7u3t8vx3c3b98e7v9PHn3t3ra19gbe3p"
"73Nc1sC5usfnXURHcFBNOUe/rqikqrjYRshpOSskKC09zLnMz+/Jw9xrRzo5QFbc8G9rbuzi3+1oXF5odHdvbG978vD0fm9tbG1z"
"dn728uzq6+36dXBv/vDt8Pn46d3a4XFfYGru5+93Wd7At7fA32BDQm1PTDhAv62moae1zULI+zgqISUrOc+2yM7pyr/ZckY5Nj1R"
"3OhubGrt4d7rZ1pcZXN6cm1ud/Xw9/9yb21sb3R6+vXv7fDx9X52cXj48fD2+/Xo3tzpbGBjbO3u8Wxd28K6u8bsYERIa1NPO0jC"
"r6qlq7nURsp2Oy0lKS490brL0ubNwdx2STw6QFbf53F0bfDl4uxqXV5odHt4dXJ4fvr5fnl1c29ubnV++u/t7e/z9vl+eXt+9/Pz"
"9vzz6N3b5HJiZGrx9PVsXN2/uLjB52JBRGVTTzlCw66ooqi2zUHK3zotIiYrOdi4yNPfzb3W+Eo6NzxQ4eJ0eXDu5eTtZVpbZXR9"
"eHJxefz4+315dXJvbGtscHv+//////////7+/v/+/37////+//////7/fn7////+/v///37/////fv//////fv///////v//////"
"fv9+/37/fv//////////fv//////fv9+fv7+//7///////9+////fv9+////fn7/////fv////7////+////////////fv////9+"
"/////////v7+//9+//////9+/35+/v///////37+fv////9+/////////////////v/////+/37//////37//////v7////+////"
"/////////v9+/////v////7////+//////5+/37//v///////v7//////v///////v9+/////37///////////////9+/////v9+"
"//9+fv9+/////35+///////////////+/v///v9+///+//5+/v/////+/v////9+/v////9+//7//37///////////7/////fv//"
"//9+/v////////////////9+//9+//////////////9+fv///////v///35+/v////9+/////v////////7+/////37//37///9+"
"//9+///+//7//n7//////////v7//v9+/v////9+/////v////7///////7///////////9+fv///////////////37//v9+////"
"/v///37///7//v///////////////v7/////////fv////////7//37///////////////7+fv7////////+/37////////+////"
"///+/////////37//////////37/fv//fv////////7////+//////////7////////////////+/v/+/////////////////v/+"
"/37+//////7//v9+/37//////v/+fv////////9+fv///////v9+////fv/+//9+/v////////7///////7///9+///////+////"
"/v//////fv/////////////+/n5+/////////v/+/37/////////////fv////////9+//9+fv////////7//////37/////////"
"////////////////fn7//v////////////////////////7+///////+/////v///////////v///////37///9+fn7/fn7+//7/"
"fv//fn7//////////v9+/////////37+/////v////////7/fv///v//////////fv//fv//fv////////////9+//////9+////"
"fv////7/////////fv//////////fv///37/fv///v9+/37////+////fn7+/////////////35+////fv/////+//9+////fv//"
"///////+/////37/fv7////////+///////+/////v//fv//fv///v///////////37/////fv///////v7/////fv//fv7/////"
"/v////9+/////37//v/////+fv9+///////////+///+fv7//v////9+/////////v/////////////////+/v7/fv/////+///+"
"/v/////////+//////7///////9+//9+//////7//v////////////9+/v//////////fn7////+fv///////////////////37/"
"/////37/fv////////////////////7+fv////////9+/////////////////37/////fv9+/////37//37//v7+/v7//////37+"
"///+/////37////////////////+//7//v9+/////////37///5+/////37//////v9+//////5+fv//////////fn7//37/////"
"//9+//////7///5+/v////////9+/37///7//37/fv////9+///////+/////////v///////v7///7///9+fv////7//v//////"
"///+fv9+//9+///////////////+//7//v7//v///n7/////fv///37/fv///37///9+/37//n7/fv///////37///////7///9+"
"/////v////7///////9+/////37///////7+//9+/37+//9+/v///37/////fv///////////v7///////////7//v///37/////"
"////fv/+///+fv///v/////////+//9+/37///9+fv///////37//v///n7////+/////////37+///+///+/v7/fv////7/////"
"//////////9+/////37//////v//////////fv///v///v/+fv/+fv//fv9+/37+/37//v9+////////fn7///////7//////37/"
"//7//n7///9+////fn5+fv//fv///37//////37///////7///9+fn7+/v/+/////////v///////37/////fv////9+////////"
"/////////v/+//////7/////////////////fv////7///////////////7/////////fv7///////////////////////7////+"
"/////37/fn7//37+fv//fv9+//7//v////////7////+/////////37////+fv/+/////v///v/+/37///////////7+//9+////"
"///+/v7//37///////5+/v///////////////////v///v///////37///////////9+/37+/37+//////9+fv///////v7/////"
"//9+/v9+/n7//////////37///////////7///7///////////7///7+////////////fn7////+////fv/+////////////////"
"/v//fn7////////////////////+///+//////////////9+///+///////+//////////////7//////n7////+//////////7/"
"/////v////5+/v////5+//9+fv////////7//37//v/+//5+/////37//37///////7//v/////+/v/+/37///////////7/////"
"/n7+/37///////////////7///9+/v////////7+//9+fv/////+/v///37////////+/////37////////////+////fn7////+"
"///+///+///////////////////+/////n7//////35+/v///37/////fv/+///+fv//////fv////9+//9+////////fv/+/37/"
"////fv/////////+/v////////////////5+fv////////7///9+/v5+/v7+/////v/+/v////5+////fv//fn7///9+///////+"
"////fv7+//9+//////////7////+//////7//////37+/////////v/+fv5+/////37/////////fv///////37///9+//////9+"
"/37+/v9+//////////7//v///37/////////fv//////////fv///////////v////////9+//5+/////37//n7/////////////"
"//////////////7/////////fv7///////////7//37+/////v///////////////37//////////////v/+//7//v///v///37/"
"/v/+//////////////////9+fn7+//////////9+/37//v////9+/37+/////v/+///////+//9+/////37/////////fv/+fv//"
"///////+/v///////37//37//v/////+/////////37/fv/////+//7+/v///37+/v///////////////////////37//////37/"
"//7////////////////+//9+///////+////fv////7/fv////9+/v//fv9+/37//37//37///7//n7//v////////9+////////"
"/37//n7//////////////35+/////////37/////////////fv////7//////////v///v///v/////////////////+/v///37/"
"//9+/v9+/////////v///////////////v///n7/////fn7//////////37+//5+/v////5+//5+/v///////////////v//////"
"//7/fv////7+/////37///////9+/v////////7//////////v///////////////////////////35+////fv//////////////"
"/////////v7//v//fv/////+/////////////v/////+//9+/n7//37//37+/v//////fv////7/////fv7//n7/////////fv//"
"/////37//////////37///////////7///7+fv/////+///////+///+///////+///+//9+//9+/////37/////fv////7/////"
"//5+////fv/////+////fn7+fv///37//////////37/fn5+//7///////9+/////v////////////7////+//9+/v/+/37//37/"
"/////////v7//v///v8=";

// Spoken phrase: Go to
#define GO_TO_AUDIO_SAMPLES 7643
const char GO_TO_AUDIO_BASE64[] PROGMEM =
"/37/fv///v//fv///35+/////v////////////7///5+//7//37/////////////////fv////9+/v5+///+//////////7//37/"
"/v9+fv9+/n7+fv5+/n7+fv5+/X7+fv5+/37//379ffx8+3r6efl4+Hj3d/h5/2/vfXa96lfx3V9X1G5pftr7eeLm8syzxNK4u9DN"
"uMnYxsnV3szZa37j/VRPXkhGR0ZEPkdGSENNTUpSXU9YaFxkbO1d5Pbv3VrcbVNhZdVT/9ZwduXdX/rlc/Zn9N5u6Nrk5MjGyszG"
"zs7Pv77JxMHB9czV1PXzUlZNRd9LUlpsSEheREZHP0Y+QnhdXWDY6F7rbGtnX1tYbGp+6OPs4+Hd6G56dHtv+l55cfPf4tjQzsjK"
"xsnPyM7L18vOyMvW0dXU3Nb79VtdT0tNREpFTEtOWktSWVVRWVZUT1FfVVxYY21venR8eP96/n38/vr79/f08u/t6+jl4d/c2tfV"
"0tDNzs7Lwry9ure2r6+0vn1240MuJiUnJisrJy9D6eZ20ca/vMHP5OTX/FxQT1VcYF9eZ3T5+311ffDj3+bg1s3Lztrtad/2Szw6"
"QkhFTUNNxK2mrauin52eqL1L03MuIRweHycwKik64by+vMPOv7zJYEdJR0xOSEZMZu3o6/Du5Oj+amZxb2lnaezSy83TzsLDxtFd"
"SkxqTjY3OUrKtK+9r5+dnJ+qzEjITCofHB0fMkAwOE3KuLKy12bx9GhNRjw8TFphY2hy793e7Hxybnzx7PZ2YVti9f5wb+zTwbe+"
"y8vIxcvRPSkwPm7Nv8Dwp5iXmJ+sTF3ALyAZFhcieFJMXW7Itaq8Tj4zNj9SSD9LWOfMy9l7dXn6fWNZXHbi3OH4evPz/GNPTFrj"
"397Q48i0rrTOdTY2b9toPU1Cy5yYmJ6nzT+8PiYdGBcd7MG9uPNKXLrFdk8vLDJL+c3F4nJ38/zt8GxrcXFxc3f+9/X08fTx7nt9"
"2dXY+FI6OV3MsKmrxF7jR8m43i8pODijkpSXn6o7z84lHRUTFDO0tKe5STduz1/WOSopNk/Ptb7VcVROVnZyfOvy/HRubXvv8PZ7"
"c2x38Pbez9LiX0k5Qd/Cr62z5V3eVb+8Xy4rO0CilpibpK87yOQoIBkXFzq6tai5UTdZ7F7PQi8tN0neur7N8VpOUmhu++rn731u"
"Zmz65+Xyd2ZjaPvp3czN3ltFOjrkv7Crr85L0XHGte4vJDQ4rJOUl5+mSla8KiAYFRIjvLelq9o3PflNzWs0LC47Tr23v85oTUhV"
"Zvrf3+X5a19fb/Hj4ut7Z19fdejXys3nUEE6PtK9sa634EvP5sS4Zy8nOD2ul5iao6pNXsAsJBsZFie/uqit0Ts/a07M5zsvLzpH"
"xbm+ynNMRU1h7NbV3fViWlpp7dnW3PlcUU9e5M2/xeJKOzY8yraurLfsQtLcw7VnLSMzOq+UlZeepG5LvCsgGRYTH8O5pqfCPDhe"
"SczKPy8rMTrOtba820s8P1D0zcnO5ltOTVrx1s/T6FxNSlXozL/B2k07ODrrubKut89EYcvVuMk9Jys9VJ6Ymp2ksD3FUCYgGhkZ"
"O720pbDpOEZPU8HjPzAvNEHEuLi/5Uc9Q1Dry8jO5ltMTFfy0MnL31tJREv9yr67yFc7NTdGvK+ur8BePs/Ew7RxLiAsPcmYlZed"
"org8vDkiHRcVF0a9rKCt6DRBP02+20MxLi47ybi0uc5LOzxDXs/Dw876T0ZJWuHNyc/uVUhGTvLJv77HYT84O0TSs7K0v+FHVLzD"
"u8A9KCQ6R6yXmpyjqG9RwSwlHRsYIN7FqaW1XTxIPP7C5Ew5My8+3MG3uclaQz5AVN3JxMndW0tIT3fQxsjUY0hBRVfWv7vB1kk3"
"ND5iwK2utshpQ0XAxsjPPColQ32smJmboaXNTcYsJB4aGB5L/6+ns9dKV0DkvM1yPzIrMUrburW7104+O0VszcHCzW1LREdZ3svH"
"zOZURkRO+cq/wdJZPzk9YMm3rrXEd08/TMzk4l49LzTezamdn6Knq/7vXyonIR8eK05htLG/321pU8e/zeVHNi83Q37AvMDUWUVB"
"THLPxsjVZUxGSl7ax8PK6FFEQ07pyb/D204+O0HkvrSutMhZSD0+5+9vX0Y2NeDJr56fn6SmwGN3KyUhHx4nQkrBtcLS7epgyLvD"
"yWU8MTE4Rte/vsXgT0NFUurLxcrfWUhFTWvYysnS+lRKS1rjzcjM7FFFRU7ev7m3vM9VRUM+U/VnZFdKPWfHvqqlpqmqs3rqOysp"
"JiYoOkVfwMjW3+d73L/Cw8tfQDo5PU7ezMfM6ldLS1bz1M3P4GFPS09h3c3JzeBdTUtSdtPJytRrS0RKXc+7t7i+2Ew+QD5M6m1f"
"U00/Wr+7qaKjpqmu5OxAKiklIyMwPUrFxdHX5HXvwb+/v9tMPTg4QF7ZycjTbk9KS1vo0MzP5F5OS09l4tLO0+dhUk9WcNvOzdb6"
"VktNYdzEu73C1VlBQEVGa+xsXVlNS82+tamoqa2uw29tNS0rKSctPD/6ydfb5v1r0cPEvsjwTUE8Pkto283Q4WZTTVJl59XR2O1g"
"U09WbODQztPpYVFOVnLbzczU7ltNTmXaxbu8w9ZZQDxCQlbm+2ddWkvevLeqpqeqrbb47j0tLCgmKDQ7TM3W3uL9ZuLEw72+z2ZJ"
"PjtCUXnUztf2XE9OVmzg1NLb911SUFlv4tfV2+1nWlddcuPX09jnbVxXYevVyMTI0fBTRUdLTXHza1tbVFfJu7SrrK+4vdJh30o9"
"Ny8rLDY8T9PX2t3teOHLzcnN42BSTEtSXmzy6vB+bmZkaG/77evu9nVsaGltefXr6Ont+ndwb3T68e3s7vv7/fnw49/a0tLU2epd"
"T05JTGBhZmNpWP/Du7CrrbS+xvpm3U9COC8sLjk/Vtvt8+rf287HztXia1xbXFpbXl1ia3V99/f+fHh1dHd4enh4dXR2eHp++/n4"
"9vf5+Pn6+ff3+fx8eHZ6/fHo39/e3uTn5ej9aV5OTFFVY/nl9+XNx7y0tbrDzPdv6FRHPTc1OURLWHBkbOnW0M7O3Oz5cmxrZ19c"
"X2BocH39+vr8fn58eXl4eHh5enl4eXp9///////////////+//7/fv////9+///+/v///37//v////7///9+///+fv/+/v/+fv7/"
"/37///9+fv////7//////////////37//////v//fv9+/////////37//37//37////+//7+/////////////////////////v//"
"fv9+//////9+////fv/+//////7/////fv///v//fn7+fv///v////////////9+///+///+//////7///////9+/////37/////"
"/v///////////////////37//////37///////7//////37///7/////////////////////////fv/+////////fv/+/////37/"
"///////+/////v/+//7///9+//////9+//7///7///7+fv//////fv7///9+/v/+fv5+/n7+//7//37/////fv5+/n79ff19/X39"
"ffx8/X39ff5+//59/Hr4dO3+31Du1W/Y9d/z3+jg9OHx6fH061rdcs9xZcvn3dbbUVzg4OFo3XHe3u37eOny2u3cdHbi+t7a49fc"
"3ljn2/3c7uX+e+nu7e3+5Pno++3r5+js7N3k9O/j6Hvsduf08+7rd+X5+Ozo+Pjf/ers5fnhaPb9bfBx1fHb2une5d325ens43ns"
"4+br33Th7u/d5eb66+rn/Obe7e7iet733fre+d/9ad/56Xva/9vq6PprZtnt1N3te33c3Pfd4GXl/uP43FvZ2+x+2fTd5l7eWul4"
"3d1b/+Nf1uZ82d7r4nhe6e/j+d1y6f/p5n7n89ji5ezb3PTz2d3danrvWW/e2mHq4efi2tvY7/zN8O5283HeYOH14dx+2vfa6XNo"
"1lNcatx25tfR3+7XzeFf1Wd0cvRt6+Lc8nTd3/Jv7Nbx+eX25+tn5trkaPbY7/B35vH2WfbW4O/n5NtsXtXY7e7Y6Nxt7eb85Ovr"
"3eXl4efVcnFs8mVxdd/eb+hx7Hjxffr++fn8/Pz1fn169e7q6+bc0tLX3NTQ5XlXVlZLSj0/UmpnW823sLKtr6yosLpL3OQ1Licu"
"LS4wLjlm0+3mybi/w83a0d3vTUtWV1BNUFpnbWtrfPD2eXV5e3hxb3N8/f99+vPw9Pr7+vl8cHXy6u7r29DNzdLSzMffT1hXXD08"
"Ozpud1ZKvaqrtKqsp6KuvzS92i8nIi0nMS4nLt/K4GjFuLyzyt7hytNLRUdLTk1JSlZ4fXF47+np7nlxdHhubXV+/PDs9vz08Hdn"
"Ymh27/Z94cjCyM7Gv7/G3ENO8lU0Kj44VmdLP8Oen7WrnqOZpq4p5qouJRkjHiw2Hh1DwMTfzLzDpa/LXc7GaFE9Nz1NSEFBT2Dv"
"8vXv4ePk/XdxeHFreP3x7fr18/5oYGNle2pn9tDIwMbKxra3yXt4Sf11OSgo6TtOQ0RNrJmkyqKcnpmpuR+tsiofGSEbODMcHlXK"
"vde61byjsMRUy99wVj00O0hIQURMW/Hl6e7f5OD3/m50bXBy/PLt7vHt/XNgZV9uZWh81srAwcbCt7O/1+JXT9Y7LCNIQDhOPUu/"
"m523q5mgmKGpKz+jLSUYIBojPCEaMM2/yL222qeqtW7kz11WQzYzPURDP0dPbe7e6OHk3OXxenV0b3p59/3t/fN4bl5fYmZjYnve"
"zcTAx8K5tL3K2e9N2EMvKDdPMU0/TNyjnq+1m6Cenqn6KqdGKh0fHx05LR4mYs6/x7HjtaqwxGzRX11UQjY6QElFSExZceTl5ujk"
"3+r1c3t1+X79efn6+HtpY19pYmVi9trOzMfHwbu3v9zf1GpYSjAtLu85N0BK0rGeqL2nm6Kcqa4uX6wwKh0iHCY+Kh4vTNC/t7hk"
"sq6wwc7sUF5zSDw9P0VKUk5WYPDp4Obp7u72/3xzeHX9+fX4+313bnFrZmNqeuXX1tjOx8G/wsv/4NHrS0E2NTZrSTNBTMS7p6m+"
"tZ+joqm0XzOvUTQnIyIhOzwoKjxFyb+y2dK4uLm/yl5RZ3NNSUNDR1NbXV5ndu7k6Ov4/XR0dG5tcnr88+3u8PH09ndvaGZpfu3m"
"393ZzMXEys/W99fVZ0A8Ojw9XkIyQ2C7uqqtwLKgo6SsuF03s1c4KSQhIjg/LC07PdbAssnPvr68vMPtV1plU1ZNSUhPWF9kbnL+"
"7+7v+/V6cnBvbm91/Hz48e/u7/H4eXx0dXZ99/bo4dnTzM7T3dXe299gRT9DSERMSTZFaL6+tbLEu6emqa692Dq+3D4zKiglNUY3"
"Mz87YM65v87Ey8nDwtP+XV9TXFtXUVRXXGVz/378/fz++P56c3V3e//9fX379vjz+f54fXt+e/z77uPb19XU2dva0d/jZ05DR01L"
"Qkk9O1nTvMW4vMWypqmrt8JaRr5XQDErJyk5Rjk+Qz5l0LrDxMXP0cjJ1OtpXVJbW1tYW1teY250/vj5/Hx8e3z9+nz/ff///37+"
"fv///37+/////////37+fv////7///7//////n5+fv//////////fv7+/////////n7/fv//fn7//37/fn5+fv////7//35+////"
"/v///37+//////////////////////5+fv9+/v/+///+//////////////7///9+//////7//v//fn7///9+//////7///7//v//"
"/v///////v/////////////+//7+/////n7///////////9+/////////////////////37//v/+////fv///v//////////////"
"////////////fn7+/////v7//35+///////+/v///37///9+/////v/////////////+////////fv///////////////////v9+"
"/////n7//////////////v/+//////////7///////9+fv///v5+/37+///+///+fv//////fv/////+/////37+//9+fv//fv/+"
"//////9+////////////fv5+/////////////37/////fn5+////////////fv////7///7////+///+//5+//////5+/////37/"
"///+/////////v9+///+///////+/v////////9+fv//////////fv//fv7///////7+///////+//9+////fv////7///5+/37/"
"//9+/////v7//////////////35+//////9+//9+//5+/v//fv///////35+//7//v7///7+//9+////fn7/////////////fv//"
"/37//v///37/fv9+/////////v/+fv///v9+//////7+/35+//7//v7//37///////9+fv///37///7///9+///////////+////"
"//7+/////37/////fv///37/fv////////7///////////7+fv7//////v//fn7///////7//v////////////9+/v/+////////"
"////fv////7///7///9+//7+//9+/v//fv/+/v////7//////////37///////9+fv7////+//7///////9+/v///v//////////"
"/////////////////////37///9+///+/////////n5+/////v////7////+//////7//v/+fv5+fv///////v/+/v///37/fn5+"
"//////7//////37///7/////////fv//////fv/+//////////9+//7+//////////////9+/37//////v////7////+fv///n7/"
"/////////v////9+//5+/37//////37///////////////7///9+//9+/////v7//v///////////////37//v9+fv/+/////v7+"
"/v9+/v//////fv/+///////+/n7///9+/////////////37////+//////9+fv//fv9+/v///35+fv9+/37//////v//////////"
"/v///////37//37///7///9+//9+//7///9+/37/////////fn7//////37/fv////////9+//7//v//fv///////v////7+/37+"
"/////v////7////////+///////+//9+/////v///v9+//////////9+//9+////////fv////9+//////7/fn7///9+/////37/"
"//9+//////7/////fv/////////+/37////+//9+/////v///////37/fv7///////////////////7/fv///37//v////////9+"
"/37//////////v///////v/////+/////v///37////////////+fv///////////37///////////7+/37///7///9+////fv//"
"/////37+//7+fv///v//////fv/+/37///5+//7////////+/37/fv////////////9+fv7///////////////7//////v7////+"
"//5+/////37//37//37/////fv/+/v/+fv////7////////+/////v////7//////v///v////7///7+//////7//37//////37/"
"//////7+////////fn7/////////////fv//fv///37//v///////v9+/v/////+/v////9+/////v///37//v/////+////////"
"////fv///37//////37//37//v///v//fn5+/////v/+////////fv///37///5+//////////9+/v/+//////7//37/////////"
"///+/35+/v/+/37/fn7+//9+//7/////fv///////v7///9+fv///////v//fv///v//fn5+//////7//////v////////9+////"
"fv7/fv7/////////fv///v///////////37///7+/v//////////fv/+//7/////////fv///v9+//5+/////37+///+//////9+"
"/////v/////////+/v////9+//7/fv///37///////7////////+fv///n7///////9+//9+/v//////////////fv//////fv7/"
"fv////9+/////////37//////////v9+/v/+///////+/37////////////////+/n7/fv///////v///v7//////v/+/37/////"
"/v/////+//9+/37//35+//7+/////////////35+//7////+//////9+//9+//7//////////////////////v//////fv//////"
"//7////+//9+///////////+//7//////v///////////v7//n7+/37//v7/fv///////37/fv///n7///7////+/////n7////+"
"///////////////+////fv////////////7/fv///////37//////37//v9+fv7/fv/+//////////5+////fn5+//9+///+fv//"
"/////////37///7///////9+//7///////////////9+/v7//37//v//fv///////35+//////9+/37/////////////fv//////"
"///+/v///v///////n7/////fv/+fn7+fn7///////7///////////7//v/////+///+/////////////////////////v//////"
"fv/+///+//////7///9+//////////////9+/////////////////n7//////v////////////////9+//7///7//37///9+////"
"/////v////9+///////+///+fv////5+//7+fv9+///+/////v///37/fv//fv9+//7///////9+fv/+/v7//////////37///5+"
"/37//////////////////v/+fv//fv////9+////////fv///35+///////+/v///37/fv/+//////9+//////////9+////////"
"////////fv/+/37//////////////v////7////+//7///9+/v/////////////////+//////////////7+fn7//37//v7/fv7/"
"/v9+///+//////////5+/////v7+//////9+//7+//////9+/////37//////////////37//////v/////+/////v////5+//7/"
"fv//////////////////////fv///////v////////7////+//7///7/fv/////+/37//////v9+//////7///7//v//////////"
"///+/////////////n7/////fn7/////fv////////////////7///////7///9+/v/+//////9+///+/37///////////9+////"
"///////////////////////+fv///v///v/////////+//7//v9+/////35+/v9+/////37///////7/////fv/+///+//9+//9+"
"fv////////////9+////fv///v7//////37//////37+/////v7//////37//37///////9+////////fv///v///////////v//"
"//////////////7+//////////9+//9+//9+//9+/v////////9+fn7///9+/37//////v///n7////////+/////////////v/+"
"/////////////37//37+/////37////////+//////////7/////////////////////fv////9+//////////9+/v7///7///7/"
"//7/fv9+fv////////////7////////////+/v/+/v///37///7//////n7/fv/////+//////9+//////9+/37+/////n7//v//"
"//7//37//v9+////fv///////////////////37/fv////7////+/v///////////v//fv9+//7+/37/fn7///////////////7/"
"/v//fn5+//9+/v9+//7////+/37//35+/////v///37////////////+/v//////fv///////////////37///7///4=";

// Spoken phrase: Toilet
#define TOILET_AUDIO_SAMPLES 8643
const char TOILET_AUDIO_BASE64[] PROGMEM =
"////fv///v///////////37///////7////////+///+//7////+//////////////7////////+//9+//9+//9+////////"
"//7/////fv///n5+///+fn7////////////+//9+///+/////v9+///+fv/+//////9+fv///v//////////fv7//v/////+"
"//////9+/////37///////9+///////////+/////37///5+/n7/////////fv///v///////35+/37//37//37//n7+////"
"/v////9+///+///+//7//////37+/n7/fn5+/v//fv////////9+///////+/35+///////+/37////////+//////////7/"
"////fv/+////////fv7///9+//7///////////9+/////////////v////5+/////////////////v///v9+////////fv//"
"fv7//n7+/////////////v9+/////v9+fv9+///+fv////////7+/37/fv//fv///////////37//v///35+////////////"
"fv///////37+fv5+/n7+fv5+/n7+fv5+/n7//v/+fv19/Xz8e/t6+3r6evl5+Xn69uv5T9Ls693u4vDf6eH2333gceBmbeLz"
"z1bcz+zY1fJN/OPc/X7gb9fr6W7t9OPf6N9m7On02N/d2Nr1WdXt6ODt6HX16PTp+e7p9ep+6ezl6+zo2+7v7d/68vb76Pnw"
"7PX843bt7uZ05ej36+fs7ulj52n2dOnY99Tj4eHg4fjg8eTtfefi6+Tpet564ODj7vPr5u724eD64+/u4+7f+dx42l/r7er9"
"5+Hw2+/peWP32uvQ7elr59zf/9Vx/u/s7uXvYtDm7/HYdNNj9flf5XbV/VvifG/Rc+Xa4u3hX27r6evr4m/kct387PDk2eXn"
"59ji/+XZ3eVh6Wpc9djqY97p4+DY3dhq2tT783z99Olh2m7U+OHl5t3sYejnT2V93WvZ19D239XNZ+/dYXx2+W7i4dxv79zm"
"fnHe2nfp6vPjfWzc3e5i3t7v/ffn8W1a3dnn7+Th3lv60t3739zj4Wff9e3m7Obc7N7r29xqcnH5Yf532Pb48Hrud+7j4mRl"
"de9t2956/Hn58ePe3/hlWlhdd2Vj7+LKvcTP4VJYbVxLRFLetamttLpa/dk6NCwnM1vPv8Xadt7TYlZCOkRVZtja6t3b2t3x"
"YVlbY3Xz8vh9dXB769/b3fNkXWBdXl1Pac/IvLza5PNi2nNENz1ru6WirrDLRMROLy4hIzj7u7K+7FB9XWRoPDpBRP/K0M7Z"
"6+jne2JaWF996Of1aGNs6dfV2+d7bWxaR0dKVMG5vLjNY8LFxtE6KShEu6CVnaqpQGG7KykeFB01zaaksWI/Ojb9WD1CNTdt"
"1b+5xNR9Vk1PU1t+3drd/V5cXHPe1M7Lz/ZQQTk+bty5tM7MzvOxrbvaLiAfOrWfkpeoom0+siwkHxAWKE6lnKK8QC4lPVNM"
"3EExPkTetbO2xFo9OTpI/ce/wM5fQjo9TM+5sLK/TTAsLjq/tbazWD/Eu6WaqdUmFhMitp2NipmWoi6vKxQZCwsbLqaSk5/d"
"KhcdLju0uz45KyzcsaWirl8tIyQw36+nqrtNMCouRb+rpqzEPCkmL0m7qrjDXi9cr6mZnr8wGRMZTaeVipGalU14yhUaEQoV"
"Js6Zk5mwQR0ZKS7Yr9lNOis+zbGoq75GLigsPsuyrLDFRC4qLke+q6arwD8pJS5Iwam0zmsuP62nmJi2NhkQFDSmlomMmpLB"
"O68YFxQIDh5Dm5CUpWIfFSApTK7AW0MrM/e5qaizbzMpKTNnuKytt+88Ly8797itrLbhOSwsOmW8rsPoTDF3rKean8kuGhUa"
"XKaXjJSemUhkvhoeFQwVJGeclZmsTx8ZJixwsMjtSS49/76urrp4Oi4uOFnCtLK62UIyLzZRwa+rrsJMMSsvSte0r+lZOy/A"
"p5+WomcjFRIcvZ6QipadnDDD1RkfEAoVIc2YkpesRRsYIijdr8XOTTA/aMGvr7rjPzAvNkrPubS4xl8/OTlD/cK3tLnOTzs0"
"OVXYv7f7SEM21qmmm6HgLBoXHd6il42XqaE8Ta8lKRwOFB8+oZiarO8oHSwuRcJuVG5J7MfHyM/rXVtRTk1QWu3Vzc/caFFN"
"T1/i0M3P2ODh2fNrTT87TWrTtsDTt8W3obfNNBsZKLycjo2gpbsmqmQkJQ4NGzirm5y4Q0UpRWoyOTIyWsG9vsv8Xt7k7WNN"
"SE1g8t3Y3ebqbWdfW19x5tTMzdx2XlD352VTQjtRwLGurMxMw0zKyy0nLEqnlJGhqsUprPkpJRITJM2uqq9MSLlm1EkpLDZY"
"0b/UZtvP0cv0VFRWWV9fZvbg2t/naV1YXG3m3NzY2NPO3U9LQUbp605hW8Oso67gXihGwz88NzGslY+cpskkpmAlHQ8VK7G6"
"ycRMvqTYRS0mMk7dRV7+07u+3nVhYWBYTlBj7t7e3+jxc2phYWNt7djP197s+PH4TD1EU3a7zuLDt621vSsrTUnRxzLcn5aZ"
"qLAks7onIhUbLLfWOta/tqbqMTM6TD5NNlLBxsfS59/a/lNQV1tpbPnm3+bt/HZoYF1m+OTd2NrP0fVXU05VUUg71re+r87L"
"u63bKi8tR6rPOaudkp+hMDSgKSIVGSZ6widNq62uxi9AVVktM0NOvtfh1cHM8m5bWV1SVG787e3m4OPobWRkZ21yefbb1Nve"
"4/frY0RIV/1Wd9XWq7fi98nMTDspL66vSK+umZ+jPi2hLSUYIS9B7iXdp7DV7lTpWEErPdlLa3HJysjebtz1VlZcYGVk+vbe"
"6+nofXpgbmRib3fe3tvf1NdyXG1QT0s/+svAUruuu8BUP3HWMSRWouiquJ+ZobAdoEcgGxw6J1IiW6W3Xsu5yFE/NEXtNERl"
"0O7f0tPN63H+cVxdY3Z79enj6/V9dm1nbvr18enq521fa2RPW2Xk3NjZx7P35s3PUz04O9xiL8ijtqi3n6WjVi+lKikeKy4s"
"LSe6wkzKtry8bPPg2jw+ZUROSVn/5m3k1N/s4+/2b2hsbGtsfn3x5+nq6ex3dWFfaF1Z6dze2sTGxMhpwN9KOVZHNj032bhO"
"wqCopbCfqqJWM6cmKyApKyUjKdA6OsO/vrvjsr3ab9P4SFFCV0tGUWJYZX747+b67uv97n74fv56cnlvanhrefrv59XY2szI"
"19nU4vFMTUpaND9aP1dN2bK/xKenqrSltKhgOq0qMyksLygmMFIyPtPWyMbassjRy83dXl1UX0pNVVlTX2d3dPfx7PH37/b4"
"efv/d3Fvdm9scH7y9eTZ0tbSycri0d7dVUxIUkEwXkJIU1W7sdCtpKavqaivqiu0XicxJDIoJiZHOzFfzdW+37u13cjNzGhj"
"Ul9RR1FWVFlebn1z8PXs+/P3+/h5/vt3fXd1e250ePR96+Ha19rTytjf1+XyTlBLXDhAX0JYUd22wceqqq23qbirXz6wLTcs"
"LzMrKjVUNkLX2cvL27bL1c7P32FfWGRNT1laWGFle3n/9/H2+vV9+/18/v95/nlzfG51fPr+6OLa1trQytff1ubuTU9LXDg9"
"Y0FYT+W1vsmqqau3qLWr2DauLjMsLTQqKTBVNT3a2MzH4rXG18zO2WJiVWhNTVZZWF1jcnv++/Dx+/R++H3+fv/9fHt8enlv"
"ef9+/O3g393b09He5NHwdlVeWE9BQ+1ETmbiv8XUr66zvK+zub04u0o2Ny48NDAuS0w/WXvTwHfMxMrW6eTu61BbXFtXV1tq"
"amxw+/j3/f/6e3x4enr9ffv4+Pj6/338d3R5+O3x7t/V2tza09TbXuf+X01IT1JZPWHb3cnQwK21uLqutrrMP75DPDQvOjk0"
"LkZOTm1U1sDRz9rL09VgYPpeXFFWXWheY2r6+/x4/H1+cW9vb3BueHf++/f08fTv8vR+/379eW9+7+ns6+TX2N7o3eXcYlZf"
"W1tLTE3t6GLXzr63wb68tb/B61bNRkY4OD1BPjlJTv3gZd7Pys3Z4+rYeWlZWF9hXFpfav7/eXzy8PD/enN4cHFub3N3fP76"
"9e/t7vDz9PN+cm11en10e+vd3Nzf2tTT4n11ZnxVTEpNWVxfVNzKw7zFvbu2vcbUWtlORzw3Oz9FPUdJZ9Xa1tjOzMrU7vFs"
"e2haU1RaX2ZjZm387+zz+Pz5+f13cm9zd3x9/v379/Lx9f17e/59dW1tevHr7O3k2tbW3eXr4O5rWExQUldPT1RwzM3DycK8"
"ubrKy1zgYUlCNzs8SUJJS1XTz8rR1NHNzeH6WVthXVlRUllreHtzdfPo5+58dXn8fHZucHz9/Xv+9+3y+n388/ZxZ2Ru/fZ2"
"d/La0M7X29bXzOZlT0tRUE4+SUb0xdK/y7m2rrjGx0nJSkU5LTU3Sz9TRWXDyLrL0NHPzeNmSE5NWFtPUFZs6eDp8vTo5ej7"
"b2pwef56dHJ3/PL0+Hv88+jr/GtjaHJxZFtdfdnR0tbeyMjCz3JSVGRKUzQ6QkrA48XPuLCqq863PchuOkAlLy5GSFZMTr3F"
"r73PzmvN4mhFP0BMZFpbUF/j0tHd9fno3ut2XmNv9/VubWzy7Oz9ee/m3ul0ZGFra1tQTVV309XP1Me8trvE3U3NTm5ALUA7"
"yb3GyL+sq5y/tOs2rC5OKB0vLU5AQTjUuritYNtvVM8+OTM2Pk1PS1n+z8fN1uTm5u5xX2JidXx+/P19/Xt8enz86eHd4vNu"
"dWpmUUhJVPDN2MzMvK+0ud70VcVaNjcpYrm+tM2xpJijsL4srkkvJRghL1QxOj56prG/10PY7FgvLzU+blBMX9LGxNd4bvpm"
"XU9RXHzu6uHe3uTvaWBcXV9vavHbz8/V53Hf32NIP0vYzs/ry7ynrr7cSsrOUiImPtqlr2ykmpCcpTs4py0hEhUePk4hNbiq"
"obhGRtHGLi4pOW/7SETcx8n4UFXwfl1OVWbzb3p+5t7g6ez5cmtoYWBsffP0fOvZz95zd9rQ41NET8vG7PjPr62q+UXPyXIr"
"JyrbpK90p56SmqdBNKgzIxMYIjhWJDC1qKm+S03gxC8sLz91ZE9H2cbTZ1hZ+f1fVGJydmhmb/rr8vrs5+77bWhofnpsdfPm"
"4/Jy79vW6HLy1tTvT0ZkxtXm972wrLFFaM7MPysqN7Wmzrykm5WlsSu8vioeFSApZDInXq6qr+9NVMhWLDA0V2toSVnMy+Ja"
"Ul72fWBccvn1bWZkbHV5fvHu7vL/cm5wb3348u7m5+z+cv7m3/N66tjY9lRObMvf6PG/trG8TPDT1j8wMEe0reK3qp6drcQx"
"t20uIR4rMmoxMtG0tsFkZHvQRTQ9SGxXWlrl0OFlX2x4bGRmeu/6cnF1dW5uc3z2+fb6/Xt4dXh5+vbv7vX0+Pd+eX7v5ejz"
"fu7rdF9kcdzW29vOwdDgZ1tpXk1FXcrIysG/tbvA91/WST84PkVGTk1t0s/e5OHga1tVVV9eX2X97vf6/f99dXJ1e3x9ff99"
"e3p6ent+fv78/P////7/////fn5+/37+//////////////9+fv7//37//////n7///////////////////9+/v9+/v9+fn7/"
"/////v/+//9+///+/////n7//v9+fn7/////////fv/+//7///////7//v/+/v9+/v//fv7//37///9+/////37//n7+/37/"
"//////////5+/n7///////7+//////////9+/////////37/////////fv////////////9+//5+/////////37//v7///5+"
"//7//////37///////////////////7//////////37//v//fv////7//v7/////////////fv/////+fv/////////+////"
"//////7//////////v/////+///+//7////+fv///v/////////+/////v9+/////////37///////////////////7/fv/+"
"///+//7////+fv9+/n7/fv9+///+///+fv5+/n39ff18+nji1NrmcfNvb2Nv3Ojc5/X5+dfZ19LS087NztTW2up0eu/23t7O"
"z9jU1drc3d3l7e/m4e7e4+Tv3+j66O715N7o3dnm8/z0ZWjf7OXg2d7g4uHpbevm3eHV1dvW4t3R4+nY2dzh3d/h7f7m4ezw"
"6N7ncuntdX7f1uTd9uLi/GvY5H7b4dnp9+ni/eji4uLY3dvh73bf9OPZ5uPd33vhan19//rgff7c8eLZ6+Xz6eh63uDUfXrg"
"ZP1rd97c73jd6u3m2+pjamr7eHvbdObP4N7LztrQ4OHfZOvP3XHe1uZbY8zf82Ld1PBZb990WVbW7+7c1c3nXezu7fVp2s3f"
"5NLVe97f6eVh7fP42Oxr5udt39fe1vra2dzp3+ne2v3+5+Ht2uHV5uDj3Xn7aFzu+eJv3t/d4ura2OLb79vU7O3s33Hr19Hc"
"2+Hf7evo8GfrcO7d6tPZ2t7gd+Hx3d7k7N3i4ebp4Obe7uPY2N/b6edsfuXe6Nre39r62dHX3dxv/+7m3Ong3d7W1tjU2d/r"
"8ufp6dvX2ezv9eH6/d7V2eTb2t73/uz3afLn2Nrd3t3f6+Pb3d7a2tbd3fV7/Hz8ffx8/X39ff1+/n7+//5+/////37+//9+"
"/37+//5+////fv9+//7/////////////////fv///37/////////////fv//fv////////7///////9+//9+//////9+///+"
"fv/+//7//35+//////9+///+/////35+/37//37////+/37///7+fn7/fv7///////9+///+////////////////////fv//"
"fv////////7/fv////9+fv///n7///9+/37///9+/37/////fv//fv7////////+/v/+////////fv7////+///////////+"
"//7///////////9+//9+/37////+////fn7///7/////fv/////+/////v//fv7////////+fv9+////////////fv//////"
"/37+/////v9+///+//7///////////7////////+/37///////7///7///7///////7///////9+//7///////7///9+////"
"///////////+/35+/v9+///////////+///////////////////+/v5+////fv//////fv9+////////fv//fv/////+fv//"
"fv7+///+/37//////v//////////////////fv////////////////7//////////////v///////37/fv///37//v9+////"
"////fv////////////9+fv9+////fv9+fv//fv7/////fv////////////9+fn5+/////37+//////////7//////////n7/"
"//9+//////9+fv/+/37/fv//fv////9+//7////////+/v//////////////fv///v///////37/fv////9+fn7//v////9+"
"/37//////v7////+fv////7///7//v//////fv///v7+/37///7///////7+fv//////////fn7/fv///////v////////9+"
"//9+/////37//v//////fv/////////+fv/////////////+//////9+/v////////7//////////v///v//fn7/////fv5+"
"/////////////////////n7+/37////////////+fv7/////////////fv//fn7/fv/////+/37/fv/////+//////7+////"
"/////////v////////////////7///9+//////7////+/////////v9+//9+/////v////9+/v///v//fv///v7///9+/v9+"
"///+/////////////37/fn5+//////7///////7//////v//////fv7///9+fv//////fv/////+fv////9+/////////37/"
"/37/////fn7//v////7///////////////////////////////9+//////////7////+//////////7+fv///v//fv//////"
"//7//37//////////37/////////fv//////fv///v////9+/v////7////////+fv7/fv/////////+/////v//fv//////"
"//9+///+/v9+///+//9+///////+//7+///////+/////////////////////37///////9+////////fv///////37/////"
"////fv/+//5+/37/fv//////fv///v///////v//fv//////fv//////fn7/////fv7/fv///37///7/////////////////"
"//9+///+///+/////v////////7//v/////+///+/37///////9+/v///37////////+//7///////////9+///+////////"
"fv///37///////////////5+/37/fv//////fv////9+////fv9+///+fv///////////37//37///////7+fv//////////"
"/v9+////////////fv///37//v9+//////////////7//////v7////+/v///////////////////v//fn7//v////7/////"
"//7///////9+////////////fv////////7/////fv//fn7//////v////7+/////////v/+/37////////+/////v//////"
"/v////7///////7//v////9+/v9+///////+fv9+//5+/v9+//9+////////////fv///////v7//37////+///////+////"
"/37+/////v9+///+///////+//9+/////////////v///v////7///////9+//7+//7//////v///////v9+fv///n7+/35+"
"/////v////////9+/////////37+//////////9+//9+///+/////37//v//fv9+fv///////37/////////////fv9+////"
"//9+//////////////7////+fv//fv9+/////v///37//////////////v///37/fv/////+////fv7+//7///7//37/fv7/"
"/v///////////v///////////37////+fv////////////9+/////////////37/fn7///9+fv////////////7/////////"
"///+//9+//////7/////fv//fv5+/v/+/////37//v7+///////+/37//v9+fv9+fv///////v/+/37+/v//////fv//fv7/"
"//////9+fv///////v///v///////37//n7/fv////////7////+/v///37//37///9+//7///9+//7///7//////v///37/"
"//9+/v////9+/////v////////////9+//7/fn7///////9+fv/+/////37////+fv////////////////////9+///+/35+"
"//7+fn7////////////+////fv///v///37////+fv///37/fv7+fv7/fv///////v5+fv///v///////////v//////////"
"fv///////37/////////fv////////////////9+fv/+//9+//7//37+////fv////7+///+/v///37////////+fv9+////"
"/////v///v//fv////////7////////+/////////35+/////////////////////////////////////v5+/v//////fv//"
"//9+//7////+/37///9+/////////37+//////////9+//5+/////37+///////+//////////////////9+fn7/////////"
"/////////////37//37//////////v//fv/////////////////////////+//7////////////+////fv9+/n7///9+////"
"fv9+////fv//fv///37///////9+/////35+//7////////+/37/////fv9+///+//////////9+/////n7//////////37/"
"//////7//v////7///9+///+fv//fv9+///////////+//9+/37//////37///////7///7//37////+/////////35+/v//"
"fv///////////v///////////n7/////////////fv7//n7//////////v//fv////9+/v///////37//////37///7/////"
"//////////7+fv9+/////n7////////////////////+/v///v9+//////5+////////////////////////////fv//////"
"/////v///////////////37////+/////37+////////fn7+///////+fv/////////+fv7///9+///+/37/fv//////////"
"/////37///////////////9+/v////5+////fv/////+/v/////+////////////fv/+/////v///////v7/////////////"
"/////////v/////////+///////+/////v///37//////v/+///////////+/v9+fv7////+/////////////v/////+////"
"/37///7//////////v///////37///7/////fv9+//9+//7//////37+/////37//v/+//////7+/v/+/////n5+/v////5+"
"/v///v////9+fv7////////+///+////////////fv///37/fv////7/fn7//////37//////v///////////37//37///9+"
"/////37//////////35+////////////fv////////7///7+/////v////////////9+/////////////37///////9+////"
"/////37+////////////////fv/+/////37///7/////fv///35+/v////7+/v////9+////////fv///v//////////////"
"/////////v/////////+/////////n7//n7///9+fv///////////37////////+//9+/////37+/////////v///////v//"
"fv///v///////37////+/v7/fv////7//////////////////37//////////37///5+//////9+/v//////fv7/////////"
"/37///9+/37+/37///////////9+fv//fv7//////n7//////////37//////v7//v//////////fv/////////////+///+"
"//////////9+fv9+//9+fv///37//////n7//v////7////////+fv///37//////37//n7////+//7//////////v//fv9+"
"//7/";

// Spoken phrase: Fine
#define FINE_AUDIO_SAMPLES 7884
const char FINE_AUDIO_BASE64[] PROGMEM =
"//7/fv7///7///////7///9+/////37//////v//fv////////9+///+////fv//fn7//////////v9+//////9+/////v////9+"
"/////n5+////fv/+/37///7///7//37/fv9+///+//9+/37+fe/f5N7e3d7c3OPd3d7i5OPg5unn4efr4N/e3djf3uHo2uDt5/Tc"
"533i8uDi69/h3t7b3OjZ3+Dg5tzf7+Lo6OPy4O/w5Obi4Ovd3eTg49/r4eXp6/Hn8unq8ujd49/c8OXc5OLg5eHq8eni7eXw7eDk"
"5uzd5ePg3uDf4u/g4uPt7Or17er14+bp5ePe493e39/k7OXm7e349ejs7uXf2+Pm2dzg3uLs5+vr7vTw+eB85d/o4N3b4t/e4+P3"
"fezz6/bob+fu3uPx3+fc6N7g4+jk7uHq8On23ujs5+/l5evw6ezh6e3b3dvm3Nvl3+Tf3/Tn/vzk+OVzbt/s4N/l3t7W5dfg297g"
"3Ozm7d7s3+vv3+zt6Xfm8/Tr9e7m4N/a69bZ09jd0d7e2vTu6v7qb2jz+e/j49zr5N/p4Xdv4e7t2eba2ezZ0t3Y397a7er97O5v"
"3vhp4u3t2ufr2d3e193s2+Tjbe5f8s1+4Ofb1Nbb1dPd7erq6nf0de7vdej/293W6t319trW3Ore793m7F7e72Dq+PXc1PfZ2ePY"
"29/b7Hzi7+p889no3+Lo3PD73+v+43d88v384fv80tXj4trez9/d2+nf6H5y3nrp6/zk3Xrn2+jl59f25djZ383Y79zc5uPwWmtG"
"UHzm2NnczcvQ08/G19bo7vjd+vnefePl+Xzd6OLp6Ozt4+De9Obe2efn9PfefuV2/eX1e+b6Ztfv3Nnv3tfZ69vk5Ont3+3d+Ovk"
"9+/57X3a5fXr4OPn2efe3N3t4t7r1eXk4d/e2tLf49Pd9N316Nvc997k3tnb3+ve6uB96/p66ez8eX7d2eHZ1+PY1t/d6uHw2Oh5"
"2t7l3NXq4eb47OjrdODdeOXe59bYa+DR3Oft5eDW4fDk5v7s6nLn6+7u5+Tk3uHn5uff2NzX2djY2d3l7OXj9+3r6Ojh3d/Z3ODd"
"3ubp52FNSE9wzsbH2F1CQEr2wbm+3VQ7Pk1U7MnAurTOOzcuV6+prrzkP8B1PC4oLuOutblRMjg+R2bSy724z108Nj5tyL6/2VVE"
"Qkz1zcbK4FdKSVbkycTJ500+Qk/Pu7jG/UM3Q0xa08G7srVONS4vvaimsr1EZr4/NCYnN7Ots8Q2Lzg/Td3KwrW97kc1N0jPv7vG"
"a0c/Q1vUxcXPYUY/R2nIvb3NVzo4P/C4rrXVSS81QknlvratqckvKyNcpJ2ksOQ3tVUvIhwkyqKqr0EnLTU9Zsi/sa3JUTAsNXm8"
"s7bSSjk4QufBur/YTD89TN++ur7bRzQ3Qs6yrLfoPiw2QlHRurSsqdssKSPcoJuksls2sFUxIBsixqGqrT8oLTM6VM7ArqvAWS4p"
"MFi+sLDFWDk0OVnIuLnGWzs0OVnAsbC8XDYrMEa7qKWyWzAjLkJjv6+tp6RfJSIdvZmUn68/L6hRLRoUG76co6c7IiotL0LbvKah"
"tmUoIChHvKqpt/02Ky0+zrKttNo/MTI/z7WtsspCLyk0W7Gko7lEKiAwTtK2rq6oqD8jIh+tlpOitDM2pkotGBMbtZ2jqDUlLC0u"
"Pf6+o5+ybScfJkO+qaey2jgrKznhs6yvyj8sKjLssqepuEgsIic/t5+cpVMqGh8+27Opqaafuh8eGEGTjZOq6h6nsi0aDA8umZ2c"
"yB8kKSctRM6mmKK7LRscK9esoKi3RywlKz+9qqiw8jUrLT7ErqiuyTssJS5VsJ+erEEoGyZOxK+qrqyl1SEjHN2Vj5mvUCOnvTEc"
"DxQ7nKOh4CYtLyotPuKom6a7Lx8fLvWxpqu3WjEpLDzLr6uwzjkqKTTcr6eouUstJSU7xKOcn8ItHRkv0rCnqLCsqTcgIB2mj46e"
"tSstnl8uFA4Vvpykoz8pNS0mKTfSn5mlvSodHi1br6Wor34xJygy3bCoq7tHLyswTrysqq/YOSspLme4pqCn5y0fHTm/raitv7qy"
"OygpJKiUkqXBLC6g1jkZEhjUo62pTTVJOCknMV2mnKa4NCQiLT+/rKqtxD8sKi9Jvq6ss903Kio04rGoqbRVLicpM8mvoqCpUSgd"
"HD62pqOtzMG3MykpJ5+SkajGJTie/TMWERi7pa2qRTtXNSQjL/qgm6W5MCMhLDvBq6ervj8rKC1Ev62qsNQ9Li43/risrLZlNCss"
"NtK1qaas6y4iHji9q6WuzMu6PS0uKKmXlqjALTekZzUaFh2/p7OvST1WOSgnNemlnqu/NSgnMD/Hr6yvx0EuLDJPv7CuuOw4LC05"
"2bGqq7pKLiksPb+vpKWuRiodH0u2paOx0b2/Ly8lL52Ula3GI8WoPCcTFSOoqq63OkRNLSInPLudn67jLCYnM0W6rKqx1jcrKzZ4"
"t62uvVo4LzNFyrKtsMo/Ly40XLyxqay7Py0fJ121pqe66cTXND0pP5+Zm7DKJ7a1NyUXGy6otrrCRWJWLCMqS7igp7fqNi0sND7E"
"sq21zj4xLzhYv7SzvP88LzE82ritr7pWNCsuPcGvq6u43jgsHithrp+ivU/kWjlWKE2el5muyCStvC8fFhw0p8nEwW3MViYeKFO4"
"oaq0xUwyKiwy1rOsr75ONy4xPtq5r7PDVDs0OEvOubK3xlE4MDNFxa6urspaODcpLlG/pKKyUUtNQNQ7N6ifm623K86wMyoaIC2w"
"ylq/zMbpLSAoTc6urri30UYtLjBQxLa0utFLODQ6T8u7tr3QSDkzOk/GtrC4z0c2MjZJxa2srM5EMDUsM1nMpZ+re0I/PMVINKmf"
"maitLd2vLCcZISux00m7vsRXKh8oW921r7Wxx0osLjFNy7m3ucxOOTU6Ts28tr3OTj85P1HOvbi900w8Oj5UzLazr9NJMzkzQnHd"
"q6St0Uk8PMROOq6nnKmtMte5LCgcKS+27Em6u85HKyYuc2TEtbe1z0wvODpO3b+8vtFQPjxCU9TDvMLRTT45QFXPv7q90U48Oz9t"
"ybm5r81OMTMzTcvlrqWovFU0MMvxNK+pmaGjPVW0JCIZJy652EWvr88+KicuTT9fs7WwzHA7RD0/V9THyNF8WEpKT/LWzNLcb1tT"
"U2Tq0NHW+2hVU1X7zb7H09RTSjc8OcC6v7WwtMZNLS7CYlSvqpujpTPC3iIgHS80wUHjqbNpQTc4Nzsx9bvEv8rHdW9BSlVjXunj"
"4PZuaHJvbf/17nj+avT65Xzp/Plia2XY1NbZxdxhSDxIT2BBtbCyucjj4EooK7jSu6ummp+mLLJEHh0eLixkMrykunrO70UyNC90"
"XknMv7/lzXvmXlNSbF9dZn13ef787v3vcPFt/W3+du737eLZ3+jd3v1aVUlxSUxcvMnLub+82j84XkUrTrLAqLKio6FbPa4mJR8v"
"KTM5P6278Lu120VKRT1KNVno82HNxtvZ3957ZVllXF5d/nrt+ej3/W9pdXH189vc2+rf/WhRUFBrSVbNx9LLtMa8flLrTSwv7Esz"
"trOoo66go6Qqt9geIyQuHzItcLlWxam438q9TERIPE86QVHoVPfL0djV1NztcXZqY19wZHF0+/nr6O3f6u94emRaU1VeWlDlz9/P"
"wr3NwefM3Do/WTwtSMxGzrCzoLKqqaJLPq0nKCc1JCo3N8RddbCy1ce50kR5RUpCPk1aVVXU1+DXz9zg8vdua2FnbWpufP1+6+ro"
"5t/m63dwZVpOUF5XTurN3M7Cusm/3szNOTtRPyo7y0x1rbWfrKumoMAsqS0jJS4mIjcuy+NQs6zKy7i/Q19LREU6SVRYTdrR3dfO"
"1Nru+nloXmFqZW51enX1/Pjv5+vk4Oz0eG5dWU9ZYFZZ09bbzL/DzMX3x1g6Rlg1L2rXRLy3raW4qaqnMc29JSsrNiMyM0fGTs+u"
"u9vBu2pLbURPP0RPXk9h1N3f1tLb5O74a2pfaWZubXx8ff76fu/u5+bi4OrwbG5cVU5aXlRcztnUy7zFyMnrxUY5S04uM9Z4SbC5"
"paixp6WuKq5FIyguLSI4L93UTLqtxtG6vkpdT0ZKPElQXUzy1t7e0tPZ5un9dWhiaWhubHx4ePb+8+7t6+zmfnxqaVtZWGFpZXXM"
"0djJwMrezWjfSDtKTzc51M9Pt7Svqbisrqw2zcYqLi04KDMwPddIW7m80cm7yeXbVfdRRkhXT0lYZ2x28ufd5ujp6Pf2/np9e3t8"
"/Pv8ff13cmtnaWlnZfDm4NrRy8jYzsnTXGF1Rks8P09LPFq8yNytrq2qtqyvrzrVxi0vLTYpLy0wXEE92MHP0b++yMPp1NJtUVZf"
"TUxOU1hZWWhwdnf49PHy9fT1+v1+fHd0d3RzeHv08Ozl29nb19PU1+nq3mtSTmNKRkxIXGJZZ8K9zr2ut7C3uLW4xz/DWDg3Nzow"
"OC84TT49XOjv79PQy8bc0c3S6fLqemtfXF9gXF1fZWdobG9vb29zeHb68e7l3dza1tbY1970729dVVZXU09TWF9cWGv3b2Ly5t7W"
"18a7uLivq6yrra6wt9V7XzkxLi0rLCsuODg6Q1VWXGVt5e5179nb39nTz9HV1dPb6v5vYFhRT09PT1JYXGNw8OPc2NLPzs/P0dfg"
"731qW1NUVVFPU1lfXFpw9W1k7ebc1tXCuri4rqusq62usbrh91M3MC4tKywrLzg4O0ZXVV5ldOPzderY3d/Y0s/S1dTU3e19bV5W"
"UE9PTk9SWF1kc+3h29bRzs7Pz9DW3+79a1xUVVVRT1JYX1xYbPdvYvPm3tbXxru4uK+rrKutrbC30XhkOjEuLSssKy03ODpCVFdb"
"Zmvl63Tx2dnf2NLOz9LS0Nfk83llWFBOTUxMTVFXXGN3697a1M/MzM3Oz9Lb63tsXlNPUVBPTlJZYFxce+11aejk29bWxbu5uK6r"
"rautrrG42HxfOTEuLSssLC44OTpDVldZZGfv/Gd83t/k2tLOzs/OzdDa4Op1XlVPTUtJSkxOUFdfbfXk2tPPzcvKyszP09rpcGFc"
"Vk5MT09PT1ZebGNm6+L5duHh2tjWxLu6ua6srayurrK64/lYODIuLiwtLC86OjxIWlddX2bzbV5t5uvt3tXPz9DOzM/W297ua1tV"
"UE1KSktMTVBXX2/25NrSzszLysrMz9Xa5XhgW1dRTUxPUE9RWmVzZnTh4P363+LZ29LBu7u3ra2tra6vtMB27Ug2MC4tLC0sNTo6"
"P05bWGFec/hkYPzl8Ojc08/Q0M3M0Nfb3/NnW1ZQTEpKTE1OU1pjdPHh2dLOzcvKy83R1tzrbl5aVk9MTlBQT1Vda21l8t/qdOnf"
"39jbyr67u7Otrq2vr7O50nRvPTUwLy4uLi86PD1FWFpcYmH5d2Fo6+jw49rS0dPRzs/W297oeGFaVVBNTE1OT1JYX2z+697Y0s/O"
"zczN0dba4vxnXltWT05SU1JTW2RwaGzo4vV64uLe29rIv729s6+xr7Kzt77m+lw8NzMyLzIvNT4+QEtdXF9iZfltX2rs7vLk29TV"
"1tPP0tjc3ud8Zl1ZU09OTk9PUlddZ3Xx5dzX09DPzs/Q1dvf6X5nXlxZVFFUWFhXXGZze23x3+d77uPm3eLUxsDBvLO0s7S2uLzN"
"cPJJPDg2NDQ1ND1BQUhZYmBoY3f+ZWF47/7z5tzY2tnV09fb3d/p/mxkXlhUUlJSU1VZXmRt/Ovi3djV09PT1Nba3+jvemhfXl1Y"
"VVdbW1pdaHR+cfni5n765Ork5dvLxMW/t7e3t7m6vsx18VA/Ozk4Nzg3PkVFSlhmY2pkc/ppYnLx+/bq39vc3NnW2d3f4Oj4cGli"
"XFhWVlZWWFtfZW18/v/+/////v///v////5+/////////v//fv//////fv////////9+//9+//9+//9+fv///v//fv////////9+"
"/v9+fv//fv//fv7//////////////v9+//7/fv///////////////v7///9+/////37+/////////v7+fv///37/fv/+/n7//n7/"
"//9+//7////+/////37///7//////////////v///v9+/v7////+/37//v///////////n5+//7///5+////////fv9+////fv//"
"///+fn7//37/fv////9+/////v///////////v//fv7////////+//9+/v7//v////////////9+////////fv9+//9+////fv//"
"fv///37//v//fv9+////////////////////fv/+fv9+/n7+//7/fv9+//////////////////////7//////////////////v7/"
"/n7///////////7/fv9+/v/+/v///37//////n5+///////+///////////+/////////////////37//////////////////n7/"
"/v7///5+//////7//////////////v7//////v////////////////7//v///////////v////7//n7+/////v///////35+////"
"/////v///////v///37///////7/////fv///37/////////fv//////////////fv////7//37////////+//7//v//fv//////"
"/////v7+/////v////////7//////v9+/v///37///////////7+fv///////37//n7//37//37/fv7//37/////////////fv//"
"//9+/37/fv///37////+//9+//7//37//37///////9+/37//////////v//fv7//////////////////////35+////fn7//v//"
"fv////7////////+fv//fn7+//////////9+//////9+///+/v/+/////////////////v9+////////////////fn7/////////"
"//9+////fv/////////+///+fv7//////////////v/////////+/37///////7//////v7////////+/37////////////+/37/"
"/v////9+/37+////fv7//37////+fv7//37/fv7+///////+////////////////fn7/////fv////7///9+/37/fv//fv//////"
"//7/////fv5+/////////////////////v7/fv7///7/fv//////////fv////9+fv///37///9+//////////9+//////9+////"
"////////fv/////+fv////////7///7//v///////v////9+//////9+fv///37///////7//////////////v////////7//37/"
"/////v///v///////37//////////v9+////////fv//////////fv7////+//////////////9+/v9+////fn5+//////////5+"
"/37///7////+/////////v7///////9+/////v///////////////37///9+//9+//////////////9+fv/////+//7//v/////+"
"fv///////37////+/37/fv7+////////////////fv7////+////fv/+/////35+////////////////////////fv9+//9+////"
"/37/fv5+//////7+//9+/37+fv//////////fv///////n7///////5+/37//////////////////////////n7//35+fv/+/37/"
"//9+/v7///7//////////////////v7//v/+//7//////37+//7////+///+//9+//9+/v/+/////////////////////n7/////"
"/////37///9+fv///37///////////////////////7//////n7////////+///+//9+//9+//////////7////+//////7/fv7/"
"/v////////7////////+//7+////////fv///v9+//7//n7//37/////fv/////+/v///v/+//////9+//9+fv///v///v///v//"
"//////////////7////////////////+///+/37//v///v///n7///////9+/////v7//////////////////////////v//////"
"/////37/////////fv/+///////////////////////+///+//7//35+/37///////////9+////fv////////7/////fv7+////"
"/v7///////9+fv//////fv//////fv//////fv7/fv////7//v///////37////+//7//////37////////+/v//////////////"
"//////////////////////////////////////////9+/v////////9+//9+//////////////7//v////////9+//9+/n5+fv//"
"//9+//////7//v////////7///////5+//////7///5+//////5+fv9+////////fv///////////v/+fv//fv//fv/+////fv//"
"//9+fv///37////////+/37/fv////7+//////////7/fv////////5+////fv/+//7////+//////////7////+//7/////////"
"/v//////////fv/////////////////+////fv//////////////fv/+fv/+////////fv7/fv///37///7///7//////n7+//7/"
"/////v///////37/fv///////v/+//////9+/v7///7///7//////37///7//////////v/////+/////n5+/35+////fv//////"
"/////////37//v///v7///////9+//9+/37////////+////fv//fv////9+/////v///v//////fv7///9+/////////////v//"
"/n7////+fn7///9+/37//////////37/fv7//////////35+/v//////fn7////////////////////+/37//////////37//37/"
"/////////37/fv////////////7+////////////////////fv//////fn5+/////37//v///////37//v/+////fv////////9+"
"///+///+//////////7///////////7/fv//fv////////7///7/////////////////fv///37///9+fv///////////v//////"
"fv//////////////////fv7///////7+//9+//7/fv///v////9+/////////37//////37///9+/37+/////37/fv7/////////"
"/v///////////v///37///////////////7//////v///////////37+///+/37//v7/fv////7//37//////////v//fv///n5+"
"////fv///37//37///9+//7//37/////fn7///9+/////////////37//v//////fv///////37+/37///9+//////////////7+"
"fv///////v///////37//v////////9+//////////////////5+//////7////////////////+//9+/37//v///v///37//37+"
"fn7+//7//////v7///////7///////7/////fn7///////7/fv//fv///n7//37/fv9+//////7///5+///+fn7/////////////"
"///////+/v///////37///7/fv/+/37+////fv/+/////v////////////9+//7//////v////////////7/fv//////fv////7/"
"//9+/37//37///7///////9+fv///37/fv//fv5+/////35+fv////////////9+/v/////////+////////fv///v7///9+////"
"//////////9+/v///////v//////////fv5+//7////////+//7/fv////////9+/n7//////////v/+/37//////37///7+////"
"///+fv9+/v////5+/v///37///////7///////////////9+/////37///9+/37///7//v////7///////////////7////+////"
"fv///v7///7//////////////37//////////v7/////////fv7///7//////////v///v//fv///v////9+////fv///37/////"
"///////+//7///////////7///7///7//////n5+/37///7////+fv///////////v7//v//////fv////9+///+//////7/////"
"//9+///+///+//9+/////35+//9+/////n5+//7/fv//fv/+//////////9+/////37////////+fv7//v7//v//fv5+//9+////"
"//7/////////";

// Spoken phrase: Like
#define LIKE_AUDIO_SAMPLES 7120
const char LIKE_AUDIO_BASE64[] PROGMEM =
"fHZ0dHNwb3X/8u3r6ODd2dzf3+Xf6n1sam/18Hbf4crGxcHDv769y8t8925LSjw+PkNARkVNZ2bnd/Hl4OT1al9kXl5YVllcY2Zp"
"bHn38fL19PT3fnZ1dHRwc3vw6OXj3dnV1Nvc4eLkcmFYXF10W23y3MPHvsO8vbjBydFc20lLPDk8PUE/R0Rsd97Y69XX09zwaGNj"
"W1pQU1ZbYGNob/zx7e/x+Px8eXFva2967uji39rU0c/a2+Xj7mhUTU9OYE1aaOzCxrzAubq0usjGUtJJRjwyOjhAPEVBXt/cyt3M"
"zMvN3/1jaVtZTk1QVl5iZm366uPn7fr7e3psZmFqeevl4N7Uz8zO2dzp5G5cSEhHSVVEWVXawMK5vrO4rrrBxkzKPkY1LzY0PjlE"
"P3rV0cLYxsfIyuh0Wl9UUkpKTlVfZmt96t7c4Of3+HVzY19bYGv47Obe0s3Hy87S1tXnbUhNQ0xQP1dL1MC+uLuwtKq7u8lKxThD"
"Lyw0MTw4Qj/lzs2/28XIzM5hWExPS0pGR05WZm527uLa2t7n8/lzbl9cWV1mdvbu3tHLxMjJzMnN0mxMUkBbRT9SSMfBubq0r62n"
"w7FdaM4wRCgsMDI6OEFCys3Dv+fB0dHkTEtFSUdHQ0pTY/rt6d7Y1tje6/R6bmNZVVZZYmdr9NrNxMbGxsLGxN9WZUBnRz1OQM3A"
"urmzrq2jv67lS74tQigmMS46Nz9BysXGu3jF0fPnQUI/QUZFRkxd/97Z2tPS0dTe6PV1aF5TUVFRWFZYb+TOx8bGv76+vW7dUkvl"
"OkhAS8O+t7mssaGovqs4wVksPh8rLi86OUBRvcu9x1jFV19NNz49Rk5RXe3VzMrP19vm7nltbX3r6Ojw82tgUkhGSE5dZfXRv7uz"
"vca/asFSOT4vWdTNwbetpZquqMI6tCouIhspL0JJ9l24rsK6QDtENkA4OUT0yr+/ysjN2exbT05SWV1rbPHw6d7b0s/S4PdrWlNF"
"PD9Be87IyLy4s658VTkwekRKXdGtnZurpkfszCUqGh0taMu/vv2yvWdMKi02RXDW0sq4vr/ZT0dJSldebe3d2tnf/GpYWldiafTq"
"z8a+v89PSDk+RDc/Tb+roq682DfSQiowLbqbkpyevjqzJB8WEyLDqa2qS1/PNzMnJjjBsa+x39b7UUc+PErrz8fN5mxcWWF859rb"
"429USU5Y2MXGycnMzdE3MSwzzbGvsbC/rr0pJx42opOWnao0ti8dGBIev5ykqEEmMy8yOUBrsqm3vkI1PUtbeOpn29rY5PVYV1Nj"
"8tPU0u9rXWVf6V5PYOu9r7DxTi0sQT1Fy66glqArHQ8hn4+Pl6Yury0XDw4cqY6aqSMRGi7rr6u4u7NTRDQwQdHAyc9QUFzs5eBd"
"VE9q5tDX4l9mb97T1lNLRFDGtLXF0zU6Oy81eLOelqQuHhInoJKTm6wvry8bFBEfrJKerSgWHjFdurC6t7JZQDMzScvAy9ZOUlvz"
"4N10V1JX+tfO1/BaXXLazcpeRT4+zrKsu8YyLTcwOfC1opacPCASGamSjpieMb/xGhUOGGCRmKY/FxoqPda1s7Opxz0wKzvWuMLK"
"UEZMbOHQ22tVTVZu2NPS62hSX37RystaRTxByrGtvNYvLjg3Ssq1ppukOCUWIKiWk52nMLpUHhgSHNeWnqs+HCAuO2e+t7KqykAx"
"LT3Xu8LJV0lJWu/P0+JbT01d7c7M0v5VTlrey8XLXj89PM+xqbbKLycyN1O/sqmcn0MmFxqtlY+aoDPNwh4ZDxg+lpqpZx0eLDRB"
"zLevpbhCMCo2dbu/wHxMR01f2M7X+FJMT2/ezdHeXlZPednLzNRQRD9FybKtu90tKzc+37u1rKCpOyoaJaqYlZ+rMbtZIRoUHtSZ"
"oq9AICczN0LOuq+ox0IyLz7ev8jIbFBMUmHc0dn4Vk1Qa9nLzN1fUU9t1szL015ERD7ctqu0xjIlLznpt7GuoqJeLRwaupqRm6A/"
"YbohGxIaNpucrtQlJDAwMUy7r6SvRzUtOVTDzcnXZFJPUW3Z0tp0UktTc9bQ0vljU11+09PT71xLW07kwba3w0coLjbisK63sqzJ"
"Pi4cR6WYm6LANrEuIRgbK66euMM9LjgzKzLTsqyscUI6P0Rk7tzJ1HRPTlTv2dXkb1lUW3je0tThaVdSZd7LztlWSkrr7sPBvcHM"
"PSguM8Knp7S4t2xJMR1ppZaan8s4rygeFhwsq6DKxk05OiwjLcqvra1wWUVEO0hg2MLP/FFRUGzx2NvfY1lRXvXa1t/3XF5e9ObT"
"2dloTkhs3MG9zdh1XDE4NWOtp66+x19JYyc5r5+bo60ytz0jHR0t2qXP1MtSQDAnK1m3vrTM2n5RPj1bd87V4GNlVV1p4d3c/2db"
"YGjr4Nni8V1aW/Pc09/ob1hRX9/Ht8zcSVE2QTxGsKWot85IP9MuL7KkmKCjNMBkHx0aLEKoyf672EEvJik+vNe9vcbMVEQ6X1vl"
"6tz481taY/7z7en6+2lvbuj27G58av149uze725oZejX1eTD3vg/QTp0y3S7r6252zcw4Vw3uK6coaJOWb0jIhwtNr7YUq6yeD0x"
"MjpKPFG6w7/b0WT3TklZaWBs9/Xofn168G10Z3xu9HTs8+nv7ndyfGxvcPXo0NzYz8pfYkFISFdEXLC5tL7B0tE6Jju5VrOvn5yf"
"tC6tLCAcIy43bziwqMVd41g+MzQ1z1Vfy77H39Vs71lRVGpbZm7o+vH+9nBmZGhxdO/y6Onr/Ht7dPz+7N7Y6N/e6V1aS1dmTFTX"
"uty+wL/DYjo+dDouybm5qbKhpqQ32r4jJiI0KDk1Wq7H5rW37UlbRkJEN2VkXl7Kzd3V1N72bGJmXV5idGp7dPZzeXd9/Xv4/PT9"
"fO/t5uvc3d71e2xfU01MfWFX0cPC0LrLvuE/VlwzLFfeOsWwrqCzpaehOFiwIiclNCAsMjvAT92ttNjBt+JHc0FNPT5LW09X0Nbe"
"1s/Y4/f4bGxdaWlqanh7cvv/fPj4/Ovs6eze7e59d2hcVFZfWlPk0ODPxb7PxejO4jtBWz0vS81IzrO2o7asq6VHQ68pKio4Ji04"
"O8VX9LG22si720p2Rk9BQU5dVljW2d/a0tvj+PRtbmJna2ttcH51/v99+/b87O7o6uHl7PJxb11WT1xcUl3O3NTKvMXIyezFRjlM"
"Ty401npJsbmmqLKopq4qr0sjKS0uITgv+c5Mva3D1by8S1VZQks8SFFeTu3S3N3R1N3r8/1pZ2RoZG9qcnB8eXr5/vn08u7u5u3n"
"6O99dG1eWFVfXlVw0eLWy7/NydnczEE/V0swPtRXX7W+qK+0rKm7L689KSwzLyg9Nd3YVL2xy9e+xUthUkpMP05XX1Pg1+Le09zl"
"7fZ1bGpmcGtvcHx1en57fPj+/vT48/Ds6+jn7ef4bWZoWlFaZVxq6dXL1cjEv2jdxkg9Sk03PV9m0L7Hpa65rqfAMK88KC46LSZO"
"P0zp0L68zcbC0Upp8D1HVFJIYOZ269jW3eLg6HtwbG1paG5xc255fXF4+/56+vf5+/L38fHt6+7s9/12aV5hZFpc7Ptx1c7U2MbT"
"5uJiYklESVBGSsy/X7arw72vrzvfvy4xQUAqPN4+TsTI3cjA0HzZbFFTUlRMWWdmc+fe5uLc5/jv/G1rc25sc3l8d3r8d3n7/nv8"
"9H789fr59O7t7u3t8XZrbGRaXm5hYN/Z+dPI0+3MzUxe30U7ZGw8XLnee6yzzbyt2T27TC0+TTIu41Q70L552rvMX9fPSk/0Tklh"
"dlVp2vD73N/u6ex7b3Vwam93d3N8+HZ1+f9zfP5+/v//fv9+/37//////////v///v/+fv/////////+///////+//////7/////"
"//////7///7//37//35+///+//9+//////////9+/37/fv/+fv/+/////v9+//5+/////////////////////////v/+//7/fv9+"
"///+/////v7/////////////fn7////////+//7///////5+/////v///v///v9+//7//////v///37////////+//7////+////"
"///////+//7///////5+//7//////v////////////7+//7//////////////v/+////////////fv///v///////37//////37/"
"fv7///////////9+//////////7////////////+//////9+///+fv///////37//37///7/fv//fv7/////fv7///9+///+//5+"
"/v/+fv5+/n7+//7/fv9+/n7+fn7p3eve4fJoce3o6O/r3+Pf3ePk495z8HbW5NxHQM7lbOvE0OnTz9Hqzcvg6ubbaO/q3VZT1+Zn"
"78zi59vP6/De4+1v+Pf/7d/q3dfX2t3W3N/a/u/o7nj/59/h29XXz9rZ493gcfLk7Gvx5/D47+nu5+nj2+Dg/evhee/f9erk3+Xx"
"2+Hj5Ovk7uzi6uDd3+HX4efe4ej24Ofq6dzb39vP693T++3j3/3e2uHf4N3q9+Lncuvt5/Dr2OHe6erf63Px4+7k2dTW2djU19rd"
"4t7e4Obr3PH/3+ry3dTs4NPj/ujiePXq9fjr6Ofb297a2ePm5uZ1+3tn9nBv8d3e/OHY6Ovd5vjg6Ozp793i4+Pm3N7d7tnbbeHm"
"7n7/7PDtduLa6N/d23336PJ1eObl3trZ29jb7/zefHHo3uLp3dnd3+Dd3e3u7Orq4+Ti4t3p5N3j5tve5t/b3O3k4+T58+Px6ero"
"5O7v7e3w6/jl3+Xk6t7d5+3v7erx49zi5tnb4ePf4t7p5d/w3eXk7ufg3+Hn3d7m5ODq6eju9urs6ejh4eni3N/o6eXo7eXj6n3+"
"//////9+/n7+fv5+///+fv5+/37///9+/////////////////37///////5+///+fn7//////v////////9+fv///n5+/////n7/"
"//9+//7///7+///+/////v//fn7/fn7+//////7/fv9+/////////////////////37//35+////fv////////////5+////////"
"//9+//9+//9+////fv///////////35+fv/////////+////fv/////+/////37///////9+///+/////37//////v7//37/fv//"
"//7+/v/+///////+fv///37///9+////fv9+/////////////37///////9+/37+///+//7///////////9+/v/////+////////"
"///////////////////////+/37/fv7/fv///////////////////////v//////////fn7+//////7+/v7//////v9+fv/+////"
"////////////fv//////fv////7///9+fv7//v///v7+/v///////v///v//////////////////////fv///////35+///////+"
"/v//fv9+//////////9+/////37/fv////9+////////fv///////35+fn7//////v///////////37////+//9+/////37+fv//"
"/////35+/////v/////+/v9+/37//////////v//////////fv7//v/+//7///////////////7/////fv////////9+/v/+////"
"/n5+/////////////v///////37//////////////////35+///+/37///9+fn7/////fn7+//7///5+/v///v///////37/////"
"//9+////fv///v/////////+/////v////////5+//7//n5+///+/v/+//9+//9+/////35+//7///////////////9+/v/+//9+"
"/////37/////////fv///////37///7//////////////37/////////////fv///37//////v///v////5+/v//fn5+//5+///+"
"/v////9+/////v7///////7+/v///////v5+//////////7//v7///7///7//////////37////+/////v///37//////37/////"
"/////37//////v7///////////////9+//9+//////7/fv///v/+/////37///////////9+//////////////9+/37/////////"
"/////37///9+/////////////37///////////////////9+////fv///37///////9+//////////9+//9+//////7///7+//9+"
"/37+/v//////fn7//37//////////////////v////////5+/////////////////////////v///37//v7/fv/+////////////"
"///+/37////////+/v//////////fv////////////5+/////37//////v/+//////9+fv//fv/+/////////////////v/////+"
"/v//fv///v/////////////////+//9+/////////37/fn7///7//////v/+/v////////////////////////7/////////fv//"
"/37//v7//////37////+//7/fv///v7///7/////////fv///v5+//////7//v9+//7///7/fv9+fv9+///+////////fn7+////"
"/37/fv////7//////////v//fv///////v//fv///v7/fv////7//v////////7//v///37//v///////////v///v///////37/"
"//9+//9+/////n7//////////37/////fv///////////v//fv//////////fv//////fv////7/fv////9+//9+//////7+////"
"fv////9+////fv///35+//9+/////37/////fv///v7///////7+////fv/+/v///37///9+/n5+/v//////////////////fv//"
"/v7//////////////////////n5+//7+///+////fv///////////37///9+/////////////37/fv////7//v///////v///37/"
"//////////////////////7//37///////////9+//9+/v///37/fv//fn7//////v///37//////////37/fv///////v//////"
"fv/+fv//////////fv///////v///////v///////n7//////////37///7//v/////+//5+//9+/////v/+////fv7//v//////"
"//////////7///////7//////////v/+///////+////fv7///9+/////////v/////////////+//////9+/////////////v//"
"//7/fv///v9+/37//37/fv9+//////7//37///////7/fv//////////fv////////////////9+///////////+/////37+////"
"///+/v////5+///////+/v///v/+//////7+////fv7//v////////////7//////////////////////v/////+/37////+////"
"//9+//7//v///n7//////v//fv////////7//37/fv7/fv///////////////n7/////fv///v////////////////////7//37/"
"/////37/fv/+/////////////v9+////fv7////////////+///////+fv7+/////37//v/////+///+/v//fv//fn7/fv///n5+"
"//7/fv///v//////fv////9+//7+/////v/+//////7/////fv///////////////v//////////fv/+////////////////////"
"fv////////9+/////37//v7///////9+/////37///////9+//9+/v////////////9+//////////5+////fn7////////+/v//"
"////////////fv///37//37//v///////////////////37//////////35+///////////////+//9+////////fv/+//////7+"
"////////fv///v//////////////fv/////////////////////+/////v////////7///7/fv////9+////////////////////"
"fv/+//////9+//7+/37//v///37///////7//////37//v////9+//9+//////////////////7//////////////37///5+/37/"
"/v////////////7//v///35+fv////7////////+//7///7+//////9+//7/////////fv7//v///////v///v7//////v//////"
"/37+//9+///+fv////9+fv////7/////////////fv9+/////37+/////////v////////9+//7////+/37//v//fv/+//7/////"
"/v/+//////////7+//////////////7//////////////v///////v///////37+/////37/fv//////////////fv/+/v//////"
"fv9+//7/////fv///v///////n7//37//v///v///////v///v/+////////fv///////////37//37///////9+///+///////+"
"fv9+/////////v///////v////9+//9+/v////7//v////////9+/////////////////v////9+/v/////////////////////+"
"///////+fn7//v/////////////////////////+//7/////////////fn5+/v9+fv7///////7///////////5+/37/fv/+fv//"
"/////////////v/////+/////37////+//////7/fv///37//////////////n7//////v///n7//37/fv/////+//9+/v9+fn7/"
"//7////+//9+/////v///////////////v/////////////+///+//9+fn7///////7//////v7//////37+//9+////////////"
"/v/////////////////+fv/+fn7///////7/fv/////////+/n7///7+/v///v///////////////v///////////37//v/+////"
"/v/+/////37////+/37//////v///v///37/fv////9+////fv/////+/v////7/fv9+/v/////+/v/////+////fv//////////"
"/////////37////////+//9+/v9+///+///+///////+/v////7/////fv7//37/fv//fv////////7///////7////+////////"
"/////////37///7///////7////////+/37+/v//fn7//////v///37+//7///9+//////////////9+//7///////9+/w==";

// Spoken phrase: Learn
#define LEARN_AUDIO_SAMPLES 7466
const char LEARN_AUDIO_BASE64[] PROGMEM =
"e3RxcW9tbG94+fLv6+Pe297i4uji7HpraW/073re38rFxMDCvr29y8t9921KSTw9PkNARURMZGTqdPXn4eX0a2FmX19aWFteZmlr"
"b37z7vDz8/P3fnVycXJub3X37Ojm39vX1dzd4uTkdWRaXV55Xm/r3MPHv8K9vbnAyc9c20tLPTk8PUE/R0Rkb+Xa79nZ1t7xZ2Rj"
"XFtSVFdcYWVpcfvw7O7w9/t8eHFua2529erl4t3W08/Z2+Hk529cT1RQY1ZXen3Exr6/vLq3uMrDWdxWREIzOjlAPURCUuLpzN7S"
"zs7O3/piaVxbUk5SVl5gZmv/7+fq7fb7fXpwa2ZqdfLn49/a0s/N19nk5O5nU0lNSFlMTmNlw8a8vrq3tbLKvVbcXT5CLzg1Pjs/"
"QkzW4cbRzsbKx9jpZ2heWFBLT1JdYGZr+enf4ebv+X55bmVfX2t+6uji3NDOytPX39/naVJESkNQSkRbUsbEvby7srWtxrpqbto6"
"SS0zMzk9O0RD09nDx9PDysPO6GNaXVdVSkxOW2htanTv3tzi8nl++nxlXVtr+uvv7d7Oy8nS2NnX2W5SQUpETko9VEvIwb67va+z"
"qsO68FbKN0ssLjE3PztGPtLQv7/Zw8zCyuhdT1hWWUpJS1hve21v9t3b3/l3fvZ4X1tddfDx/PDWy8jK2dLUzN5oRkRMQ1k6P0xZ"
"vMe6wbOxq6zKtj7GUzo+JjEuPztCPkvFzbbJzMXNv9X4TUxOU1NIS0xdefLz9+rj3uPs9m9pX19nbnNrcu3SysvL2szMyNhlSENU"
"QVU4OUtMu8i8wrSuqqfOsj/Q3TVHJC0tOz5AQj/EzbO90MLbwM3nT0ZHTFlMTUlSbObf6PP25d3f62deXWh6cmRcZ+bOyc7N2sjF"
"w9ZgSURmQFE2M0tGu8jAw7esqqLOsEVqvy9SIikrNUU/SjrHy7Kzz8DxxsjcWkBAQ1lPUElMY+LV2ef45tra32tdXF9saF1WWXjZ"
"y8/N0si+vcLcZkbpTFJALkQ+yr3IxMCsrZ69uM41rzRKLB8uLktASznzvb2t3dblV8xOQjg3PUhNTE9U99PP0tng3t3e6H1jXVte"
"YFdQTFBv18/Mz8S5sbW4013EUMtBLD8yx7jIxMCtp5m6sM4spzE+KhoqL0k/SzT+r72r10viTO9FOC83PUtfT1fw18vL1+Dj6Ofk"
"c/JmamlnX1xNTlRt2tfoysi3srzG7uDuwz04OC7AwM2837GlnLaw/y6pODsqHCc7YzlYPE6qubrGQGtndDs8MjlXU19ibd/MzdTl"
"53nq//V3+25yeGheT0hLZ3ra3d7Kr7iwwWHaydE9PyQ2uuW1vkyonZqyqTQ2oiw2HhohUdstUUlio67K6kpZUNsuMTg8We5cX9jM"
"zM7ccu9xcmt++Oz1+nl9VVNMTm7ecOnPv7qsztrMasf9PiQ76Wmvwjern5urrD4roDovIB0gP7UvN9DdrqjOS0TLQFM+LTxPfFvm"
"5tzLy+LwdGthX2Ft5d7c4ev3eFdHTFZX5ORaxbWtxLduTLPPOCY0Nu2mySunn5ieqEUfnFwpHBofLKk8J+GusKm9TDDJ1TI5MT1H"
"z+BV2cnN1uJvX2NeWVts8d7b3+fj6WldUEpT23dP0cq4uLTZPrfEUjMwLT2uri7aoqOZqa4fzKIsJxklItazLC+4ra+32kA6vEo1"
"NTxMV85qYtPK0t/1bmBpX1xibe3g2t3n3ut3X1BHT+XlTdjLva+zwDm7u1k7LSowsKk6P6Gkl6CpKC+cNicXHiE5rTYozKmstNFa"
"McbfNDA0UEra4l7kycrZ8v9iYF9cX2r86eHf5OLqfnFZTE966Fx6ztS0tbxKfLhiWTYuLdCrzzSuqZ+drN8hqL0sHxwqKbzVLDuv"
"rbPV1z5Kw0M3MUxWW+B5d9jK0u97emRiX2Jlduzg397g5332X0tKY/Ftbc/guq233kW4215GLCg9rLE51aSml6OuJUqhNCYYIyU+"
"tzgqzKmrv/DxN8/2Oi42ZVJu8P7qzcrab3xsYV1eYWn96ePf3+Xw+vJaTVNrdnLm2ei4sr5zXLxg8kYtLFKtvT7BqqibqLgn26k1"
"JxspKkm/Oi7JrK7NcOU+2GU+MTpuW11+7uDTz910/HhmXWBlbHfv6ODd3Oj99XtTTlNhaefZ4t2ysL/4c79S1UEqKmurv0C5qaOY"
"prsjxakyIhgnKUjGNy3DqavXYN1B21U9LjlvX1dl5NvRz9t0/XtnW11hbG/47+jh3eD4ffpuVVRXXnnZ2/nPsrnHet7OU846Ky/I"
"r9dPs66hnKzfJ7C4MCAdLS1k3DU2u6uvW/J+TulLPjFBdWVVat7W19bhe354Z1xgZW5x+O7o4Nzd7/n7cFVUUldv1dn807K0wdX/"
"zlHJPygs3q7HVLWsopimwiW9rTIfGSkrRNo5MMOqqvRW2lDtRz8wOV90U1rl0NLV3Pp+eGtaXGFscf707OTe3ez5fntfWVVSaNzW"
"9+K9tcDHdNxe2uYtLUG2u3TKr62cobM1OKlVKxwjLzV2TzNOsqu7Rtxq71NCOzZLeGFSatrP2tvv+315ZV5fam15/e/q49ve6/9+"
"d2BXUk1n2dHu4b2yvL/m/mHiyy0qN7u1182urZyaq04qrMItGxstLkhXNjq5qq1OX+HsYz48Mz1ceVFa687V2N/1b3RvZWNlbm94"
"fvTz7Ojk6u/z925hXVhQbNzV79zBub+/2V5k9Mk0LDjIt83MtrKhnKx8LLa7Mx8dLjNCUDw7w66taU7b4Pw/Pzs+UmxZWnvQ1N7l"
"6/V0a2hpanNue3b9+fXz7+rm5ufyd2pnXVNNa9zU49bAubu70U9f7MUxKjbLtcbFs6+fmajlKra1MR0aKzA9STs5xK2q/Uvd1uk8"
"Ozk9TWJVV3TOz9vn6+x8bGFjaHRxeG98//P1+PXu5eXp+3lqcGFaT2Le19/dyL29vs5RYvzHOi052LnHybi0pZyr2C2+tzgiHS41"
"P0w+PMuyrehN5djiQD48QE5kWVps09Pd6e3se25lZ2l0cnlvfH7z8vfy7+Xj5vl3Z29fWE1h3NXc2MW7u7zMTV/5xjcrN9a2w8W2"
"saGaqdoru7U1HxosMj1IPDrIr6vnS+fV3z48Oj9MX1hZZ9PR2+rs7HxsZGhob292b31+8/b18u7o5+r6c2pwYVlQcN3Y39fHvr6+"
"2U9n5c01Lz3OusjHubWjna5tL7m9NiEeLzdBSj0/yLOuclHp1OpAPz1EUGBZW2zV1t7u7+7/bmZobHR1e3N+fff1+/Py6OXm9Hlr"
"c2VaT1ve2Njdyb67vMNRWnbJSis0V7m+x7y0qpqiuzFMrUooGiUzOURBOGm4rLtIeN/NSzw7PUdZW1hc58/Y3/Dv+npqZ2Jvcn11"
"dHj79vT8+fLp5uvxe3dva11XV+vd2d/Rxb6/wXRRb9nhMDNGxb3Iw7mzoKC0SDWyzzIfITQ6REc8ScOys1dd7c9tPz4/R1ReWV51"
"1tjd7u/9fm1saWpwdv56fHj2/ft+7+3o6evy/XZuaFpWWt/Y1NnNw72+wldLYNhyLjVLvrm/vLavnqC2Pziv5C8dITM3PDw2R8Wz"
"tVj40MLhRkJESk9PTU9a6+jp8+vv8nd0bGloaW5wdHr39fHw6+zs9Pd+c3Z9+vz96tPPztTW19PR3EhBTfzwPERrvbW4uLi1pqa4"
"Vjq8zzonJjU4NzY0OmzLv2xyzL/EfGFsd2RaUFJSXF9fYWlxeHZ2eXh1cXN2enr+/Pr48vL3+/r08fHs5N3b2trT1dTdfV5ecW1E"
"PUh00E5a17qvr7G1t6qks946zcBCLCUwNzMvLzBCW9dpTtnFvtHq2tDX6mZjYl9hW1xeZGhpam90fHp5eXv8/P99e/37+vv89/Ds"
"6ujn4+Hf5e/yfX5tW1BPXGtSSVbpyNfXyL2zsrO4vLastcxJXMNcOy0yPTk0MjM6RExSRFPs0db35NLP1t/m4env/nJta2ppampp"
"amlpamlqbG95/fHn493Z2NjW19vk7fhtXVhaWFRSV1xkXFxx/Ghj7+zc2NXEu7q5r62trK6vs7vg+VU4Mi4uLC4sMDo5PEdYV19o"
"eOLvd+va3uDZ1NDT1tXV3u96bF9XUlJRUFBUWV5mde3i3NjT0NDR0tTa4/B+altVVlZSUFZbY11cc/hrZO7p3djWxLy6ua+tra2u"
"r7O75vZSODIvLiwuLTE6Oj1IWVdgaHnj83br2t/g2dPP09bV1d7veWxeVlFQUE9PUlhcY3Dv4tzX0s/P0NDS19/t+m5dVVZWU1BU"
"WmFeWmz4b2L36N/X2Me8urqwra6srq+yudd7XzozLy4sLi0vOTo8RVZXXWhv5O128dvd4drUz9LV1dTc7HxuYFhSUE9PTlFWW19s"
"9+Xd2dTPz8/Q0dTb6Pd1Y1dTVVNPT1VbYVpee/pnaurn2tnRwbu6uK6tra2vr7S+9fBMNzEvLi0uLTM6Oj1KWVdhZ/7j+3bn2t/f"
"2NHP0dPR0tvp+XNgVlFPTk1NTlJXXGV66d3Z1M/Nzc7P0NTc7HpsXlRPUVFPTlRbZF5efe12aurl29bWxby6uq+trqyur7K52n5d"
"OjMvLi0uLS86OjxFVlZcY2rr+2r+3uHn29TPz9DPztPc5e9uXVVRT01LTE5PU1pjcu7g2NPPzszMzc/T1+D3a2JaUU1PUE9PVV1q"
"ZWH36Ptt6OLc19jGvbq7sK2ura+vs7rc/l06My8vLS4tMDs6PEZYV1xjae18Z3bi5uzd1s/Q0M/N0drf6HtgWFNPTUtLTU5QVV1q"
"/ejc1dDOzczMztDU2eR9Z19ZUE1PUVBQWF9tZmTy535u5+Tc2NjGvby7sa6vrrCxtbzl+Fk7NTEwLjAuMzw8PkpaWF9ja+13Z3nj"
"6uze19HS09DO09vh6X1jWlZST01NTk9SV15qfuve2NPQz83Nz9LW2uP7amJcVU9QVFRSV19rbmV95u1w8eLi2dzPwr6+urGysbK0"
"trrLcPNHOjU0MjIzMjw/P0VUXVtkYnv1amf36PPr39jV1tXR0dfd4Ol+Z11aVlJPT1BSU1hdaHjw49vW09LRz9DU2Nzf63hmX11X"
"UlJXV1dZYm15bXjn5n3+4+be39vKwsK/trW2tbe4u8X29Fc+Ojc3NTc1O0NCRVBgXWNka/dtY2/v+vrr39ra2tbT1tve4Op+amNe"
"WVVUU1NUVVldZG766uHc2NXU1NTV2Nzg5+54aGBeW1dVWVxbXGRvfnhy7OXyduvm5uDj0sjFxr24uri6urzB23d4SD47Ojg6OTpF"
"RkdOX2NmaGf6fGdp+vX/8+ne3N3c19fb3uHl73ZrZV9bWFdXV1dZXGFodP7/////fv////9+//9+/37//37///////7//v//////"
"///+fv9+///+/v///////v////////7//v///v7/////fv///v////5+/v////////7///7///9+//7/////////////////////"
"/35+fv/////+//9+//5+/37////+//7/////////////fv///37//37////+/37//////37////////+/v////7/fv///37///5+"
"//9+fv///v///////////v//////fv///////35+//////////9+fn7+/////////v//fv7/fv///////////n7+////fv5+///+"
"/////37+/37////////+fv/////////+fn7////+/////////37////+/////37+//9+/n7//35+/37/fv///v//fv////7/fv/+"
"//9+/v///v////9+///+////fn5+////////fv////////9+///////+///+//////9+/37///////9+fv//////////////////"
"//////7+//////7+///////+/37+/////37///////////////////////7//////37+/////37/fn7/////fn7/fv7//n7//n7/"
"//9+/37/fv////7////+////////////fv///////37////+////////////////fv////////7//////37//////////37//v//"
"//////7///9+////fn7//v9+fv//////fn5+/////37/////////////fv/+//////7//////////37/////////fv////9+//9+"
"///////+///+fv/////+fv7/fv/+/////v////9+fv//fn7/fv7+/37//37/////fv////9+/v///35+///////+//9+/////37/"
"///+//////9+//////7///7//////////v///////////////////////35+/35+/v/+/v///////v////////////7//v9+/v//"
"/37+/////////37+fv///n7/fv///37//v9+/v9+//5+////////fv/+/////35+////fv///37/////fn7//////37/fv7/////"
"/////////////v/+//7+///////+/37//////////v9+///+/////////////v///v/////+///+/37///////////7+////////"
"/////v//fv///////v////////9+//9+//7///9+fv////////////9+fv///////37/////////fv////9+//9+fv///////37+"
"//7//v/////////////+/////37/////fv//////////fv7/////fv//fv/////////////////+/v9+///+///+//9+////////"
"//7///7///9+///+/////v///////35+fv7////+//////7///7//37/////fv//////fv9+/v/////+//////////7/////////"
"/v7/fv////7///7+///+/v////////7//v////////////7//////37//37//////////////v9+/37/fv/+/v/+//////9+/v//"
"//5+/////////////37//////37/fv//fn7///////9+//////////////5+////////fv5+//7/////fv/+//////////9+////"
"fv///n7+fv//////fv//////fv/////+////fv////////////7//v///v///v////7//////37+/v///////v/+fv/+//9+fv//"
"/v/+fn7///7/fn7///////////////////7//////37//v/////+/v9+//////7//n7//v///37////////////+fv7//35+////"
"///////////+////////fv7/fn7///7//37//////37+/v////////7//////v///v/+/37////+//9+//7+///////////+/37/"
"/37//v///////37////+/v///////37///7///////9+/////v/////////+//9+/37+fv///v////////////////9+//7///7/"
"fv//fv///////v///////v//////////////////////////////////fv7/////////////////////fv////9+///+//////7/"
"/////v/////+/37////////+/////////35+fv9+///+/37//////////////v////9+////////fv///////v7/fv////9+//7/"
"//9+//////7//v/////////////+////////fv//fn7//37//n7/fv/+/37/////fn7/fv//fv///////v//fv///////v////9+"
"/37//////////v//////////fv////////9+////////fv////7//////v9+fv///////////////////n7////////+////fv//"
"/v///35+/v/+/v///////37////////////////+/v//////////fv////////////////////9+//7//////////////v///37/"
"fv7//////////v///v//fv/+///+//////9+//////7///7//v///v/////////////////////+fv///////////37//////v//"
"/////////37///////////////7///7//////////v/////+//5+/v///////v////7//n7////+//9+/37/fv///v//////fv7/"
"//7///////////////////7+/n7//////////////////v///35+/////37////////////////+////fv////////////////7/"
"////fv9+/////////37////+//////////5+fn7//////////v///////////////v///////37/////////////fv//fv//////"
"//9+/37/fv///////////37///////////7/////////////////fv5+//////9+/////v/////+//////7/fv/////////+////"
"//7///////7////+fv/+/////////////////////////////v////////7//v//////fv//fv//////////////fv//////////"
"/37////////+/////////37//v7///////////9+//9+////fv///////////v///////////////////37/fv//fv7/////fv//"
"//////////5+//////////////////////9+//9+/v9+fv//fv///v//////////////////fv////////////9+///+/////v/+"
"/////////////////////////37//v///////v///v//////////fv7+//////////////7+/v/+///////////////+///+////"
"/v///n7//////////v7/fv/////+//////9+//7//v/+//////7+fv///////////////v///////37/////////fv////////7/"
"///////////+////fv9+/37/////////////////////////fv///////37///7///9+////fv////////////9+//////////9+"
"/////v//fv//fv///37//////v7///////9+///+///////+//////7////+/n7////////+/////////////v////9+//////7/"
"fv9+fv//fv////7/////////fv9+////////////////fv///v////////9+/n5+//9+////////////fv9+/v7///////////7/"
"//////7///9+/37+//////7///9+////fv///37/////fv///////////37///9+/v//fv9+//7//////////v7//v/////////+"
"fn7//////37//////n5+/////////37///7//v/+/////37//v/+///////+/35+/v9+//////9+/v7///////7/////fv///v9+"
"fv///37//////37////+///////////////+fv////9+////fv///37///////////5+//////////////7///7//v7//v7/////"
"////fv/+/////////////////v7///7//v///////37///9+fn7//37///9+/37///////////////////7//37//v////9+////"
"/37///////////////5+fv/////+//7//37//v////9+///////////////////+fn7/////////fv///n5+/////v/////////+"
"fv///v/+////fn5+fv9+fv9+//9+///+/37////+/37+//////7//////////////////v////////////7//v/////////+////"
"////////////////fv/////+//////7/fv///v////7///////////8=";

// Spoken phrase: Sign
#define SIGN_AUDIO_SAMPLES 7856
const char SIGN_AUDIO_BASE64[] PROGMEM =
"fv//fn7/fv/+////////fv///////n7//n7///7//v////////9+//7+/v7///////7///9+///+//5+/v////7///9+/n7+////"
"/37+fv5+/n7+fv5+/n7+fv9+//5+/n79ff18/Hz7e/t7+3z++dzj29z32Xzd2+be3/bp7PPc5tzf4+zf5OfY3dbs69/g19Te49dx"
"1t/k1e7f3PR31m7d3WzacuLgc+He49ze5dlz4+Jo3Ozd1/jg3Obk2NrY6PHh6u/m4dvn/dZ32uf94dzi7O343OvWfuD1ffbz4N7v"
"9OvX2/vS493Y+dza6djz0+3f1N/e2Nnl6ujf2tbr4enf/9550tzn7lnaa+rVa9t1ZdTr1N/1Y9fTes/f4eX82uvY2/Xb5N3r5t3o"
"9ubv6tfd5eX/5O3l1O3n6u718fLW09raefHs+Nnl5uTf9vT65tv03PZv4N3g3OLf5t3f3u3UbO3addbz49Tc49j67O3o3XPs5+Lw"
"8vXv13ze2vfp7H7c1Op6efHw+tvf79rb3ezd3tLg3W1s227Xcu/22dr14t/Q7vFc5G753uDdbOl80d5w2vrp0l7b3fPN7tXW39rv"
"3u/Z7NZ56tLryn3m/N7Y7e3j3/57+N7Yc8/h7O9k6+Xy3eL6z2vU7tlo1d7b5GXW9c3a2uXU1uXb627S7O9w3u/u5n7O+P7b7OXX"
"5ubz//h93Nvq3vT119fm1e/i4dfYctHr49rZ3eb939x00O/W5ezT/OHs5H7X7eTa8Ob8293l19re1erg6m7X99L1fOr22W7O3dzl"
"6d3e39ff/eXe6Njd3tXu2vHe4N/g4+nq5OTu6GDt3mrdb3Tafup36eXq49nl6Nx92/Dd29ro1HLS69nP3OBt6XPYauT45Nb79eVr"
"2OfY7fVj3OLy3HnYYtNz1vp+1Ozf0PHg2e3a5Onk6OTY/M51zdt80Ov30Pvo3W/YfvLf127d6vPd7/zVdd7oftfv6+Df6uP44d/U"
"2+fa3dzf5+jl6d/X6vLfz23PbOrbbNlq6+Ts+d3v1+Df6tJm2Oze13LUZ9be62vc3tTtX1FOXezOzM77UkdLXNLJxczTWkU9PFvE"
"ur7J311fRzpdv66vt99d1j4yLTVUvrvY23xMQjk6VsG4vchdSUVES23OxsfcV0hJV+fPzNX8WE5RZtrMytdfSUhP883GxcjYRzw3"
"QdG5usPJ315TNTrMr6qvvE3UZDMsKjnZsL7c7kxEPjY837e1utpGQT9HVdnJw8twS0VLZ9TKzN5aSEdS3sbAyP9GP0ZW0MG6u79M"
"MS4z+bWwu73Ad1AyKW2tn6WsX1C/MygfJkCtqsfZRDo9My9EvK2st0o5NTpI3cW9vtdOPj1J4cK+xvlJQEVgzL6/zVw/PkVUzLuz"
"uMY6LC46y7Gyvrm7Y0QtJ82mnaavRlS7NCgdITmrqL7OPTY+NC4+w66orlg4LjJA3MG5uctVPDg9Z8a6vc5LOjdE3bq0utFHNTQ9"
"U7utqbPaKiErP7yrrrqvsEg0IyKym5airTJbri8jFhkrpaCvvTEtOC4qN8utn6TWNSQmM9y5ra+/WzcuMUfGsq+7aDoyOFTCsrC8"
"/D0vLz35sKamvkUjIDJguKyvt6uxNisgKKWXlqe3KtStMyQVFyijoKm5LCszLS072q+eo786ISEt77iprLpgNSsuP8qwrbTiNyos"
"O8etp63GOyojLU6tnZysQCMZJ0rDraeqpaBLHR0Zr4+Nmq0yKJ5qKhMMEsOXnp46HycoKDFTv56ZqNEjGh41wKehq8I6KSUuU7Sp"
"qblOLysvT7qrqbPtNCkmNd+pnp+6NiIcLnG8rauuqqg7ICAeqZKQn7MuL6FRLRYQGLudpKU6Jy8tKjBIx6Ccq9AnHiE4zKynrb9E"
"LSkuSb2tq7VuMSgrPMOrpqvDPCokKUy2n5yjVykaHD3CrKaqsamvKCIdKJqPkKi+H9CiOyYPDxuln6KrLiw2KiUrP7qbm6rqIx0g"
"Ndaqpam4SywmKjvEraetyTstKzZ5tKurt18yKik00bCioaxDKh0gT7iqqLHDtbotKiUvnJOVr88i4aNNLhUTHq2nq685OkgwJyk4"
"yp+eqsssIiQxTLWrqrHYNysrNGS3rK26WjApLDzHraeqvUArJyo9vKufoq84JRofeq+kpLTSusIqLCQ4mJCVsu8eu6RHKBISH6iq"
"qrM4Q04tIiY2vpydqdIqISMvRraqp67ONikoMFm3q6u3bTctLj3PsqutvUkvKy0+wrGmp7BGKx8hT7aop7XWwcAwMSkznpaYsc8l"
"yalELBYXJKusr7g8RU8wJio8wJ+hrtwuJyk2SruurLTbOS0tOHm5r6++VDIsLkLEramtxz0rKi5Tuqyjp7k3Jhsm366iprvRuOQs"
"MSHvmZKas1Mjqbg3HhIXNaGvrMw3TkIpIitLrZujs0cqJSo3XrKrq7deMCosPc+yrbDHSTQvNlW/r6216jktLjjXuq6prss4KB4v"
"166kqsbfv1U0OibEnZeftUorqdw0HhgdVqe+tt5G/EMoIy90rZ+svE4zLC42TLuvrrryOS8vPfS6s7TEVzYvM0TJs62wwkQwKzBK"
"t66qrrxTNScfNdOnnqfUU9pFPkYmuJyWnq9DK6VQLhoXHuCp5LvP8ss9IR4u6q2grrbZQy0rLDvCr6yzy0IzLjRIybWvt85HODQ8"
"WMO2srrPRDUvN1G5rq2z2Uo3NCc3ZbOipL9JT0dI0C9opJ2dr8oprs0xIhsjPq1d28HMxk4nIC5twquytrziOi0uNPO+tLa98EM1"
"NT1qxLi4wOlANjQ+ZL20sb3lPTUyOlW5rauvfDwvNSs9XrqgoLRPQT1EvjJVpZ2bq7kor9IrHxojOa1K7rq/yj8kHy7q0q6zsrbR"
"OisvNWzEt7e73kY2Nj1mxrm4v+BIPTpEYMa8uMLkQzw6Q2C/trC0dz8zOjNVYMOnpbP7QzpHvjhhq6KdrLYssmMrIR4rPrJG9bi+"
"3jsoJzfjY7q4tLrdPTA6PF/Rvb2/6Uo8PkVnzb+9xeNGPDpGY8e+usHeRDw7SOq/urWz60QvNjPj2cqqpazJRTA4vT5JrKOZpKgt"
"uHcjHhsrN7FI7a21eDYoKTRWPMm0srbXUTpGO0ZizMjI3G5PSkpZ5dDM1eFkWVFXbN7P0ttxYlFVV9/Ivc/O7FJBODtBt767tK+5"
"zj0qOrtByq+jnaOvLq81Ix0hMEDDOrupvU89Njc4OTTEv8DFx85xWj9OWGVi3+ffc29lfWh4euz7/XFx+O7r/Or9e2Fqb9HW1tPF"
"eGI+P0dgTFWus7G9zO/gOiQ4s3Ourp6cnrouqykfGyQsNU81rKfJ7c5kPTIyNNRIXcm8ydzQcudWU1VyW2Fn9276d+/68vx6+W7/"
"bfp36/3m4dfo4N7dZV1NT2dHTuq52L+7vr5sOj1qOSvHuLipsJ+lpDLJxSIkIjInPTLmrMnctbpnQU4+REA47/T6aMXN2dzd5nVd"
"XGNcXWP5fOz55nv4aG9vfPvp2tve6OF0X09QVmZF+8zG3r63xb5TYPQ+KjraOz6ut6KpqaGftCqpMCAhKighNi6/w1S2qsPdwsQ/"
"TT5BSTpHXe9Q2M3T2tLX3v12cGlfZW1ld3T2+efs6eHs9HZ3X1lRWF5WVdTW282+wcvE+8VXOUZXMy9t2EK5tqujtaeopi7GwCMp"
"KTQhMTFIxU7MrbrZwLpiSmtBTT1CT11PZ8/c39PS3uT2/GtqYGtra2/8fPjq6ujj4OfucW9gVk1VXlJR1dDZy769ycH4wm43P1c1"
"K0/NQcGvrp6xpqefPEetIyYlMiAqMjfAVvCtsNXDt9RCdEJKPjxMWVBW0Nfc1M7X3Pn3cGVdZ2hnb3h3evT99O3n6+Dj7ft4Z1xW"
"UF1eU2jO3dXJvcrHzeXIRDxNTS831mxNs7qnqrOpqLAssEglKi8vJDox7dFNva7G1L2+TFlYRU09SVNeTu/U397S1d3p7nprZWFp"
"Z3Bu/Xl+/vv+7e7k6N7l6X5taFpSUF1cUXzM3s/Hu8zD19XNPDtUQCw9y0zwr7egr6ypot8yqismJzMmKDczxWNgsK/SyLjNRnFG"
"TEI+TFdXTtzY39zQ1tzp6nh0Y2ZoamxufnV+9n7u7+vt6Ot8d2lmWVlZZmhk6crY0ce/1dXVauc+Pk9INELI8WevuKqttKysuDG2"
"SSstMTIoOC9T6UTdtsLSw7vX2/RZ+0tGSlpLTF1qbP7w4d/o6Ojr+fZ7fHx7ev/7+/5+/nVxaWhoamRs6uTf2M/Iy9rKy9xU/VlG"
"SDpFUUU93LvWyKuyqq6yra66NbpQLS4vMikyKzpbPETIxdPNvcLFyfDO3F9PXVpMTU9UWlhcbHJ2evT07/T19Pb7/n16dXZ3c3V4"
"fvLv6+LZ2trV09Ta7+PlX05TX0VKSUxhX1fnvcDPtq+3r7u1uLd9Sb8/OTU6NjI3Lj9KPEF16vLnz9HHy97Ozdfz7O50aF5dYV5c"
"XmFmZ2lub29wb3V3ePbw7OLd3NjW19jY5fTzaFpVV1ZRUFVaYFpbdvloZ+rn2tfSwLq4t62srKutrrK87fFNNi8tLSssKzA5ODxI"
"WFVgZHzj+Xbm2N7e19HP09XU1d7ve2xdVVBPT05PVFleZ3js4NvW0M7Oz8/S2ub0d2RYU1VUUE9VW2BZXX35Z2rn5tnYz765ubWs"
"rKysra6zv3vvRjQvLSwrLCszOTk9S1hWY2X15P564djf3dbRz9PW1Nbg8nhpXFRPT09OT1RZXmh86t/a1c/Ozs/P0dnk8npmWFNV"
"VFBPVFpgWVt3+Whm6uba19PBuri3rausq62usbvo9E82Ly0tKywrLzk4O0dYVl9ld+L2dujX3N3W0M7Q09LR2un5cl9VT05NTExO"
"U1hdaP3n3djSzszMzc7P1N7ydmlaUU9RUE5OVFthWmD272xx4+TY2NG/urm2raysrK6usrzz7002MC4tKy0rMDk5PElZVV1kbexz"
"aPDc4+HX0M3Pz87N1Nzj72xbU09MSklKTE5SWWN07uDY0c7My8rLzdDV3e9qX1tTTU1PT05QWGFsYG/l5Xf43+DX2s+/urq2ra2t"
"ra6vtL947Ug2MC4tLC0sNDo6Pk1bV2BebvVmX37k7+nb08/Q0M3M0djb4flkWVRPTElKS0xOUlljde/f2NHOzMvKys3R19zsbV5a"
"Vk9MTVBQT1Rcam9l997ld+3f4NjczL66u7OsrqytrrC2y27+PjQvLi0tLS44OjtCVVlaYV/7emBl7efx49nRz9HPzM3T2dvjfmFZ"
"VE9MSktMTU9VXGh87N7W0M7My8rLztPY3vNoXVlVTkxPUU9QWGBvaWro3/N44eHc2djFvLu6r62ura+wtLzp9VY6MzAvLS8tMzw7"
"PkpcWV9gaPRsYG/n7O3f19HS08/N0Njc3+1uXlhUT0xMTU5PVFpicPjn3NbRz83MzM7S19zod2NdWlROT1NTUlVdZ3BmeePlffff"
"49vd1cS+vruwsLCws7S4xHjvTTo1MzEwMS84Pj5CT15bYmBt+WdgdOrz7eDZ1NXV0c/U2dzf7HJiXFdSTk5OT1BUWV9qfO3h29bR"
"0M/Oz9HX3OLtdGNdXFhTUVZYV1heaXl1b+nf7Xrn5ePe4c7DwMG4s7Wztba5vtt3b0I6NzYzNTQ2P0FCS15gY2dl/XVhZfnzfu7i"
"2tna2NTU2dzd4e14amJcVlNSUlJTVlpfZnH16ODb19TT09PU19zi6vNyZF5eXFdVWFtaWl9reXty7uHqeO7l6uPm1cjExb22uLe4"
"ubu/1nP9ST06OTc4NzhCRUVMXmRmaWb/fGVlfvL88ufd29zb19fa3t/i7H5tZ19bV1ZWVlZZXGBncH5+//7//35+/v9+/////v//"
"fv///v///v9+//5+////fv7/fv7+/////////37//35+////////fv/////////+///+///+/37/fv///35+////fv7///9+////"
"fv///37///////7//////37////////+//////////7/////fv///////v////7+//9+/////37///////7//37///7+/v/+///+"
"//////9+//////9+//9+////fv9+/////35+//9+////fv////////////////9+//7+///+////////////fv/+//9+/37///7+"
"/////n7////////////+/////37//35+/////////v////7//37+/////////v////////7//////35+/37+////fv7//////37/"
"/v///v////7////+///////+/37///9+//7//37///7///9+fv////////////7/fv////7///////////9+fv//////fv//////"
"//////////////7//////n7//n7//////////v/+//7//37////////////+/v////////7////////+/37///////9+/////37+"
"////fv/+/////////37//37//////v///n7+/////////////37//37///9+/////////////////////////37+////////////"
"/37+/v/////////////////+////fv////7/fv///v/+////////fv///37+////////fv//fn7///////////9+fv///35+fv//"
"/v////////9+/////v7+/v////9+/37///9+//////7///////7/fv////7///////7+/37//n7+////fn7////+//7+/v///v//"
"////fv///////v//fv////9+//////9+//9+//7///////////7//////v////////////7///////////9+/////////v9+///+"
"///+/v/+fv//fv/+/v/////////////+fv//////fv//////fv//fn7+////////////////fv////5+//7/////////////////"
"//////7+/////37/fv///////////v////////9+/37/fv//////fv/+/v9+///////+/37///////////////9+fv///////37+"
"///+/v7//////v///////37/////fv9+////fv////9+//////////7//////v//////////////////fv////////////9+/37/"
"/////v9+///+///////+///////+//9+//9+//////////////////7//v///////////////n7//////37/fv/////////////+"
"//////7+/37//v7////+fv/+fv/////////////+fv/////////+/////////////v7//n5+/37///////5+/////////37//v9+"
"///+/35+fv//////fv/+//////////////7//37/////fv///v///v////9+///+///+///////////////+/v/+///////+/37/"
"////fv/+//9+/v//////fn7///9+//7///5+/////37////////+////////fv////9+fv/////+//9+///+///////////+////"
"/v////7////+/v////9+fv/+////fv///////////////37/////fv7+/////n7+/37/////fv///////v9+/37////////+////"
"/37///////7+///////+/v/+///+//////////////////9+//7+//////////////7////+/37+fn7//////////////////37+"
"//////////9+/////v7////+/v////9+///+//////9+fv///37//v///n7//////37///9+//////9+/37+/v////////9+//9+"
"////////////fv////////9+/37///9+//////////9+///////+///+/////35+//9+//9+/////35+fv7//n7+//7/fv///37/"
"//7///////////7//////////////37//////v7//37///////9+////////////fv///n7//v///n7//v7/fv/////+////////"
"///////+/v//////fv////////////7///7/fv7+/////v9+/37//////v////9+/35+///////////////////+/////37/fv9+"
"fn7//n7///////5+fv///37//v5+/////37//////v////////////9+/37////+/37////+/v///37/////fv//fv7+//9+///+"
"////fv////9+///+//7+/////v//////fv///////////v9+/v///////37+/37////////////+/35+/37///9+////////////"
"////////////fn7///9+fv/+//9+//////9+/////////////////37/fv////////////7///9+fv///37///7//////////35+"
"///+fv///////v9+fv//fv/////+fv9+//////////9+//9+fv///37+//7+/v7////+/////////////37//35+///+fv9+////"
"fv//////////////////////fv9+//7////+/////////////v9+fv////9+/v/+/37+/////v///////37/fn7//37+fv//////"
"/////////v//fv///////////////////////37///7//37//////////v////////////9+/v///37//37////+/35+//////7/"
"/37//////n5+//9+////fv7////+///+/////n7///9+/37//////////v//////////fv//////fv///35+///////////+//9+"
"fv/+///+/v/////////+/v///v//////fv/+///+/n7+//9+//7//v7///9+fv7+///+fv/////////////////////////+fv9+"
"/37///////9+//////7////////////+//9+////////fv////7+//////7//////////v////////9+/////37/////fn7+////"
"//////////7///9+///////+/37+/////v////////7////////////////+///////+//9+/////v/+//7/////////fv////5+"
"/////////////37///9+/////v9+fv////////////9+fv////7////+//////7///9+fv//////////////////fv9+/v////9+"
"/v///////v7//////v9+fv9+//7////////+/v///////v//fv/////+/v///v/////+//////////////7//////v////7/////"
"//////9+fv///v///v/+/37///9+/v9+///+/////37+/37///7/fn7//37////////+///+//7+/v//////fv//////fv7/////"
"///+/37////+//9+//////9+/v//fv///////////37+//////7//////37//v5+/v////////7//v9+//7///////////////7/"
"//7//37///7+//5+/37//n7//n7//////////37///////7//////v7////+///+//7+/v////7//v///////37////+/v////7/"
"/37///7///9+/37+/37///5+/////////v////9+///////+///+/37//////////v////7////////////////////////+///+"
"fv///v9+///+/35+///+fv///v///37////+/v9+fv/+/v7+fv////9+/////35+/////////////////37////////////+/37/"
"//9+////////fv7////////+//////9+////fv//////////fv/+/v7///7///////////7//////////////37///////9+////"
"/////n7//v///v/+///////////+////fv/+/37////+/////////////////37///5+/v/+///+fv/////+//7+//5+//7/////"
"fv//fv/////+////fv///////v7//n7///9+/37+///////+fv//fv//////fv/////+fv9+fn7//35+///////+/////v//////"
"//7+////fv///v///35+///////////+/37/fv////7///////9+//////////9+/v7///////////7///////9+fn5+fv7//v//"
"/////v////9+//5+/n5+/v//////////fn7+fv////////9+/35+/////////////37//v///////////v//////fv////5+fv//"
"///////////////+///////+/////v/////////////////////+////fv/+//////////////8=";

// Spoken phrase: Finish
#define FINISH_AUDIO_SAMPLES 7946
const char FINISH_AUDIO_BASE64[] PROGMEM =
"fv7////+/////v9+////////////fv//fn7+/v9+///+fv///37/fv/+/////v////9+/v/////////////////+////////"
"/v7///5+///////+/v7+fn7////+/////37///9+//////7//37////+/v/+fv5879/k3t7d3tzc493d3uLk4+Dm6Ofh6Org"
"397d2N/e4ufa4O7n89zofuLx4OPr3+He3tvd59nf4ODm29/v4enn4/Lf7+/k5+Lg693d5ODj3+vg5ujr8efy6evy6N3j39zx"
"5Nzk4uDl4erx6eLt5PDs4OTm7N3m4+De4N7i7+Di4+7r6vXt6vXi5ujk497j3d/f3+Tt5eft7fj16Ozt5d/b4+XZ3ODe4uzn"
"7Ovv9PH44Hvl3+fg3dvi397j4/h97PPs9uhw5+7d4/Df59zn3uDj6OTu4erv6fbe6Ozn7+Xl7PDq7OHp7dvd2+bc2+Xg5N/f"
"9Od+++T45XJv3+zf3+Te3tbl1+Da3+Dc6+bt3ezf7O7f7e3qd+Xz9Ov17ubg39vr1tnT2N3R3t3a9O7q/upuafP67+Pj3Ovk"
"3+jhdm/h7+zZ5tra7NnS3djf3trt6v7s73De+mri7e3a6OrZ3d/X3ezb5eNs7l/uzn3g59vU1tvV093t6enqd/N07u926P7b"
"3dbr3ff12tbc6t7v3ebtXt3xYen49dzU+NnZ4tjb39vtfeLv6nvy2ejf4+fc8frf6/zjdnzy/Pzh/frS1ePh2t7P393b6d/o"
"fHTeeejr++Tdeebb5+bm1/fk2Nnfzdnv3Nzm4vNaakZQfObX2dzNy9DTzsbX1uju+N38+N594+X6fd3o4ero7O3j4N/05t7Z"
"5+b19t9+5XX85fZ75fxm1+/b2u/e19nr2+Tk6ezf7N356uT27/ntfdrm9evg4+fZ597c3e3h3+vV5uTh397a0uDi093z3fbo"
"29z33eXd2dvf6t7q4Hzq+3rp7Px5fd3Z4dnX49jW393q4fDY6Xra3uXb1erh5/js5+t04N145d7n1tlr4NHd6O3l39bi8OTm"
"/+zrcufs7u3n5OPe4ejl59/Y3NfZ2NfZ3ebs5eP57Ozn6eDc3tnc4N3d6Ofr/271/Pjp8e/h2+ze5fHg4H19ZFJLSk1QWW/a"
"yMTGvL3F2NReST83PTw4PezO4rOurKmtq62vT959LS0rLCctLTlrSdi4tru4tcLHc1JPPjk7Qz9IXOLSy8jEx8/ieVxOR0dL"
"Tldz1sbCwrq7wtXPWkc9Mzs5NDvfzd+urKmmqqiqrkXMUikqKCkjKyk6bkbLs7O5tLPBxl5OSzs2OUA9SV/bzsbEv8PM3nRU"
"Rj49QENNX9K/ubyysbjKyPw9OiwvMCwtUL5+rKGim6CeoZ9rVMEhIR4fGx8hJm8//62rrq6ptrvhREo1Li43NTtN68m/u7e4"
"vcbWYUxBPDk8QEha28O3ubSusr/Myj89LiswLCo2v9u/n6ObnJ2enqs0ti8dHh0cGiIeO18+tKmqr6isu75ISz0vLDI1Mz9b"
"18a9t7O2u8DMb0Y9ODIwNDtCVNm9sLCvqKu2yLxAOCwiKyYiKMbI2ZqclZOWlpecLbM0FRcUFRAbFy3UNq+foqijpbi4Pzw6"
"KSYrMC06T9jAubOur7K3vMtzSz06NTIwNzxDVta+r7Kup6q2vrg/PCoiKSMfKL9nw5ickpOUlZSgK6wlExUTEhAbFz/ZPqae"
"oKifqbvBNDsxJyUtMS9AYMy/urSws7a6vc3dWUk7NzQwLjE5PUzkvq6urKOkrruvRjkqHiYfHB/L13CWmI+OkJCSmCitLQ8T"
"DxENGRQvxjapnZ6moKS6uzY3NCYkKzAuPVjOv7y3sbS4ur3H1fFjT0U+PTo3Nz0/SGLQu7K2q6itvbXHOzslKCgiIDm7QKaZ"
"m5GWlJiV2UK0GBgUFhIXGx7HQ9ejoaeqpLe7ZjM/LyorNjU6UOjIwr+7u7/FyczY5fp5ZldMR0Q+Ojg9P0NU2b6zta2nqre5"
"tz09KSQpIR8puky5l5uQk5KVk6Usqx8UFRMSERsXRuA+p5+jqqOsvckyPDMpKDE2Nkp2yb+/vLq/yc/Y+HVndn3x7Or/X1VP"
"RT49Q0BIXdi/ubquq7C/t8s9PigsKyUlP75Cp5yclJmWm5htTrsaGhcZFRsdIMhF16alqa2ou75aND8xLC05OUBb2sO/wL2+"
"yNbiallTT1xp8d7RzdHZeF1NQDo8PjxHaM+7uLOpqrS8slQ+MyYsJSIn1tpjm52WlJaVmJwvtjUWGBQXEh0ZMMk8sKOkq6ir"
"v786OTotKi85OEhpzMC9vbq9yNfpX09MTlFe/uHU0c7U42taS0E9QEFFT+jKu7y0rbC+vr5DQi8sLyopNr5cuJ2hmZybnZy2"
"ObQkHRwcGhshH15PTK6qrLGrt7/TPEY6Ly85PD5R88zFwr++xc/d+ltRTVFUYPvc0M7L0NxuWUtAPD1BQkz8zLu5t62uuMe8"
"UTw1KS8sKS7Lw9+en5uZnJudoDa9QhweGhwYHx0u8Te/q6uwrK29vElCRDItMTo4Qljcx8O/u77H0N9mUktJTE9ae9vUzszN"
"1eJlVUtCPkFGSVbky727urG0vs/ETj05LTYxLzXPv9+mo6Gdop+kpELT7yIlHyIdJCEsbDnntLG1s6+9uutNWj42Nj06Pklb"
"5dPOx8THzNHb72ZcWVhcYG7t39zb3OL2Y1dQTUpKT15+58vBvbu9vbvJeltoODU3MTw6PlO6t7+ko6SgpqSqqUvo1ikqJCYg"
"JSMlPTM58si/v7W7tLjNxs3uTk5IREA+QEdIS1JfcfPi29XT09XV2d/q+W1kXFlWV1ZZXmv55tvQy8jIxsTJztXmfVpIRERD"
"OUBGSF1m18S1ubeqra2sr7C3uVfk/jc3MDEtLywsOjc3RVJ4683PxL7HxcXI0drscWJYTk5OTk1PVFleZnL+7uvq6O32+H5+"
"e/jv7eTd3NnZ2tvg6nJkXlROTE1PT05QWWBoZm7r2t3d0MrDwL+8tbS3tre4vcfq+1xDOzc3NjczNz0/QURMVF5bW3Xr6evf"
"18/P0c/MzdDW2t3pdGFcV1NPT09SVFheaHjx5d3Y1dPR0NLW297ofGVdWVZQTU5TU1FVXWt0bnjh193a0crDwsG8tri5ubq7"
"wdBz8k9BOzo5OTg2PUFCQ0pRWFxWYPn0/e3c1tLU0s7N0dbY2+T9aWFcVlFRUlRVWF1ndvjo3dfU0tHPz9Ta3+f6ZlpWVFBM"
"TE9TUlRZafj+d+TU2NbRy8LAwb61t7i4urq/y27sVUA7OTk4ODU8QUBBSFFTV1Ncfnlv89vW1NTPy8zP09PY4vxtY1tUUE9Q"
"UFJWXGNv+urf2tbT0tLS09jf7P1uX1ZRU1FOTVJaYFxd8d3ydNrZ1tjmy8DEz7q4u7u/urzER9LmOzw9Pzg8NEtUPEl4dk5i"
"Y/Z6UHba+mba09jU1s3O2d7b521gXlxVUFJZWFZebX7+797V1tnRztLb3+DsY1VZWk1LVVdqXV3Rz2j+w3dmZ1LTVkf0smbM"
"q7WqtrGvqTxUsystLjMpMCo4zDRcvsDzycXq0kRMbUM+TmRVeOPNx8zLyMvb7XNgU0tMTU5OVWV4797Sz87OzdDc9m1cTUVH"
"SUhLX+vIy8a2t8fNvFZJPDA9MC04uV/Fn6ebn56fnMQ3riMeHR4bHSEhzENdq6iusqm8vm06STYtLjw6PVToysTDvbzDzdDh"
"YVVNS0tNTl557dTLysjJzNrzTklAOjY6P0JV37+wsq+mqbS9tz87KiApIR8ky9/smZuTkZSUlpsssTEUFxMVEBwYL8g7rJ+g"
"qKOmvLs9OjorKC03Mj9e1ca/vbq9xMvU/V9PT01MT1NeY/fn3dbQzs/Q1N70WU9KQT0+REVPeMy6ubesrbXAuV89NycsKSUm"
"WMhKoZ6blpqYnJs9z+QZHBgaFh4dKMU/wKamrKypvr5OOkIzLS88OUNX3srGxb7AydLb8mFXUVVTVltka217/vPr6ODX1tTS"
"0troa1lQRz9BR0ZNbdXAvLyvrrbFueQ+PCouLCgpWsNMo5+dmJyanp1A1d8cHhsdGB8eKs48yauqr66tvr1QP0k3MDQ9O0RU"
"78/LysPEy9TZ7W1eWVhZXFtmaGx2fv3y6+nh293Z3N3q/V9YUUtHSk9SX9/NwL6/trjD2MhTPjwvODUyOcrE2qanoqCkpKar"
"O8FIJCcjJR8pJDdePM23try1t8PBXFpXPzs/RT9KVWvr3dXNztHW2OPxcmpgXl1dYWJpcf/06eTf3t3e4OnyeGlcV1ZTUVVd"
"bu/j0MbLyMTH0t72UVY+PUdCQEzN3sW0uK2vs7S0w0/PRTU2NTMvOjlHX1TTx8nMxMXh2e9iV09PT1JTXWl4+OHh3uHf5+rz"
"/Hhvb2pvanZr+Grm3md48fLyVtBZ79ZQ0ljs8eda3+dd3HBTz+9X3GPne/pXxk953mt7Ws17W23ITFzW2VLt3+9yVdh9cmvM"
"T3fd/k7N20rSX+7eUurEPuPFT0u8Sl7Z9tk9tkdTydo70MFE42nGR13N+U/NbvPnSdHaXkq+bkfW7+pTz1j+ylBZ5M1P/Oni"
"bVDDTlTBUlfN6krf2OdS99VPy07RUdv25+0/ulpM78FHU8NaX9zmUvXIS1nJYnJbxklmvja850zqXsZK5GbIPsNfXs9X40/K"
"bOhCukdo0m5a0mtv20zNSLo80e9u+u34Vso/vV5abt39Xl/AXEPHelTdyjzE7k7Z3mhRzG5vWsxNz3FJu0xI1ro03bw+3uTi"
"UOj12uBCyNRCzl/43PJPz+RO5WPQXOr7enH22Uvvv0Vs0lrPSt3STNT7fVnc3lDMVGfV50/VYt7pRL1OaN51bnTLQtXMTV7O"
"zzKxXzm2TF7k1U7MStPTStha2eVkZGLBTEW5UUrUyEHqxULpvjx8vD1uvUxMwU3t93HVW2vbd1/dWtzmXWvKSfPFP/u+TkS7"
"U1ba4EvIXWjQTdxlyj/FamHbbFPIZ0fH5l5IukVu6dvdR9nXaFbMW1/ZXM9TYchVac5JdMFE2+FncPx90EnZ10/TSr1KUsld"
"bN1sfeNnb95g+dlU399Z3txGyV1v2uJO3OBPz0zFTOXkbutKw1hc3NtaUcBNePLl+U7BUlzn0Ejfz1h3acdD199qdnzfT8FH"
"V8FzRc/LQtN2/txtXevaWvPn+G5m2X5Y5c5MaMhL1W5p5mneYvB35VrbeOtjX8tPbdjbTmfIVkzCZ0zM50vZ0UnQdW54a85S"
"ddt+8kvEXVjUZupbzk/XaVfJX1b9w0BtuzvYzFNQwVZHuDzky1lZzFhUxE971mJOw09s3nDgV9vpW+9u2utQ399tU8lT5Wzz"
"7mnqaM1Ieb9JSb1VV85rVM1YaM9V4nJv2l793Ffo5XR4c+Fn8H7wbnfhaNtS2Hlu/uFn/N5V2PJmXsdOdffb/VjYaupmb+nV"
"UHXe5V727HL7evXmZGjUb1TOZlXJUOrj+FLU2knV21J+0V5h3uRT1nRdzl9Rz/BX4+Rk8ela2PlgbN3kWXrj4Fhuz15o7Nli"
"Uc3hSdzJSHXKUl3LU/Ts3E3U30rLTs1R0lXm0UXJYXH743Nq31/jfVrV/29n2mhj2md9+uzkWOd+6Wd41UnFVF/Y8mVdyk/s"
"6F3SWvF9+dxP0lvQS9PnUcxIy13dVefXT93peFzVaV3X81TZ203Z31Hd32JcylhbzFfra91f699Qz1r74Xpd5NlS4/z9ff5+"
"fv59/Xz8fPx8/Hz8fP18/X79fv5+/v///37+//5+/37/fv5+/n7+//7/fv////////9+//9+/////35+/////////n7//37/"
"/////////////v//fv9+//9+//7+/////37+/v///37//37///7//n7/fv////////5+fv/+///////////////+/37/////"
"///////////+fv/+/v7///9+//7///9+//7+////////fv///v7//37///////9+//5+//////9+/////v/+//9+//7/fv//"
"/v/+/////v/+/////////////n7////////////////+/////37//////////n7/fn7+fv//fv/+/v////7/////fn7/fv//"
"/////v///////37//v////////9+/////37/fv/////+fn7//////v9+///+//7+/////////v////////9+//7////+//7/"
"/////////////v////9+/v/+///+/////////37////////////+//////7/fv///////v//fv9+/37///////9+//5+///+"
"//////7//v////////////////7//v9+////fn7+/v9+///+/////v9+///+/////////n7///5+///////+/////////v//"
"fv7//37///7/fv/////////////////+//////7///9+////fv/+/v///v7///9+//////////7//37//////////37/////"
"fn7//////////////v///35+//////////////////7/fv/+///+/////v//fn7////////+//////5+/37+/37/////fv//"
"/37///9+//7///////////7+fv7///////////////////9+//////9+/////37/fn7////+fv9+/37+//9+//7///////9+"
"//////////////////7///9+/v///////v///n5+fv9+/v//////////fv///37+//////7//v/+/v//fv//////fv//////"
"/37/////////////fv9+fv/+fv//fv//fv//////////fv9+///+////////fv9+/////////37/fv5+//////7/////////"
"fv///v//fv//fv////5+fv9+////////////fv5+//////////////9+/////v//////fv7/////fv///n7+/37+/v/+////"
"///+/37///7/////////fv7//v///35+/////v7//v7///////////7///////7+///+fv/+///////////////+fv7///9+"
"/v////7//37+/////37///////7//////////////n7+/////////v///////v/+//////9+//9+/37//v7///7/////////"
"///////////////+//////7//////v///////v7///////7/////fv//fv///////////////////////37///7///9+////"
"///////////////+////fv//fv/////////////+fv///////37/////////fv////////7/////////fv7/////////////"
"//////9+//9+fv///v7/////////////////fv9+//7////+/////v//////fv9+/////37//v///////v/+/37//37+////"
"fv///v9+/////////////37////////////////////+/v9+/35+//////7+//////7+////////////fv///37+///+/v7/"
"fv7+//7////+/////v9+/n7////////////+///+//////////////9+/37//////////////37+///+////////////fv/+"
"/v9+///////+fv///37+////////////////fv7///7////////////////////////+fv7//v////5+////////////fv/+"
"///+/37///////7+//7//v///////v9+//9+/v///37//////37/////fv9+//9+//9+////fv9+/////37//35+/37//v7/"
"fv9+//////9+//////7+//7//v//fv7+//////9+///+/v9+fv///////v///37+///+/////v///37/fv7//37/////fv//"
"/////////////////////v///////v9+//9+//9+//////7//v///v/////+fv7/fv///v////5+////fv/+/v//////////"
"////fv7////////////+///+fn7///////////7//v/+/37///7//v////////7//37/fv/+fv///////37/fv////////7/"
"/////////v////9+///+fv/////////+///+fv/+/////v////////9+/////37/fv///v///////n7+///////////+////"
"//7//v7+//9+//7///5+/37//37/fv/+/n5+////fv/////////////+//7//n7///7/fv/+//////////////7//37/////"
"fv//////fv///////35+fv//fv////////////9+////fv//////////fv///////37//n7///////////9+/////37//v//"
"//7///////9+//7//////v/////+///+///////+/////////////v//////fv////////////9+fv7///////////7/////"
"/////v7/fv/////////+//////////5+/37+/////////////v////////////9+/////////37///7//37////+/n5+/v//"
"////fv///n5+//9+/////v9+fv//fv///37///5+//////////7+//7/////fv//fn7//37//v//fv7+//////////9+////"
"//7///9+//////////////////9+//9+/35+//////7+fv7+/v///////37//////////v7//37//////////n5+//9+////"
"//7//v////5+/v5+////////fv///n7//////v//fv/////+/////////v////9+///+////fn7/////fv7/////////fv9+"
"/////37//////37//v///////37/fv///v7////////////////+fv//////fv///37///////////////9+///////+////"
"/v//fv////9+/n7///7/////fv////////////7+//9+/////v////7//////v////7/////fv////////////////////7/"
"/37/fv////5+fv7+/////37////////+/////37/////////fv/////+/v///////v////////7+/v9+///+/v/+//////5+"
"/////////37//////37///////////9+/////n7//////////////////////////v//fv////9+///////////////+////"
"fv/////////+/35+fv/////+//7/////////fv///////n7///////9+//////7//37////+/37///////7////+/////37/"
"/v//fv//fv////9+//////////9+/v///////v//fn5+///////////////////+////fv5+/37///7//v///v///////37/"
"/////////////v9+/n5+/////////////////////n7/fv/+//7///9+//9+//////9+///+/37////+////////////////"
"//9+//7/////////fv7/fv////7///5+//////9+/37///////9+///+//////////////////7///9+//////////9+////"
"/37//////v///37//v//////////fv/+/////////////37////////////+/35+fv7///9+//////7//v9+/v7//n7///7+"
"//////7//v///////////v9+fv////5+///+/37/fv///37///7//////v7///////////////9+///////////////////+"
"/////37//v////5+////fv///35+//////7//v//////////fv5+///+fv//////fv7+/////v////////7+//////////5+"
"////fv////9+fv////7+///+///////////+/////////37//37////////+//////7//////////////////n7+fv9+////"
"/////37///7/////fv7///7///////////////7////+/v9+/v7/////fv////////////////5+/37//37/fv///37//37/"
"/v///n7+///+/v7//37+/v7+///+fv9+/////v7//v///n5+//7/fv/////+/////v///////////////v///v///v5+/v//"
"/////v///v7+/37+//7///9+////////fv///37///////7+//////9+fv9+//////9+/v9+/v5+/////35+/v//////////"
"/v7+/////////37+//7//////v///v//fv/+//////9+////fv9+//9+/////37//////////////37/////fv///////v//"
"////////fv/////+///////////+//////////7///9+fn7///////9+//////5+/37//v//fn5+///+/v////////7///7/"
"///+//////////7//////v7/fv9+//9+fv8=";


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
// PLAY ANY BASE64 PHRASE THROUGH MAX98357A
// ==================================================

void playAudioPhrase(const char *text, int maxSamples, const char *label) {
  Serial.print("PLAYING: "); Serial.println(label);
  i2s_driver_uninstall(CENTRE_I2S_PORT);
  delay(5);
  if (!setupAmplifierTX()) { setupCentreMicrophoneRX(); return; }

  // Decode this phrase into a temporary byte buffer.
  uint8_t *sampleBytes = (uint8_t *)malloc(maxSamples);
  if (!sampleBytes) {
    Serial.println("ERROR: Not enough memory for audio");
    i2s_driver_uninstall(CENTRE_I2S_PORT); delay(5); setupCentreMicrophoneRX(); return;
  }
  int outIndex = 0, accumulator = 0, bits = 0;
  for (int i = 0; text[i] != '\0'; i++) {
    int value = base64Value(text[i]);
    if (value < 0) continue;
    accumulator = (accumulator << 6) | value;
    bits += 6;
    while (bits >= 8) {
      bits -= 8;
      if (outIndex < maxSamples) sampleBytes[outIndex++] = (accumulator >> bits) & 0xFF;
    }
  }

  int txIndex = 0;
  for (int i = 0; i < outIndex - 1; i++) {
    int16_t sample0 = muLawDecode(sampleBytes[i]);
    int16_t sample1 = muLawDecode(sampleBytes[i + 1]);
    for (int step = 0; step < 6; step++) {
      int32_t interpolated = sample0 + (((int32_t)sample1 - sample0) * step) / 6;
      interpolated = (interpolated * 2);
      if (interpolated > 32767) interpolated = 32767;
      if (interpolated < -32768) interpolated = -32768;
      int32_t i2sSample = interpolated << 16;
      audioTxBuffer[txIndex++] = i2sSample;
      audioTxBuffer[txIndex++] = i2sSample;
      if (txIndex >= AUDIO_TX_WORDS) {
        size_t bytesWritten = 0;
        i2s_write(CENTRE_I2S_PORT, audioTxBuffer, sizeof(audioTxBuffer), &bytesWritten, portMAX_DELAY);
        txIndex = 0;
      }
    }
  }
  if (txIndex > 0) {
    for (int i = txIndex; i < AUDIO_TX_WORDS; i++) audioTxBuffer[i] = 0;
    size_t bytesWritten = 0;
    i2s_write(CENTRE_I2S_PORT, audioTxBuffer, sizeof(audioTxBuffer), &bytesWritten, portMAX_DELAY);
  }
  free(sampleBytes);
  // Allow DMA to drain based on phrase duration.
  delay((outIndex * 1000UL / 8000UL) + 100);
  i2s_zero_dma_buffer(CENTRE_I2S_PORT);
  i2s_driver_uninstall(CENTRE_I2S_PORT);
  delay(5);
  setupCentreMicrophoneRX();
  Serial.print("PLAYBACK COMPLETE: "); Serial.println(label);
}

void playMeSound() {
  playAudioPhrase(ME_AUDIO_BASE64, ME_AUDIO_SAMPLES, "Me");
}

void dispatchAudioCommand(String command);

bool isCompleteAudioCommand(String command) {
  command.trim();
  const char *commands[] = {
    "hello", "see_you_later", "me", "yes", "no", "help",
    "please", "thank_you", "want", "what", "again",
    "eat_food", "more", "go_to", "toilet", "fine", "like",
    "learn", "sign", "finish"
  };
  for (const char *candidate : commands) {
    if (command.equalsIgnoreCase(candidate)) return true;
  }
  return false;
}

void handleSerialCommand() {
  static String command = "";
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      command.trim();
      if (command.length()) dispatchAudioCommand(command);
      command = "";
      continue;
    }

    if (command.length() < 32) command += c;
    command.trim();

    // Also trigger as soon as a complete known command is typed.
    // This supports Serial Monitor set to "No line ending", while still
    // supporting Newline / Both NL & CR settings.
    if (isCompleteAudioCommand(command)) {
      String ready = command;
      command = "";
      dispatchAudioCommand(ready);
    }
  }
}

void dispatchAudioCommand(String command) {
  command.trim();
  if (command.equalsIgnoreCase("hello")) playAudioPhrase(HELLO_AUDIO_BASE64, HELLO_AUDIO_SAMPLES, "Hello");
  else if (command.equalsIgnoreCase("see_you_later")) playAudioPhrase(SEE_YOU_LATER_AUDIO_BASE64, SEE_YOU_LATER_AUDIO_SAMPLES, "See you later");
  else if (command.equalsIgnoreCase("me")) playMeSound();
  else if (command.equalsIgnoreCase("yes")) playAudioPhrase(YES_AUDIO_BASE64, YES_AUDIO_SAMPLES, "Yes");
  else if (command.equalsIgnoreCase("no")) playAudioPhrase(NO_AUDIO_BASE64, NO_AUDIO_SAMPLES, "No");
  else if (command.equalsIgnoreCase("help")) playAudioPhrase(HELP_AUDIO_BASE64, HELP_AUDIO_SAMPLES, "Help");
  else if (command.equalsIgnoreCase("please")) playAudioPhrase(PLEASE_AUDIO_BASE64, PLEASE_AUDIO_SAMPLES, "Please");
  else if (command.equalsIgnoreCase("thank_you")) playAudioPhrase(THANK_YOU_AUDIO_BASE64, THANK_YOU_AUDIO_SAMPLES, "Thank you");
  else if (command.equalsIgnoreCase("want")) playAudioPhrase(WANT_AUDIO_BASE64, WANT_AUDIO_SAMPLES, "Want");
  else if (command.equalsIgnoreCase("what")) playAudioPhrase(WHAT_AUDIO_BASE64, WHAT_AUDIO_SAMPLES, "What");
  else if (command.equalsIgnoreCase("again")) playAudioPhrase(AGAIN_AUDIO_BASE64, AGAIN_AUDIO_SAMPLES, "Again");
  else if (command.equalsIgnoreCase("eat_food")) playAudioPhrase(EAT_FOOD_AUDIO_BASE64, EAT_FOOD_AUDIO_SAMPLES, "Eat food");
  else if (command.equalsIgnoreCase("more")) playAudioPhrase(MORE_AUDIO_BASE64, MORE_AUDIO_SAMPLES, "More");
  else if (command.equalsIgnoreCase("go_to")) playAudioPhrase(GO_TO_AUDIO_BASE64, GO_TO_AUDIO_SAMPLES, "Go to");
  else if (command.equalsIgnoreCase("toilet")) playAudioPhrase(TOILET_AUDIO_BASE64, TOILET_AUDIO_SAMPLES, "Toilet");
  else if (command.equalsIgnoreCase("fine")) playAudioPhrase(FINE_AUDIO_BASE64, FINE_AUDIO_SAMPLES, "Fine");
  else if (command.equalsIgnoreCase("like")) playAudioPhrase(LIKE_AUDIO_BASE64, LIKE_AUDIO_SAMPLES, "Like");
  else if (command.equalsIgnoreCase("learn")) playAudioPhrase(LEARN_AUDIO_BASE64, LEARN_AUDIO_SAMPLES, "Learn");
  else if (command.equalsIgnoreCase("sign")) playAudioPhrase(SIGN_AUDIO_BASE64, SIGN_AUDIO_SAMPLES, "Sign");
  else if (command.equalsIgnoreCase("finish")) playAudioPhrase(FINISH_AUDIO_BASE64, FINISH_AUDIO_SAMPLES, "Finish");
  else { Serial.println("Unknown command. Try hello, see_you_later, me, yes, no, help, please, thank_you, want, what, again, eat_food, more, go_to, toilet, fine, like, learn, sign, finish."); }
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

#define EARLY_MIN_VOTES 3     // frames of timing data before trusting a direction
#define REPUBLISH_MS 500      // refresh rate while someone keeps talking
unsigned long lastDirectionPublishMs = 0;
bool directionSentThisEvent = false;

void resetSoundEvent() {

  directionSentThisEvent = false;

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


void publishDirectionIfReady() {
  if (speechFrames < MIN_SPEECH_FRAMES || validTDOAFrames < EARLY_MIN_VOTES) return;
  if (directionSentThisEvent && millis() - lastDirectionPublishMs < REPUBLISH_MS) return;

  const char* direction = nullptr;
  if (leftTDOAVotes > rightTDOAVotes && leftTDOAVotes > centreTDOAVotes) {
    direction = "LEFT";
  } else if (rightTDOAVotes > leftTDOAVotes && rightTDOAVotes > centreTDOAVotes) {
    direction = "RIGHT";
  }
  if (direction == nullptr) return;

  Serial.print("SOUND DETECTED ON ");
  Serial.println(direction);
  publishDirectionTelemetry(direction);
  directionSentThisEvent = true;
  lastDirectionPublishMs = millis();
}


// ==================================================
// PROCESS COMPLETED EVENT
// ==================================================
//
// Serial output is intentionally limited to the
// confirmed Left/Right result.
// ==================================================

void processSoundEvent() {

  // Left/Right was already reported during the event.
  if (directionSentThisEvent) {
    return;
  }

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


    publishDirectionIfReady();


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


// ==================================================
// SPEAK INFERENCE RESULTS
// ==================================================

// TCN label (SIGNS in tcn_infer.py) -> dispatchAudioCommand() phrase.
// Returns nullptr for signs with no recorded phrase.
const char* phraseForSign(const char* sign) {
  static const char* const table[][2] = {
    {"hello", "hello"}, {"see_you_later", "see_you_later"}, {"me", "me"},
    {"yes", "yes"}, {"no", "no"}, {"help", "help"}, {"please", "please"},
    {"thank_you", "thank_you"}, {"want", "want"}, {"what", "what"},
    {"again", "again"}, {"eat_food", "eat_food"}, {"more", "more"},
    {"go_to", "go_to"}, {"toilet", "toilet"}, {"fine", "fine"},
    {"like", "like"}, {"learn", "learn"}, {"sign", "sign"}, {"finish", "finish"},
  };
  for (const auto& entry : table) {
    if (strcasecmp(entry[0], sign) == 0) return entry[1];
  }
  return nullptr;
}

void speakPendingInference() {
  if (signQueueCount == 0) return;

  // Wait for the gap without taking the word off the queue, so it isn't lost.
  unsigned long now = millis();
  if (lastSpokenMs != 0 && now - lastSpokenMs < MIN_SPEECH_GAP_MS) return;

  char sign[sizeof(signQueue[0])];
  strcpy(sign, signQueue[signQueueHead]);
  unsigned long queuedMs = signQueueMs[signQueueHead];
  signQueueHead = (signQueueHead + 1) % SIGN_QUEUE_LEN;
  signQueueCount--;

  if (now - queuedMs > MAX_SIGN_AGE_MS) {
    Serial.print("Skipping stale sign: ");
    Serial.println(sign);
    return;
  }

  const char* phrase = phraseForSign(sign);
  if (phrase == nullptr) {
    Serial.print("No audio for sign: ");
    Serial.println(sign);
    return;
  }

  dispatchAudioCommand(phrase);

  lastSpokenMs = millis();

  // The mic was off during playback, so drop any half-finished event and
  // don't let the end of our own phrase count as a new sound.
  resetSoundEvent();
  lastDetectionTime = millis();
}


void mqttTask(void* param) {
  mqtt.setBufferSize(512);
  connectMQTT();

  for (;;) {
    if (!mqtt.connected()) {
      isRunning = false;
      connectMQTT();
    }
    // Read a few packets so results that arrived during playback get queued promptly.
    for (int i = 0; i < 4 && mqtt.connected(); i++) {
      mqtt.loop();
    }
    handleSerialCommand();
    speakPendingInference();
    publishHeartbeat();
    processMicrophone();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}


void loop() {
  // Audio processing, serial playback commands, and MQTT run in mqttTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
