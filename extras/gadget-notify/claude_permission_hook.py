#!/usr/bin/env python3
"""Claude Code PermissionRequest hook: answer a permission request from a Hermes gadget.

Register it in ~/.claude/settings.json:

    "PermissionRequest": [{"matcher": "*", "hooks": [{"type": "command",
        "command": "/usr/bin/python3 ~/.local/share/waveform/claude_permission_hook.py", "timeout": 60}]}]

The gadget shows the tool and exactly what it would do, with Yes and No. Yes allows this one
call, No denies it. No answer in time, a gadget that is offline, or a request too long to show
in full prints nothing, so Claude Code's own prompt carries on as if this hook were not there:
the gadget never approves what it could not show.

Environment: GADGET_DEVICE (required, hg-...), GADGET_ASK_SECONDS (default 30).
"""
import json, os, subprocess, sys
from pathlib import Path

MAX_SHOWN = 220  # what the gadget's question shows without cutting; longer requests stay on the computer


def describe(tool: str, args: dict):
    """One line that says all of what the call does, or None when it cannot be said in full."""
    if tool == "Bash":
        text = str(args.get("command") or "")
    elif tool in ("Edit", "Write", "NotebookEdit", "Read"):
        text = str(args.get("file_path") or args.get("notebook_path") or "")
        if tool != "Read":
            return None if not text else f"{tool} {text}"  # the path is shown; the change itself is not
    elif tool in ("WebFetch", "WebSearch"):
        text = str(args.get("url") or args.get("query") or "")
    else:
        text = json.dumps(args, ensure_ascii=False, sort_keys=True)
    text = " ".join(text.split())
    if not text or len(text) > MAX_SHOWN:
        return None
    return text


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except ValueError:
        return 0
    tool = str(event.get("tool_name") or "")
    args = event.get("tool_input") if isinstance(event.get("tool_input"), dict) else {}
    shown = describe(tool, args)
    if not tool or shown is None or not os.environ.get("GADGET_DEVICE"):
        return 0
    project = Path(str(event.get("cwd") or "")).name
    title = f"Claude: allow {tool}?"[:64]
    text = f"{shown}\n({project})" if project else shown
    seconds = os.environ.get("GADGET_ASK_SECONDS", "30")
    notify = Path(__file__).with_name("gadget_notify.py")
    try:
        run = subprocess.run([sys.executable, str(notify), title, text, "--ask", "--seconds", seconds],
                             capture_output=True, text=True, timeout=int(seconds) + 20)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return 0
    answer = run.stdout.strip()
    if run.returncode not in (0, 1) or answer not in ("yes", "no"):
        return 0  # nobody answered: the computer's own prompt decides
    decision = {"behavior": "allow"} if answer == "yes" else {"behavior": "deny", "message": "Denied from the desk gadget."}
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
