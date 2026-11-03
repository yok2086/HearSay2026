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
NORM_PATH = os.path.join(MODEL_DIR, "norm_stats.npz")   # saved by train.py

# ---- Sliding window + merging (starting values, tune on the board) ----
# The last WINDOW_SIZE frames (2.56 s) are classified every HOP frames.
# A word is published when AGREE_N predictions in a row give the same sign,
# each with confidence >= MIN_CONFIDENCE. The same word is not published again
# until a full window has passed, so one sign isn't reported several times.
HOP = 10                 # frames between predictions (10 -> 10 predictions/s at 100 Hz)
AGREE_N = 1              # predictions in a row that must agree
MIN_CONFIDENCE = 0.2     # softmax probability needed to count towards a word
PRINT_PREDICTIONS = True # print every window prediction (for tuning)
PRINT_SAMPLES = False    # print every sample's glove timestamp (about 200 lines/s, for gaps.py)

# Replaced with the training values from NORM_PATH when the handler starts.
FEATURE_MEAN = np.zeros(22, dtype=np.float32)
FEATURE_STD = np.ones(22, dtype=np.float32)

MIN_FLEX_RANGE = 50  # ADC counts; same clamp as dataset.py, avoids dividing by ~0

# Two-hand alignment, same idea as logger.py: each glove's millis() is put on
# this board's clock, then samples are paired by time rather than arrival order.
PAIR_TOLERANCE_MS = 8       # a sample with no partner this close is dropped (100 Hz = 10 ms apart)
CLOCK_DRIFT_PER_MS = 0.0002 # how fast a clock offset may creep up (0.2 ms per second)
SAMPLE_PERIOD_MS = 10       # 100 Hz, as in the training data
# A small gap in the paired frames is filled with copies of the last frame so the
# window keeps its time scale. Anything bigger restarts the window.
MAX_FILL = 20               # frames (0.2 s) of a gap that are filled


FEATURE_NAMES = (
    "ax", "ay", "az", "gx", "gy", "gz",
    "flex1", "flex2", "flex3", "flex4", "flex5",
)

sample_queues = {
    GLOVE_IDS[0]: collections.deque(maxlen=WINDOW_SIZE * 3),
    GLOVE_IDS[1]: collections.deque(maxlen=WINDOW_SIZE * 3),
}
window_frames = collections.deque(maxlen=WINDOW_SIZE)   # last WINDOW_SIZE paired (22,) frames
last_frame_time = None
frames_since_prediction = 0
frames_since_emit = WINDOW_SIZE
streak_label, streak_count = None, 0
last_emitted = None
clock_offsets = {}     # device_id -> (estimated arrival - millis, when it was last updated)
unpaired_samples = 0   # dropped since the last prediction
filled_frames = 0      # filled since the last prediction
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
    # streaming while this one calibrated, so restart pairing and the window from here.
    for queue in sample_queues.values():
        queue.clear()
    reset_window()
    print(f"[calibration] {device_id}: straight={straight.tolist()} range={flex_range.tolist()} "
          f"gyro_offset={gyro_offset.tolist()}")


def update_clock_offset(device_id, newest_timestamp_ms):
    # The network can only delay a message, never deliver it early, so the
    # smallest (arrival - millis) is the best estimate of the offset between the
    # clocks. It only creeps up slowly, to follow clock drift without following
    # a glove that is seconds behind. Only the newest sample in a batch is fresh.
    now_ms = time.perf_counter() * 1000
    transit_ms = now_ms - newest_timestamp_ms
    if device_id in clock_offsets:
        offset, updated_ms = clock_offsets[device_id]
        transit_ms = min(transit_ms, offset + (now_ms - updated_ms) * CLOCK_DRIFT_PER_MS)
    clock_offsets[device_id] = (transit_ms, now_ms)
    return transit_ms


def glove_vector(sample, calibration):
    # sample is [t, ax, ay, az, gx, gy, gz, f1, f2, f3, f4, f5], from one telemetry batch.
    if not isinstance(sample, list) or len(sample) != 12 or any(value is None for value in sample):
        return None

    # flex is raw ADC: 0 = straight, 1 = fist (not clipped), same as dataset.py.
    vector = np.asarray(sample[1:], dtype=np.float32)
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


def reset_window():
    """Restart the window and the merging state."""
    global last_frame_time, frames_since_prediction, frames_since_emit
    global streak_label, streak_count, last_emitted

    window_frames.clear()
    last_frame_time = None
    frames_since_prediction = 0
    frames_since_emit = WINDOW_SIZE
    streak_label, streak_count = None, 0
    last_emitted = None


def merge(client, label, class_index, confidence):
    """Turn the stream of window predictions into words (see AGREE_N / MIN_CONFIDENCE)."""
    global streak_label, streak_count, last_emitted, frames_since_emit

    if confidence < MIN_CONFIDENCE:
        streak_label, streak_count = None, 0
        return
    if label == streak_label:
        streak_count += 1
    else:
        streak_label, streak_count = label, 1

    # Publish once per streak, when it reaches AGREE_N. The same word again only
    # after a full window, so a brief wobble mid-sign doesn't publish it twice.
    if streak_count == AGREE_N and not (label == last_emitted and frames_since_emit < WINDOW_SIZE):
        publish_result(client, label, class_index, confidence, WINDOW_SIZE)
        last_emitted = label
        frames_since_emit = 0


def add_frame(client, frame):
    global frames_since_prediction, frames_since_emit, unpaired_samples, filled_frames

    window_frames.append(frame)
    frames_since_prediction += 1
    frames_since_emit += 1
    if len(window_frames) < WINDOW_SIZE or frames_since_prediction < HOP:
        return
    frames_since_prediction = 0

    # TcnDpu.predict expects (22, WINDOW_SIZE), while the frames are (WINDOW_SIZE, 22).
    window = normalize(np.stack(window_frames).T)
    label, class_index, confidence = classifier.predict(window)
    if PRINT_PREDICTIONS:
        print(f"  window: {label:14s} {confidence:.3f}")
    if unpaired_samples or filled_frames:
        print(f"[align] dropped {unpaired_samples} unpaired samples, filled {filled_frames} missing frames")
        unpaired_samples = filled_frames = 0
    merge(client, label, class_index, confidence)


def add_paired_frame(client, frame, frame_time):
    """Add one paired frame, filling small gaps and restarting the window after big ones."""
    global last_frame_time, filled_frames

    if last_frame_time is not None:
        missing = round((frame_time - last_frame_time) / SAMPLE_PERIOD_MS) - 1
        if missing > MAX_FILL:
            print(f"[warn] {missing} frames missing, restarting the window")
            reset_window()
        elif missing > 0:
            filled_frames += missing
            for _ in range(missing):
                add_frame(client, window_frames[-1])
    last_frame_time = frame_time
    add_frame(client, frame)


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
        add_paired_frame(client, np.concatenate((left_vector, right_vector)), (left_time + right_time) / 2)


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

    samples = message.get("payload", {}).get("samples")
    if not samples or not isinstance(samples, list):
        print(f"[warn] Invalid telemetry from {device_id} (old glove firmware?)")
        return

    vectors = [glove_vector(sample, calibration) for sample in samples]
    if any(vector is None for vector in vectors):
        print(f"[warn] Invalid telemetry from {device_id}")
        return

    offset = update_clock_offset(device_id, samples[-1][0])
    for sample, vector in zip(samples, vectors):
        if PRINT_SAMPLES:
            print(f"[sample] {device_id} timestamp_ms={sample[0]}")
        sample_queues[device_id].append((sample[0] + offset, vector))
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
