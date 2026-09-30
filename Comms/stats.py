import json
import statistics
import threading
import time
import uuid

import paho.mqtt.client as mqtt

# ---- Details ----
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt"
STATS_INTERVAL_S = 5
PING_INTERVAL_S = 1.0
PING_TIMEOUT_S = 5.0
# ------------------------

SELF_TOPIC = f"latency/mac/{uuid.uuid4().hex[:8]}/echo"
PHONE_PING_TOPIC = "latency/phone/ping"

lock = threading.Lock()
# device_id -> {"bytes", "msgs", "first", "last"} for tele/<device>/raw
rx_stats = {}
# ping seq -> perf_counter() when that round of pings was sent
pending = {}
# ("mac", None) / ("esp", device_id) / ("phone", client_id) / ("ack", client_id) -> [rtt_ms, ...]
rtt_samples = {}
# (result topic, seq) -> perf_counter() when that RESULT reached the Mac
results_seen = {}


def record_rx(device_id, nbytes):
    now = time.time()
    with lock:
        s = rx_stats.setdefault(device_id, {"bytes": 0, "msgs": 0, "first": now, "last": now})
        s["bytes"] += nbytes
        s["msgs"] += 1
        s["last"] = now


def record_pong(target, payload):
    received = time.perf_counter()
    try:
        seq = int(payload)
    except ValueError:
        return
    with lock:
        sent = pending.get(seq)
        if sent is not None:
            rtt_samples.setdefault(target, []).append((received - sent) * 1000)


def record_result(topic, payload):
    received = time.perf_counter()
    try:
        seq = json.loads(payload).get("seq")
    except (ValueError, AttributeError):
        return
    if seq is None:
        return
    with lock:
        for key in [k for k, t in results_seen.items() if received - t > PING_TIMEOUT_S]:
            del results_seen[key]
        results_seen[(topic, seq)] = received


def record_ack(client_id, payload):
    received = time.perf_counter()
    try:
        ack = json.loads(payload)
        key = (ack["ack_topic"], ack["ack_seq"])
    except (ValueError, KeyError, TypeError):
        return
    with lock:
        seen = results_seen.get(key)
        if seen is not None:
            rtt_samples.setdefault(("ack", client_id), []).append((received - seen) * 1000)


def ping_loop(client):
    seq = 0
    while True:
        with lock:
            devices = list(rx_stats)
            now = time.perf_counter()
            for old in [s for s, t in pending.items() if now - t > PING_TIMEOUT_S]:
                del pending[old]
            pending[seq] = now
        payload = str(seq)
        client.publish(SELF_TOPIC, payload, qos=0)
        client.publish(PHONE_PING_TOPIC, payload, qos=0)
        for device_id in devices:
            client.publish(f"cmd/{device_id}/ping", payload, qos=0)
        seq += 1
        time.sleep(PING_INTERVAL_S)


def mean(values):
    return statistics.mean(values) if values else None


def fmt_ms(value):
    return f"{value:6.1f} ms" if value is not None else "   n/a   "


def stats_loop():
    previous = {}   # device_id -> (bytes, msgs) at the last report
    last_report = time.time()
    while True:
        time.sleep(STATS_INTERVAL_S)
        now = time.time()
        interval = now - last_report
        last_report = now

        with lock:
            snapshot = {d: dict(s) for d, s in rx_stats.items()}
            samples = {k: list(v) for k, v in rtt_samples.items()}
            rtt_samples.clear()

        # One-way legs from this interval's echo pings.
        mac_rtt = mean(samples.get(("mac", None), []))

        def one_way(rtts):
            if not rtts or mac_rtt is None:
                return None
            return max(0.0, (mean(rtts) - mac_rtt) / 2)

        # Per phone: word-ack RTT when words arrived this interval, else the ping estimate.
        phone_lines = []
        phone_legs = {}
        phones = sorted({k[1] for k in samples if k[0] in ("phone", "ack")})
        for phone in phones:
            acks = samples.get(("ack", phone), [])
            ping_leg = one_way(samples.get(("phone", phone), []))
            if acks:
                phone_legs[phone] = mean(acks) / 2
                phone_lines.append(f"  phone {phone}: Ultra96<->phone RTT {fmt_ms(mean(acks))} "
                                   f"(word acks, n={len(acks)}) -> one-way {fmt_ms(phone_legs[phone])}"
                                   f"  | ping estimate {fmt_ms(ping_leg)}")
            elif ping_leg is not None:
                phone_legs[phone] = ping_leg
                phone_lines.append(f"  phone {phone}: no words this interval -> ping estimate "
                                   f"one-way {fmt_ms(ping_leg)}")
        phone_leg = mean(list(phone_legs.values()))

        lines = [f"[stats {time.strftime('%H:%M:%S')}] last {interval:.1f}s"]
        if not snapshot:
            lines.append("  no telemetry received yet")
        total_kbps = 0.0
        for device_id, s in sorted(snapshot.items()):
            prev_bytes, prev_msgs = previous.get(device_id, (0, 0))
            kbps = (s["bytes"] - prev_bytes) * 8 / 1000 / interval
            msg_rate = (s["msgs"] - prev_msgs) / interval
            elapsed = s["last"] - s["first"]
            avg_kbps = s["bytes"] * 8 / 1000 / elapsed if elapsed > 0 else 0.0
            total_kbps += kbps
            previous[device_id] = (s["bytes"], s["msgs"])

            esp_leg = one_way(samples.get(("esp", device_id), []))
            end_to_end = esp_leg + phone_leg if esp_leg is not None and phone_leg is not None else None
            lines.append(f"  {device_id:<12} {kbps:7.2f} kbps  {msg_rate:6.1f} msg/s  | "
                         f"avg {avg_kbps:7.2f} kbps, {s['msgs']} msgs, {s['bytes'] / 1024:.1f} KiB  | "
                         f"ESP32->Ultra96->phone {fmt_ms(end_to_end)}")
            lines.append(f"  {'':<12} latency breakdown: ESP32->Ultra96 {fmt_ms(esp_leg)}"
                         f"  + Ultra96->phone {fmt_ms(phone_leg)}")
        if snapshot:
            lines.append(f"  {'TOTAL':<12} {total_kbps:7.2f} kbps")

        lines.append(f"  Mac<->broker RTT {fmt_ms(mac_rtt)}"
                     + ("" if phone_legs else "  (no phone answering: open the app's MQTT connection)"))
        lines.extend(phone_lines)
        print("\n".join(lines), flush=True)


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected to broker ({reason_code}), watching telemetry and latency echoes")
    client.subscribe([("tele/+/raw", 0), ("tele/+/pong", 0), ("tele/+/result", 0),
                      ("phone/+/ack", 0), ("latency/phone/+/pong", 0), (SELF_TOPIC, 0)])


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")
    if msg.topic == SELF_TOPIC:
        record_pong(("mac", None), msg.payload)
    elif len(parts) == 3 and parts[0] == "tele" and parts[2] == "raw":
        record_rx(parts[1], len(msg.payload))
    elif len(parts) == 3 and parts[0] == "tele" and parts[2] == "pong":
        record_pong(("esp", parts[1]), msg.payload)
    elif len(parts) == 3 and parts[0] == "tele" and parts[2] == "result":
        record_result(msg.topic, msg.payload)
    elif len(parts) == 3 and parts[0] == "phone" and parts[2] == "ack":
        record_ack(parts[1], msg.payload)
    elif len(parts) == 4 and parts[0] == "latency" and parts[1] == "phone" and parts[3] == "pong":
        record_pong(("phone", parts[2]), msg.payload)


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    client.loop_start()

    threading.Thread(target=ping_loop, args=(client,), daemon=True).start()

    print(f"Speed + latency every {STATS_INTERVAL_S}s (pings every {PING_INTERVAL_S}s). Ctrl+C to stop.")
    try:
        stats_loop()
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
