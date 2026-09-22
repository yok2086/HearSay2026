"""Template for teammates sending sensor / ASL-model data to the HearSay demo.

1. Set SERVER_URL to the /update URL printed by HearSayDummyServer.py.
2. Replace the example values in build_payload() with real sensor/model output.
3. Call send_update() whenever the values change (for example, 5–10 times per second).
"""

import json
from urllib.request import Request, urlopen


SERVER_URL = "http://192.168.1.2:8080/update"  # Replace with the laptop IP shown by the server.


def build_payload():
    return {
        "soundActive": True,
        "soundDirection": "left",  # left, right, front, or back
        "aslDetected": True,
        "aslWord": "hello",        # Must be a HearSay word-bank value, e.g. "thank_you"
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
