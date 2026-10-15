import collections
import json
import os
import sys
import time

import numpy as np
import paho.mqtt.client as mqtt

MODEL_DIR = "/home/xilinx/jupyter_notebooks/tcn"
sys.path.insert(0, MODEL_DIR)
from tcn_infer import TcnDpu

BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/home/xilinx/mqtt/mosquitto_certs/ca.crt"

GLOVE_IDS = ("glove_01A", "glove_01B")
RESULT_DEVICE_ID = "glove_pair"
WINDOW_SIZE = 96

# The model expects normalized features. Replace these values with the exact
# mean/std used when the TCN training windows were created. The feature order
# is glove_01's 11 features followed by glove_02's 11 features.
FEATURE_MEAN = np.zeros(22, dtype=np.float32)
FEATURE_STD = np.ones(22, dtype=np.float32)


FEATURE_NAMES = (
    "ax", "ay", "az", "gx", "gy", "gz",
    "flex1", "flex2", "flex3", "flex4", "flex5",
)

sample_queues = {
    GLOVE_IDS[0]: collections.deque(maxlen=WINDOW_SIZE * 3),
    GLOVE_IDS[1]: collections.deque(maxlen=WINDOW_SIZE * 3),
}
paired_window = collections.deque(maxlen=WINDOW_SIZE)
classifier = None


def glove_vector(payload):
    imu = payload.get("imu", {})
    flex = payload.get("flex", [])
    if len(flex) != 5:
        return None

    values = [
        imu.get("ax"), imu.get("ay"), imu.get("az"),
        imu.get("gx"), imu.get("gy"), imu.get("gz"),
        *flex,
    ]
    if any(value is None for value in values):
        return None
    return np.asarray(values, dtype=np.float32)


def normalize(window_22_by_96):
    # Normalize a raw (22, 96) window using the training statistics
    return (window_22_by_96 - FEATURE_MEAN[:, None]) / FEATURE_STD[:, None]


def publish_result(client, label, class_index, confidence, sample_count):
    timestamp_ms = int(time.time() * 1000)
    result = {
        "type": "RESULT",
        "device_id": RESULT_DEVICE_ID,
        "session_id": "glove_pair",
        "seq": timestamp_ms % 100000,
        "timestamp_ms": timestamp_ms,
        "payload": {
            "event": label,
            "class_index": class_index,
            "confidence": round(confidence, 4),
            "window_size": sample_count,
            "source_devices": list(GLOVE_IDS),
        },
    }
    topic = f"tele/{RESULT_DEVICE_ID}/result"
    client.publish(topic, json.dumps(result), qos=0)
    print(f"[inference] {label} ({confidence:.3f}) -> {topic}")


def infer_window(client):
    global paired_window

    if len(paired_window) < WINDOW_SIZE:
        return

    # TcnDpu.predict expects (22, 96), while the buffer is (96, 22).
    raw_window = np.asarray(paired_window, dtype=np.float32).T
    window = normalize(raw_window)
    label, class_index, confidence = classifier.predict(window)
    publish_result(client, label, class_index, confidence, len(paired_window))
    paired_window.clear()


def pair_samples(client):
    left_queue = sample_queues[GLOVE_IDS[0]]
    right_queue = sample_queues[GLOVE_IDS[1]]
    while left_queue and right_queue:
        paired_window.append(np.concatenate((left_queue.popleft(), right_queue.popleft())))
        infer_window(client)


def on_connect(client, userdata, flags, rc):
    print(f"Connected to broker (rc={rc})")
    client.subscribe([("tele/glove_01/raw", 0), ("tele/glove_02/raw", 0)])
    print(f"Subscribed to telemetry from {', '.join(GLOVE_IDS)}")


def on_message(client, userdata, msg):
    try:
        message = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        print(f"[warn] Could not parse message on {msg.topic}")
        return

    if message.get("type") != "TELEMETRY":
        return

    parts = msg.topic.split("/")
    device_id = parts[1] if len(parts) > 2 else ""
    if device_id not in sample_queues:
        return

    vector = glove_vector(message.get("payload", {}))
    if vector is None:
        print(f"[warn] Invalid telemetry from {device_id}")
        return

    sample_queues[device_id].append(vector)
    pair_samples(client)


def main():
    global classifier

    os.chdir(MODEL_DIR)
    print("Loading TCN DPU model...")
    classifier = TcnDpu()
    print("TCN DPU model loaded")

    client = mqtt.Client()
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    print("Two-glove TCN MQTT inference bridge running. Ctrl+C to stop.")
    client.loop_forever()


if __name__ == "__main__":
    main()
