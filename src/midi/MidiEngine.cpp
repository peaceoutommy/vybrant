#include "midi/MidiEngine.h"
#include "audio/AudioEngine.h"
#include "core/Log.h"
#include <alsa/asoundlib.h>
#include <poll.h>
#include <QSettings>
#include <QMetaObject>

namespace mt {

MidiEngine::MidiEngine(AudioEngine& engine, QObject* parent) : QObject(parent), m_engine(engine) {}
MidiEngine::~MidiEngine() { stop(); }

bool MidiEngine::start() {
    if (m_running.load()) return true;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) { Log::warn("ALSA sequencer unavailable; MIDI disabled"); return false; }
    snd_seq_set_client_name(seq, "MusicThingy");
    m_inPort = snd_seq_create_simple_port(seq, "MusicThingy In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    m_outPort = snd_seq_create_simple_port(seq, "MusicThingy Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    m_seq = seq; m_running.store(true);
    loadSettings();
    for (auto& p : ports()) { if (p.input && m_enabledInputs.contains(p.id())) connectInput(p, true); if (p.output && m_enabledOutputs.contains(p.id())) connectOutput(p, true); }
    m_thread = std::thread([this] { threadLoop(); });
    Log::info("MIDI engine started (ALSA sequencer)");
    return true;
}

void MidiEngine::stop() {
    if (!m_running.load()) return;
    m_running.store(false); if (m_thread.joinable()) m_thread.join();
    if (m_seq) { snd_seq_close((snd_seq_t*)m_seq); m_seq = nullptr; }
}

std::vector<MidiPortInfo> MidiEngine::ports() const {
    std::vector<MidiPortInfo> v; if (!m_seq) return v;
    snd_seq_t* seq = (snd_seq_t*)m_seq; snd_seq_client_info_t* ci; snd_seq_port_info_t* pi; snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    int self = snd_seq_client_id(seq);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        int client = snd_seq_client_info_get_client(ci); if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pi, client); snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pi); unsigned type = snd_seq_port_info_get_type(pi);
            if (!(type & (SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_SYNTHESIZER))) continue;
            MidiPortInfo p; p.client = client; p.port = snd_seq_port_info_get_port(pi); p.name = QString("%1: %2").arg(snd_seq_client_info_get_name(ci)).arg(snd_seq_port_info_get_name(pi));
            p.input = (caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ); p.output = (caps & SND_SEQ_PORT_CAP_WRITE) && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE);
            if (p.input || p.output) v.push_back(p);
        }
    }
    return v;
}

void MidiEngine::connectInput(const MidiPortInfo& p, bool connect) { if (!m_seq) return; if (connect) snd_seq_connect_from((snd_seq_t*)m_seq, m_inPort, p.client, p.port); else snd_seq_disconnect_from((snd_seq_t*)m_seq, m_inPort, p.client, p.port); }
void MidiEngine::connectOutput(const MidiPortInfo& p, bool connect) { if (!m_seq) return; std::lock_guard<std::mutex> lk(m_mutex); if (connect) { snd_seq_connect_to((snd_seq_t*)m_seq, m_outPort, p.client, p.port); m_outputTargets.push_back({p.client, p.port}); } else { snd_seq_disconnect_to((snd_seq_t*)m_seq, m_outPort, p.client, p.port); std::erase(m_outputTargets, std::make_pair(p.client, p.port)); } }

void MidiEngine::setEnabledInputs(const QStringList& ids) { for (auto& p : ports()) if (p.input) { bool was = m_enabledInputs.contains(p.id()), now = ids.contains(p.id()); if (was != now) connectInput(p, now); } m_enabledInputs = ids; saveSettings(); }
void MidiEngine::setEnabledOutputs(const QStringList& ids) { for (auto& p : ports()) if (p.output) { bool was = m_enabledOutputs.contains(p.id()), now = ids.contains(p.id()); if (was != now) connectOutput(p, now); } m_enabledOutputs = ids; saveSettings(); }
void MidiEngine::loadSettings() { QSettings s; m_enabledInputs = s.value("midi/inputs").toStringList(); m_enabledOutputs = s.value("midi/outputs").toStringList(); }
void MidiEngine::saveSettings() const { QSettings s; s.setValue("midi/inputs", m_enabledInputs); s.setValue("midi/outputs", m_enabledOutputs); }

void MidiEngine::threadLoop() {
    snd_seq_t* seq = (snd_seq_t*)m_seq; int npfd = snd_seq_poll_descriptors_count(seq, POLLIN); std::vector<pollfd> pfds(npfd); snd_seq_poll_descriptors(seq, pfds.data(), npfd, POLLIN);
    while (m_running.load()) {
        int r = poll(pfds.data(), npfd, 5);
        if (r > 0) { snd_seq_event_t* ev = nullptr; while (snd_seq_event_input(seq, &ev) >= 0 && ev) { handleAlsaEvent(ev); } }
        flushOutput();
    }
}

void MidiEngine::handleAlsaEvent(void* evp) {
    auto* ev = (snd_seq_event_t*)evp; MidiEvent e; bool ok = true;
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON: e.type = ev->data.note.velocity ? MidiEvent::NoteOn : MidiEvent::NoteOff; e.channel = ev->data.note.channel; e.data1 = ev->data.note.note; e.data2 = ev->data.note.velocity; e.velocity = ev->data.note.velocity / 127.f; break;
    case SND_SEQ_EVENT_NOTEOFF: e.type = MidiEvent::NoteOff; e.channel = ev->data.note.channel; e.data1 = ev->data.note.note; e.data2 = ev->data.note.velocity; e.velocity = ev->data.note.velocity / 127.f; break;
    case SND_SEQ_EVENT_CONTROLLER: e.type = MidiEvent::CC; e.channel = ev->data.control.channel; e.data1 = (uint8_t)ev->data.control.param; e.data2 = (uint8_t)ev->data.control.value; break;
    case SND_SEQ_EVENT_PITCHBEND: e.type = MidiEvent::PitchBend; e.channel = ev->data.control.channel; e.pitchBend = ev->data.control.value; break;
    case SND_SEQ_EVENT_CHANPRESS: e.type = MidiEvent::ChannelPressure; e.channel = ev->data.control.channel; e.data2 = (uint8_t)ev->data.control.value; break;
    case SND_SEQ_EVENT_KEYPRESS: e.type = MidiEvent::PolyPressure; e.channel = ev->data.note.channel; e.data1 = ev->data.note.note; e.data2 = ev->data.note.velocity; break;
    case SND_SEQ_EVENT_PGMCHANGE: e.type = MidiEvent::ProgramChange; e.channel = ev->data.control.channel; e.data1 = (uint8_t)ev->data.control.value; break;
    default: ok = false; break;
    }
    if (!ok) return;
    m_activity.fetch_add(1);
    m_engine.midiInQueue().push(e);
    if (m_learning.load() && e.type == MidiEvent::CC) { int cc = e.data1, ch = e.channel, val = e.data2; QMetaObject::invokeMethod(this, [this, cc, ch, val] { Q_EMIT ccReceived(cc, ch, val); }, Qt::QueuedConnection); }
    if (m_learning.load() && (e.type == MidiEvent::NoteOn || e.type == MidiEvent::NoteOff)) { int n = e.data1, ch = e.channel; bool on = e.type == MidiEvent::NoteOn; QMetaObject::invokeMethod(this, [this, n, ch, on] { Q_EMIT noteReceived(n, ch, on); }, Qt::QueuedConnection); }
}

static void fillAlsa(snd_seq_event_t& ae, const MidiEvent& e) {
    snd_seq_ev_clear(&ae);
    switch (e.type) {
    case MidiEvent::NoteOn: snd_seq_ev_set_noteon(&ae, e.channel, e.data1, (unsigned char)clamp((int)(e.velocity * 127), 1, 127)); break;
    case MidiEvent::NoteOff: snd_seq_ev_set_noteoff(&ae, e.channel, e.data1, e.data2); break;
    case MidiEvent::CC: snd_seq_ev_set_controller(&ae, e.channel, e.data1, e.data2); break;
    case MidiEvent::PitchBend: snd_seq_ev_set_pitchbend(&ae, e.channel, e.pitchBend); break;
    case MidiEvent::ProgramChange: snd_seq_ev_set_pgmchange(&ae, e.channel, e.data1); break;
    case MidiEvent::ChannelPressure: snd_seq_ev_set_chanpress(&ae, e.channel, e.data2); break;
    case MidiEvent::Clock: ae.type = SND_SEQ_EVENT_CLOCK; break;
    case MidiEvent::Start: ae.type = SND_SEQ_EVENT_START; break;
    case MidiEvent::Stop: ae.type = SND_SEQ_EVENT_STOP; break;
    case MidiEvent::Continue: ae.type = SND_SEQ_EVENT_CONTINUE; break;
    case MidiEvent::AllNotesOff: snd_seq_ev_set_controller(&ae, e.channel, 123, 0); break;
    default: break;
    }
}

void MidiEngine::flushOutput() {
    if (!m_seq) return; snd_seq_t* seq = (snd_seq_t*)m_seq; TimedMidiOut t; int sent = 0;
    while (m_engine.midiOutQueue().pop(t)) { snd_seq_event_t ae; fillAlsa(ae, t.event); snd_seq_ev_set_source(&ae, m_outPort); snd_seq_ev_set_subs(&ae); snd_seq_ev_set_direct(&ae); snd_seq_event_output(seq, &ae); if (++sent > 512) break; }
    if (sent) snd_seq_drain_output(seq);
}

void MidiEngine::sendNow(const MidiEvent& e, int) { if (!m_seq) return; snd_seq_t* seq = (snd_seq_t*)m_seq; snd_seq_event_t ae; fillAlsa(ae, e); snd_seq_ev_set_source(&ae, m_outPort); snd_seq_ev_set_subs(&ae); snd_seq_ev_set_direct(&ae); snd_seq_event_output_direct(seq, &ae); }

} // namespace mt
