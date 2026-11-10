import os
import json
import time
import threading
import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mqtt", "mosquitto_certs", "ca.crt")
WATCHDOG_TIMEOUT_S = 10
PRINT_EACH_TELEMETRY = True
# ------------------------

devices = {}
devices_lock = threading.Lock()


def set_state(device_id, state, reason):
    with devices_lock:
        info = devices.setdefault(device_id, {"state": None, "last_seen": time.time()})
        changed = info["state"] != state
        info["state"] = state
        info["last_seen"] = time.time()
    if changed:
        print(f"[{device_id}] -> {state} ({reason})")


def handle_hello(device_id, msg):
    caps = msg.get("payload", {}).get("capabilities")
    set_state(device_id, "RUNNING", f"HELLO, capabilities={caps}")


def handle_status(device_id, msg):
    if msg.get("payload", {}).get("state") == "offline":
        set_state(device_id, "OFFLINE", "LWT")


def handle_telemetry(device_id, msg):
    set_state(device_id, "RUNNING", "telemetry received")
    if PRINT_EACH_TELEMETRY:
        print(f"[{device_id}] telemetry seq={msg.get('seq')} payload={msg.get('payload', {})}")


def handle_calibration_status(device_id, msg):
    # Calibration can outlast the watchdog, and these show the glove is alive.
    set_state(device_id, "RUNNING", "calibrating")
    payload = msg.get("payload", {})
    print(f"[{device_id}] CALIBRATION {payload.get('stage', '?')}: {payload.get('message', '')}")


def on_connect(client, userdata, flags, rc):
    print(f"Connected to broker (rc={rc})")
    client.subscribe("sys/+/status", qos=1)
    client.subscribe("cmd/+/ack", qos=1)
    client.subscribe("tele/+/raw", qos=0)
    client.subscribe("tele/+/result", qos=0)


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        print(f"[warn] Could not parse message on {msg.topic}")
        return

    topic_parts = msg.topic.split("/")
    device_id = topic_parts[1] if len(topic_parts) > 1 else "unknown"
    msg_type = payload.get("type")

    if msg_type == "HELLO":
        handle_hello(device_id, payload)
    elif msg_type == "STATUS":
        handle_status(device_id, payload)
    elif msg_type == "CALIBRATION_STATUS":
        handle_calibration_status(device_id, payload)
    elif msg_type == "HEARTBEAT":
        set_state(device_id, "RUNNING", "heartbeat")
    elif msg_type == "TELEMETRY":
        handle_telemetry(device_id, payload)
    elif msg_type == "CONFIG_ACK":
        # Only seen if someone manually publishes a CONFIG override
        print(f"[{device_id}] acknowledged CONFIG override")
    elif msg_type == "RESULT":
        print(f"[{device_id}] result: {payload.get('payload')}")


def watchdog_loop():
    while True:
        time.sleep(1)
        now = time.time()
        with devices_lock:
            for device_id, info in devices.items():
                if info["state"] == "RUNNING" and now - info["last_seen"] > WATCHDOG_TIMEOUT_S:
                    print(f"[watchdog] {device_id} has not been heard from in {WATCHDOG_TIMEOUT_S}s — marking STALE")
                    info["state"] = "STALE"


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)

    watchdog_thread = threading.Thread(target=watchdog_loop, daemon=True)
    watchdog_thread.start()

    print("Coordinator running (passive monitor, subscribe-only). Ctrl+C to stop.")
    client.loop_forever()


if __name__ == "__main__":
    main()
