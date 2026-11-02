import json
import time
import queue
import threading
import collections

import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/home/xilinx/mqtt/mosquitto_certs/ca.crt"
WINDOW_SIZE = 10  # number of TELEMETRY samples per inference window
# ------------------------

# device_id -> list of recent payload dicts (per-device telemetry buffer)
windows = collections.defaultdict(list)

inference_queue = queue.Queue()


def run_inference(window):
    samples = [
        sample for sample in window
        if len(sample.get("flex", [])) == 5 and all(v in (0, 1) for v in sample["flex"])
    ]

    if not samples or len(samples) < len(window) / 2:
        print(f"[inference] only {len(samples)}/{len(window)} usable samples: flex must be "
              "five 0/1 values (is the glove sending raw ADC counts instead of binary?)")
        return {"event": "NONE", "confidence": 0.0}

    n = len(samples)

    # Average flex reading across all 5 fingers and all samples in the window
    avg_flex = sum(sum(sample["flex"]) / 5 for sample in samples) / n

    avg_gyro_mag = sum(
        abs(sample.get("imu", {}).get("gx", 0))
        + abs(sample.get("imu", {}).get("gy", 0))
        + abs(sample.get("imu", {}).get("gz", 0))
        for sample in samples
    ) / n

    if avg_flex > 0.6:
        event = "hello"
        confidence = min(0.99, 0.5 + (avg_flex - 0.6) / 0.4)
    elif avg_flex < 0.2:
        event = "yes"
        confidence = min(0.99, 0.5 + (0.2 - avg_flex) / 0.4)
    elif avg_gyro_mag > 40.0:
        event = "thank_you"
        confidence = min(0.99, 0.5 + (avg_gyro_mag - 40.0) / 60.0)
    else:
        event = "please"
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


def inference_worker(client):
    """
    The single "DPU" thread. Pulls one (device_id, session_id, window) at a
    time from the shared queue, runs inference, publishes the result, then
    moves to the next queued item - whichever device it belongs to. This is
    the only place run_inference() is ever called, so two devices' windows
    can never be processed at the same instant.
    """
    print("[inference] worker thread started")
    while True:
        device_id, session_id, window = inference_queue.get()
        result = run_inference(window)
        publish_result(client, device_id, session_id, result)


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

        inference_queue.put((device_id, session_id, window))
        print(f"[queue] {device_id} window ready, queue depth now {inference_queue.qsize()}")


def main():
    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)

    worker_thread = threading.Thread(target=inference_worker, args=(client,), daemon=True)
    worker_thread.start()

    print("DPU inference bridge running (multi-glove, single shared inference worker). Ctrl+C to stop.")
    client.loop_forever()


if __name__ == "__main__":
    main()