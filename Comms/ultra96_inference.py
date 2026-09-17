"""
Ultra96 DPU inference bridge.

Runs directly ON the Ultra96 (not the laptop) - connects to the broker via
localhost, since they're on the same machine. No SSH tunnel needed for this
script; it's the one piece in the whole pipeline that doesn't need one.

Buffers incoming TELEMETRY into fixed-size windows per device, runs
inference on each full window, and publishes a RESULT message back to
the broker. coordinator.py and any display script already know how to
handle RESULT messages - no changes needed there.
"""

import json
import time
import collections

import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/tmp/mosquitto_certs/ca.crt"
WINDOW_SIZE = 10 # number of TELEMETRY samples per inference window
# ------------------------

windows = collections.defaultdict(list)


def run_inference(window):
    """
    Threshold-based placeholder - Replace with a real Vitis-AI/PYNQ DPU
    call once your model is ready (see the real-implementation sketch above
    this function in earlier versions of this file).
 
    'window' is a list of the last WINDOW_SIZE payload dicts, each shaped like:
      {"imu": {"ax":.., "ay":.., "az":.., "gx":.., "gy":.., "gz":..}, "flex": [..]}
    """
    n = len(window)
    if n == 0:
        return {"event": "NONE", "confidence": 0.0}
 
    # Average flex reading across all 5 fingers and all samples in the window
    avg_flex = sum(
        sum(sample.get("flex", [0])) / max(len(sample.get("flex", [])), 1)
        for sample in window
    ) / n
 
    # Average |gyro| magnitude
    avg_gyro_mag = sum(
        abs(sample.get("imu", {}).get("gx", 0))
        + abs(sample.get("imu", {}).get("gy", 0))
        + abs(sample.get("imu", {}).get("gz", 0))
        for sample in window
    ) / n
 
    # Thresholds tuned to the dummy data's ranges from firebeetle_ble.ino:
    # flex ~400-700, gyro axes each roughly -0.20..0.20 (so |gx|+|gy|+|gz| usually < ~0.3)
    if avg_flex > 600:
        event = "HELLO"
        confidence = min(0.99, 0.5 + (avg_flex - 600) / 200)
    elif avg_flex < 450:
        event = "BYE"
        confidence = min(0.99, 0.5 + (450 - avg_flex) / 150)
    elif avg_gyro_mag > 0.35:
        event = "THANK YOU"
        confidence = min(0.99, 0.5 + (avg_gyro_mag - 0.35) / 0.3)
    else:
        event = "WELCOME"
        confidence = 0.6
 
    return {"event": event, "confidence": round(confidence, 2)}


def publish_result(client, device_id, session_id, result):
    msg = {
        "type": "RESULT",
        "device_id": "ultra96",
        "session_id": session_id,
        "seq": int(time.time() * 1000) % 100000,
        "timestamp_ms": int(time.time() * 1000),
        "payload": result,
    }
    client.publish(f"tele/{device_id}/result", json.dumps(msg), qos=0)
    print(f"[inference] {device_id}: {result}")


def on_connect(client, userdata, flags, rc):
    print(f"Connected to broker (rc={rc})")
    client.subscribe("tele/+/raw", qos=0)


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return

    if payload.get("type") != "TELEMETRY":
        return

    topic_parts = msg.topic.split("/")
    device_id = topic_parts[1] if len(topic_parts) > 1 else "unknown"
    session_id = payload.get("session_id", "sess_001")

    windows[device_id].append(payload.get("payload", {}))

    if len(windows[device_id]) >= WINDOW_SIZE:
        window = windows[device_id][:WINDOW_SIZE]
        windows[device_id] = windows[device_id][WINDOW_SIZE:]   # slide, non-overlapping

        result = run_inference(window)
        publish_result(client, device_id, session_id, result)


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    print("DPU inference bridge running. Ctrl+C to stop.")
    client.loop_forever()


if __name__ == "__main__":
    main()
