#!/usr/bin/env python3
"""UI behaviour tests driven through the control socket (see src/ui/ControlServer.cpp).

Starts an offscreen instance with a private config directory and socket, so it never touches a running
Vybrant or the user's settings. Run from the repository root after building:

    python3 tests/ui_test.py [path/to/vybrant]
"""
import os, socket, subprocess, sys, tempfile, time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "bin", "vybrant")
fails = 0


def check(cond, msg, detail=""):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg + (f"   [{detail}]" if detail and not cond else ""))
    if not cond:
        fails += 1


class Instance:
    def __init__(self, cfg, sock):
        self.sock = sock
        env = dict(os.environ, XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=os.path.join(cfg, "data"), QT_QPA_PLATFORM="offscreen", MT_CONTROL_SOCKET=sock)
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
    print("Vybrant UI test")

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

    # ---- the window fits a small screen: no hint text or toolbar may force it wider (the rack's Steps box went off screen)
    cfg5 = os.path.join(tmp, "cfg5")
    os.makedirs(cfg5)
    ins = Instance(cfg5, sock)
    ins.send("patsteps 128")
    r = ins.send("winsize 1100 800")
    check(r.startswith("ok 1100x800"), "the main window shrinks to 1100 px wide with a 128-step pattern", r)
    ins.quit()

    # ---- the browser and the project strip show and hide on their own: hiding the browser leaves the strip, alone at its own width
    cfg15 = os.path.join(tmp, "cfg15")
    os.makedirs(cfg15)
    ins = Instance(cfg15, sock)
    ins.send("action view.browser")
    time.sleep(0.5)
    r = ins.send("layout") + " | " + ins.send("leftsizes")
    check(r.endswith("stripVisible=1 browserVisible=0 | dock=180 browser=260 strip=180 panel=180"),
          "hiding the browser keeps the project strip, which then fills the dock on its own", r)
    ins.quit()
    ins = Instance(cfg15, sock)
    r = ins.send("layout")
    check(r.endswith("stripVisible=1 browserVisible=0"), "the hidden browser stays hidden after a restart", r)
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
    ins.send("key pr Ctrl+A")
    v0 = [int(x) for x in vel().split(",")]
    ins.send("prwheel 3 alt")   # three notches up on the selection: +6% each, not a zoom
    v1 = [int(x) for x in vel().split(",")]
    ins.send("action edit.undo")
    check(all(abs(b - a - min(6, 100 - a)) <= 1 for a, b in zip(v0, v1)) and [int(x) for x in vel().split(",")] == v0,
          "Alt+wheel on selected notes raises their velocity (2% a notch), and one undo takes the whole run back", f"{v0} -> {v1}")
    ins.send("key pr Escape")
    # the Release lane edits stick (its edits used to be compared away and undone on release)
    ins.send("prlane 2")
    rel0 = ins.send("prrel")
    ins.send(f"prmouse {tx(0) + 1} {vt + 25} {tx(3000)} {vb - 10}")
    rel1 = ins.send("prrel")
    ins.send("prlane 0")
    check(rel1 != rel0 and not rel1.startswith("err"), "dragging in the Release lane changes release velocity", f"{rel0} -> {rel1}")
    ppt0 = float(info["ppt"])
    ppt1 = float(dict(kv.split("=", 1) for kv in ins.send("prpinch 0.5").split())["ppt"])
    check(abs(ppt1 - ppt0 * 1.5) < 1e-6, "trackpad pinch zooms the piano roll", f"{ppt0} -> {ppt1}")
    # dragging a just-placed note moves it and never changes its length (it used to flip between 1 and 2 grid units)
    lens = []
    for i, (off, cells) in enumerate([(2, 3), (15, 2), (9, 4), (16, 1), (1, 5)]):
        t0, pitch = 3840, 65 + i   # same start, own row: stays in view and never lands on an earlier note
        before = set(ins.send("patdata").split(" | ")[1].split(" notes=")[1].split(" vel=")[0].replace(")(", ") (").split())
        ins.send(f"prdrag {t0} {pitch} {t0 + cells * 240} {pitch} {off}")
        after = set(ins.send("patdata").split(" | ")[1].split(" notes=")[1].split(" vel=")[0].replace(")(", ") (").split())
        lens += [n.split("+")[1].rstrip(")") for n in after - before]
    check(lens == ["240"] * 5, "dragging a just-placed note keeps its 1-unit length", str(lens))
    ins.quit()

    # ---- the '+' on a browser sample row adds it to the channel rack
    import wave
    lib = os.path.join(tmp, "lib")
    os.makedirs(lib)
    with wave.open(os.path.join(lib, "snare_hit.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\x10\x00" * 4410)
    cfg7 = os.path.join(tmp, "cfg7")
    os.makedirs(os.path.join(cfg7, "Vybrant"))
    with open(os.path.join(cfg7, "Vybrant", "Vybrant.conf"), "w") as f:
        f.write(f"[browser]\ndirs={lib}\n")
    ins = Instance(cfg7, sock)
    before = len(ins.send("channels").split(" | "))
    r = "err"
    for _ in range(20):   # the library is indexed in the background
        r = ins.send("browserplus snare_hit")
        if r.startswith("channels="):
            break
        time.sleep(0.25)
    check(r.startswith(f"channels={before + 1} "), "the '+' on a browser sample adds it to the channel rack", f"{before} -> {r}")
    check("toast=Added" in r and "snare_hit" in r and "Channel Rack" in r, "adding it shows a toast by the cursor", r)
    n = len(ins.send("channels").split(" | "))
    r = ins.send(f"droptest {os.path.join(lib, 'snare_hit.wav')}")   # a browser drag dropped on the rack
    check(r.startswith("enter=1 move=1 drop=1") and r.endswith(f"channels={n + 1}"), "a sample dropped on the channel rack adds a channel", r)
    ins.quit()

    # ---- project samples panel: browser drops save a sample unplaced, double-click opens its details, 'Show on playlist'
    cfg8 = os.path.join(tmp, "cfg8")
    os.makedirs(cfg8)
    ins = Instance(cfg8, sock)
    kept = os.path.join(lib, "snare_hit.wav")
    with wave.open(os.path.join(lib, "pad_unused.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\x10\x00" * 4410)
    r = ins.send(f"samples drop {kept}")
    clips0 = len([x for x in ins.send("clips").split() if x])
    check(r == "sources=1" and clips0 == 1, "a sample dropped on the project panel is saved without being placed", f"{r} clips={clips0}")
    r = ins.send("samples open")
    check(r == f"clipEditors=1 clips={clips0}", "double-click opens the sample's details and places nothing", r)
    ins.send(f"audioclip {kept}")
    ins.send(f"audioclip {kept}")
    r = ins.send("samples show")
    check(r == "selected=2", "'Show on playlist' selects every clip of that sample", r)
    ins.send(f"samples drop {os.path.join(lib, 'pad_unused.wav')}")   # a second sample, never placed
    clips = len(ins.send("clips").split())
    r = ins.send("samples unused")
    check(r == f"sources=1 clips={clips}", "'Remove all unused samples' drops only the unplaced one", r)
    r = ins.send("samples remove")
    ins.send("action edit.undo")
    back = ins.send("samples unused")
    check(r == f"sources=0 clips={clips - 2}" and back == f"sources=1 clips={clips}",
          "removing a sample takes its playlist clips with it, in one undo step", f"{r} / undo -> {back}")
    ins.quit()

    # ---- the Release lane on a step-only channel: the edit must not be dropped by turning the notes back into steps
    cfg10 = os.path.join(tmp, "cfg10")
    os.makedirs(cfg10)
    ins = Instance(cfg10, sock)
    ins.send("action view.pianoRoll")   # the demo kick: steps only
    info = dict(kv.split("=", 1) for kv in ins.send("prwheel -4").split())
    vt, vb, ppt = int(info["velY"].split("..")[0]), int(info["velY"].split("..")[1]), float(info["ppt"])
    ins.send("prlane 2")
    ins.send(f"prmouse 61 {vt + 25} {int(60 + 3000 * ppt)} {vb - 10}")
    rel = ins.send("prrel")
    check(rel != "" and set(rel.split(",")) != {"50"}, "a Release-lane edit on a step-only channel sticks", repr(rel))
    ins.quit()

    # ---- keyboard shortcuts of both editors (shared ones live in TimelineView::handleSharedKey)
    cfg11 = os.path.join(tmp, "cfg11")
    os.makedirs(cfg11)
    ins = Instance(cfg11, sock)
    ins.send("action view.pianoRoll")   # the demo kick: 4 steps
    pr = lambda: dict(kv.split("=", 1) for kv in ins.send("prinfo").split())  # noqa: E731
    kick = lambda: ins.send("patdata").split(" | ")[1]  # noqa: E731
    seq = []
    for k in ["Ctrl+A", "Ctrl+D", "Shift+I", "Esc"]:
        ins.send(f"key pr {k}")
        seq.append(pr()["sel"])
    check(seq == ["4", "0", "4", "0"], "piano roll: Ctrl+A, Ctrl+D, Shift+I, Esc select / deselect / invert / clear", str(seq))
    tools = []
    for k in ["2", "T", "3", "E", "P"]:
        ins.send(f"key pr {k}")
        tools.append(pr()["tool"])
    check(tools == ["1", "4", "2", "1", "0"], "piano roll: 2 / T / 3 / E / P pick select, mute, erase, select, draw", str(tools))
    p0 = float(pr()["ppt"])
    ins.send("key pr PgUp")
    check(abs(float(pr()["ppt"]) - p0 * 1.3) < 1e-6, "piano roll: Page Up zooms in", f"{p0} -> {pr()['ppt']}")
    ins.send("key pr Ctrl+A")
    ins.send("key pr Ctrl+C")
    ins.send("key pr Del")
    gone = kick()
    ins.send("key pr Ctrl+V")
    back = kick()
    check("steps=................" in gone and "steps=x...x...x...x..." in back, "piano roll: Ctrl+C, Del, Ctrl+V copy, delete and paste back", f"{gone} / {back}")
    ins.send("action view.playlist")
    pl = lambda: dict(kv.split("=", 1) for kv in ins.send("plinfo").split())  # noqa: E731
    ins.send("key pl Ctrl+A")
    s1 = pl()["sel"]
    ins.send("key pl Ctrl+D")
    s2 = pl()["sel"]
    check((s1, s2) == ("1", "0"), "playlist: Ctrl+A / Ctrl+D select all and deselect", f"{s1} {s2}")
    tools = []
    for k in ["4", "5", "B", "C", "D", "1"]:
        ins.send(f"key pl {k}")
        tools.append(pl()["tool"])
    check(tools == ["3", "4", "0", "3", "2", "0"], "playlist: 4 / 5 / B / C / D / 1 pick slice, mute, draw, slice, erase, draw", str(tools))
    ins.quit()

    # ---- a clip gain-handle drag is one undo step (it used to change the clip without undo, rebuild or 'unsaved')
    cfg9 = os.path.join(tmp, "cfg9")
    os.makedirs(cfg9)
    ins = Instance(cfg9, sock)
    ins.send("action view.playlist")
    long_wav = os.path.join(lib, "pad_long.wav")
    with wave.open(long_wav, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\x10\x00" * 44100 * 4)
    ins.send(f"audioclip {long_wav}")
    dragged = ins.send("plgain 30")
    ins.send("action edit.undo")
    undone = ins.send("plgain 0")
    check(dragged not in ("gain=1.00", "") and not dragged.startswith("err") and undone == "gain=1.00",
          "a gain-handle drag changes the clip and one undo restores it", f"{dragged} -> undo -> {undone}")
    ins.quit()

    # ---- clicking the workspace puts away an open editor window; reopening the editor brings it back
    cfg6 = os.path.join(tmp, "cfg6")
    os.makedirs(cfg6)
    ins = Instance(cfg6, sock)
    ins.send("action view.pianoRoll")
    ins.send("editor 0")
    time.sleep(0.5)
    opened = ins.send("editors")
    ins.send("prclick 0 40 right")   # a click in the piano roll (right button on an empty cell: changes nothing)
    hidden = ins.send("editors")
    ins.send("editor 0")
    time.sleep(0.5)
    check((opened, hidden, ins.send("editors")) == ("1", "0", "1"), "a workspace click hides the editor; reopening shows it",
          f"{opened} {hidden} {ins.send('editors')}")
    ins.quit()

    # ---- playlist: a clicked / resized pattern clip is the brush the draw tool places (pattern and size)
    cfg4 = os.path.join(tmp, "cfg4")
    os.makedirs(cfg4)
    ins = Instance(cfg4, sock)
    ins.send("action view.playlist")
    info = ins.send("plinfo")
    opened, closed = ins.send("pltempo"), ins.send("pltempo")
    check(info.endswith("tempo=0") and opened.endswith("tempo=1") and closed.endswith("tempo=0"),
          "the tempo lane starts closed; clicking Tempo opens and closes it", f"{info} | {opened} | {closed}")
    ins.send("pldrag 0 3840 0 7680 -5")   # stretch the demo's 1-bar clip to 2 bars by its right edge
    check(ins.send("clips") == "0:0+7680", "playlist clip resizes by its right edge", ins.send("clips"))
    ins.send("pldrag 1 0 1 0")
    check(ins.send("clips").split()[-1] == "1:0+7680", "a resized clip sets the size of the next placed clip", ins.send("clips"))
    ins.send("pldrag 0 960 0 960")
    ins.send("pldrag 2 7680 2 7680")
    check(ins.send("clips").split()[-1] == "2:7680+7680", "clicking a clip keeps its size as the brush", ins.send("clips"))
    # clicking a sample on the playlist makes the next placed clip that sample (not the selected pattern)
    import wave
    wav = os.path.join(tmp, "hat.wav")
    with wave.open(wav, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\x10\x00" * 11025)
    ins.send(f"audioclip {wav}")   # track 0 at 0: a quarter-second sample
    hat = ins.send("clips").split()[-1]
    ins.send("pldrag 0 10 0 10")   # click it
    panel = ins.send("projpanel")
    check(panel.startswith("tab=0 current=audio|") and panel.endswith("hat.wav"), "clicking an audio clip shows its sample in the project strip", panel)
    ins.send("pldrag 3 3840 3 3840")
    placed = ins.send("clips").split()[-1]
    check(hat.endswith("A") and placed == "3:3840+" + hat.split("+")[1], "clicking a sample makes the draw tool place that sample",
          f"{hat} -> {placed}")
    ins.send("projpick 1 0")   # the pattern in the project panel: it is placed next, not the sample
    ins.send("pldrag 4 3840 4 3840")
    after_pattern = ins.send("clips").split()[-1]
    ins.send("projpick 0 0")   # and the sample again
    ins.send("pldrag 5 3840 5 3840")
    after_sample = ins.send("clips").split()[-1]
    check(after_pattern.startswith("4:3840+") and not after_pattern.endswith("A") and after_sample.startswith("5:3840+") and
          after_sample.endswith("A"), "what is picked in the project panel (pattern, then sample) is what the draw tool places next",
          f"{after_pattern} {after_sample}")
    b, z = map(float, ins.send("plpinch 0.5").split())
    check(abs(z - b * 1.5) < 1e-6, "trackpad pinch zooms the playlist", f"{b} -> {z}")
    ins.quit()

    # ---- an audio clip's menu button (top left of its header) opens its menu; Transpose sets its semitones
    cfg16 = os.path.join(tmp, "cfg16")
    os.makedirs(cfg16)
    ins = Instance(cfg16, sock)
    pad = os.path.join(tmp, "pad.wav")
    with wave.open(pad, "wb") as w:   # two seconds: wide enough for the button at the default zoom
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\x10\x00" * 88200)
    ins.send(f"audioclip {pad}")
    r = ins.send("plclipmenu Transpose 5")
    check("Transpose...;" in r and r.endswith("pitch=5"), "the audio clip's menu button opens its menu, and Transpose sets the semitones", r)
    ins.send(f"clipeditor {os.path.join(tmp, 'clipeditor.png')}")
    r = ins.send("clipmode 1")   # Stretch, picked with the mouse in the dropdown's list
    check(r == "mode=1 open=1", "a dropdown in the audio clip editor applies its choice (the click no longer closes the editor)", r)
    ins.quit()

    # ---- the project strip follows what was picked: a pattern clip, the pattern dropdown's "+ New pattern"
    cfg13 = os.path.join(tmp, "cfg13")
    os.makedirs(cfg13)
    ins = Instance(cfg13, sock)
    ins.send("pldrag 0 10 0 10")   # the demo's pattern clip
    first = ins.send("projpanel")
    check(first.startswith("tab=1 current=pattern|"), "clicking a pattern clip shows that pattern in the strip's Patterns tab", first)
    r = ins.send("patcombo")
    second = ins.send("projpanel")
    check(r == "selected=Pattern 2 patterns=2" and second.startswith("tab=1 current=pattern|") and second != first,
          "'+ New pattern' in the pattern dropdown adds a pattern, selects it and shows it in the strip", f"{r} {second}")
    ins.send("pldrag 0 10 0 10")
    check(ins.send("projpanel") == first, "clicking the clip again goes back to its pattern", ins.send("projpanel"))
    ins.quit()

    # ---- the effect / instrument picker: cards by default, a remembered list view, search, a star that makes a favourite
    cfg14 = os.path.join(tmp, "cfg14")
    os.makedirs(cfg14)
    ins = Instance(cfg14, sock)
    png = os.path.join(tmp, "picker.png")
    r = ins.send(f"picker effect - {png}")
    check(r.startswith("view=cards ") and "sections=Dynamics,EQ / Filter,Time" in r and os.path.getsize(png) > 0,
          "the effect picker opens as cards, grouped by what the effects do", r)
    ins.send(f"picker effect list {png}")
    r = ins.send(f"picker instrument - {png}")
    check(r.startswith("view=list ") and "Synths" in r, "the list view is remembered for the next picker", r)
    r = ins.send(f"picker effect cards {png} param eq")
    check("items=1 " in r and r.endswith("first=Parametric EQ"), "the picker's search filters by name", r)
    r = ins.send(f"picker effect star {png}")
    check("sections=Favorites,Dynamics" in r, "clicking a card's star lists it under Favorites", r)
    r = ins.send(f"picker effect - {png}")
    check("sections=Favorites,Dynamics" in r, "favourites are kept for the next picker", r)
    ins.send("editor 0")
    dx, dy = map(int, ins.send("editors geo").split())
    check(abs(dx) <= 2 and abs(dy) <= 40, "an editor window opens in the middle of the screen", f"{dx} {dy}")
    ins.quit()

    # ---- the license dialog tells a bad key apart (a good one needs the real signing key, see --selftest for the format)
    ins = Instance(cfg14, sock)
    r = ins.send(f"licensedialog {png} eyJ2IjoxfQ.bm90IGEgc2lnbmF0dXJl")
    check("not a valid" in r and os.path.getsize(png) > 0, "the license dialog rejects a key that is not signed by Vybrant", r)
    ins.quit()

    # ---- the app was called MusicThingy, then Tactum: settings and data move to Vybrant on the first start
    for n, old in enumerate(["MusicThingy", "Tactum"]):
        c = os.path.join(tmp, f"cfg-old{n}")
        os.makedirs(os.path.join(c, old))
        os.makedirs(os.path.join(c, "data", old, old, "thumbnails"))
        with open(os.path.join(c, old, f"{old}.conf"), "w") as f:
            f.write("[license]\nkey=abc\n")
        ins = Instance(c, sock)
        ins.quit()
        conf = os.path.join(c, "Vybrant", "Vybrant.conf")
        moved = os.path.exists(conf) and "key=abc" in open(conf).read() and os.path.isdir(os.path.join(c, "data", "Vybrant", "Vybrant", "thumbnails"))
        check(moved and not os.path.exists(os.path.join(c, old)), f"settings and data from {old} move to Vybrant",
              sorted(os.path.relpath(os.path.join(r, x), c) for r, ds, fs in os.walk(c) for x in ds + fs))

    print("\n%s" % ("All UI checks passed" if fails == 0 else f"{fails} UI check(s) FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
