import collections
import json
import os
import sys
import time

import numpy as np
import paho.mqtt.client as mqtt

MODEL_DIR = "/home/xilinx/jupyter_notebooks/tcn"

os.environ.setdefault("XILINX_XRT", "/usr")

sys.path.insert(0, MODEL_DIR)
from tcn_infer import TcnDpu

BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/home/xilinx/mqtt/mosquitto_certs/ca.crt"

GLOVE_IDS = ("glove_01A", "glove_01B")
RESULT_DEVICE_ID = "glove_pair"

# ---- Must match dataset.py, which prepared the training data ----
# Frame order: glove_01A (left hand) then glove_01B (right hand), each
# ax, ay, az, gx, gy, gz, flex thumb..pinky.
# The stream is cut into consecutive, non-overlapping blocks of WINDOW_SIZE
# frames (2.56 s at 100 Hz); each block is classified once, then cleared.
WINDOW_SIZE = 256
NORM_PATH = os.path.join(MODEL_DIR, "norm_stats.npz")   # saved by train.py

# Replaced with the training values from NORM_PATH when the handler starts.
FEATURE_MEAN = np.zeros(22, dtype=np.float32)
FEATURE_STD = np.ones(22, dtype=np.float32)

MIN_FLEX_RANGE = 50  # ADC counts; same clamp as dataset.py, avoids dividing by ~0

# Two-hand alignment, same idea as logger.py: each glove's millis() is put on
# this board's clock, then samples are paired by time rather than arrival order.
PAIR_TOLERANCE_MS = 8       # a sample with no partner this close is dropped (100 Hz = 10 ms apart)
OFFSET_WINDOW_MS = 5000     # clock offsets are re-estimated over the last 1-2 windows, to follow drift


FEATURE_NAMES = (
    "ax", "ay", "az", "gx", "gy", "gz",
    "flex1", "flex2", "flex3", "flex4", "flex5",
)

sample_queues = {
    GLOVE_IDS[0]: collections.deque(maxlen=WINDOW_SIZE * 3),
    GLOVE_IDS[1]: collections.deque(maxlen=WINDOW_SIZE * 3),
}
window_frames = []     # paired (22,) frames of the current block
clock_offsets = {}     # device_id -> smallest (arrival - millis) seen, for the current and previous window
unpaired_samples = 0   # dropped since the last block, printed with its result
classifier = None
flex_calibration = {}  # device_id -> (straight, range, gyro offset), from the glove's CALIBRATION message
waiting_for_calibration = set()


def store_calibration(device_id, payload):
    straight = payload.get("straight", [])
    bent = payload.get("bent", [])
    if len(straight) != 5 or len(bent) != 5:
        print(f"[warn] Invalid calibration from {device_id}")
        return

    straight = np.asarray(straight, dtype=np.float32)
    flex_range = np.asarray(bent, dtype=np.float32) - straight
    weak = np.abs(flex_range) < MIN_FLEX_RANGE
    if weak.any():
        print(f"[warn] {device_id}: small flex range for finger(s) {np.where(weak)[0].tolist()}, "
              f"clamped to {MIN_FLEX_RANGE}")
        flex_range = np.where(weak, np.where(flex_range < 0, -MIN_FLEX_RANGE, MIN_FLEX_RANGE), flex_range)

    gyro_offset = payload.get("gyro_offset")
    if gyro_offset is None or len(gyro_offset) != 3:
        print(f"[warn] {device_id}: no gyro offset in calibration, using 0 (old glove firmware?)")
        gyro_offset = [0.0, 0.0, 0.0]
    gyro_offset = np.asarray(gyro_offset, dtype=np.float32)

    flex_calibration[device_id] = (straight, flex_range, gyro_offset)
    waiting_for_calibration.discard(device_id)
    clock_offsets.pop(device_id, None)   # millis() restarts if the glove rebooted

    # Samples buffered so far used the old scale, and the other glove kept
    # streaming while this one calibrated, so restart pairing and the block from here.
    for queue in sample_queues.values():
        queue.clear()
    window_frames.clear()
    print(f"[calibration] {device_id}: straight={straight.tolist()} range={flex_range.tolist()} "
          f"gyro_offset={gyro_offset.tolist()}")


def board_time_ms(device_id, timestamp_ms):
    # The network can only delay a message, never deliver it early, so the
    # smallest (arrival - millis) is the best estimate of the offset between the
    # clocks. The spacing then comes from the glove's own clock, without network jitter.
    now_ms = time.perf_counter() * 1000
    transit_ms = now_ms - timestamp_ms
    offset = clock_offsets.setdefault(
        device_id, {"current": float("inf"), "previous": float("inf"), "rotate_at_ms": 0.0})
    if now_ms >= offset["rotate_at_ms"]:
        offset["previous"] = offset["current"]
        offset["current"] = transit_ms
        offset["rotate_at_ms"] = now_ms + OFFSET_WINDOW_MS
    else:
        offset["current"] = min(offset["current"], transit_ms)
    return timestamp_ms + min(offset["current"], offset["previous"])


def glove_vector(payload, calibration):
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

    # flex is raw ADC: 0 = straight, 1 = fist (not clipped), same as dataset.py.
    vector = np.asarray(values, dtype=np.float32)
    straight, flex_range, gyro_offset = calibration
    vector[3:6] -= gyro_offset
    vector[6:] = (vector[6:] - straight) / flex_range
    return vector


def load_norm_stats(path=NORM_PATH):
    global FEATURE_MEAN, FEATURE_STD

    stats = np.load(path)
    if stats["mean"].shape != (22,) or stats["std"].shape != (22,):
        raise SystemExit(f"{path}: expected 22 means and stds")
    FEATURE_MEAN = stats["mean"].astype(np.float32)
    FEATURE_STD = stats["std"].astype(np.float32)


def normalize(window):
    # Normalize a (22, WINDOW_SIZE) window using the training statistics
    return (window - FEATURE_MEAN[:, None]) / FEATURE_STD[:, None]


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


def add_frame(client, frame):
    global unpaired_samples

    window_frames.append(frame)
    if len(window_frames) < WINDOW_SIZE:
        return

    # TcnDpu.predict expects (22, WINDOW_SIZE), while the frames are (WINDOW_SIZE, 22).
    window = normalize(np.stack(window_frames).T)
    window_frames.clear()
    label, class_index, confidence = classifier.predict(window)
    publish_result(client, label, class_index, confidence, WINDOW_SIZE)
    if unpaired_samples:
        print(f"[align] dropped {unpaired_samples} unpaired samples since the last block")
        unpaired_samples = 0


def pair_samples(client):
    global unpaired_samples

    left_queue = sample_queues[GLOVE_IDS[0]]
    right_queue = sample_queues[GLOVE_IDS[1]]
    while left_queue and right_queue:
        left_time, left_vector = left_queue[0]
        right_time, right_vector = right_queue[0]
        if abs(left_time - right_time) > PAIR_TOLERANCE_MS:
            # The earlier sample's partner was lost or never sent.
            (left_queue if left_time < right_time else right_queue).popleft()
            unpaired_samples += 1
            continue
        left_queue.popleft()
        right_queue.popleft()
        add_frame(client, np.concatenate((left_vector, right_vector)))


def on_connect(client, userdata, flags, rc):
    print(f"Connected to broker (rc={rc})")
    client.subscribe([(f"tele/{device_id}/raw", 0) for device_id in GLOVE_IDS]
                     + [(f"sys/{device_id}/calibration", 0) for device_id in GLOVE_IDS])
    print(f"Subscribed to telemetry and calibration from {', '.join(GLOVE_IDS)}")


def on_message(client, userdata, msg):
    try:
        message = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        print(f"[warn] Could not parse message on {msg.topic}")
        return

    parts = msg.topic.split("/")
    device_id = parts[1] if len(parts) > 2 else ""
    if device_id not in sample_queues:
        return

    if message.get("type") == "CALIBRATION":
        store_calibration(device_id, message.get("payload", {}))
        return

    if message.get("type") != "TELEMETRY":
        return

    calibration = flex_calibration.get(device_id)
    if calibration is None:
        if device_id not in waiting_for_calibration:
            waiting_for_calibration.add(device_id)
            print(f"[warn] Dropping telemetry from {device_id} until it sends its calibration")
        return

    vector = glove_vector(message.get("payload", {}), calibration)
    timestamp_ms = message.get("timestamp_ms")
    if vector is None or timestamp_ms is None:
        print(f"[warn] Invalid telemetry from {device_id}")
        return

    print(f"[sample] {device_id} timestamp_ms={timestamp_ms}")
    sample_queues[device_id].append((board_time_ms(device_id, timestamp_ms), vector))
    pair_samples(client)


def main():
    global classifier

    os.chdir(MODEL_DIR)
    load_norm_stats()
    print(f"Loaded training mean/std from {NORM_PATH}")
    print("Loading TCN DPU model...")
    classifier = TcnDpu()
    print("TCN DPU model loaded")
    # Input shape is (1, 1, frames, 22). A mismatch means this isn't the model dataset.py trained.
    if classifier.shape_in[2] != WINDOW_SIZE:
        raise SystemExit(f"Model expects {classifier.shape_in[2]} frames but WINDOW_SIZE is {WINDOW_SIZE}")

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
