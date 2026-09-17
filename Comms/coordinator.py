import json
import time
import threading
import paho.mqtt.client as mqtt

# ---- Fill these in ----
BROKER_HOST = "Nathans-MacBook-Pro-4.local" 
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/mqtt/mosquitto_certs/ca.crt"
DEFAULT_SAMPLE_RATE_HZ = 50
WATCHDOG_TIMEOUT_S = 20
# ------------------------

devices = {}
devices_lock = threading.Lock()


def now_ms():
    return int(time.time() * 1000)


def publish_json(client, topic, payload_dict, qos=1, retain=False):
    client.publish(topic, json.dumps(payload_dict), qos=qos, retain=retain)


def handle_hello(client, device_id, msg):
    session_id = msg.get("session_id", "sess_001")
    with devices_lock:
        devices[device_id] = {
            "state": "HANDSHAKING",
            "last_seen": time.time(),
            "session_id": session_id,
        }
    print(f"[registry] {device_id} said HELLO (capabilities={msg.get('payload', {}).get('capabilities')})")

    config_msg = {
        "type": "CONFIG",
        "device_id": "coordinator",
        "session_id": session_id,
        "seq": 1,
        "timestamp_ms": now_ms(),
        "payload": {"sample_rate_hz": DEFAULT_SAMPLE_RATE_HZ},
    }
    publish_json(client, f"cmd/{device_id}/set", config_msg, qos=1)
    print(f"[registry] Sent CONFIG to {device_id}")


def handle_config_ack(device_id, msg):
    with devices_lock:
        if device_id in devices:
            devices[device_id]["state"] = "RUNNING"
            devices[device_id]["last_seen"] = time.time()
    print(f"[registry] {device_id} is now RUNNING")


def handle_telemetry(device_id, msg):
    with devices_lock:
        if device_id in devices:
            devices[device_id]["last_seen"] = time.time()
    payload = msg.get("payload", {})
    seq = msg.get("seq")
    print(f"[telemetry] {device_id} seq={seq} payload={payload}")


def handle_status(device_id, msg):
    payload = msg.get("payload", {})
    if payload.get("state") == "offline":
        with devices_lock:
            if device_id in devices:
                devices[device_id]["state"] = "OFFLINE"
        print(f"[registry] {device_id} went OFFLINE (via LWT)")


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
        handle_hello(client, device_id, payload)
    elif msg_type == "CONFIG_ACK":
        handle_config_ack(device_id, payload)
    elif msg_type == "TELEMETRY":
        handle_telemetry(device_id, payload)
    elif msg_type == "STATUS":
        handle_status(device_id, payload)
    elif msg_type == "RESULT":
        print(f"[result] {device_id}: {payload.get('payload')}")


def watchdog_loop():
    """Periodically checks for devices that have gone silent without a clean LWT."""
    while True:
        time.sleep(5)
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

    print("Coordinator running. Ctrl+C to stop.")
    client.loop_forever()


if __name__ == "__main__":
    main()
