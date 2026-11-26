#!/usr/bin/env python3
"""LR-160: headless check of the multiplayer server (no window, no browser).

Starts lr_game_2d --no-browser, plays it with several clients over HTTP (the
way the page and the native window talk to it) and checks by numbers:
  - each client token is its own player; the 9th of 8 is turned away (503);
  - pause and speed are shared: one sets them, every client sees them, with
    an event naming who did it;
  - a menu's pause holds the world only for a player alone;
  - one dynasty per player: the other's person / kin are refused;
  - a player away hands their person to free will, and gets them back;
  - the world steps with nobody connected;
  - a remote client's Quit only leaves, the server goes on;
  - lr_game_2d --probe (the native client's HTTP path) joins and reads the state.

  python3 tools/game_2d/mp_check.py [path/to/lr_game_2d] [port]
Exit code 0 when every check passed.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-sim-linux", "lr_game_2d")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18766
BASE = f"http://127.0.0.1:{PORT}"
fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


class Client:
    def __init__(self, token, name, kind="web"):
        self.h = {"X-LR-Player": token, "X-LR-Name": urllib.request.quote(name), "X-LR-Client": kind}

    def call(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(BASE + path, data=data, headers=self.h, method="POST" if body is not None else "GET")
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def get(self, path):
        return self.call(path)[1]

    def post(self, path, body):
        return self.call(path, body)[1]

    def state(self):
        return self.get("/api/state")


def main():
    srv = subprocess.Popen([EXE, "--no-browser", "--port", str(PORT), "--seed", "5", "--run-hours", "7", "--release-sec", "4",
                            "--autosave-min", "0", "--no-hot-reload"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=dict(os.environ, LR_SAVE_DIR="/tmp/lr160_saves"))
    log = []
    try:
        t0 = time.time()
        while True:
            line = srv.stdout.readline()
            if line:
                log.append(line.rstrip())
            if "LivelyRealms 2D:" in line:
                break
            if srv.poll() is not None or time.time() - t0 > 300:
                print("\n".join(log[-20:]))
                raise SystemExit("server did not start")
        import threading
        threading.Thread(target=lambda: [log.append(l.rstrip()) for l in srv.stdout], daemon=True).start()

        a, b = Client("tokA", "Анна"), Client("tokB", "Bob")
        ha, hb = a.get("/api/hello"), b.get("/api/hello")
        check(ha["game"] == "LivelyRealms" and ha["proto"] == 1, "hello names the game and the protocol")
        check(ha["player"] == 1 and hb["player"] == 2, f"two tokens, two players ({ha['player']}, {hb['player']})")
        check(a.get("/api/hello")["player"] == 1, "the same token comes back as the same player")
        sa = a.state()
        me_a = sa["me"]
        check(me_a, "player 1 leads the village head from the start")
        names = sorted(p["name"] for p in sa["net"]["players"])
        check(names == ["Bob", "Анна"], f"the players list has both names {names}")

        # the world runs with nobody polling
        t1 = a.state()["tick"]
        time.sleep(2.5)
        check(a.state()["tick"] > t1, "the world steps on its own")

        # shared pause and speed
        check(a.post("/api/cmd", {"op": "pause", "on": True})["result"] == "ok", "Анна pauses")
        sb = b.state()
        check(sb["paused"] is True, "Bob sees the pause")
        ev = [e for e in sb["net"]["events"] if e["what"] == "pause"]
        check(ev and ev[-1]["by"] == 1 and ev[-1]["name"] == "Анна", "the pause event names who did it")
        b.post("/api/cmd", {"op": "speed", "tps": 12})
        check(a.state()["tps"] == 12, "Анна sees Bob's speed")
        b.post("/api/cmd", {"op": "pause", "on": False})
        check(a.state()["paused"] is False, "Bob's resume reaches Анна")
        # a menu's pause with two in: the world goes on
        a.post("/api/cmd", {"op": "pause", "on": True, "menu": True})
        check(b.state()["paused"] is False, "a menu does not pause a game of two")

        # one dynasty each
        menu = b.get("/api/menu")
        check(menu["players_online"] == 2 and not menu["me"], "Bob's menu: two in, he leads nobody yet")
        r = b.post("/api/begin", {"id": me_a})
        check(r["result"].startswith("rejected:controlled_by_other"), f"Bob cannot take Анна's person ({r['result']})")
        fams = b.get("/api/dynasties")["families"]
        mine = next(f for f in fams if any(p["id"] == me_a for p in f["people"]))
        kin = [p["id"] for p in mine["people"] if p["adult"] and p["id"] != me_a]
        if kin:
            r = b.post("/api/begin", {"id": kin[0]})
            check(r["result"].startswith("rejected:") and "dynasty" in r["result"], f"nor her kin ({r['result']})")
        other = next(p["id"] for f in fams if f is not mine and f["full"] for p in f["people"] if p["adult"])
        check(b.post("/api/begin", {"id": other})["result"] == "ok", "Bob begins as a person of another family")
        sa = a.state()
        check(b.state()["me"] == other and sa["me"] == me_a, "each leads their own")
        pb = next(p for p in sa["people"] if p["id"] == other)
        check(pb["pl"] and pb["pid"] == 2, "Анна sees Bob's person as player 2's")
        # orders go only as oneself
        r = a.post("/api/cmd", {"op": "cmd", "type": "steer", "args": {"id": other, "x": 1, "y": 1, "player": 2}})
        check(r["result"].startswith("rejected"), "Анна cannot steer Bob's person (even naming player 2)")

        # Bob away: his person lives on their own, then he is back
        deadline = time.time() + 12
        while time.time() < deadline:
            a.state()
            if not next(p for p in a.state()["people"] if p["id"] == other)["pl"]:
                break
            time.sleep(0.5)
        check(not next(p for p in a.state()["people"] if p["id"] == other)["pl"], "Bob away: his person goes on alone")
        b.state()
        time.sleep(1.6)
        check(b.state()["me"] == other, "Bob back: the same person again")

        # the native client's path
        p = subprocess.run([EXE, "--probe", f"127.0.0.1:{PORT},Пробник"], capture_output=True, text=True, timeout=60)
        last = (p.stdout.strip().splitlines() or ["{}"])[-1]
        pj = json.loads(last)
        check(p.returncode == 0 and pj.get("ok") and pj.get("player", 0) > 0 and len(pj.get("players", [])) >= 2,
              f"--probe joins as a client: {last[:160]}")

        # full game: 8 players at most
        extra = [Client(f"tokX{i}", f"X{i}") for i in range(6)]
        codes = [c.call("/api/hello")[0] for c in extra]
        check(all(c == 200 for c in codes), "players 3..8 come in")
        code, body = Client("tokZ", "Z").call("/api/hello")
        check(code == 503 and body.get("result") == "rejected:full", f"the 9th is turned away ({code})")

        # the page and its network script are served
        with urllib.request.urlopen(BASE + "/") as r:
            page = r.read().decode()
        check("/web/net.js" in page, "the page loads net.js")
        n = a.get("/api/net")
        check(n["port"] == PORT and isinstance(n["addrs"], list), f"/api/net gives the port and the addresses {n['addrs']}")

        # a client that is not the host only leaves
        r = b.post("/api/quit", {"autosave": False})
        check(r.get("left") is True, "Bob's Quit only takes him out")
        time.sleep(0.5)
        check(srv.poll() is None and a.state()["tick"] > 0, "the server goes on")
    finally:
        srv.terminate()
        try:
            srv.wait(10)
        except subprocess.TimeoutExpired:
            srv.kill()
    joined = [l for l in log if "joined" in l or "away" in l or "back:" in l]
    print("server log:", *joined[:12], sep="\n  ")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
