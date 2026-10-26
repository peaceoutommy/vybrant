// WinMM backend of MidiEngine (Windows). Port ids: inputs are "0:<device index>", outputs "1:<device index>".
// ponytail: a device is known by its WinMM index, which shifts when one before it is unplugged; match by name if that bites.
#include "midi/MidiEngine.h"
#include "audio/AudioEngine.h"
#include "core/Log.h"
#include <map>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

namespace mt {

namespace {
struct WinMidi {
    std::map<int, HMIDIIN> ins;
    std::map<int, HMIDIOUT> outs;
};
bool decode(DWORD_PTR msg, MidiEvent& e) {
    uint8_t st = msg & 0xF0, d1 = (msg >> 8) & 0x7F, d2 = (msg >> 16) & 0x7F;
    e.channel = msg & 0x0F;
    e.data1 = d1;
    e.data2 = d2;
    e.velocity = d2 / 127.f;
    switch (st) {
    case 0x90: e.type = d2 ? MidiEvent::NoteOn : MidiEvent::NoteOff; return true;
    case 0x80: e.type = MidiEvent::NoteOff; return true;
    case 0xB0: e.type = MidiEvent::CC; return true;
    case 0xE0: e.type = MidiEvent::PitchBend, e.pitchBend = (d1 | d2 << 7) - 8192; return true;
    case 0xD0: e.type = MidiEvent::ChannelPressure, e.data1 = 0, e.data2 = d1; return true;
    case 0xA0: e.type = MidiEvent::PolyPressure; return true;
    case 0xC0: e.type = MidiEvent::ProgramChange; return true;
    default: return false;
    }
}
DWORD encode(const MidiEvent& e) {
    auto msg = [&](int st, int d1 = 0, int d2 = 0) { return DWORD((st | e.channel) | d1 << 8 | d2 << 16); };
    switch (e.type) {
    case MidiEvent::NoteOn: return msg(0x90, e.data1, std::clamp((int)(e.velocity * 127), 1, 127));
    case MidiEvent::NoteOff: return msg(0x80, e.data1, e.data2);
    case MidiEvent::CC: return msg(0xB0, e.data1, e.data2);
    case MidiEvent::PitchBend: return msg(0xE0, (e.pitchBend + 8192) & 0x7F, ((e.pitchBend + 8192) >> 7) & 0x7F);
    case MidiEvent::ProgramChange: return msg(0xC0, e.data1);
    case MidiEvent::ChannelPressure: return msg(0xD0, e.data2);
    case MidiEvent::AllNotesOff: return msg(0xB0, 123, 0);
    case MidiEvent::Clock: return 0xF8;
    case MidiEvent::Start: return 0xFA;
    case MidiEvent::Continue: return 0xFB;
    case MidiEvent::Stop: return 0xFC;
    default: return 0;
    }
}
}   // namespace

bool MidiEngine::start() {
    if (m_running.load()) return true;
    m_seq = new WinMidi;
    m_running.store(true);
    timeBeginPeriod(1);   // 1 ms sleeps for the output thread
    loadSettings();
    for (auto& p : ports()) {
        if (p.input && m_enabledInputs.contains(p.id())) connectInput(p, true);
        if (p.output && m_enabledOutputs.contains(p.id())) connectOutput(p, true);
    }
    m_thread = std::thread([this] { threadLoop(); });
    Log::info("MIDI engine started (WinMM)");
    return true;
}

void MidiEngine::stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    auto* w = (WinMidi*)m_seq;
    for (auto& [i, h] : w->ins) {
        midiInStop(h);
        midiInReset(h);
        midiInClose(h);
    }
    for (auto& [i, h] : w->outs) midiOutClose(h);
    delete w;
    m_seq = nullptr;
    timeEndPeriod(1);
}

std::vector<MidiPortInfo> MidiEngine::ports() const {
    std::vector<MidiPortInfo> v;
    if (!m_seq) return v;
    for (UINT i = 0, n = midiInGetNumDevs(); i < n; ++i) {
        MIDIINCAPSW c;
        if (midiInGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR)
            v.push_back({0, (int)i, QString::fromWCharArray(c.szPname), true, false});
    }
    for (UINT i = 0, n = midiOutGetNumDevs(); i < n; ++i) {
        MIDIOUTCAPSW c;
        if (midiOutGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR)
            v.push_back({1, (int)i, QString::fromWCharArray(c.szPname), false, true});
    }
    return v;
}

void MidiEngine::connectInput(const MidiPortInfo& p, bool connect) {
    if (!m_seq) return;
    auto& ins = ((WinMidi*)m_seq)->ins;
    if (auto it = ins.find(p.port); it != ins.end() && !connect) {
        midiInStop(it->second);
        midiInReset(it->second);
        midiInClose(it->second);
        ins.erase(it);
    }
    if (!connect || ins.count(p.port)) return;
    // WinMM calls this on its own thread, one device at a time is not promised: m_mutex keeps the queue single-producer
    void (*cb)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR) = [](HMIDIIN, UINT msg, DWORD_PTR self, DWORD_PTR data, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* me = (MidiEngine*)self;
        std::lock_guard<std::mutex> lk(me->m_mutex);
        uint8_t st = data & 0xFF;
        MidiEvent e;
        if (st == 0xF8 || st == 0xFA || st == 0xFB || st == 0xFC || st == 0xF2) {
            if (me->m_clockIn.load()) me->handleClock(st, ((data >> 8) & 0x7F) | ((data >> 16) & 0x7F) << 7);
        } else if (decode(data, e)) me->received(e);
    };
    HMIDIIN h = nullptr;
    if (midiInOpen(&h, p.port, (DWORD_PTR)cb, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        Log::warn("Could not open MIDI input " + p.name);
        return;
    }
    midiInStart(h);
    ins[p.port] = h;
}
void MidiEngine::connectOutput(const MidiPortInfo& p, bool connect) {
    if (!m_seq) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto& outs = ((WinMidi*)m_seq)->outs;
    if (auto it = outs.find(p.port); it != outs.end() && !connect) {
        midiOutClose(it->second);
        outs.erase(it);
    }
    HMIDIOUT h = nullptr;
    if (!connect || outs.count(p.port)) return;
    if (midiOutOpen(&h, p.port, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) Log::warn("Could not open MIDI output " + p.name);
    else outs[p.port] = h;
}

void MidiEngine::threadLoop() {
    while (m_running.load()) {
        flushOutput();
        Sleep(1);
    }
}

void MidiEngine::flushOutput() {
    TimedMidiOut t;
    int sent = 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    while (++sent <= 512 && m_engine.midiOutQueue().pop(t))
        if (DWORD msg = encode(t.event))
            for (auto& [i, h] : ((WinMidi*)m_seq)->outs) midiOutShortMsg(h, msg);
}

}   // namespace mt
