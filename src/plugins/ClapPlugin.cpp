#include "plugins/ClapPlugin.h"
#include "plugins/PluginEmbedWidget.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QThread>
#include "core/DynLib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mt {
using namespace clap;

static thread_local bool t_audioThread = false;   // this thread is making the plugin's audio-thread calls right now

static bool hasFeature(const clap_plugin_descriptor* d, const char* f) {
    for (auto* p = d->features; p && *p; ++p)
        if (std::strcmp(*p, f) == 0) return true;
    return false;
}
// deleteLater: the plugin may unregister a timer / fd from inside its own callback
static void dropTimer(QTimer* t) {
    t->stop();
    t->deleteLater();
}
static void dropNotifier(QSocketNotifier* n) {
    n->setEnabled(false);
    n->deleteLater();
}

// ---------------------------------------------------------------- module
std::shared_ptr<ClapModule> ClapModule::load(const QString& path, QString* error) {
    // clap_entry init/deinit pair once per loaded binary, so all instances of one file share the module (UI thread only)
    static std::map<QString, std::weak_ptr<ClapModule>> loaded;
    if (auto m = loaded[path].lock()) return m;
    void* h = dynlib::open(path);
    if (!h) {
        if (error) *error = dynlib::lastError();
        return nullptr;
    }
    auto m = std::shared_ptr<ClapModule>(new ClapModule());
    m->m_handle = h;
    auto* entry = (const clap_plugin_entry*)dynlib::symbol(h, "clap_entry");
    if (!entry || !clap_version_is_compatible(entry->clap_version)) {
        if (error) *error = entry ? "Incompatible CLAP version" : "clap_entry not exported (not a CLAP plugin)";
        return nullptr;
    }
    if (!entry->init(path.toLocal8Bit().constData())) {
        if (error) *error = "clap_entry.init failed";
        return nullptr;
    }
    m->m_entry = entry;
    m->m_factory = (const clap_plugin_factory*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!m->m_factory) {
        if (error) *error = "No CLAP plugin factory";
        return nullptr;
    }
    loaded[path] = m;
    return m;
}
ClapModule::~ClapModule() {
    if (m_entry) m_entry->deinit();
    if (m_handle) dynlib::close(m_handle);
}
std::vector<const clap_plugin_descriptor*> ClapModule::descriptors() const {
    std::vector<const clap_plugin_descriptor*> v;
    for (uint32_t i = 0, n = m_factory->get_plugin_count(m_factory); i < n; ++i) {
        auto* d = m_factory->get_plugin_descriptor(m_factory, i);
        if (d && d->id && clap_version_is_compatible(d->clap_version)) v.push_back(d);
    }
    return v;
}

// ---------------------------------------------------------------- host side
struct ClapHost {
    static ClapPlugin* self(const clap_host* h) { return (ClapPlugin*)h->host_data; }
    static bool onMainThread() { return !t_audioThread && QThread::currentThread() == QCoreApplication::instance()->thread(); }
    static bool registerFd(const clap_host* h, int fd, uint32_t flags) {
        auto* p = self(h);
        for (auto* n : p->m_fds[fd]) dropNotifier(n);
        p->m_fds[fd].clear();
        const std::pair<uint32_t, QSocketNotifier::Type> kinds[] = {{CLAP_POSIX_FD_READ, QSocketNotifier::Read},
            {CLAP_POSIX_FD_WRITE, QSocketNotifier::Write}, {CLAP_POSIX_FD_ERROR, QSocketNotifier::Exception}};
        for (auto& k : kinds) {
            if (!(flags & k.first)) continue;
            auto* n = new QSocketNotifier(fd, k.second);
            QObject::connect(n, &QSocketNotifier::activated, [p, fd, f = k.first] {
                if (p->m_fdExt) p->m_fdExt->on_fd(p->m_plugin, fd, f);
            });
            p->m_fds[fd].push_back(n);
        }
        return true;
    }
    static const void* extension(const clap_host*, const char* id) {
        static const clap_host_params params = {
            [](const clap_host* h, uint32_t flags) {
                if (!(flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL))) return;
                self(h)->refreshParamsFromPlugin();   // e.g. a preset loaded in the plugin's GUI
                self(h)->m_edited = true;
            },
            [](const clap_host*, clap_id, uint32_t) {},
            [](const clap_host*) {},   // process() runs every block and collects the plugin's pending changes
        };
        static const clap_host_state state = {[](const clap_host* h) { self(h)->m_edited = true; }};
        static const clap_host_latency latency = {[](const clap_host* h) { self(h)->m_latencyDirty = true; }};   // re-read after activate
        static const clap_host_gui gui = {
            [](const clap_host*) {},
            [](const clap_host* h, uint32_t w, uint32_t ht) {
                QPointer<PluginEmbedWidget> e = self(h)->m_embed;
                if (!e) return false;
                QMetaObject::invokeMethod(e, [e, w, ht] {
                    if (e) e->requestResize((int)w, (int)ht);
                });   // direct on the UI thread, queued from any other
                return true;
            },
            [](const clap_host*) { return false; },
            [](const clap_host*) { return false; },
            [](const clap_host*, bool) {},
        };
        static const clap_host_timer_support timers = {
            [](const clap_host* h, uint32_t ms, clap_id* id) {
                auto* p = self(h);
                clap_id tid = p->m_nextTimerId++;
                auto* t = new QTimer();
                QObject::connect(t, &QTimer::timeout, [p, tid] {
                    if (p->m_timerExt) p->m_timerExt->on_timer(p->m_plugin, tid);
                });
                t->start((int)std::max<uint32_t>(ms, 1));
                p->m_timers[tid] = t;
                *id = tid;
                return true;
            },
            [](const clap_host* h, clap_id id) {
                auto& ts = self(h)->m_timers;
                auto it = ts.find(id);
                if (it == ts.end()) return false;
                dropTimer(it->second);
                ts.erase(it);
                return true;
            },
        };
        static const clap_host_posix_fd_support fds = {
            registerFd,
            registerFd,
            [](const clap_host* h, int fd) {
                auto& fs = self(h)->m_fds;
                auto it = fs.find(fd);
                if (it == fs.end()) return false;
                for (auto* n : it->second) dropNotifier(n);
                fs.erase(it);
                return true;
            },
        };
        static const clap_host_thread_check threads = {
            [](const clap_host*) { return onMainThread(); },
            [](const clap_host*) { return t_audioThread; },
        };
        static const clap_host_log log = {[](const clap_host* h, int32_t severity, const char* msg) {
            if (severity == CLAP_LOG_DEBUG) return;
            if (!onMainThread()) {   // the log sink feeds UI widgets
                std::fprintf(stderr, "[CLAP] %s\n", msg);
                return;
            }
            QString m = "[CLAP] " + self(h)->m_name + ": " + QString::fromUtf8(msg);
            if (severity >= CLAP_LOG_ERROR) Log::error(m);
            else if (severity == CLAP_LOG_WARNING) Log::warn(m);
            else Log::info(m);
        }};
        const std::pair<const char*, const void*> exts[] = {{CLAP_EXT_PARAMS, &params}, {CLAP_EXT_STATE, &state},
            {CLAP_EXT_LATENCY, &latency}, {CLAP_EXT_GUI, &gui}, {CLAP_EXT_TIMER_SUPPORT, &timers}, {CLAP_EXT_POSIX_FD_SUPPORT, &fds},
            {CLAP_EXT_THREAD_CHECK, &threads}, {CLAP_EXT_LOG, &log}};
        for (auto& e : exts)
            if (std::strcmp(id, e.first) == 0) return e.second;
        return nullptr;
    }
};

// ---------------------------------------------------------------- plugin
std::unique_ptr<ClapPlugin> ClapPlugin::create(const QString& path, const QString& pluginId, QString* error) {
    std::unique_ptr<ClapPlugin> p(new ClapPlugin());
    if (!p->init(path, pluginId, error)) return nullptr;
    return p;
}

bool ClapPlugin::init(const QString& path, const QString& pluginId, QString* error) {
    m_module = ClapModule::load(path, error);
    if (!m_module) return false;
    const clap_plugin_descriptor* desc = nullptr;
    for (auto* d : m_module->descriptors())
        if (!desc && (pluginId.isEmpty() || pluginId == QString::fromUtf8(d->id))) desc = d;
    if (!desc) {
        if (error) *error = "Plugin id not found in module";
        return false;
    }
    m_id = QString::fromUtf8(desc->id);
    m_name = QString::fromUtf8(desc->name);
    m_isInstrument = hasFeature(desc, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    m_host = {CLAP_VERSION, this, "Vybrant", "Vybrant", "", "1.0", ClapHost::extension,
        [](const clap_host* h) { ClapHost::self(h)->m_restartRequested = true; },
        [](const clap_host*) {},   // the engine calls process() every block anyway
        [](const clap_host* h) { ClapHost::self(h)->m_callbackRequested = true; }};
    m_plugin = m_module->factory()->create_plugin(m_module->factory(), &m_host, desc->id);
    if (!m_plugin) {
        if (error) *error = "create_plugin failed";
        return false;
    }
    if (!m_plugin->init(m_plugin)) {
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
        if (error) *error = "clap_plugin.init failed";
        return false;
    }
    auto ext = [this](const char* id) { return m_plugin->get_extension(m_plugin, id); };
    m_paramsExt = (const clap_plugin_params*)ext(CLAP_EXT_PARAMS);
    m_state = (const clap_plugin_state*)ext(CLAP_EXT_STATE);
    m_latencyExt = (const clap_plugin_latency*)ext(CLAP_EXT_LATENCY);
    m_gui = (const clap_plugin_gui*)ext(CLAP_EXT_GUI);
    m_timerExt = (const clap_plugin_timer_support*)ext(CLAP_EXT_TIMER_SUPPORT);
    m_fdExt = (const clap_plugin_posix_fd_support*)ext(CLAP_EXT_POSIX_FD_SUPPORT);
    m_hasEditor = m_gui && m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_X11, false);
    if (auto* ap = (const clap_plugin_audio_ports*)ext(CLAP_EXT_AUDIO_PORTS))
        for (bool input : {true, false}) {
            auto& chans = input ? m_inChans : m_outChans;
            int& main = input ? m_mainIn : m_mainOut;
            for (uint32_t i = 0, n = ap->count(m_plugin, input); i < n; ++i) {
                clap_audio_port_info info{};
                if (!ap->get(m_plugin, i, input, &info)) info.channel_count = 0;
                if (main < 0 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)) main = (int)i;
                chans.push_back(info.channel_count);
                if (!input) m_outNames << QString::fromUtf8(info.name);
            }
            if (main < 0 && !chans.empty()) main = 0;
        }
    for (int i = 0; i < (int)m_outChans.size(); ++i)
        if (i != m_mainOut && m_outChans[i] > 0) m_auxOuts.push_back(i);
    if (auto* np = (const clap_plugin_note_ports*)ext(CLAP_EXT_NOTE_PORTS)) {
        clap_note_port_info info{};
        if (np->count(m_plugin, true) > 0 && np->get(m_plugin, 0, true, &info)) {
            m_clapNotes = info.supported_dialects & CLAP_NOTE_DIALECT_CLAP;
            m_midiNotes = info.supported_dialects & CLAP_NOTE_DIALECT_MIDI;
        }
    }
    for (uint32_t i = 0, n = m_paramsExt ? m_paramsExt->count(m_plugin) : 0; i < n; ++i) {
        clap_param_info pi{};
        if (!m_paramsExt->get_info(m_plugin, i, &pi)) continue;
        pi.name[CLAP_NAME_SIZE - 1] = 0;
        ParamDesc d;
        d.id = pi.id;
        d.name = pi.name;
        d.min = (float)pi.min_value;
        d.max = (float)pi.max_value;
        d.def = (float)pi.default_value;
        d.automatable = (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) && !(pi.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_HIDDEN));
        if (pi.flags & CLAP_PARAM_IS_STEPPED) d.steps = (int)std::lround(pi.max_value - pi.min_value) + 1;
        if (d.steps == 2 && d.min == 0.f) d.unit = ParamUnit::Toggle;
        m_params.add(d);
        m_paramIds.push_back(pi.id);
        m_cookies.push_back(pi.cookie);
    }
    m_lastSent.resize(m_paramIds.size());
    refreshParamsFromPlugin();
    m_evStore.resize(MidiEventList::kCapacity + m_paramIds.size() + 16);
    m_evOrder.resize(m_evStore.size());
    m_inEvents = {this, [](const clap_input_events* l) { return ((ClapPlugin*)l->ctx)->m_evCount; },
        [](const clap_input_events* l, uint32_t i) {
            auto* p = (ClapPlugin*)l->ctx;
            return i < p->m_evCount ? p->m_evOrder[i] : nullptr;
        }};
    // parameter changes made by the plugin (its GUI) land in the shared parameter set, like VST3 performEdit
    m_outEvents = {this, [](const clap_output_events* l, const clap_event_header* e) {
                       auto* p = (ClapPlugin*)l->ctx;
                       if (e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE) return true;
                       auto* pv = (const clap_event_param_value*)e;
                       int idx = p->m_params.indexOf(pv->param_id);
                       if (idx < 0) return true;
                       p->m_params.setAt(idx, (float)pv->value);
                       p->m_lastSent[idx] = p->m_params.getAt(idx);
                       p->m_lastEdited = pv->param_id;
                       p->m_edited = true;
                       return true;
                   }};
    QObject::connect(&m_mainTimer, &QTimer::timeout, [this] {
        if (m_callbackRequested.exchange(false)) m_plugin->on_main_thread(m_plugin);
        restartIfRequested();
        if (m_edited.exchange(false) && onParamEdited) onParamEdited(m_lastEdited, m_params.get(m_lastEdited));
    });
    m_mainTimer.start(16);
    return true;
}

ClapPlugin::~ClapPlugin() {
    m_mainTimer.stop();
    if (!m_plugin) return;
    if (m_embed) {   // the editor widget may outlive us: its callbacks must not reach a destroyed plugin
        m_embed->onAttach = nullptr;
        m_embed->onDetach = nullptr;
        m_embed->onResized = nullptr;
    }
    if (m_guiCreated) m_gui->destroy(m_plugin);
    deactivate();
    m_plugin->destroy(m_plugin);
    for (auto& [id, t] : m_timers) dropTimer(t);
    for (auto& [fd, ns] : m_fds)
        for (auto* n : ns) dropNotifier(n);
}

void ClapPlugin::activate() {
    if (m_active) return;
    if (!m_plugin->activate(m_plugin, m_sr, 1, (uint32_t)m_maxBlock)) {
        Log::warn("CLAP plugin failed to activate: " + m_name);
        return;
    }
    m_active = true;
    int lat = m_latencyExt ? (int)m_latencyExt->get(m_plugin) : 0;
    if (m_latency.exchange(lat) != lat) m_latencyDirty = true;
}
void ClapPlugin::deactivate() {
    if (!m_active) return;
    if (m_processing) {   // the audio thread no longer runs this plugin, so this thread stands in for it
        t_audioThread = true;
        m_plugin->stop_processing(m_plugin);
        t_audioThread = false;
        m_processing = false;
    }
    m_plugin->deactivate(m_plugin);
    m_active = false;
}
// request_restart: deactivate + activate on the UI thread, once the audio thread has handed the plugin over
void ClapPlugin::restartIfRequested() {
    if (!m_restartRequested) return;
    if (!m_active) {   // the next prepare() activates it with fresh settings
        m_restartRequested = false;
        return;
    }
    int st = m_rt.load();
    if (st == 0) {
        m_rt = 1;
        m_restartWait = 0;
        return;
    }
    // no block came for a few ticks: the audio thread isn't running this plugin, so take it over
    if (st == 1 && (++m_restartWait < 4 || !m_rt.compare_exchange_strong(st, 3))) return;
    if (m_inProcess) return;   // a block is still running (inside stop_processing, or one that began before the request): next tick
    deactivate();
    activate();
    m_restartRequested = false;
    m_rt = 0;
}

void ClapPlugin::prepare(double sr, int maxBlock) {
    deactivate();
    m_sr = sr;
    m_maxBlock = maxBlock;
    m_rt = 0;
    m_restartRequested = false;
    m_scratch.clear();
    auto setup = [&](const std::vector<uint32_t>& chans, std::vector<std::vector<float*>>& ptrs, std::vector<clap_audio_buffer>& bufs) {
        ptrs.assign(chans.size(), {});
        bufs.assign(chans.size(), clap_audio_buffer{});
        for (size_t p = 0; p < chans.size(); ++p) {
            for (uint32_t c = 0; c < chans[p]; ++c) ptrs[p].push_back(m_scratch.emplace_back(maxBlock, 0.f).data());
            bufs[p].data32 = ptrs[p].data();
            bufs[p].channel_count = chans[p];
        }
    };
    setup(m_inChans, m_inPtrs, m_inBufs);
    setup(m_outChans, m_outPtrs, m_outBufs);
    activate();
}

void ClapPlugin::refreshParamsFromPlugin() {
    for (size_t i = 0; i < m_paramIds.size(); ++i) {
        double v;
        if (!m_paramsExt->get_value(m_plugin, m_paramIds[i], &v)) continue;
        m_params.setAt((int)i, (float)v);
        m_lastSent[i] = m_params.getAt((int)i);
    }
}

clap_event_header* ClapPlugin::addEvent(uint32_t time, uint16_t type, uint32_t size) {
    if (m_evCount >= m_evStore.size()) return nullptr;
    auto* e = &m_evStore[m_evCount].header;
    *e = {size, time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    uint32_t i = m_evCount++;
    for (; i > 0 && m_evOrder[i - 1]->time > time; --i) m_evOrder[i] = m_evOrder[i - 1];   // CLAP wants input events sorted by time
    m_evOrder[i] = e;
    return e;
}

void ClapPlugin::addMidi(const MidiEvent& e, uint32_t t) {
    bool note = e.type == MidiEvent::NoteOn || e.type == MidiEvent::NoteOff || e.type == MidiEvent::AllNotesOff;
    if (note && m_clapNotes) {
        bool all = e.type == MidiEvent::AllNotesOff;
        if (auto* h = addEvent(t, e.type == MidiEvent::NoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note))) {
            auto* ne = (clap_event_note*)h;
            ne->note_id = all ? -1 : e.noteId;
            ne->port_index = 0;
            ne->channel = e.channel;
            ne->key = all ? -1 : e.data1;   // -1 matches every key
            ne->velocity = e.velocity;
        }
        return;
    }
    if (!m_midiNotes) return;
    uint8_t ch = e.channel & 0x0F, d[3] = {0, 0, 0};
    switch (e.type) {
    case MidiEvent::NoteOn: d[0] = 0x90 | ch, d[1] = e.data1, d[2] = (uint8_t)std::clamp((int)(e.velocity * 127), 1, 127); break;
    case MidiEvent::NoteOff: d[0] = 0x80 | ch, d[1] = e.data1, d[2] = 64; break;
    case MidiEvent::AllNotesOff: d[0] = 0xB0 | ch, d[1] = 123; break;
    case MidiEvent::CC: d[0] = 0xB0 | ch, d[1] = e.data1, d[2] = e.data2; break;
    case MidiEvent::PitchBend: d[0] = 0xE0 | ch, d[1] = (e.pitchBend + 8192) & 0x7F, d[2] = ((e.pitchBend + 8192) >> 7) & 0x7F; break;
    case MidiEvent::ChannelPressure: d[0] = 0xD0 | ch, d[1] = e.data2; break;
    case MidiEvent::PolyPressure: d[0] = 0xA0 | ch, d[1] = e.data1, d[2] = e.data2; break;
    case MidiEvent::ProgramChange: d[0] = 0xC0 | ch, d[1] = e.data1; break;
    default: return;
    }
    if (auto* h = addEvent(t, CLAP_EVENT_MIDI, sizeof(clap_event_midi))) {
        auto* me = (clap_event_midi*)h;
        me->port_index = 0;
        std::memcpy(me->data, d, 3);
    }
}

void ClapPlugin::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    t_audioThread = true;
    m_inProcess = true;
    int st = m_rt.load();
    if (st == 1 && m_rt.compare_exchange_strong(st, 2) && m_processing) {   // hand the plugin to the UI thread for a restart
        m_plugin->stop_processing(m_plugin);
        m_processing = false;
    }
    if (st == 0 && m_active && !m_processing) m_processing = m_plugin->start_processing(m_plugin);
    for (auto& port : m_outPtrs)   // also what the extra outputs show when this block doesn't run
        for (float* ch : port) std::fill(ch, ch + n, 0.f);
    if (st != 0 || !m_processing) {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
        m_inProcess = false;
        t_audioThread = false;
        return;
    }
    // parameter changes from automation / UI, then notes and MIDI
    m_evCount = 0;
    for (size_t i = 0; i < m_paramIds.size(); ++i) {
        float v = m_params.getAt((int)i);
        if (v == m_lastSent[i]) continue;
        m_lastSent[i] = v;
        if (auto* h = addEvent(0, CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value))) {
            auto* pv = (clap_event_param_value*)h;
            pv->param_id = m_paramIds[i];
            pv->cookie = m_cookies[i];
            pv->note_id = -1;
            pv->port_index = pv->channel = pv->key = -1;
            pv->value = v;
        }
    }
    for (int i = 0; i < midi.count; ++i) addMidi(midi.events[i], (uint32_t)std::max(0, std::min(midi.events[i].sampleOffset, n - 1)));
    // audio: the main input gets the track's stereo pair, every other input port silence
    for (size_t p = 0; p < m_inPtrs.size(); ++p)
        for (uint32_t c = 0; c < m_inChans[p]; ++c) {
            float* dst = m_inPtrs[p][c];
            if ((int)p == m_mainIn && m_inChans[p] == 1)
                for (int i = 0; i < n; ++i) dst[i] = (L[i] + R[i]) * 0.5f;
            else if ((int)p == m_mainIn && c < 2) std::copy(c ? R : L, (c ? R : L) + n, dst);
            else std::fill(dst, dst + n, 0.f);
        }
    // transport
    auto& tr = m_transport;
    tr.header = {sizeof(clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
        CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (ctx.playing ? CLAP_TRANSPORT_IS_PLAYING : 0u) |
        (ctx.recording ? CLAP_TRANSPORT_IS_RECORDING : 0u) | (ctx.looping ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0u);
    auto beats = [](double b) { return (clap_beattime)std::llround(b * CLAP_BEATTIME_FACTOR); };
    auto beatSecs = [&](double b) { return (clap_sectime)std::llround(b * 60.0 / ctx.bpm * CLAP_SECTIME_FACTOR); };
    tr.song_pos_beats = beats(ctx.positionBeats);
    tr.song_pos_seconds = (clap_sectime)std::llround(ctx.positionSamples / m_sr * CLAP_SECTIME_FACTOR);
    tr.tempo = ctx.bpm;
    tr.tempo_inc = 0;
    tr.loop_start_beats = beats(ctx.loopStartBeats);
    tr.loop_end_beats = beats(ctx.loopEndBeats);
    tr.loop_start_seconds = beatSecs(ctx.loopStartBeats);
    tr.loop_end_seconds = beatSecs(ctx.loopEndBeats);
    tr.bar_start = beats(ctx.barStartBeats);
    tr.bar_number = (int32_t)ctx.barNumber;
    tr.tsig_num = (uint16_t)ctx.tsNum;
    tr.tsig_denom = (uint16_t)ctx.tsDen;
    clap_process pr{m_steadyTime, (uint32_t)n, &tr, m_inBufs.data(), m_outBufs.data(), (uint32_t)m_inBufs.size(),
        (uint32_t)m_outBufs.size(), &m_inEvents, &m_outEvents};
    m_plugin->process(m_plugin, &pr);
    m_steadyTime += n;
    if (m_mainOut >= 0 && m_outChans[m_mainOut] > 0) {
        auto& o = m_outPtrs[m_mainOut];
        float* right = o[o.size() > 1 ? 1 : 0];   // a mono main output feeds both sides
        std::copy(o[0], o[0] + n, L);
        std::copy(right, right + n, R);
    } else {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    }
    m_inProcess = false;
    t_audioThread = false;
}

QJsonObject ClapPlugin::saveState() const {
    QJsonObject o;
    o["key"] = key();
    o["format"] = "clap";
    QByteArray data;
    clap_ostream os{&data, [](const clap_ostream* s, const void* buf, uint64_t size) -> int64_t {
                        ((QByteArray*)s->ctx)->append((const char*)buf, (qsizetype)size);
                        return (int64_t)size;
                    }};
    if (m_state && m_state->save(m_plugin, &os)) o["pluginState"] = QString::fromLatin1(data.toBase64());
    QJsonObject params;
    for (size_t i = 0; i < m_paramIds.size(); ++i) params[QString::number(m_paramIds[i])] = m_params.getAt((int)i);
    o["params"] = params;
    return o;
}
void ClapPlugin::restoreState(const QJsonObject& s) {
    QByteArray data = QByteArray::fromBase64(s["pluginState"].toString().toLatin1());
    if (data.isEmpty() || !m_state) {   // presets of plugins without the state extension carry parameter values only
        IProcessor::restoreState(s);
        return;
    }
    struct Reader {
        const QByteArray& data;
        qsizetype pos = 0;
    } r{data};
    clap_istream is{&r, [](const clap_istream* s, void* buf, uint64_t size) -> int64_t {
                        auto* r = (Reader*)s->ctx;
                        qsizetype k = std::min<qsizetype>((qsizetype)size, r->data.size() - r->pos);
                        std::memcpy(buf, r->data.constData() + r->pos, k);
                        r->pos += k;
                        return k;
                    }};
    if (!m_state->load(m_plugin, &is)) Log::warn("CLAP plugin rejected its saved state: " + m_name);
    refreshParamsFromPlugin();   // the plugin holds these values now: only later host changes are sent to it
}

QString ClapPlugin::formatValue(uint32_t id) const {
    char buf[256] = {};
    if (m_paramsExt && m_paramsExt->value_to_text(m_plugin, id, m_params.get(id), buf, sizeof buf)) return QString::fromUtf8(buf);
    return IProcessor::formatValue(id);
}

QWidget* ClapPlugin::createEditor(QWidget* parent) {
    if (!m_hasEditor) return nullptr;
    auto* w = new PluginEmbedWidget(parent);
    m_embed = w;
    // onParamEdited is set by the window showing this editor and dies with it; m_mainTimer scopes the connection to us
    QObject::connect(w, &QObject::destroyed, &m_mainTimer, [this] { onParamEdited = nullptr; });
    w->onAttach = [this, w](unsigned long xid, int& width, int& height) {
        if (m_guiCreated || !m_gui->create(m_plugin, CLAP_WINDOW_API_X11, false)) return false;
        m_guiCreated = true;
        m_gui->set_scale(m_plugin, w->devicePixelRatioF());   // X11 sizes are physical pixels, like the embed widget's
        uint32_t gw = 0, gh = 0;
        if (m_gui->get_size(m_plugin, &gw, &gh) && gw && gh) {
            width = (int)gw;
            height = (int)gh;
        }
        clap_window win{CLAP_WINDOW_API_X11, {}};
        win.x11 = xid;
        if (!m_gui->set_parent(m_plugin, &win)) {
            m_gui->destroy(m_plugin);
            m_guiCreated = false;
            return false;
        }
        m_gui->show(m_plugin);
        return true;
    };
    w->onDetach = [this] {
        if (m_guiCreated) m_gui->destroy(m_plugin);
        m_guiCreated = false;
    };
    w->onResized = [this](int width, int height) {
        uint32_t gw = (uint32_t)width, gh = (uint32_t)height;
        if (m_guiCreated && m_gui->can_resize(m_plugin) && m_gui->adjust_size(m_plugin, &gw, &gh)) m_gui->set_size(m_plugin, gw, gh);
    };
    return w;
}

}   // namespace mt
