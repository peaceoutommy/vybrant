#!/usr/bin/env python3
"""UI behaviour tests driven through the control socket (see src/ui/ControlServer.cpp).

Starts an offscreen instance with a private config directory and socket, so it never touches a running
MusicThingy or the user's settings. Run from the repository root after building:

    python3 tests/ui_test.py [path/to/musicthingy]
"""
import os, socket, subprocess, sys, tempfile, time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "bin", "musicthingy")
fails = 0


def check(cond, msg, detail=""):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg + (f"   [{detail}]" if detail and not cond else ""))
    if not cond:
        fails += 1


class Instance:
    def __init__(self, cfg, sock):
        self.sock = sock
        env = dict(os.environ, XDG_CONFIG_HOME=cfg, QT_QPA_PLATFORM="offscreen", MT_CONTROL_SOCKET=sock)
        self.proc = subprocess.Popen([BIN, "--demo"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            time.sleep(0.2)
            if self.send("ping") == "pong":
                return
        raise RuntimeError("instance did not start")

    def send(self, cmd, timeout=5.0):
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.settimeout(timeout)
                s.connect(self.sock)
                s.sendall((cmd + "\n").encode())
                data = b""
                while not data.endswith(b"\n"):
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                return data.decode().strip()
        except (OSError, socket.timeout):
            return ""

    def quit(self):
        self.send("quit")
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def kick_row(ins):
    """patdata field for the first channel (the demo's Kick)"""
    return ins.send("patdata").split(" | ")[1]


def main():
    tmp = tempfile.mkdtemp(prefix="mt-uitest-")
    cfg, sock = os.path.join(tmp, "cfg"), os.path.join(tmp, "s.sock")  # short socket path: AF_UNIX limit
    os.makedirs(cfg)
    print("MusicThingy UI test")

    # ---- rack ⇄ piano roll: step-shaped notes stay steps, anything else becomes piano-roll notes
    ins = Instance(cfg, sock)
    ins.send("action view.pianoRoll")
    ins.send("select 0")
    ins.send("prwheel -4")   # scroll so C5 (MIDI 60, the kick's root) is in view
    check(kick_row(ins).startswith("ch") and "steps=x...x...x...x... notes=" in kick_row(ins), "demo kick starts as 4 steps", kick_row(ins))
    ins.send("prclick 240 60")
    check("steps=xx..x...x...x... notes=" in kick_row(ins), "piano-roll click on the root note adds a step", kick_row(ins))
    ins.send("prclick 480 62")
    check("(62@480+240)" in kick_row(ins) and "steps=................" in kick_row(ins), "note off the root note turns the channel into piano-roll notes", kick_row(ins))
    ins.send("prclick 480 62 right")
    check("steps=xx..x...x...x... notes=" in kick_row(ins), "erasing it converts back to steps", kick_row(ins))
    ins.send("prdrag 0 60 0 62")
    check("(62@0+240)" in kick_row(ins), "moving a step note up a tone works in step mode", kick_row(ins))
    ins.send("prdrag 0 62 0 60")
    check("steps=xx..x...x...x... notes=" in kick_row(ins), "moving it back restores steps", kick_row(ins))
    ins.send("prdrag 1200 60 1440 60 -6")
    check("(60@960+480)" in kick_row(ins), "resizing a step note by its right edge works in step mode", kick_row(ins))
    clap = lambda: ins.send("patdata").split(" | ")[2]
    ins.send("rackclick 1 0")
    check("steps=x...x.......x..." in clap(), "rack click sets a step", clap())
    ins.send("rackclick 1 0")
    check("steps=....x.......x..." in clap(), "second rack click clears it", clap())

    # ---- piano roll vertical range: never scrolls past the lowest key, at any zoom
    ins.send("prwheel -15 alt")
    info = ins.send("prwheel -40")
    top, mn = [int(x.split("=")[1]) for x in info.split()[:2]]
    check(top == mn and mn > 0, "fully zoomed out, scrolling down stops at the last row", info)
    ins.send("prwheel 15 alt")
    info = ins.send("prwheel -40")
    top, mn = [int(x.split("=")[1]) for x in info.split()[:2]]
    check(top == mn, "zoomed in, scrolling down stops at the last row", info)

    # ---- rack docking: single separator (rack lives in the split top area), position survives a restart
    ins.send("dock bottom")
    time.sleep(1.5)
    check(ins.send("layout").startswith("rack=bottom visible=1"), "rack docks at the bottom", ins.send("layout"))
    time.sleep(1.5)   # debounced layout save
    ins.quit()
    ins = Instance(cfg, sock)
    check(ins.send("layout").startswith("rack=bottom visible=1"), "bottom dock is restored after a restart", ins.send("layout"))
    ins.quit()

    cfg2 = os.path.join(tmp, "cfg2")
    os.makedirs(cfg2)
    ins = Instance(cfg2, sock)
    ins.send("dock top")
    time.sleep(3)
    check(ins.send("layout").startswith("rack=top visible=1"), "rack docks at the top", ins.send("layout"))
    ins.quit()
    ins = Instance(cfg2, sock)
    check(ins.send("layout").startswith("rack=top visible=1"), "top dock is restored after a restart", ins.send("layout"))
    ins.quit()

    # ---- piano roll selection, velocity lane modifiers, trackpad zoom
    cfg3 = os.path.join(tmp, "cfg3")
    os.makedirs(cfg3)
    ins = Instance(cfg3, sock)
    ins.send("action view.pianoRoll")
    ins.send("select 2")
    ins.send("prwheel -4")
    xs = [int(ins.send(f"prclick {t} {p}").split()[1].split(",")[0]) - 3 for t, p in [(0, 62), (480, 62), (960, 64)]]
    info = dict(kv.split("=", 1) for kv in ins.send("prinfo").split())
    check(info["sel"] == "0", "a drawn note is not left selected", info["sel"])
    tx = lambda tick: int(xs[0] + tick * float(info["ppt"]))  # noqa: E731  grid x of a tick
    ry, vt, vb = int(info["rulerY"]), int(info["velY"].split("..")[0]), int(info["velY"].split("..")[1])
    sel = ins.send(f"prmouse {tx(0) + 1} {ry} {tx(600)} {ry} c").split()
    check("sel=3" in sel, "Ctrl+drag on the ruler selects the notes starting inside the time selection", " ".join(sel))
    sel = ins.send(f"prmouse {tx(0) + 1} {ry} {tx(0) + 1} {ry} c").split()
    check("sel=0" in sel, "an empty time selection clears the note selection", " ".join(sel))
    vel = lambda: ins.send("patdata").split(" | ")[1].split(" vel=")[1]  # noqa: E731
    before = vel()
    ins.send(f"prmouse {tx(0) + 1} {vt + 25} {tx(3000)} {vb - 10} s")
    after = vel().split(",")
    check(len(set(after)) == 1 and after[0] != "80", "Shift+drag in the velocity lane gives every swept note the first value", vel())
    ins.send(f"prmouse {tx(0) + 1} {vt + 25} {tx(3000)} {vb - 10} a")
    check(vel() == before, "Alt+drag in the velocity lane restores the original velocities", f"{before} -> {vel()}")
    ins.send(f"prmouse {tx(0) + 1} {vt + 25} {tx(3000)} {vb - 10}")
    check(len(set(vel().split(","))) > 1, "a plain velocity drag still follows the cursor", vel())
    ppt0 = float(info["ppt"])
    ppt1 = float(dict(kv.split("=", 1) for kv in ins.send("prpinch 0.5").split())["ppt"])
    check(abs(ppt1 - ppt0 * 1.5) < 1e-6, "trackpad pinch zooms the piano roll", f"{ppt0} -> {ppt1}")
    ins.quit()

    # ---- playlist: a clicked / resized pattern clip is the brush the draw tool places (pattern and size)
    cfg4 = os.path.join(tmp, "cfg4")
    os.makedirs(cfg4)
    ins = Instance(cfg4, sock)
    ins.send("action view.playlist")
    ins.send("pldrag 0 3840 0 7680 -5")   # stretch the demo's 1-bar clip to 2 bars by its right edge
    check(ins.send("clips") == "0:0+7680", "playlist clip resizes by its right edge", ins.send("clips"))
    ins.send("pldrag 1 0 1 0")
    check(ins.send("clips").split()[-1] == "1:0+7680", "a resized clip sets the size of the next placed clip", ins.send("clips"))
    ins.send("pldrag 0 960 0 960")
    ins.send("pldrag 2 7680 2 7680")
    check(ins.send("clips").split()[-1] == "2:7680+7680", "clicking a clip keeps its size as the brush", ins.send("clips"))
    b, z = map(float, ins.send("plpinch 0.5").split())
    check(abs(z - b * 1.5) < 1e-6, "trackpad pinch zooms the playlist", f"{b} -> {z}")
    ins.quit()

    print("\n%s" % ("All UI checks passed" if fails == 0 else f"{fails} UI check(s) FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
