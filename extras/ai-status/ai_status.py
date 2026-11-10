#!/usr/bin/env python3
"""Claude Code and Codex status as two tiles on a Hermes Gadget's dashboard.

Reads only local session metadata (never conversation text):
  Claude Code  ~/.claude/sessions/<pid>.json   one file per running process, with `status`
  Codex        ~/.codex/sessions/**/rollout-*.jsonl   the last task event of recent sessions
and sets the tiles `claude` and `codex` with `hermes gadget tile set`, only when one changed.

Run it every 10-15 seconds (launchd StartInterval), or once by hand:
  python3 ai_status.py --dry-run     print what it would set, change nothing
Python 3.9+, standard library only.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
STATE_FILE = os.path.join(HOME, ".hermes", "plugin-data", "gadget", "ai_status_last.json")
DONE_SHOWN_S = 10 * 60      # "done" stays up this long after a task ends, then "idle"
CODEX_RECENT_S = 24 * 3600  # sessions older than this are not looked at
CODEX_STALE_S = 30 * 60     # a task "started" this long ago with no file activity is not working
TAIL_BYTES = 256 * 1024


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except (OSError, ValueError, TypeError):
        return False


def claude_tile():
    """(value, state): what the running Claude Code sessions are doing."""
    counts = {}
    for path in glob.glob(os.path.join(HOME, ".claude", "sessions", "*.json")):
        try:
            with open(path, encoding="utf-8") as f:
                s = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(s, dict) or not alive(s.get("pid")):
            continue  # a file left by a session that has gone
        status = str(s.get("status") or "idle").lower()
        counts[status] = counts.get(status, 0) + 1
    if not counts:
        return "off", None
    waiting = sum(n for k, n in counts.items() if any(w in k for w in ("wait", "permission", "approval", "input")))
    busy = sum(n for k, n in counts.items() if k in ("busy", "working", "running"))
    if waiting:
        return ("needs you" if waiting == 1 else "%d need you" % waiting), "warn"
    if busy:
        return ("working" if busy == 1 else "%d working" % busy), "ok"
    return "idle", None


def last_task_event(path):
    """The last task_started / task_complete / turn_aborted event in a Codex rollout file."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, size - TAIL_BYTES))
            lines = f.read().splitlines()
    except OSError:
        return None
    for raw in reversed(lines):
        if b'"event_msg"' not in raw:
            continue
        try:
            rec = json.loads(raw)
        except ValueError:
            continue  # the first line of the tail may be cut
        kind = (rec.get("payload") or {}).get("type") if rec.get("type") == "event_msg" else None
        if kind in ("task_started", "task_complete", "turn_aborted"):
            return kind
    return None


def codex_tile(now):
    working = 0
    newest_done = 0.0
    seen = False
    pattern = os.path.join(HOME, ".codex", "sessions", "*", "*", "*", "rollout-*.jsonl")
    for path in glob.glob(pattern):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if now - mtime > CODEX_RECENT_S:
            continue
        seen = True
        kind = last_task_event(path)
        if kind == "task_started" and now - mtime < CODEX_STALE_S:
            working += 1
        elif kind in ("task_complete", "turn_aborted"):
            newest_done = max(newest_done, mtime)
    if working:
        return ("working" if working == 1 else "%d working" % working), "ok"
    if newest_done and now - newest_done < DONE_SHOWN_S:
        return "done", "warn"  # finished a moment ago: worth a look
    return ("idle" if seen else "off"), None


def set_tile(hermes, name, label, value, state):
    cmd = [hermes, "gadget", "tile", "set", name, value, "--label", label]
    if state:
        cmd += ["--state", state]
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="print the tiles, set nothing")
    ap.add_argument("--hermes", default=shutil.which("hermes") or os.path.join(HOME, ".local", "bin", "hermes"))
    args = ap.parse_args()

    now = time.time()
    tiles = {
        "claude": ("Claude",) + claude_tile(),
        "codex": ("ChatGPT Codex",) + codex_tile(now),
    }
    if args.dry_run:
        for name, (label, value, state) in tiles.items():
            print("%-7s %-14s %-12s %s" % (name, label, value, state or "-"))
        return 0

    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            last = json.load(f)
    except (OSError, ValueError):
        last = {}
    changed = False
    for name, (label, value, state) in tiles.items():
        if last.get(name) == [value, state]:
            continue
        r = set_tile(args.hermes, name, label, value, state)
        if r.returncode != 0:
            sys.stderr.write("ai_status: %s: %s\n" % (name, r.stderr.decode("utf-8", "replace").strip()))
            continue  # try again next run
        last[name] = [value, state]
        changed = True
    if changed:
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(last, f)
        os.replace(tmp, STATE_FILE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
