#!/usr/bin/env python3
"""Claude Code and Codex status as two tiles on a Hermes Gadget's dashboard.

Reads only local session metadata (never conversation text):
  Claude Code  ~/.claude/sessions/<pid>.json   one file per running process, with `status`
  Codex        ~/.codex/sessions/**/rollout-*.jsonl   the last task event of recent sessions
and sets the tiles `claude` and `codex` with `hermes gadget tile set` when one changed, and once a
minute otherwise. The tiles expire after two minutes, so they vanish if this stops running.

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
TILE_TTL_S = 120            # a tile nobody refreshes goes away: no stale "working" when this stops
REFRESH_S = 60              # an unchanged tile is set again this often, well inside its TTL


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except (OSError, ValueError, TypeError):
        return False


# What a session is blocked on, as the gadget says it after "Claude": the headline is
# "Claude has a question". An open menu (/hooks, /permissions, a picker) is the user's own
# doing, not Claude asking for anything, so it does not count as waiting.
ASKS = {"input needed": "has a question", "sandbox request": "wants approval",
        "goal proposal": "has a proposal"}
NOT_WAITING = ("dialog open",)
APPS = {"cli": "Terminal", "claude-desktop": "Claude app"}
NOTE_CHARS = 26
NAME_CHARS = 26     # what the gadget takes for a row's name; it cuts it to fit beside "working"
SESSION_ROWS = 2    # the gadget holds eight tiles at most: these, claude, its note, codex, music and its position
BUSY_HOLD_S = 45


def asks(session):
    what = str(session.get("waitingFor") or "").lower()
    if what in ASKS:
        return ASKS[what]
    return "wants approval" if any(w in what for w in ("permission", "approv", "tool")) else "needs you"


def where(session, now):
    """Where a waiting session is, in a few words: the app, the folder and for how long."""
    app = APPS.get(str(session.get("entrypoint") or ""), "")
    cwd = os.path.normpath(str(session.get("cwd") or ""))
    folder = "home folder" if cwd == os.path.normpath(HOME) else os.path.basename(cwd)
    try:
        minutes = int((now * 1000 - float(session.get("statusUpdatedAt") or 0)) / 60000)
    except (TypeError, ValueError):
        minutes = 0
    age = "%d min" % minutes if 2 <= minutes < 600 else ""
    # The gadget's line holds about 26 characters. The folder says which session it is, so the
    # age goes first when it is too long, then the app, and only then is the folder cut.
    for parts in ((app, folder, age), (app, folder), (folder, age), (folder,)):
        text = ", ".join(p for p in parts if p)
        if len(text) <= NOTE_CHARS:
            return text
    return folder[:NOTE_CHARS]


def desktop_titles():
    """{session id: title} for the threads of the Claude desktop app, which names them.

    Only the title the app shows in its own list is read, never a conversation. A thread nobody
    named keeps the app's stock title, which says nothing, so it is left out.
    """
    titles = {}
    pattern = os.path.join(HOME, "Library", "Application Support", "Claude", "claude-code-sessions", "*", "*", "local_*.json")
    for path in glob.glob(pattern):
        try:
            with open(path, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(d, dict):
            continue
        title = " ".join(str(d.get("title") or "").split())
        if d.get("cliSessionId") and title and not title.lower().startswith("local session"):
            titles[str(d["cliSessionId"])] = title
    return titles


def display_name(session, titles):
    """A session's row on the gadget: the thread's title, or the app and folder where it has none."""
    title = titles.get(str(session.get("sessionId") or ""))
    if title:
        return title[:NAME_CHARS].rstrip(" -_.,:")
    app = APPS.get(str(session.get("entrypoint") or ""), "Claude")
    cwd = os.path.normpath(str(session.get("cwd") or ""))
    folder = "home" if cwd == os.path.normpath(HOME) else os.path.basename(cwd)
    return ("%s: %s" % (app, folder))[:NAME_CHARS].rstrip(" -_.,:")


def claude_tiles(now=None):
    """The tiles for the running Claude Code sessions, as {id: (label, value, state)}.

    `claude` sums them up. Each session that is working or blocked gets a row of its own under
    its name (`claude.1` ...), so the gadget says which ones. `claude.note` says where the one
    that waits on the user is; the gadget shows it under "Claude has a question" and does not
    list it. Only sessions on this computer are seen: one in the cloud leaves no file here.
    """
    now = time.time() if now is None else now
    seen = 0
    waiting_sessions, busy_sessions = [], []
    for path in glob.glob(os.path.join(HOME, ".claude", "sessions", "*.json")):
        try:
            with open(path, encoding="utf-8") as f:
                s = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(s, dict) or not alive(s.get("pid")):
            continue  # a file left by a session that has gone
        seen += 1
        status = str(s.get("status") or "idle").lower()
        try:
            since = now - float(s.get("statusUpdatedAt") or 0) / 1000.0
        except (TypeError, ValueError):
            since = BUSY_HOLD_S
        if any(w in status for w in ("wait", "permission", "approval", "input")):
            if str(s.get("waitingFor") or "").lower() not in NOT_WAITING:
                waiting_sessions.append(s)
        elif status in ("busy", "working", "running") or (status == "idle" and 0 <= since < BUSY_HOLD_S):
            # A session reads idle for a moment between steps: it counts as working a little longer.
            busy_sessions.append(s)
    if not seen:
        return {"claude": ("Claude", "off", None)}
    tiles = {}
    titles = desktop_titles() if waiting_sessions or busy_sessions else {}
    waiting, busy = len(waiting_sessions), len(busy_sessions)
    # The longest wait first: that is the one to go to.
    waiting_sessions.sort(key=lambda s: s.get("statusUpdatedAt") or 0)
    busy_sessions.sort(key=lambda s: str(s.get("sessionId") or s.get("pid") or ""))  # a steady order
    if waiting:
        head = asks(waiting_sessions[0]) if waiting == 1 else "has %d waiting" % waiting
        tiles["claude"] = ("Claude", head, "warn")
        first = waiting_sessions[0]
        named = titles.get(str(first.get("sessionId") or ""))
        tiles["claude.note"] = ("Claude", display_name(first, titles) if named else where(first, now), None)
    elif busy:
        tiles["claude"] = ("Claude", "working" if busy == 1 else "%d working" % busy, "ok")
    else:
        tiles["claude"] = ("Claude", "idle", None)
    # Under the `claude` tile the gadget lists these as plain lines: which sessions it counted.
    rows = [(s, asks(s), "warn") for s in waiting_sessions] + [(s, "working", "ok") for s in busy_sessions]
    # Sessions nobody named, in one folder, read alike: they share a line that counts them.
    lines = []
    for s, value, state in rows:
        name = display_name(s, titles)
        for line in lines:
            if line[0] == name and line[2] == state:
                line[3] += 1
                break
        else:
            lines.append([name, value, state, 1])
    for n, (name, value, state, count) in enumerate(lines[:SESSION_ROWS], 1):
        if count > 1:
            name = "%s (%d)" % (name[:NAME_CHARS - 4].rstrip(" -_.,:"), count)
        tiles["claude.%d" % n] = (name, value, state)
    return tiles


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
        return "done", "ok"  # finished a moment ago; not a warning, nothing waits on the user
    return ("idle" if seen else "off"), None


MUSIC_RETRY_S = 300  # after Spotify did not answer, it is left alone this long


def music_tile(last, now):
    """((title, artist, None), "seconds in/seconds long") for what Spotify plays on this Mac, or None.

    Spotify is asked only while it is already running, so this never starts it. The first time,
    macOS asks whether this script may control Spotify; until that is answered the call hangs,
    so after a failure Spotify is left alone for a few minutes (`last` remembers until when).
    """
    if sys.platform != "darwin" or not shutil.which("osascript"):
        return None
    try:
        if now < float(last.get("_music_retry_at") or 0):
            return None
    except (TypeError, ValueError):
        pass
    why = ""
    try:
        if subprocess.run(["pgrep", "-xq", "Spotify"], timeout=5).returncode != 0:
            return None
        script = ('tell application "Spotify" to if player state is playing then '
                  'return (name of current track) & "\n" & (artist of current track) & "\n" & '
                  '((player position) as integer) & "\n" & (((duration of current track) / 1000) as integer)')
        r = subprocess.run(["osascript", "-e", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        if r.returncode != 0:  # most often -1743: this script may not control Spotify
            why = r.stderr.decode("utf-8", "replace").strip()[:200] or "osascript failed"
    except subprocess.TimeoutExpired:
        why = "no answer; macOS may be waiting for you to allow control of Spotify"
    except (OSError, subprocess.SubprocessError) as exc:
        why = str(exc)
    if why:
        sys.stderr.write("ai_status: spotify: %s (next try in %d s)\n" % (why, MUSIC_RETRY_S))
        last["_music_retry_at"] = now + MUSIC_RETRY_S
        return None
    parts = (r.stdout.decode("utf-8", "replace").strip().split("\n") + ["", "", ""])[:4]
    title, artist = " ".join(parts[0].split()), " ".join(parts[1].split())
    if not title:
        return None
    try:
        where = "%d/%d" % (int(parts[2]), int(parts[3]))
    except ValueError:
        where = ""
    return (title[:NAME_CHARS].rstrip(), artist[:NAME_CHARS].rstrip() or "Spotify", None), where


EVENTS = {"music.toggle": "playpause", "music.next": "next track", "music.previous": "previous track"}


def music_event(name):
    """A button on the gadget's now playing screen (the plugin's `event_hook` runs this)."""
    command = EVENTS.get(name)
    if not command or sys.platform != "darwin":
        return 0
    try:
        if subprocess.run(["pgrep", "-xq", "Spotify"], timeout=5).returncode == 0:
            subprocess.run(["osascript", "-e", 'tell application "Spotify" to ' + command], timeout=8,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        return 1
    return 0


def set_tile(hermes, name, label, value, state):
    cmd = [hermes, "gadget", "tile", "set", name, value, "--label", label, "--ttl", str(TILE_TTL_S)]
    if state:
        cmd += ["--state", state]
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="print the tiles, set nothing")
    ap.add_argument("--hermes", default=shutil.which("hermes") or os.path.join(HOME, ".local", "bin", "hermes"))
    ap.add_argument("--event", nargs="+", metavar="NAME", help="act on a gadget event (music.toggle, music.next, music.previous)")
    args = ap.parse_args()
    if args.event:
        return music_event(args.event[0])

    now = time.time()
    tiles = claude_tiles(now)
    tiles["codex"] = ("ChatGPT",) + codex_tile(now)
    if args.dry_run:
        for name, (label, value, state) in tiles.items():
            print("%-11s %-26s %-26s %s" % (name, label, value, state or "-"))
        return 0

    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            last = json.load(f)
    except (OSError, ValueError):
        last = {}
    retry_was = last.get("_music_retry_at")
    song = music_tile(last, now)
    if song:
        tiles["music"] = song[0]
        if song[1]:
            tiles["music.pos"] = ("Music", song[1], None)
    changed = last.get("_music_retry_at") != retry_was
    for name in [n for n in last if n not in tiles and not n.startswith("_")]:
        # A session that stopped working or waiting: its row goes now, not when it times out.
        r = subprocess.run([args.hermes, "gadget", "tile", "clear", name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=60)
        if r.returncode != 0:
            sys.stderr.write("ai_status: clear %s: %s\n" % (name, r.stderr.decode("utf-8", "replace").strip()))
        del last[name]  # gone either way within its time to live
        changed = True
    for name, (label, value, state) in tiles.items():
        was = last.get(name)
        if isinstance(was, list) and was[:2] == [value, state] and len(was) > 3 and was[3] == label and 0 <= now - was[2] < REFRESH_S:
            continue  # unchanged and set recently
        r = set_tile(args.hermes, name, label, value, state)
        if r.returncode != 0:
            sys.stderr.write("ai_status: %s: %s\n" % (name, r.stderr.decode("utf-8", "replace").strip()))
            continue  # try again next run
        last[name] = [value, state, now, label]
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
