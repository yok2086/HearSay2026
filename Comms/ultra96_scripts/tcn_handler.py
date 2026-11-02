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
WINDOW_SIZE = 256
STILL_FRAMES = 10
NORM_PATH = os.path.join(MODEL_DIR, "norm_stats.npz")   # saved by dataset.py
GYRO_RANGES = ((3, 6), (14, 17))
FLEX_CH = list(range(6, 11)) + list(range(17, 22))

# Training windows hold one take each, so the live stream is cut into takes.
# STARTING GUESSES, tune on the board: dataset.py doesn't define these.
MOTION_THRESHOLD_DPS = 30.0     # a hand rotating faster than this is moving
FLEX_CHANGE_THRESHOLD = 0.25    # flex change over STILL_FRAMES that counts as moving
END_STILL_FRAMES = 30           # still for this long (0.3 s) ends a take
MIN_TAKE_FRAMES = 40            # anything shorter is treated as a twitch

# Replaced with the training values from NORM_PATH when the handler starts.
FEATURE_MEAN = np.zeros(22, dtype=np.float32)
FEATURE_STD = np.ones(22, dtype=np.float32)

MIN_FLEX_RANGE = 50  # ADC counts; same clamp as dataset.py, avoids dividing by ~0


FEATURE_NAMES = (
    "ax", "ay", "az", "gx", "gy", "gz",
    "flex1", "flex2", "flex3", "flex4", "flex5",
)

sample_queues = {
    GLOVE_IDS[0]: collections.deque(maxlen=WINDOW_SIZE * 3),
    GLOVE_IDS[1]: collections.deque(maxlen=WINDOW_SIZE * 3),
}
recent_frames = collections.deque(maxlen=STILL_FRAMES)
current_take = []
still_run = 0
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

    # Samples buffered so far used the old scale, and the other glove kept
    # streaming while this one calibrated, so restart pairing from here.
    for queue in sample_queues.values():
        queue.clear()
    recent_frames.clear()
    reset_take()
    print(f"[calibration] {device_id}: straight={straight.tolist()} range={flex_range.tolist()} "
          f"gyro_offset={gyro_offset.tolist()}")


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


def fit_window(frames, length=WINDOW_SIZE):
    # Centre-pad with repeated first/last frames, or centre-crop if too long
    count = frames.shape[0]
    if count >= length:
        start = (count - length) // 2
        return frames[start:start + length]
    before = (length - count) // 2
    after = length - count - before
    return np.pad(frames, ((before, after), (0, 0)), mode="edge")


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


def reset_take():
    global still_run

    current_take.clear()
    still_run = 0


def is_moving(frame):
    for start, end in GYRO_RANGES:
        if np.linalg.norm(frame[start:end]) > MOTION_THRESHOLD_DPS:
            return True
    if recent_frames:
        return np.abs(frame[FLEX_CH] - recent_frames[0][FLEX_CH]).max() > FLEX_CHANGE_THRESHOLD
    return False


def finish_take(client):
    # Keep STILL_FRAMES of the still frames that ended the take, as training
    # takes also have a short pause around the sign.
    extra_still = max(0, still_run - STILL_FRAMES)
    frames = current_take[:len(current_take) - extra_still]
    reset_take()
    if len(frames) < MIN_TAKE_FRAMES:
        return

    # TcnDpu.predict expects (22, WINDOW_SIZE), while the frames are (T, 22).
    window = normalize(fit_window(np.stack(frames)).T)
    label, class_index, confidence = classifier.predict(window)
    publish_result(client, label, class_index, confidence, len(frames))


def add_frame(client, frame):
    global still_run

    moving = is_moving(frame)
    if current_take:
        current_take.append(frame)
        still_run = 0 if moving else still_run + 1
        if still_run >= END_STILL_FRAMES or len(current_take) >= WINDOW_SIZE:
            finish_take(client)
    elif moving:
        current_take.extend(recent_frames)   # the pause before the sign
        current_take.append(frame)
    recent_frames.append(frame)


def pair_samples(client):
    left_queue = sample_queues[GLOVE_IDS[0]]
    right_queue = sample_queues[GLOVE_IDS[1]]
    while left_queue and right_queue:
        add_frame(client, np.concatenate((left_queue.popleft(), right_queue.popleft())))


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
    if vector is None:
        print(f"[warn] Invalid telemetry from {device_id}")
        return

    sample_queues[device_id].append(vector)
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
