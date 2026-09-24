"""HearSay Ultra96 sender for sound direction and ASL results."""
import argparse
import json
from urllib.request import Request, urlopen

WORDS = (
    "hello", "see_you_later", "i_me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again_repeat", "eat_food", "more", "go_to", "bathroom",
    "fine", "like", "learn", "sign", "finish_done",
)

def build_payload(direction=None, word=None, confidence=0.0):
    """Build one message from the latest sound direction and recognised sign."""
    if direction not in (None, "left", "right"):
        raise ValueError("direction must be left, right or None")
    normalized = word.lower().strip().replace("/", "_").replace(" ", "_") if word else None
    if normalized is not None and normalized not in WORDS:
        raise ValueError("Unknown ASL word: " + normalized)
    if not 0 <= confidence <= 1:
        raise ValueError("confidence must be 0..1")
    return {
        "soundActive": direction is not None,
        "soundDirection": direction or "left",  # ignored when soundActive is false
        "aslDetected": normalized is not None,
        "aslWord": normalized or "hello",  # ignored when aslDetected is false
        "aslConfidence": confidence if normalized else 0.0,
    }

def send_update(server, payload):
    """Send both results to the laptop server at http://LAPTOP_IP:8080/update."""
    request = Request(server.rstrip("/") + "/update",
                      data=json.dumps(payload).encode("utf-8"),
                      headers={"Content-Type": "application/json"}, method="POST")
    with urlopen(request, timeout=3) as response:
        return json.load(response)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="http://LAPTOP_IP:8080")
    parser.add_argument("--sound", choices=["left", "right", "off"], default="off")
    parser.add_argument("--asl", default=None, help='e.g. "thank you"; omit for no sign')
    parser.add_argument("--confidence", type=float, default=0.95)
    args = parser.parse_args()
    payload = build_payload(None if args.sound == "off" else args.sound, args.asl, args.confidence)
    try:
        print("SENDING:", json.dumps(payload))
        print("REPLY:", send_update(args.server, payload))
    except (OSError, ValueError) as error:
        parser.exit(1, "Send failed: " + str(error) + "\n")

if __name__ == "__main__":
    main()
