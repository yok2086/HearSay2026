"""
Ultra96 DPU inference

Buffers incoming TELEMETRY into fixed-size windows per device, runs
inference on each full window, and publishes a RESULT message back to
the broker.
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
WINDOW_SIZE = 50    # number of TELEMETRY samples per inference window

# ---- Classification thresholds (PLACEHOLDER) ----
# Flex sensors: raw 12-bit ADC (0-4095) 
FLEX_THRESHOLD_HIGH = 2800   # avg flex above this -> fingers curled tight
FLEX_THRESHOLD_LOW = 1200    # avg flex below this -> fingers extended

# Gyro: now in real deg/s
GYRO_THRESHOLD_DEG_S = 40.0
# ------------------------------------------------------------------------


# device_id -> list of recent payload dicts (sliding window)
windows = collections.defaultdict(list)


def run_inference(window):
    n = len(window)
    if n == 0:
        return {"event": "NONE", "confidence": 0.0}

    # Average flex reading across all 5 fingers and all samples in the window
    avg_flex = sum(
        sum(sample.get("flex", [0])) / max(len(sample.get("flex", [])), 1)
        for sample in window
    ) / n

    # Average gyro magnitude in deg/s
    avg_gyro_mag = sum(
        abs(sample.get("imu", {}).get("gx", 0))
        + abs(sample.get("imu", {}).get("gy", 0))
        + abs(sample.get("imu", {}).get("gz", 0))
        for sample in window
    ) / n

    print(f"[calibration] avg_flex={avg_flex:.1f}  avg_gyro_mag={avg_gyro_mag:.2f} deg/s")

    if avg_flex > FLEX_THRESHOLD_HIGH:
        # High flex reading = fingers curled tight 
        event = "A"
        confidence = min(0.99, 0.5 + (avg_flex - FLEX_THRESHOLD_HIGH) / 800)
    elif avg_flex < FLEX_THRESHOLD_LOW:
        # Low flex reading = fingers extended/open
        event = "B"
        confidence = min(0.99, 0.5 + (FLEX_THRESHOLD_LOW - avg_flex) / 800)
    elif avg_gyro_mag > GYRO_THRESHOLD_DEG_S:
        # Larger-than-usual rotation across the window
        event = "C"
        confidence = min(0.99, 0.5 + (avg_gyro_mag - GYRO_THRESHOLD_DEG_S) / 60)
    else:
        # Nothing crossed a threshold - default classification
        event = "D"
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
        windows[device_id] = windows[device_id][WINDOW_SIZE:]

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