"""
Dummy RESULT sender - runs ON the Ultra96.

Publishes fake RESULT messages straight to the broker on a timer, with no
glove, no coordinator handshake, and no telemetry involved at all. This lets
you test the phone's SSH-tunneled MQTT connection and its ASL panel display
independently of whether the physical glove can currently reach stujump.

Since this runs ON the Ultra96, it talks to the broker over localhost - same
as ultra96_inference.py / dummy_inference.py - no SSH tunnel needed here.
The phone's own SshMqttClient does the tunneling on its end.

Usage:
    python3 dummy_phone_sender.py
"""

import json
import time
import random

import paho.mqtt.client as mqtt

# ---- Fill these in ----
BROKER_HOST = "localhost"        # same machine as the broker - no tunnel needed
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/tmp/mosquitto_certs/ca.crt"   # path on the Ultra96, not the laptop

DEVICE_ID = "glove_01"           # matches the real glove's device_id, so this
                                  # looks identical to a real result from the
                                  # phone's point of view

SEND_INTERVAL_S = 4              # how often to publish a new result

# Cycles through these in order so every word gets tested, rather than
# picking randomly and possibly never hitting one of them. These match the
# keys in HearSaySshMqttManager.cs's EventToAslWord - keep both in sync if
# you change one.
EVENTS = ["HELLO", "YES", "THANK_YOU", "PLEASE"]
# ------------------------


def build_result_message(seq, event, confidence):
    return {
        "type": "RESULT",
        "device_id": "ultra96",
        "session_id": "dummy_session",
        "seq": seq,
        "timestamp_ms": int(time.time() * 1000),
        "payload": {
            "event": event,
            "confidence": round(confidence, 2),
        },
    }


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    client.loop_start()   # background thread handles the network I/O

    print(f"Dummy phone sender running - publishing to tele/{DEVICE_ID}/result "
          f"every {SEND_INTERVAL_S}s. Ctrl+C to stop.")

    seq = 0
    event_index = 0
    try:
        while True:
            event = EVENTS[event_index % len(EVENTS)]
            confidence = random.uniform(0.75, 0.98)
            msg = build_result_message(seq, event, confidence)

            client.publish(f"tele/{DEVICE_ID}/result", json.dumps(msg), qos=0)
            print(f"[sent] {event} ({confidence:.0%})")

            seq += 1
            event_index += 1
            time.sleep(SEND_INTERVAL_S)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()