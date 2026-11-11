#!/usr/bin/env python3
"""HearSay tutorial mode (runs on the laptop).

Plays a short demo video for each action, lets the user try it, and checks the
result live from the Ultra96 over MQTT:
  - signs           -> tele/glove_pair/result   (payload.event, payload.confidence)
  - sound direction -> tele/<mic>/raw            (payload.direction LEFT/RIGHT)
  - glove calibration -> sys/<glove>/status and sys/<glove>/calibration

Usage (from Comms/tutorial, with the MQTT tunnel to the Ultra96 open, the same
one dashboard.py uses: localhost:8883):
    python3 tutorial.py                 # real gloves / mic
    python3 tutorial.py --demo          # no Ultra96: simulate results with the keyboard
then open http://localhost:8000 in a browser (full screen: Ctrl+Cmd+F on a Mac).

Videos go in videos/<step id>.mp4, e.g. videos/hello.mp4, videos/calibration.mp4.
Missing videos show a placeholder, so the tutorial still runs.
Needs: pip install paho-mqtt   (not needed for --demo)
"""
import argparse
import json
import os
import queue
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))

# All 20 signs the model knows (same order as tcn_infer.py SIGNS).
ALL_SIGNS = [
    "hello", "see_you_later", "me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again", "eat_food", "more", "go_to", "toilet", "fine",
    "like", "learn", "sign", "finish",
]
# Signs practised in the tutorial by default (change with --signs).
DEFAULT_SIGNS = ["hello", "yes", "no", "help", "please", "thank_you"]

PASS_NEEDED = 3   # correct attempts needed to pass a sign
MAX_TRIES = 5     # attempts allowed per sign before moving on


def pretty(label):
    return label.replace("_", " ").capitalize()


def build_steps(signs):
    steps = [
        {"id": "welcome", "kind": "info", "title": "Welcome to HearSay",
         "text": "HearSay translates sign language into speech and turns speech into captions in VR. "
                 "This tutorial shows each feature and lets you try it. Watch the video, "
                 "then press Try and do the same."},
        {"id": "calibration", "kind": "calibration", "title": "Calibrate the gloves",
         "text": "Switch on both gloves. One short buzz: straighten all fingers and hold still. "
                 "Two short buzzes: make a comfortable fist and hold. A long buzz means the step was saved."},
        {"id": "vr_navigation", "kind": "manual", "title": "Move around the VR menu",
         "text": "Put on the headset. Look at a button and hold still for 2 seconds to press it. "
                 "On phones without a gyroscope, look down to move to the next button and up to go back."},
        {"id": "roles", "kind": "manual", "title": "Choose a role",
         "text": "Pick Deaf user (see captions of what others say) or Conversation partner "
                 "(see the sign translation and type replies)."},
    ]
    for s in signs:
        steps.append({"id": s, "kind": "sign", "sign": s, "title": f"Sign: {pretty(s)}",
                      "text": f"Watch how to sign “{pretty(s)}”. Press Try, then sign it. "
                              f"Get {PASS_NEEDED} out of {MAX_TRIES} right to pass."})
    steps += [
        {"id": "direction", "kind": "direction", "title": "Where is the sound coming from?",
         "text": "As the Deaf user, have someone speak from your LEFT, then from your RIGHT. "
                 "Sound waves appear on that side of the screen."},
        {"id": "captions", "kind": "manual", "title": "Live captions",
         "text": "Have the conversation partner speak. Their words appear as a caption next to their face."},
        {"id": "summary", "kind": "summary", "title": "All done!", "text": ""},
    ]
    return steps


class Hub:
    """Fans events out to every open browser page (Server-Sent Events)."""

    def __init__(self):
        self.clients = []
        self.lock = threading.Lock()
        self.mqtt_state = "off"

    def add(self):
        q = queue.Queue()
        with self.lock:
            self.clients.append(q)
        return q

    def remove(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def send(self, event):
        event.setdefault("t", time.time())
        with self.lock:
            for q in self.clients:
                q.put(event)


hub = Hub()


def normalize(topic, data):
    """Turn an MQTT message into a tutorial event (or None)."""
    payload = data.get("payload") or {}
    parts = topic.split("/")
    device = parts[1] if len(parts) > 1 else "?"
    if topic.endswith("/result") and payload.get("event"):
        return {"kind": "sign", "label": str(payload["event"]),
                "confidence": float(payload.get("confidence") or 0)}
    if payload.get("direction"):
        return {"kind": "direction", "direction": str(payload["direction"]).lower(), "device": device}
    if device.startswith("glove") and device != "glove_pair":
        stage = str(payload.get("stage") or payload.get("state") or "").upper()
        calibrated = payload.get("calibrated")
        if data.get("type") == "CALIBRATION" or "CALIBRATION" in str(data.get("type", "")) or calibrated is not None:
            ok = bool(calibrated) and stage in ("", "SUCCESS", "DONE")
            return {"kind": "calibration", "glove": device, "ok": ok or stage == "SUCCESS",
                    "stage": stage or ("SUCCESS" if calibrated else ""),
                    "message": str(payload.get("message") or "")}
    return None


def start_mqtt(args):
    import paho.mqtt.client as mqtt

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:  # paho-mqtt 1.x
        client = mqtt.Client()

    def on_connect(c, *rest):
        hub.mqtt_state = "connected"
        hub.send({"kind": "status", "mqtt": "connected"})
        for t in ("tele/+/result", "tele/+/raw", "sys/+/status", "sys/+/calibration"):
            c.subscribe(t, qos=0)
        print("[mqtt] connected, listening for results")

    def on_disconnect(c, *rest):
        hub.mqtt_state = "disconnected"
        hub.send({"kind": "status", "mqtt": "disconnected"})
        print("[mqtt] disconnected (retrying)")

    def on_message(c, userdata, msg):
        try:
            data = json.loads(msg.payload.decode())
        except Exception:
            return
        event = normalize(msg.topic, data)
        if event:
            print(f"[mqtt] {msg.topic}: {event}")
            hub.send(event)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    if args.ca:
        client.tls_set(ca_certs=args.ca)
        if args.insecure:
            client.tls_insecure_set(True)
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.reconnect_delay_set(1, 5)
    hub.mqtt_state = "connecting"
    client.connect_async(args.host, args.port, keepalive=15)
    client.loop_start()
    return client


class Handler(SimpleHTTPRequestHandler):
    config = {}

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=HERE, **kw)

    def log_message(self, fmt, *a):
        pass  # keep the terminal for MQTT messages

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/":
            self.path = "/index.html"
            return super().do_GET()
        if url.path == "/config":
            cfg = dict(self.config, mqtt=hub.mqtt_state,
                       videos=sorted(f for f in os.listdir(os.path.join(HERE, "videos"))
                                     if f.lower().endswith((".mp4", ".webm", ".gif", ".mov"))))
            return self._json(cfg)
        if url.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = hub.add()
            try:
                self.wfile.write(b"retry: 1000\n\n")
                self.wfile.flush()
                while True:
                    try:
                        ev = q.get(timeout=15)
                        self.wfile.write(f"data: {json.dumps(ev)}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                hub.remove(q)
            return
        if url.path == "/simulate":
            if not self.config.get("demo"):
                return self._json({"error": "only in --demo mode"}, 403)
            ev = {k: v[0] for k, v in parse_qs(url.query).items()}
            if "confidence" in ev:
                ev["confidence"] = float(ev["confidence"])
            if "ok" in ev:
                ev["ok"] = ev["ok"] == "1"
            hub.send(ev)
            return self._json({"sent": ev})
        return super().do_GET()


def main():
    p = argparse.ArgumentParser(description="HearSay tutorial mode")
    p.add_argument("--demo", action="store_true", help="no Ultra96: simulate results with the keyboard")
    p.add_argument("--signs", default=",".join(DEFAULT_SIGNS),
                   help="comma-separated signs to practise, or 'all' (default: %(default)s)")
    p.add_argument("--web-port", type=int, default=8000)
    p.add_argument("--host", default="localhost", help="MQTT broker (the SSH tunnel to the Ultra96)")
    p.add_argument("--port", type=int, default=8883)
    p.add_argument("--user", default="nathan")
    p.add_argument("--password", default="mqtt")
    p.add_argument("--ca", default=os.path.join(HERE, "..", "mqtt", "mosquitto_certs", "ca.crt"))
    p.add_argument("--insecure", action="store_true", help="skip TLS host-name check")
    args = p.parse_args()

    signs = ALL_SIGNS if args.signs == "all" else [s.strip() for s in args.signs.split(",") if s.strip()]
    unknown = [s for s in signs if s not in ALL_SIGNS]
    if unknown:
        p.error(f"unknown sign(s): {', '.join(unknown)}. Known: {', '.join(ALL_SIGNS)}")

    os.makedirs(os.path.join(HERE, "videos"), exist_ok=True)
    Handler.config = {"steps": build_steps(signs), "demo": args.demo, "signs": signs,
                      "allSigns": ALL_SIGNS, "passNeeded": PASS_NEEDED, "maxTries": MAX_TRIES}

    if args.demo:
        hub.mqtt_state = "demo"
        print("Demo mode: no MQTT. In the page, press C (correct), W (wrong), L/R (direction), K (gloves calibrated).")
    else:
        try:
            start_mqtt(args)
        except ImportError:
            raise SystemExit("paho-mqtt is not installed: pip install paho-mqtt   (or run with --demo)")
        print(f"Connecting to the Ultra96 broker at {args.host}:{args.port} "
              "(open the tunnel first, e.g. with run_hearsay_pipeline.sh)")

    server = ThreadingHTTPServer(("127.0.0.1", args.web_port), Handler)
    server.daemon_threads = True
    print(f"Tutorial ready: open http://localhost:{args.web_port}   (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
