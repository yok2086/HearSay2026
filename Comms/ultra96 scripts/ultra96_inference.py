import json
import time
import collections

import paho.mqtt.client as mqtt

# ---- Fill these in ----
BROKER_HOST = "localhost" 
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/tmp/mosquitto_certs/ca.crt"   # path on the Ultra96, not the laptop
WINDOW_SIZE = 50                 # number of TELEMETRY samples per inference window


# A finger counts as bent for the whole window if it was bent in at least this
# fraction of the window's samples
FLEX_BENT_FRACTION = 0.5

# The window's five-finger pattern must show up (all five fingers exactly) in
# at least this fraction of samples to count as a confident match. Below that
# the hand was probably mid-transition, so fall through to the motion/idle checks.
MIN_PATTERN_MATCH = 0.6

FINGER_PATTERNS = {
    (1, 1, 1, 1, 1): "BYE",     # closed fist
    (0, 0, 0, 0, 0): "HELLO",   # open hand
}

# Gyro: real deg/s. Resting noise is typically a few deg/s; deliberate motion
# is often tens to low-hundreds deg/s. 40 is a starting guess, NOT a calibrated value.
GYRO_THRESHOLD_DEG_S = 40.0
# ------------------------------------------------------------------------

# device_id -> list of recent payload dicts (sliding window)
windows = collections.defaultdict(list)


def run_inference(window):
    samples = []
    for sample in window:
        flex = sample.get("flex", [])
        if len(flex) == 5 and all(v in (0, 1) for v in flex):
            samples.append(sample)

    # Don't decide from a handful of samples if most of the window was unusable
    if not samples or len(samples) < len(window) / 2:
        print(f"[inference] only {len(samples)}/{len(window)} usable samples: flex must be "
              "five 0/1 values (is the glove still running the old raw-ADC firmware?)")
        return {"event": "NONE", "confidence": 0.0}

    n = len(samples)

    # Per finger: fraction of samples in which it was bent, then a majority vote
    bent_fraction = [sum(s["flex"][i] for s in samples) / n for i in range(5)]
    pattern = tuple(1 if f >= FLEX_BENT_FRACTION else 0 for f in bent_fraction)

    # Fraction of samples that showed exactly this five-finger pattern
    match_fraction = sum(1 for s in samples if tuple(s["flex"]) == pattern) / n

    # Average |gyro| magnitude in deg/s, as a rough "how much rotation happened" signal
    avg_gyro_mag = sum(
        abs(s.get("imu", {}).get("gx", 0))
        + abs(s.get("imu", {}).get("gy", 0))
        + abs(s.get("imu", {}).get("gz", 0))
        for s in samples
    ) / n

    print(f"[calibration] pattern={pattern} match={match_fraction:.2f} "
          f"bent_fraction={[round(f, 2) for f in bent_fraction]} "
          f"avg_gyro_mag={avg_gyro_mag:.2f} deg/s ({n}/{len(window)} samples usable)")

    if pattern in FINGER_PATTERNS and match_fraction >= MIN_PATTERN_MATCH:
        event = FINGER_PATTERNS[pattern]
        confidence = min(0.99, match_fraction)
    elif avg_gyro_mag > GYRO_THRESHOLD_DEG_S:
        # Larger-than-usual rotation across the window
        event = "THANK YOU"
        confidence = min(0.99, 0.5 + (avg_gyro_mag - GYRO_THRESHOLD_DEG_S) / 60)
    else:
        # No recognised finger pattern and no strong motion - default/idle
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