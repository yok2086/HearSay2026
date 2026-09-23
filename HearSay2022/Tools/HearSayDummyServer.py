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
state = {
    "soundActive": False,
    "soundDirection": "front",
    "aslDetected": False,
    "aslWord": "hello",
    "aslConfidence": 0.95,
    "source": "terminal",
}
next_change = 0.0
last_real_update = 0.0
# Terminal mode is the default: no visual data appears until the presenter types a command.
manual_mode = True
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
        if manual_mode:
            return state.copy()

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


def terminal_state():
    """The current state, with safe defaults before the first random update."""
    return state.copy() if state else {
        "soundActive": False,
        "soundDirection": "front",
        "aslDetected": False,
        "aslWord": "hello",
        "aslConfidence": 0.95,
    }


def set_terminal_state(**changes):
    """Store a manual Terminal command until the user chooses `random` again."""
    global state, manual_mode
    with state_lock:
        state = {**terminal_state(), **changes, "source": "terminal"}
        manual_mode = True
    print("Unity display:", json.dumps(state))


def normalize_word(word):
    return word.lower().strip().replace(" ", "_").replace("/", "_").replace("-", "_")


def print_terminal_help():
    print("\nTerminal controls (Unity updates within a second):")
    print("  sound left | right | front | back   (up means front; down means back)")
    print("  sound off")
    print("  asl hello                         (also accepts: asl thank you)")
    print("  asl off")
    print("  both left hello")
    print("  clear                             (hide sound and ASL UI)")
    print("  status                            (show the current JSON)")
    print("  random                            (return to automatic demo values)\n")


def terminal_input_loop():
    """Lets the presenter manually drive the Unity UI from this Terminal."""
    print_terminal_help()
    direction_aliases = {"up": "front", "down": "back"}
    while True:
        try:
            command = input("HearSay > ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nStopping HearSay server.")
            return

        if not command:
            continue
        parts = command.split()
        action = parts[0].lower()

        if action in {"help", "?"}:
            print_terminal_help()
        elif action == "status":
            print(json.dumps(current_state(), indent=2))
        elif action == "random":
            global manual_mode, next_change
            with state_lock:
                manual_mode = False
                next_change = 0.0
            print("Random demo data restored.")
        elif action == "clear":
            set_terminal_state(soundActive=False, aslDetected=False)
        elif action == "sound" and len(parts) == 2 and parts[1].lower() == "off":
            set_terminal_state(soundActive=False)
        elif action in {"sound", "left", "right", "front", "back", "up", "down"}:
            direction = parts[1].lower() if action == "sound" and len(parts) == 2 else action
            direction = direction_aliases.get(direction, direction)
            if direction in DIRECTIONS:
                set_terminal_state(soundActive=True, soundDirection=direction)
            else:
                print("Use: sound left, sound right, sound front, sound back, or sound off")
        elif action == "asl" and len(parts) == 2 and parts[1].lower() == "off":
            set_terminal_state(aslDetected=False)
        elif action == "asl" and len(parts) >= 2:
            word = normalize_word(" ".join(parts[1:]))
            if word in WORDS:
                set_terminal_state(aslDetected=True, aslWord=word, aslConfidence=0.95)
            else:
                print("Not in word bank. Type `help` for the supported command format.")
        elif action == "both" and len(parts) >= 3:
            direction = direction_aliases.get(parts[1].lower(), parts[1].lower())
            word = normalize_word(" ".join(parts[2:]))
            if direction in DIRECTIONS and word in WORDS:
                set_terminal_state(soundActive=True, soundDirection=direction,
                                   aslDetected=True, aslWord=word, aslConfidence=0.95)
            else:
                print("Example: both left hello")
        else:
            print("Unknown command. Type `help`.")


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
        """Receive teammate input, or print Unity's local Whisper captions."""
        global state, last_real_update, manual_mode
        if self.path == "/caption":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                caption = json.loads(self.rfile.read(length).decode("utf-8")).get("caption", "").strip()
            except (ValueError, TypeError, json.JSONDecodeError) as error:
                self.send_error(400, str(error))
                return

            if caption:
                print("\nWHISPER CAPTION:", caption)
            body = json.dumps({"ok": True}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

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
            manual_mode = False

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
    print("Terminal mode starts empty: type a command to show sound or ASL data.")
    threading.Thread(target=terminal_input_loop, daemon=True).start()
    ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
