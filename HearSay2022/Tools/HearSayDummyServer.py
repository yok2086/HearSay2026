#!/usr/bin/env python3
"""Random dummy sensor server for the HearSay Unity demo.

Run this on the laptop: python3 Tools/HearSayDummyServer.py
Then set HearSay Demo Visuals > Server Url in Unity to the printed /status address.
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import random
import socket
import threading
import time

WORDS = [
    "hello", "see_you_later", "i_me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again_repeat", "eat_food", "more", "go_to", "bathroom", "fine",
    "like", "learn", "sign", "finish_done",
]
DIRECTIONS = ["left", "right", "front", "back"]
state = {}
next_change = 0.0
last_real_update = 0.0
state_lock = threading.Lock()
REAL_DATA_TIMEOUT_SECONDS = 10.0


def local_ip():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("8.8.8.8", 80))
        return probe.getsockname()[0]
    finally:
        probe.close()


def current_state():
    """Return recent teammate data, or random fallback data for the demo."""
    global state, next_change
    with state_lock:
        using_real_data = time.monotonic() - last_real_update < REAL_DATA_TIMEOUT_SECONDS
        if not using_real_data and time.monotonic() >= next_change:
            state = {
                "soundActive": random.random() > 0.2,
                "soundDirection": random.choice(DIRECTIONS),
                "aslDetected": random.random() > 0.35,
                "aslWord": random.choice(WORDS),
                "aslConfidence": round(random.uniform(0.78, 0.98), 2),
                "source": "dummy",
            }
            next_change = time.monotonic() + random.uniform(3.5, 6.0)
        return state.copy()


def validate_update(update):
    """Keep the API predictable, even while teammates are still integrating."""
    required = {"soundActive", "soundDirection", "aslDetected", "aslWord", "aslConfidence"}
    missing = required - update.keys()
    if missing:
        raise ValueError("Missing fields: " + ", ".join(sorted(missing)))
    if update["soundDirection"] not in DIRECTIONS:
        raise ValueError("soundDirection must be one of: " + ", ".join(DIRECTIONS))
    if update["aslWord"] not in WORDS:
        raise ValueError("aslWord is not in the HearSay word bank")
    update["soundActive"] = bool(update["soundActive"])
    update["aslDetected"] = bool(update["aslDetected"])
    update["aslConfidence"] = max(0.0, min(1.0, float(update["aslConfidence"])))
    return {key: update[key] for key in required}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/status":
            self.send_error(404, "Use /status")
            return
        body = json.dumps(current_state()).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        """Receive real sensor/model data from a teammate at POST /update."""
        global state, last_real_update
        if self.path != "/update":
            self.send_error(404, "Use POST /update")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            update = json.loads(self.rfile.read(length).decode("utf-8"))
            update = validate_update(update)
        except (ValueError, TypeError, json.JSONDecodeError) as error:
            body = json.dumps({"ok": False, "error": str(error)}).encode("utf-8")
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        with state_lock:
            state = {**update, "source": "teammate"}
            last_real_update = time.monotonic()

        body = json.dumps({"ok": True, "message": "Unity will receive this update."}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


if __name__ == "__main__":
    address = local_ip()
    print("HearSay dummy server is running.")
    print(f"Set Unity Server Url to: http://{address}:8080/status")
    print(f"Teammates send real data to: http://{address}:8080/update")
    print("If no real update arrives for 10 seconds, it returns to random demo data.")
    ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
