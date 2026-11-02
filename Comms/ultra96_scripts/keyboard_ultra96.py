import json
import queue
import random
import threading
import time

import paho.mqtt.client as mqtt

BROKER_HOST = "localhost"        
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/home/xilinx/mqtt/mosquitto_certs/ca.crt" 

EVENTS = [
    "hello", "see_you_later", "me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again", "eat_food", "more", "go_to", "toilet", "fine",
    "like", "learn", "sign", "finish",
]

relay_queue = queue.Queue()


def event_worker(client):
    seq = 0
    print("[worker] AI event thread started", flush=True)
    while True:
        device_id, packet = relay_queue.get()
        payload = packet.get("payload", {})
        event = random.choice(EVENTS)
        confidence = round(random.uniform(0.75, 0.98), 2)
        result = {
            "type": "RESULT",
            "device_id": "ultra96",
            "session_id": "keyboard_test",
            "seq": seq,
            "timestamp_ms": int(time.time() * 1000),
            "payload": {
                "event": event,
                "confidence": confidence,
                "source_device": payload.get("source_device", device_id),
                "source_seq": payload.get("source_seq"),
                "key": payload.get("key"),
            },
        }
        client.publish(f"tele/{device_id}/result", json.dumps(result), qos=0)
        print(f"[{device_id}] #{payload.get('source_seq')} key {payload.get('key')!r} "
              f"modified={payload.get('modified')} -> AI event {event} ({confidence:.0%})", flush=True)
        seq += 1


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected to broker ({reason_code}), waiting for relay/+/dummy", flush=True)
    client.subscribe("relay/+/dummy", qos=1)


def on_message(client, userdata, msg):
    try:
        packet = json.loads(msg.payload)
    except ValueError:
        return
    if packet.get("type") != "DUMMY_RELAY":
        return
    relay_queue.put((msg.topic.split("/")[1], packet))


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.tls_set(ca_certs=CA_CERT_PATH)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)

    threading.Thread(target=event_worker, args=(client,), daemon=True).start()
    client.loop_forever()


if __name__ == "__main__":
    main()
