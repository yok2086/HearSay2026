import json
import threading
import queue

import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt"
# ------------------------

latest_result = {}
latest_result_lock = threading.Lock()

# device_id -> Queue of (msg_type, payload) tuples waiting to be displayed
device_queues = {}
device_queues_lock = threading.Lock()

print_lock = threading.Lock()


def safe_print(text):
    with print_lock:
        print(text)


def device_worker(device_id, q):
    safe_print(f"[{device_id}] worker thread started")
    while True:
        msg_type, payload = q.get()

        if msg_type == "RESULT":
            result = payload.get("payload", {})
            with latest_result_lock:
                latest_result[device_id] = result
            event = result.get("event", "?")
            confidence = result.get("confidence", 0)
            bar = "#" * int(confidence * 20)
            safe_print(f"[{device_id}] {event:12s} {confidence:.0%} {bar}")

        elif msg_type == "STATUS":
            state = payload.get("payload", {}).get("state")
            safe_print(f"[{device_id}] connection status: {state}")

        elif msg_type == "HELLO":
            safe_print(f"[{device_id}] online (HELLO), waiting for inference results...")


def get_or_create_device_queue(device_id):
    with device_queues_lock:
        if device_id not in device_queues:
            q = queue.Queue()
            device_queues[device_id] = q
            t = threading.Thread(target=device_worker, args=(device_id, q), daemon=True)
            t.start()
            safe_print(f"[registry] Spawned display thread for new device: {device_id}")
        return device_queues[device_id]


def on_connect(client, userdata, flags, rc):
    safe_print(f"Connected to broker (rc={rc})")
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

    q = get_or_create_device_queue(device_id)
    q.put((msg_type, payload))


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    print("Dashboard running (multi-device, one worker thread per ESP32). Ctrl+C to stop.\n")
    client.loop_forever()


if __name__ == "__main__":
    main()
