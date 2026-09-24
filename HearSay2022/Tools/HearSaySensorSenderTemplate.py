"""HearSay sensor and ASL sender.

Uses test values for the sound direction and recognised sign.
Both values are sent together to the laptop server at /update.
"""

import json
from urllib.request import Request, urlopen


SERVER_URL = "http://192.168.1.2:8080/update"  # Laptop server address; depends on the Wi-Fi network.


def build_payload():
    return {
        "soundActive": True,
        "soundDirection": "left",  # left or right; soundActive=False means off
        "aslDetected": True,
        "aslWord": "hello",        # Word-bank label, e.g. "thank_you"
        "aslConfidence": 0.91,       # 0.0 to 1.0
    }


def send_update(payload):
    request = Request(
        SERVER_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request, timeout=2) as response:
        return json.loads(response.read().decode("utf-8"))


if __name__ == "__main__":
    print(send_update(build_payload()))
