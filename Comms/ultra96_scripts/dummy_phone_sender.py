import json
import time
import random

import paho.mqtt.client as mqtt

# ---- Fill these in ----
BROKER_HOST = "localhost"       
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/home/xilinx/mqtt/mosquitto_certs/ca.crt"

DEVICE_ID = "glove_01"

SEND_INTERVAL_S = 4

EVENTS = ["HELLO", "YES", "THANK_YOU", "PLEASE"]


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