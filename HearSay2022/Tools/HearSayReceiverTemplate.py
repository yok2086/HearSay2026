"""HearSay receiver test: print the latest server data, like the phone Data Monitor."""
import argparse
import json
import time
from urllib.request import urlopen

def receive_status(server):
    """Read the latest sound and ASL values from /status."""
    with urlopen(server.rstrip("/") + "/status", timeout=3) as response:
        return json.load(response)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="http://LAPTOP_IP:8080")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    try:
        while True:
            try:
                print(time.strftime("%H:%M:%S"), json.dumps(receive_status(args.server)), flush=True)
            except (OSError, ValueError) as error:
                print("RECEIVE ERROR:", error, flush=True)
                if args.once:
                    raise SystemExit(1)
            if args.once:
                return
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nReceiver stopped.")

if __name__ == "__main__":
    main()
