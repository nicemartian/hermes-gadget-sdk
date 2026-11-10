#!/usr/bin/env python3
"""Show a card on a Hermes gadget from any script on the gateway's computer.

    gadget_notify.py "Build finished" "eyes-skin built in 3m 12s" --seconds 30

It writes a request into the gadget plugin's desk inbox (the same file handoff the Hermes
Desktop page uses) and waits for the gateway to say whether the card was shown. Standard
library only. The device comes from --device, or the GADGET_DEVICE environment variable.
"""
import argparse, json, os, re, sys, tempfile, time, uuid
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Show a card on a Hermes gadget")
    p.add_argument("title")
    p.add_argument("text", nargs="?", default="")
    p.add_argument("--seconds", type=int, default=20, help="how long the card stays (0: until dismissed)")
    p.add_argument("--device", default=os.environ.get("GADGET_DEVICE", ""), help="device id (hg-...)")
    p.add_argument("--home", default=os.environ.get("HERMES_HOME", str(Path.home() / ".hermes")))
    p.add_argument("--no-wait", action="store_true", help="queue the card and return at once")
    a = p.parse_args()
    if not re.fullmatch(r"hg-[0-9a-f]{16}", a.device):
        print("set --device or GADGET_DEVICE to the gadget's id (hermes gadget devices)", file=sys.stderr)
        return 2
    desk = Path(a.home) / "plugin-data" / "gadget" / "desk"
    inbox, done = desk / "inbox", desk / "done"
    inbox.mkdir(parents=True, exist_ok=True)
    req_id = uuid.uuid4().hex
    req = {"device": a.device, "kind": "card", "at": time.time(), "title": a.title.strip()[:64],
           "text": a.text.strip()[:600], "seconds": max(0, min(a.seconds, 3600))}
    fd, tmp = tempfile.mkstemp(dir=inbox, prefix=".req.", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(req, f)
    os.replace(tmp, inbox / f"{req_id}.json")
    if a.no_wait:
        return 0
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            out = json.loads((done / f"{req_id}.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            time.sleep(0.15)
            continue
        if out.get("ok"):
            return 0
        print(out.get("error") or "the gadget did not show it", file=sys.stderr)
        return 1
    (inbox / f"{req_id}.json").unlink(missing_ok=True)
    print("the gateway did not answer; is it running?", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
