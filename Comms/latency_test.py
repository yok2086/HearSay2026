"""
MQTT round-trip latency test - runs on the Mac while run_hearsay_pipeline.sh is up.

Publishes timestamped pings to latency/<id>/ping through the local 8883 tunnel
(Mac -> stujump -> ultra96 broker) and times how long each takes to come back.
Both timestamps come from the Mac's clock, so no clock sync is needed; the
one-way latency is roughly half the round trip.

Usage:
    python3 latency_test.py            # 50 pings
    python3 latency_test.py -n 200 -i 0.05
"""

import argparse
import statistics
import threading
import time
import uuid

import paho.mqtt.client as mqtt

BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--count", type=int, default=50)
    parser.add_argument("-i", "--interval", type=float, default=0.2, help="seconds between pings")
    args = parser.parse_args()

    topic = f"latency/{uuid.uuid4().hex[:8]}/ping"
    sent = {}
    rtts = []
    subscribed = threading.Event()

    def on_connect(client, userdata, flags, reason_code, properties):
        client.subscribe(topic, qos=0)

    def on_subscribe(client, userdata, mid, reason_codes, properties):
        subscribed.set()

    def on_message(client, userdata, msg):
        received = time.perf_counter()
        seq = int(msg.payload)
        if seq in sent:
            rtts.append((received - sent.pop(seq)) * 1000)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
    client.loop_start()

    if not subscribed.wait(10):
        raise SystemExit("Could not subscribe - is run_hearsay_pipeline.sh running?")

    for seq in range(args.count):
        sent[seq] = time.perf_counter()
        client.publish(topic, str(seq), qos=0)
        time.sleep(args.interval)
    time.sleep(2)  # let stragglers arrive

    client.loop_stop()
    client.disconnect()

    lost = args.count - len(rtts)
    if not rtts:
        raise SystemExit(f"No pings came back ({lost} lost).")
    rtts.sort()
    p95 = rtts[min(len(rtts) - 1, int(len(rtts) * 0.95))]
    print(f"{len(rtts)}/{args.count} pings returned, {lost} lost")
    print(f"round trip ms: min {rtts[0]:.1f}  median {statistics.median(rtts):.1f}  "
          f"mean {statistics.mean(rtts):.1f}  p95 {p95:.1f}  max {rtts[-1]:.1f}")
    print(f"approx one-way ms (median / 2): {statistics.median(rtts) / 2:.1f}")


if __name__ == "__main__":
    main()
