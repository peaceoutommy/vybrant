#!/usr/bin/env python3
"""The landing page's carousel images, made by the app itself from a generated song.

    python3 web/scripts/showcase.py [path to the vybrant binary]

Writes a short song (drums, bass, chords, lead, an audio loop and automation) as a Vybrant project, opens it offscreen
through the control socket (app/src/ui/ControlServer.cpp) and saves the pattern and playlist views, then puts a few native
plugin editors (--editor-shot) side by side. Output: web/public/showcase-*.webp. Needs ImageMagick (magick).
"""
import json, os, socket, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "app", "bin", "vybrant")
OUT = os.path.join(ROOT, "web", "public")
PPQ, BAR = 960, 3840
SIXTEENTH = PPQ // 4


class Song:
    """A Vybrant project document (app/src/project/ProjectSerializer.cpp's format), built up piece by piece."""

    def __init__(self, bpm):
        self.next = 1
        self.channels, self.patterns, self.tracks, self.clips, self.lanes, self.audio, self.mixer = [], [], [], [], [], [], []
        self.bpm = bpm
        master = self.mixer_track("Master", master=True, effects=[("native.parametriceq", "Parametric EQ"), ("native.limiter", "Limiter")])
        self.master = master["id"]
        tracks = [self.mixer_track(n, effects=fx) for n, fx, _, _ in INSERTS]
        for t, (_, _, vol, pan) in zip(tracks, INSERTS):
            t["volume"], t["pan"] = vol, pan
        self.inserts = [t["id"] for t in tracks]
        # the drums sum into a bus; chords, lead and clap send to a shared reverb
        bus = self.mixer_track("Drum bus", effects=[("native.compressor", "Compressor"), ("native.saturation", "Saturation")])
        verb = self.mixer_track("Reverb", effects=[("native.reverb", "Reverb"), ("native.parametriceq", "Parametric EQ")])
        bus["volume"], verb["volume"] = 0.9, 0.6
        for t in tracks[:4]:
            t["output"] = bus["id"]
        for t in (tracks[1], tracks[5], tracks[6]):
            t["sends"].append({"target": verb["id"], "level": 0.35, "pre": False, "enabled": True, "sidechain": False})

    def id(self):
        self.next += 1
        return self.next - 1

    def mixer_track(self, name, master=False, effects=()):
        t = {"id": self.id(), "name": name, "color": "#ffd07050" if master else "#ff3a404a", "volume": 0.8, "pan": 0.0,
             "output": 0 if master else getattr(self, "master", 0), "master": master, "customName": not master,
             "effects": [{"id": self.id(), "key": k, "plugin": 0, "bypass": False, "mix": 1.0, "state": {}, "name": n}
                         for k, n in effects], "sends": []}
        self.mixer.append(t)
        return t

    def channel(self, name, color, kind, key, insert, state=None):
        c = {"id": self.id(), "name": name, "color": color, "type": kind, "instrument": key, "plugin": 0, "sample": "",
             "volume": 0.8, "pan": 0.0, "enabled": True, "mixerTrack": self.inserts[insert], "rootNote": 60,
             "midiOutPort": -1, "auxOutputs": [], "state": state or {}}
        self.channels.append(c)
        return c["id"]

    def pattern(self, name, color, length=0):
        p = {"id": self.id(), "name": name, "color": color, "length": length, "stepsPerBeat": 4, "stepCount": 16,
             "swing": 0.0, "channels": [], "automation": []}
        self.patterns.append(p)
        return p

    def data(self, pat, ch):
        for d in pat["channels"]:
            if d["channelId"] == ch:
                return d
        d = {"channelId": ch, "notes": [], "steps": [{"e": False, "v": 0.8} for _ in range(16)], "controls": []}
        pat["channels"].append(d)
        return d

    def steps(self, pat, ch, hits, vel=None):
        for s in hits:
            self.data(pat, ch)["steps"][s] = {"e": True, "v": (vel or {}).get(s, 0.8)}

    def note(self, pat, ch, pitch, start, dur, vel=0.75):
        self.data(pat, ch)["notes"].append({"id": self.id(), "p": pitch, "s": start, "d": dur, "v": vel})

    def track(self, name, color, height=84):
        t = {"id": self.id(), "name": name, "color": color, "height": height, "mixerTrack": 0}
        self.tracks.append(t)
        return t["id"]

    def clip(self, kind, track, source, start, length, name="", loop=False):
        self.clips.append({"id": self.id(), "type": kind, "track": track, "source": source, "start": start, "length": length,
                           "gain": 1.0, "stretch": 1.0, "loop": loop, "name": name})

    def lane(self, name, color, target, points, length):
        l = {"id": self.id(), "name": name, "color": color, "target": target, "length": length,
             "points": [{"t": t, "v": v, "c": 2} for t, v in points]}
        self.lanes.append(l)
        return l["id"]

    def audio_file(self, path, name):
        a = {"id": self.id(), "path": path, "name": name}
        self.audio.append(a)
        return a["id"]

    def doc(self, selected_pattern, selected_channel):
        return {"formatVersion": 1, "application": "Vybrant",
                "project": {"title": "Showcase", "sampleRate": 44100, "bitDepth": 24, "nextId": self.next,
                            "selectedPattern": selected_pattern, "selectedChannel": selected_channel},
                "transport": {"bpm": self.bpm, "tsNum": 4, "tsDen": 4, "loop": False, "loopStart": 0, "loopEnd": 16 * BAR,
                              "songMode": True, "snap": True, "snapDivision": 4},
                "patterns": self.patterns, "channels": self.channels, "playlist": {"tracks": self.tracks, "clips": self.clips},
                "mixer": self.mixer, "plugins": [], "automation": self.lanes, "audioReferences": self.audio, "markers": []}


# the mixer inserts the channels play into (Song.channel's insert index), with their effects
INSERTS = [   # name, effects, fader, pan
    ("Kick", [("native.transient", "Transient Processor"), ("native.compressor", "Compressor")], 0.95, 0.0),
    ("Clap", [("native.parametriceq", "Parametric EQ")], 0.7, -0.1),
    ("Hat", [("native.highpass", "High Pass"), ("native.stereowidth", "Stereo Width")], 0.55, 0.25),
    ("Snare", [("native.saturation", "Saturation")], 0.6, -0.2),
    ("Bass", [("native.saturation", "Saturation"), ("native.compressor", "Compressor")], 0.8, 0.0),
    ("Chords", [("native.parametriceq", "Parametric EQ"), ("native.chorus", "Chorus")], 0.65, 0.0),
    ("Lead", [("native.delay", "Delay"), ("native.compressor", "Compressor")], 0.72, 0.1),
    ("Vocal chops", [("native.phaser", "Phaser"), ("native.pingpong", "Ping-Pong Delay")], 0.5, -0.3),
]
SYNTH, DRUM = 2, 1   # ChannelType
PATTERN, AUDIO, AUTOMATION = 0, 1, 2   # ClipType
# A minor: Am7, Fmaj7, Cmaj7, G (a bar each), as voiced on the keyboard
CHORDS = [[57, 60, 64, 67], [53, 57, 60, 64], [55, 60, 64, 67], [55, 59, 62, 67]]
ROOTS = [33, 29, 36, 31]
LEAD = [(76, 0, 2), (74, 2, 1), (72, 3, 1), (74, 4, 2), (76, 6, 2), (79, 8, 3), (76, 11, 1), (74, 12, 4),
        (72, 16, 2), (71, 18, 1), (72, 19, 1), (74, 20, 4), (72, 24, 2), (69, 26, 2), (71, 28, 4)]   # (pitch, eighth, eighths)


def build(song, with_audio):
    kick = song.channel("Kick", "#ffe2665c", DRUM, "native.drumsampler", 0, {"builtin": "Kick"})
    clap = song.channel("Clap", "#ffe0a040", DRUM, "native.drumsampler", 1, {"builtin": "Clap"})
    hat = song.channel("Hat", "#ff6dc26d", DRUM, "native.drumsampler", 2, {"builtin": "Hat"})
    snare = song.channel("Snare", "#ff5aa9e6", DRUM, "native.drumsampler", 3, {"builtin": "Snare"})
    bass = song.channel("Bass", "#ff4fc3b0", SYNTH, "native.fm", 4)
    keys = song.channel("Chords", "#ffc07be0", SYNTH, "native.wavetable", 5)
    lead = song.channel("Lead", "#fff07aa8", SYNTH, "native.synth", 6)

    # Pattern 1: the whole groove in one, the way it is written in the channel rack and piano roll
    groove = song.pattern("Groove", "#ffc07be0")
    song.steps(groove, kick, [0, 4, 8, 12])
    song.steps(groove, clap, [4, 12])
    song.steps(groove, hat, [2, 6, 10, 14, 15], {15: 0.5})
    song.steps(groove, snare, [7, 13], {7: 0.45, 13: 0.6})
    for bar, (chord, root) in enumerate(zip(CHORDS, ROOTS)):
        for p in chord:
            song.note(groove, keys, p, bar * BAR, BAR - SIXTEENTH, 0.62)
        for k, (step, length) in enumerate([(0, 3), (3, 3), (6, 2), (10, 2), (12, 4)]):
            song.note(groove, bass, root + (12 if k == 3 else 0), bar * BAR + step * SIXTEENTH, length * SIXTEENTH - 60, 0.8)
    for pitch, eighth, eighths in LEAD:
        song.note(groove, lead, pitch, eighth * PPQ // 2, eighths * PPQ // 2 - 60, 0.7 + 0.1 * (eighth % 4 == 0))

    # Parts for the arrangement
    drums = song.pattern("Drums", "#ffe2665c")
    song.steps(drums, kick, [0, 4, 8, 12])
    song.steps(drums, clap, [4, 12])
    song.steps(drums, hat, [2, 6, 10, 14])
    parts = {}
    for name, color, ch, src in [("Bass", "#ff4fc3b0", bass, None), ("Chords", "#ffc07be0", keys, None), ("Lead", "#fff07aa8", lead, None)]:
        p = song.pattern(name, color)
        for n in song.data(groove, ch)["notes"]:
            song.note(p, ch, n["p"], n["s"], n["d"], n["v"])
        parts[name] = p["id"]

    t_drums = song.track("Drums", "#ffe2665c")
    t_bass = song.track("Bass", "#ff4fc3b0")
    t_chords = song.track("Chords", "#ffc07be0")
    t_lead = song.track("Lead", "#fff07aa8")
    t_loop = song.track("Vocal chops", "#ff5aa9e6")
    t_filter = song.track("Chords filter", "#ffe09a40", 64)
    t_vol = song.track("Lead volume", "#ffe09a40", 64)
    for bar in range(0, 24):
        if bar >= 4 and not 12 <= bar < 14:
            song.clip(PATTERN, t_drums, drums["id"], bar * BAR, BAR, "Drums")
    for bar in range(0, 24, 4):
        song.clip(PATTERN, t_chords, parts["Chords"], bar * BAR, 4 * BAR, "Chords")
        if bar >= 4:
            song.clip(PATTERN, t_bass, parts["Bass"], bar * BAR, 4 * BAR, "Bass")
        if bar >= 8 and bar != 12:
            song.clip(PATTERN, t_lead, parts["Lead"], bar * BAR, 4 * BAR, "Lead")
    if with_audio:
        loop = song.audio_file(with_audio, "chops.wav")
        for bar in (8, 16, 20):
            song.clip(AUDIO, t_loop, loop, bar * BAR, 4 * BAR, "chops.wav")
    # the chords' filter opens over the intro, the lead fades in and out
    cutoff = song.lane("Chords cutoff", "#ffe09a40", {"kind": 7, "owner": keys, "slot": 0, "param": 10},
                       [(0, 0.15), (4 * BAR, 0.45), (8 * BAR, 0.9)], 8 * BAR)
    song.clip(AUTOMATION, t_filter, cutoff, 0, 8 * BAR, "Chords cutoff")
    song.clip(AUTOMATION, t_filter, cutoff, 12 * BAR, 8 * BAR, "Chords cutoff")
    vol = song.lane("Lead volume", "#ffe09a40", {"kind": 1, "owner": lead, "slot": 0, "param": 0},
                    [(0, 0.2), (2 * BAR, 0.8), (6 * BAR, 0.8), (8 * BAR, 0.35)], 8 * BAR)
    song.clip(AUTOMATION, t_vol, vol, 8 * BAR, 8 * BAR, "Lead volume")
    song.clip(AUTOMATION, t_vol, vol, 16 * BAR, 8 * BAR, "Lead volume")
    return groove["id"], keys


class Instance:
    def __init__(self, project, cfg, sock):
        self.sock = sock
        env = dict(os.environ, XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=os.path.join(cfg, "data"), QT_QPA_PLATFORM="offscreen",
                   QT_SCALE_FACTOR="2", MT_CONTROL_SOCKET=sock)
        self.proc = subprocess.Popen([BIN, project], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            time.sleep(0.2)
            if self.send("ping") == "pong":
                return
        raise RuntimeError("instance did not start")

    def send(self, cmd):
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.settimeout(20)
                s.connect(self.sock)
                s.sendall((cmd + "\n").encode())
                data = b""
                while not data.endswith(b"\n"):
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                return data.decode().strip()
        except OSError:
            return ""

    def quit(self):
        self.send("quit")
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:   # an edit left the project unsaved: the save prompt waits for an answer
            self.proc.kill()


def magick(*args):
    subprocess.run(["magick", *args], check=True)


# Editor slides: native editors in rows, with settings that give their displays something to show
EFFECTS = [[("parametriceq", "10=1 13=4 20=1 22=180 23=-3 30=1 32=650 33=-5 35=1 37=3500 38=4.5"),   # band b: 10 + 5b + (on, type, freq, gain, q)
            ("compressor", "")],
           [("reverb", ""), ("delay", "")]]
INSTRUMENTS = [[("wavetable", ""), ("synth", "")], [("fm", "")]]
BG = "#0f1216"   # the app window's background
# Feature tiles on the landing page: parts of those shots (geometry in the 3200 x 1800 window shots), and their width
FEATURES = [
    ("pattern", "pattern.png", "2264x1318+916+226", 1400),   # channel rack over the piano roll
    ("mixer", "mixer.png", "976x1504+1904+226", 640),   # five strips, the chords' send wire
    ("playlist", "playlist.png", "2264x1144+916+280", 1400),
    ("eq", "parametriceq.png", "1460x600+0+0", 1100),   # the curve, without the band controls
]


def editors(work, env, name, rows):
    args = []
    for row in rows:
        args.append("(")
        for key, sets in row:
            shot = os.path.join(work, f"{key}.png")
            subprocess.run([BIN, "--editor-shot", "native." + key, shot, *sets.split()], env=dict(env, QT_SCALE_FACTOR="2"),
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
            cell = os.path.join(work, f"{key}-cell.png")
            magick(shot, "-resize", "1520x830", "-background", BG, "-gravity", "center", "-extent", "1580x880", cell)
            args.append(cell)
        args += ["-background", BG, "+append", ")"]
    magick(*args, "-background", BG, "-gravity", "center", "-append", "-extent", "3200x1800", os.path.join(work, name + ".png"))


def main():
    work = tempfile.mkdtemp(prefix="vybrant-showcase-")
    cfg = os.path.join(work, "cfg")
    env = dict(os.environ, XDG_CONFIG_HOME=cfg, QT_QPA_PLATFORM="offscreen")

    # an audio loop for the playlist, rendered by the app: the chords and lead played an octave up, cut into a loop
    loop = Song(124)
    build(loop, None)
    loop.clips = [c for c in loop.clips if c["type"] == PATTERN and (c["name"], c["start"]) in (("Chords", 0), ("Lead", 8 * BAR))]
    for c in loop.clips:
        c["start"] = 0
    with open(os.path.join(work, "loop.mtp"), "w") as f:
        json.dump(loop.doc(loop.patterns[0]["id"], loop.channels[0]["id"]), f)
    wav = os.path.join(work, "chops.wav")
    subprocess.run([BIN, os.path.join(work, "loop.mtp"), "--render", wav, "--quit"], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)

    song = Song(124)
    groove, keys = build(song, wav)
    project = os.path.join(work, "showcase.mtp")
    with open(project, "w") as f:
        json.dump(song.doc(groove, keys), f)

    sock = os.path.join("/tmp", f"vb-showcase-{os.getpid()}.sock")   # socket paths are capped near 108 bytes
    app = Instance(project, cfg, sock)
    try:
        app.send("winsize 1600 900")
        steps = [("playlist", ["fit"]), ("pattern", ["dock top", "select 6 roll", "action view.pianoRoll", "fit"]),
                 ("mixer", ["action view.mixer", "mixsel 6"])]
        for name, cmds in steps:
            for c in cmds:
                app.send(c)
            time.sleep(4)   # past the status bar's "Opened ..." message
            app.send(f"screenshot {os.path.join(work, name + '.png')}")
    finally:
        app.quit()
    editors(work, env, "plugins", EFFECTS)
    editors(work, env, "instruments", INSTRUMENTS)
    for name, src, geometry, width in FEATURES:
        magick(os.path.join(work, src), "-crop", geometry, "+repage", "-resize", f"{width}x", "-quality", "85",
               os.path.join(OUT, f"feature-{name}.webp"))
    for name in ("pattern", "playlist", "mixer", "plugins", "instruments"):   # 2400 px wide: sharp on a 2x screen at the page's width
        magick(os.path.join(work, name + ".png"), "-resize", "2400x", "-quality", "82", os.path.join(OUT, f"showcase-{name}.webp"))
    print(work)


if __name__ == "__main__":
    main()
