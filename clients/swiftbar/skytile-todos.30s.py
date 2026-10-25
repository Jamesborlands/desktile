#!/usr/bin/env python3
# <xbar.title>SkyTile To-do</xbar.title>
# <xbar.desc>Open to-dos from the SkyTile Worker. Click one to check it off.</xbar.desc>
# <swiftbar.hideAbout>true</swiftbar.hideAbout>
# <swiftbar.hideRunInTerminal>true</swiftbar.hideRunInTerminal>
# <swiftbar.hideLastUpdated>true</swiftbar.hideLastUpdated>
# <swiftbar.hideDisablePlugin>true</swiftbar.hideDisablePlugin>
# <swiftbar.hideSwiftBar>true</swiftbar.hideSwiftBar>
#
# SwiftBar plugin: menu bar shows the open to-do count; the dropdown lists open items
# (click = done), "Add todo...", an "Edit" submenu (click an item to edit or delete it)
# and items checked off today (click = undo; they disappear on their own after 24 hours). The ".30s" in the file name makes SwiftBar refresh
# it every 30 seconds.
#
# Config: ~/.config/skytile/config (same format as the repo's .env):
#   SKYTILE_URL=https://skytile.seobubbles.workers.dev
#   SKYTILE_API_TOKEN=...
# Standard library only, so the macOS system python3 is enough.
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

CONFIG = os.path.expanduser(os.environ.get("SKYTILE_CONFIG", "~/.config/skytile/config"))
SELF = os.path.abspath(sys.argv[0])


def load_config():
    cfg = {}
    try:
        with open(CONFIG) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip().strip('"').strip("'")
    except FileNotFoundError:
        pass
    url, token = cfg.get("SKYTILE_URL", "").rstrip("/"), cfg.get("SKYTILE_API_TOKEN", "")
    if not url or not token:
        raise RuntimeError(f"Missing SKYTILE_URL or SKYTILE_API_TOKEN in {CONFIG}")
    return url, token


def call(method, path, body=None):
    url, token = load_config()
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url + "/api/v1/todos" + path, data=data, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("User-Agent", "SkyTile-SwiftBar/1.0")
    if data:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read() or b"{}")


def dialog(prompt, text, buttons, default):
    # Native macOS input dialog; the text goes in as an argument (no quoting issues).
    # Returns (button, text), or None if cancelled.
    script = f"""on run argv
    set r to display dialog (item 1 of argv) default answer (item 2 of argv) with title "SkyTile" buttons {{{", ".join(f'"{b}"' for b in buttons)}}} default button "{default}" cancel button "Cancel"
    return (button returned of r) & linefeed & (text returned of r)
end run"""
    r = subprocess.run(["osascript", "-e", script, prompt, text], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    button, _, value = r.stdout.rstrip("\n").partition("\n")
    return button, value.strip()


def ask_text():
    r = dialog("Add a to-do:", "", ["Cancel", "Add"], "Add")
    return r[1] if r else None


def edit(todo_id):
    todo = next((t for t in call("GET", "")["todos"] if t["id"] == todo_id), None)
    if not todo:
        return
    r = dialog("Edit to-do:", todo["text"], ["Delete", "Cancel", "Save"], "Save")
    if not r:
        return
    button, text = r
    if button == "Delete":
        call("DELETE", f"/{todo_id}")
    elif text and text != todo["text"]:
        call("PATCH", f"/{todo_id}", {"text": text[:120]})


def notify(msg):
    subprocess.run(["osascript", "-e", f"display notification {json.dumps(msg)} with title \"SkyTile\""])


def clean(text):
    # SwiftBar treats "|" as the start of parameters and leading "-" as submenu markers.
    return text.replace("|", "│").lstrip("-") or "(blank)"


def action(args):
    try:
        if args[0] == "add":
            text = ask_text()
            if text:
                call("POST", "", {"text": text[:120], "source": "mac"})
        elif args[0] in ("complete", "uncomplete"):
            call("POST", f"/{args[1]}/{args[0]}")
        elif args[0] == "edit":
            edit(args[1])
        elif args[0] == "clear":
            call("POST", "/clear-completed")
    except Exception as e:  # show it, don't fail silently
        notify(f"Couldn't save: {e}")


def menu():
    try:
        todos = call("GET", "")["todos"]
    except (urllib.error.URLError, RuntimeError, ValueError, KeyError) as e:
        print("! | sfimage=checklist")
        print("---")
        print(f"SkyTile unreachable | color=red")
        print(clean(str(e))[:200])
        print("Retry | refresh=true")
        return
    open_ = [t for t in todos if not t["done"]]
    done = [t for t in todos if t["done"]]
    print(f"{len(open_)} | sfimage=checklist")
    print("---")
    act = f'bash="{SELF}" terminal=false refresh=true'
    if not open_:
        print("All done | color=gray")
    for t in open_:
        print(f"{clean(t['text'])} | sfimage=circle {act} param1=complete param2={t['id']}")
    print("---")
    print(f"Add todo... | sfimage=plus {act} param1=add shortcut=CMD+OPTION+T")
    if open_:
        print("Edit | sfimage=pencil")
        for t in open_:
            print(f"--{clean(t['text'])} | {act} param1=edit param2={t['id']}")
    if done:
        print("---")
        print("Done today (click to undo) | color=gray size=12")
        for t in done:
            print(f"{clean(t['text'])} | sfimage=checkmark.circle.fill color=gray {act} param1=uncomplete param2={t['id']}")
        print(f"Clear done | sfimage=trash {act} param1=clear")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        action(sys.argv[1:])
    else:
        menu()
