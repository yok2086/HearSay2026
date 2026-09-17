"""
BLE-to-MQTT bridge for HearSay.

Runs on the laptop. Scans for the FireBeetle's BLE advertisement, connects,
subscribes to its telemetry notify characteristic, and republishes every
message to the MQTT broker over localhost (reached via the existing SSH
relay tunnel to the Ultra96). Also relays CONFIG commands from MQTT back
to the FireBeetle over BLE.

coordinator.py needs NO changes - from its side, this looks identical to
the FireBeetle publishing/subscribing directly over WiFi.
"""

import asyncio
import json
import threading

import paho.mqtt.client as mqtt
from bleak import BleakScanner, BleakClient

# ---- Fill these in ----
BLE_DEVICE_NAME = "glove_01"
BROKER_HOST = "localhost"
BROKER_PORT = 8883
MQTT_USER = "nathan"
MQTT_PASS = "mqtt"
CA_CERT_PATH = "/Users/NathanTan_1/mqtt/mosquitto_certs/ca.crt"
# ------------------------

# Must match firebeetle_ble.ino exactly
SERVICE_UUID        = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
TELEMETRY_CHAR_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # NOTIFY
COMMAND_CHAR_UUID    = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # WRITE


def topic_for(msg_type: str, device_id: str) -> str:
    if msg_type == "HELLO":
        return f"sys/{device_id}/status"
    if msg_type == "CONFIG_ACK":
        return f"cmd/{device_id}/ack"
    if msg_type == "TELEMETRY":
        return f"tele/{device_id}/raw"
    return f"evt/{device_id}/error"


class Bridge:
    def __init__(self):
        self.loop = None
        self.ble_client = None
        self.mqtt_client = None
        self.write_queue = None   # asyncio.Queue, created inside the event loop

    # ---- BLE side ----

    def handle_notification(self, _sender, data: bytearray):
        try:
            payload = json.loads(data.decode())
        except (json.JSONDecodeError, UnicodeDecodeError):
            print(f"[warn] Could not parse BLE notification: {data!r}")
            return

        msg_type = payload.get("type")
        device_id = payload.get("device_id", "unknown")
        topic = topic_for(msg_type, device_id)

        retain = (msg_type == "HELLO")
        self.mqtt_client.publish(topic, json.dumps(payload), qos=1, retain=retain)
        print(f"[ble->mqtt] {topic}: {payload}")

    async def ble_writer_task(self):
        """Drains queued CONFIG writes (populated from the MQTT thread) into BLE."""
        while True:
            data = await self.write_queue.get()
            if self.ble_client and self.ble_client.is_connected:
                await self.ble_client.write_gatt_char(COMMAND_CHAR_UUID, data)
                print(f"[mqtt->ble] wrote CONFIG: {data}")

    async def run_ble(self):
        print(f"Scanning for BLE device named '{BLE_DEVICE_NAME}'...")
        device = await BleakScanner.find_device_by_name(BLE_DEVICE_NAME, timeout=15.0)
        if device is None:
            print("Device not found. Is the FireBeetle powered on and advertising?")
            return

        print(f"Found {device.address}, connecting...")
        async with BleakClient(device) as client:
            self.ble_client = client
            print("Connected. Subscribing to telemetry notifications...")
            await client.start_notify(TELEMETRY_CHAR_UUID, self.handle_notification)

            self.write_queue = asyncio.Queue()
            writer = asyncio.create_task(self.ble_writer_task())

            print("Bridge running. Ctrl+C to stop.")
            try:
                while client.is_connected:
                    await asyncio.sleep(1)
            finally:
                writer.cancel()

    # ---- MQTT side ----

    def on_mqtt_connect(self, client, userdata, flags, rc):
        print(f"Connected to broker (rc={rc})")
        client.subscribe("cmd/+/set", qos=1)   # CONFIG messages from coordinator.py

    def on_mqtt_message(self, client, userdata, msg):
        # This callback runs on paho's own network thread, not the asyncio loop -
        # hand the write off to the BLE event loop thread-safely.
        if self.loop is None or self.write_queue is None:
            return
        self.loop.call_soon_threadsafe(self.write_queue.put_nowait, msg.payload)

    def start_mqtt(self):
        client = mqtt.Client()
        client.tls_set(ca_certs=CA_CERT_PATH)
        client.username_pw_set(MQTT_USER, MQTT_PASS)
        client.on_connect = self.on_mqtt_connect
        client.on_message = self.on_mqtt_message
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
        client.loop_start()   # runs in its own background thread
        self.mqtt_client = client

    async def main(self):
        self.loop = asyncio.get_running_loop()
        self.start_mqtt()
        await self.run_ble()


if __name__ == "__main__":
    asyncio.run(Bridge().main())
