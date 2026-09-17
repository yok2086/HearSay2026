"""
HearSay display / dashboard.

Runs anywhere with access to the broker (laptop, via the existing SSH
relay tunnel to localhost). Subscribes to inference results and shows
them live in the terminal. This is a stand-in for a real UI (phone
visualizer, Unity app, etc.) - swap the print() calls for whatever
actual rendering you build later; the MQTT side stays the same.
"""

import json
import time

import paho.mqtt.client as mqtt

# ---- Fill these in ----
BROKER_HOST = "localhost"   # reached via your existing -L relay tunnel
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt"
# ------------------------

latest_result = {}   # device_id -> most recent result, for a simple live summary


def clear_line():
    print("\r" + " " * 80 + "\r", end="")


def on_connect(client, userdata, flags, rc):
    print(f"Connected to broker (rc={rc})")
    client.subscribe("tele/+/result", qos=0)
    client.subscribe("sys/+/status", qos=1)


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return

    topic_parts = msg.topic.split("/")
    device_id = topic_parts[1] if len(topic_parts) > 1 else "unknown"
    msg_type = payload.get("type")

    if msg_type == "RESULT":
        result = payload.get("payload", {})
        latest_result[device_id] = result
        event = result.get("event", "?")
        confidence = result.get("confidence", 0)
        bar = "#" * int(confidence * 20)
        clear_line()
        print(f"[{device_id}] {event:12s} {confidence:.0%} {bar}")

    elif msg_type == "STATUS":
        state = payload.get("payload", {}).get("state")
        print(f"\n[{device_id}] connection status: {state}\n")


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    print("Dashboard running. Ctrl+C to stop.\n")
    client.loop_forever()


if __name__ == "__main__":
    main()
