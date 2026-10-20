#!/usr/bin/env python3
"""Transport sync tests: JACK transport (both directions) and incoming MIDI clock, against real servers.

Starts a private dummy JACK server and an ALSA sequencer test client, plus offscreen instances with private configs and
sockets. Parts whose server isn't available (no jackd, no /dev/snd/seq) are skipped. Run from the repository root:

    python3 tests/sync_test.py [path/to/tactum]
"""
import ctypes, ctypes.util, os, shutil, socket, struct, subprocess, sys, tempfile, time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "bin", "tactum")
fails = 0


def check(cond, msg, detail=""):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg + (f"   [{detail}]" if detail and not cond else ""))
    fails += not cond


class Instance:
    def __init__(self, settings, env=None):
        tmp = tempfile.mkdtemp(prefix="mts")   # short: unix socket paths max out at 108 chars
        os.makedirs(tmp + "/cfg/Tactum")
        open(tmp + "/cfg/Tactum/Tactum.conf", "w").write(settings)
        self.sock = tmp + "/s.sock"
        e = dict(os.environ, **(env or {}), XDG_CONFIG_HOME=tmp + "/cfg", XDG_DATA_HOME=tmp + "/data", QT_QPA_PLATFORM="offscreen", MT_CONTROL_SOCKET=self.sock)
        self.proc = subprocess.Popen([BIN], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            time.sleep(0.2)
            if self.send("ping") == "pong":
                return
        raise RuntimeError("instance did not start")

    def send(self, cmd):
        try:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(5)
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
        self.proc.kill()   # nothing to save; a dirty project would ask first
        self.proc.wait(5)


def jack_test():
    if not shutil.which("jackd") or not ctypes.util.find_library("jack"):
        print("  skip JACK transport (no jackd)")
        return
    env = {"JACK_DEFAULT_SERVER": "mt-sync-test"}
    os.environ.update(env)
    jd = subprocess.Popen(["jackd", "-n", "mt-sync-test", "-d", "dummy", "-r", "48000", "-p", "256"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    J = ctypes.CDLL(ctypes.util.find_library("jack"))
    J.jack_client_open.restype = ctypes.c_void_p
    J.jack_client_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
    for f in ("jack_activate", "jack_transport_start", "jack_transport_stop", "jack_client_close"):
        getattr(J, f).argtypes = [ctypes.c_void_p]
    J.jack_transport_locate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    J.jack_transport_query.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    c = J.jack_client_open(b"sync-test", 1, None)   # JackNoStartServer
    if not c:
        print("  skip JACK transport (server did not start)")
        jd.terminate()
        return
    J.jack_activate(c)
    pos = ctypes.create_string_buffer(1024)

    def jack():   # (state, frame): jack_position_t.frame sits after unique_1, usecs and frame_rate
        st = J.jack_transport_query(c, pos)
        return st, int.from_bytes(pos.raw[20:24], "little")

    ins = Instance("[sync]\njack=true\n", env)
    try:
        ins.send("action transport.songMode")   # pattern mode loops by itself, so only song mode follows JACK positions
        time.sleep(0.5)
        J.jack_transport_start(c)
        time.sleep(0.6)
        check("playing=1" in ins.send("stats"), "JACK start: Tactum plays", ins.send("stats"))
        J.jack_transport_stop(c)
        time.sleep(0.4)
        check("playing=0" in ins.send("stats"), "JACK stop: Tactum stops", ins.send("stats"))
        J.jack_transport_locate(c, 48000 * 4)
        time.sleep(0.5)
        want = 4 * float(ins.send("bpm")) / 60 * 960
        check(abs(float(ins.send("pos") or -1) - want) < 2, "JACK locate: the playhead follows", ins.send("pos"))
        ins.send("stop")   # while stopped: back to the start
        time.sleep(0.4)
        check(jack()[1] == 0, "stop while stopped: JACK locates to the start", jack())
        ins.send("play")
        time.sleep(0.5)
        check(jack()[0] == 1, "play: JACK rolls", jack())
        ins.send("stop")
        time.sleep(0.4)
        check(jack()[0] == 0, "stop: JACK stops", jack())
    finally:
        ins.quit()
        J.jack_client_close(c)
        jd.terminate()
        jd.wait(5)


def midi_clock_test():
    if not os.path.exists("/dev/snd/seq") or not ctypes.util.find_library("asound"):
        print("  skip MIDI clock in (no ALSA sequencer)")
        return
    A = ctypes.CDLL(ctypes.util.find_library("asound"))
    seq = ctypes.c_void_p()
    if A.snd_seq_open(ctypes.byref(seq), b"default", 1, 0) != 0:
        print("  skip MIDI clock in (sequencer unavailable)")
        return
    A.snd_seq_set_client_name(seq, b"mt-clock-test")
    port = A.snd_seq_create_simple_port(seq, b"out", 1 | 32, 2)   # CAP_READ|SUBS_READ, MIDI_GENERIC
    me = A.snd_seq_client_id(seq)
    ins = Instance("[midi]\nclockIn=true\n")
    try:
        info = ctypes.c_void_p()
        A.snd_seq_client_info_malloc(ctypes.byref(info))
        dest = None
        for _ in range(25):   # this instance's client, not another Tactum that happens to run
            for line in open("/proc/asound/seq/clients"):
                if line.startswith("Client") and "Tactum" in line:
                    cid = int(line.split()[1])
                    if A.snd_seq_get_any_client_info(seq, cid, info) == 0 and A.snd_seq_client_info_get_pid(info) == ins.proc.pid:
                        dest = cid
            if dest is not None:
                break
            time.sleep(0.2)
        check(dest is not None, "the instance has a sequencer client")
        if dest is None:
            return
        A.snd_seq_connect_to(seq, port, dest, 0)   # its first port is 'Tactum In'

        def ev(etype, value=0):   # snd_seq_event_t, sent direct to subscribers
            b = struct.pack("<BBBB8sBBBB", etype, 0, 0, 253, b"\0" * 8, me, port, 254, 253) + struct.pack("<BBBBiI", 0, 0, 0, 0, 0, value)
            A.snd_seq_event_output_direct(seq, ctypes.create_string_buffer(b, 28))

        CLOCK, START, STOP, SONGPOS = 36, 30, 32, 20
        interval = 60.0 / (100 * 24)
        for _ in range(48):   # clocks while stopped: the tempo is measured before Start too
            ev(CLOCK)
            time.sleep(interval)
        ev(START)
        t0, n = time.monotonic(), 0
        while time.monotonic() - t0 < 4.0:
            ev(CLOCK)
            n += 1
            time.sleep(max(0, t0 + n * interval - time.monotonic()))
        check("playing=1" in ins.send("stats"), "MIDI Start: Tactum plays", ins.send("stats"))
        check(abs(float(ins.send("bpm") or -1) - 100) < 1, "the tempo follows the clock", ins.send("bpm"))
        off = (float(ins.send("pos") or -1) - (n - 1) * 40) % 3840   # pattern mode: the playhead wraps every bar
        check(min(off, 3840 - off) < 120, "the playhead stays phase-locked to the clock", off)
        ev(STOP)
        time.sleep(0.3)
        check("playing=0" in ins.send("stats"), "MIDI Stop: Tactum stops", ins.send("stats"))
        ev(SONGPOS, 16)   # in sixteenths: one bar
        time.sleep(0.3)
        check(abs(float(ins.send("pos") or -1) - 3840) < 2, "Song Position Pointer moves the playhead", ins.send("pos"))
    finally:
        ins.quit()
        A.snd_seq_close(seq)


print("Tactum sync test")
jack_test()
midi_clock_test()
print("\nAll sync checks passed" if not fails else f"\n{fails} sync check(s) FAILED")
sys.exit(1 if fails else 0)
