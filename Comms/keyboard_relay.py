import argparse
import json
import random
import sys
import termios
import threading
import time
import tty

import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt"
NUM_VALUES = 5
# ------------------------

lock = threading.Lock()
online = set()          # FireBeetles seen via HELLO and not since gone offline
sent = {}               # seq -> {"t": perf_counter, "values": [...], "key": str, "devices": [...]}
result_to_seq = {}      # (result topic, result seq) -> keystroke seq, for matching phone acks
print_lock = threading.Lock()


def log(text):
    # Terminal is in cbreak mode, so end lines with \r\n explicitly.
    with print_lock:
        sys.stdout.write(text + "\r\n")
        sys.stdout.flush()


def now_ms():
    return int(time.time() * 1000)


def elapsed_ms(seq):
    entry = sent.get(seq)
    return (time.perf_counter() - entry["t"]) * 1000 if entry else float("nan")


def expected_modification(values):
    return [v * 2 + 1 for v in values]


def on_connect(client, userdata, flags, reason_code, properties):
    log(f"Connected to broker ({reason_code})")
    client.subscribe([("sys/+/status", 1), ("tele/+/dummy", 1),
                      ("tele/+/result", 0), ("phone/+/ack", 0)])


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")
    try:
        data = json.loads(msg.payload)
    except ValueError:
        return

    if parts[0] == "sys" and parts[2] == "status":
        with lock:
            if data.get("type") == "HELLO":
                online.add(parts[1])
            elif data.get("payload", {}).get("state") == "offline":
                online.discard(parts[1])
        return

    if parts[0] == "tele" and parts[2] == "dummy":
        handle_firebeetle_reply(client, parts[1], data)
    elif parts[0] == "tele" and parts[2] == "result":
        handle_result(msg.topic, data)
    elif parts[0] == "phone" and parts[2] == "ack":
        handle_phone_ack(parts[1], data)


def handle_firebeetle_reply(client, device_id, data):
    payload = data.get("payload", {})
    seq = payload.get("source_seq")
    with lock:
        entry = sent.get(seq)
        rtt = elapsed_ms(seq)
    if entry is None:
        return

    ok = payload.get("values") == expected_modification(entry["values"]) and \
        payload.get("sum") == sum(entry["values"])
    log(f"  [{device_id}] #{seq} modified by FireBeetle {'OK' if ok else 'MISMATCH'} "
        f"({rtt:.0f} ms): {entry['values']} -> {payload.get('values')}, sum {payload.get('sum')}")
    if not ok:
        return

    relay = {
        "type": "DUMMY_RELAY",
        "device_id": "laptop",
        "seq": seq,
        "timestamp_ms": now_ms(),
        "payload": {"source_device": device_id, "source_seq": seq, "key": entry["key"],
                    "original": entry["values"], "modified": payload.get("values"),
                    "sum": payload.get("sum")},
    }
    client.publish(f"relay/{device_id}/dummy", json.dumps(relay), qos=1)
    log(f"  [{device_id}] #{seq} relayed to Ultra96")


def handle_result(topic, data):
    source_seq = data.get("payload", {}).get("source_seq")
    if source_seq is None:
        return
    with lock:
        result_to_seq[(topic, data.get("seq"))] = source_seq
        rtt = elapsed_ms(source_seq)
    p = data["payload"]
    log(f"  [{topic.split('/')[1]}] #{source_seq} Ultra96 AI event {p.get('event')} "
        f"({p.get('confidence', 0):.0%}) sent to phone ({rtt:.0f} ms)")


def handle_phone_ack(client_id, data):
    with lock:
        seq = result_to_seq.pop((data.get("ack_topic"), data.get("ack_seq")), None)
        rtt = elapsed_ms(seq)
    if seq is not None:
        log(f"  [phone {client_id}] #{seq} displayed and acked - keystroke to phone ack {rtt:.0f} ms")


def send_keystroke(client, key, seq, devices):
    values = [random.randint(0, 999) for _ in range(NUM_VALUES)]
    with lock:
        sent[seq] = {"t": time.perf_counter(), "values": values, "key": key, "devices": devices}
        for old in [s for s in sent if s < seq - 200]:
            del sent[old]
    packet = {
        "type": "DUMMY",
        "device_id": "laptop",
        "seq": seq,
        "timestamp_ms": now_ms(),
        "payload": {"key": key, "values": values},
    }
    for device_id in devices:
        client.publish(f"cmd/{device_id}/dummy", json.dumps(packet), qos=1)
    log(f"key {key!r} -> #{seq} sent {values} to {', '.join(devices)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", action="append", help="FireBeetle device_id (repeatable); default: all online")
    args = parser.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    client.loop_start()
    time.sleep(1)

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    tty.setcbreak(fd)
    log("Keyboard pipeline test. Press any key to send a dummy packet; Esc or Ctrl+C to quit.")
    with lock:
        log(f"Online FireBeetles: {', '.join(sorted(online)) or 'none yet'}")
    seq = 0
    try:
        while True:
            key = sys.stdin.read(1)
            if key in ("\x1b", ""):
                break
            with lock:
                devices = args.device or sorted(online)
            if not devices:
                log(f"key {key!r}: no FireBeetle online - power one on or pass --device")
                continue
            send_keystroke(client, key, seq, devices)
            seq += 1
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        client.loop_stop()
        client.disconnect()
        print("\nStopped.")


if __name__ == "__main__":
    main()
