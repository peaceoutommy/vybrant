#include "plugins/Lv2Plugin.h"
#include "plugins/PluginHost.h"
#include "plugins/PluginEmbedWidget.h"
#include "core/Log.h"
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <dlfcn.h>
#include <X11/Xlib.h>   // last: its macros (None, Bool...) clash with Qt headers

namespace mt {

namespace {
// ---------------------------------------------------------------- URID map (process-wide: every instance and UI agree)
struct UridTable {
    std::mutex lock;
    std::unordered_map<std::string, LV2_URID> ids;
    std::deque<std::string> uris;   // id - 1 -> uri; a deque keeps c_str() stable
};
UridTable& uridTable() {
    static UridTable t;
    return t;
}
LV2_URID mapUri(LV2_URID_Map_Handle, const char* uri) {
    auto& t = uridTable();
    std::lock_guard g(t.lock);
    auto [it, added] = t.ids.try_emplace(uri, (LV2_URID)t.uris.size() + 1);
    if (added) t.uris.emplace_back(uri);
    return it->second;
}
const char* unmapUri(LV2_URID_Unmap_Handle, LV2_URID id) {
    auto& t = uridTable();
    std::lock_guard g(t.lock);
    return id >= 1 && id <= t.uris.size() ? t.uris[id - 1].c_str() : nullptr;
}
LV2_URID_Map g_map{nullptr, mapUri};
LV2_URID_Unmap g_unmap{nullptr, unmapUri};

struct Urids {
    LV2_URID atomSequence = mapUri(nullptr, LV2_ATOM__Sequence), atomChunk = mapUri(nullptr, LV2_ATOM__Chunk),
             atomFloat = mapUri(nullptr, LV2_ATOM__Float), atomDouble = mapUri(nullptr, LV2_ATOM__Double),
             atomInt = mapUri(nullptr, LV2_ATOM__Int), atomLong = mapUri(nullptr, LV2_ATOM__Long),
             eventTransfer = mapUri(nullptr, LV2_ATOM__eventTransfer), midiEvent = mapUri(nullptr, LV2_MIDI__MidiEvent),
             timePosition = mapUri(nullptr, LV2_TIME__Position), timeFrame = mapUri(nullptr, LV2_TIME__frame),
             timeSpeed = mapUri(nullptr, LV2_TIME__speed), timeBar = mapUri(nullptr, LV2_TIME__bar),
             timeBarBeat = mapUri(nullptr, LV2_TIME__barBeat), timeBeatUnit = mapUri(nullptr, LV2_TIME__beatUnit),
             timeBeatsPerBar = mapUri(nullptr, LV2_TIME__beatsPerBar), timeBeatsPerMinute = mapUri(nullptr, LV2_TIME__beatsPerMinute),
             sampleRate = mapUri(nullptr, LV2_PARAMETERS__sampleRate), minBlock = mapUri(nullptr, LV2_BUF_SIZE__minBlockLength),
             maxBlock = mapUri(nullptr, LV2_BUF_SIZE__maxBlockLength), nominalBlock = mapUri(nullptr, LV2_BUF_SIZE__nominalBlockLength),
             sequenceSize = mapUri(nullptr, LV2_BUF_SIZE__sequenceSize), updateRate = mapUri(nullptr, LV2_UI__updateRate);
};
const Urids& U() {
    static const Urids u;
    return u;
}

constexpr size_t kAtomBytes = 32768;   // atom port buffers, unless the port asks for more (rsz:minimumSize)

// ---------------------------------------------------------------- lilv world
LilvWorld* newWorld() {
    QStringList dirs = qEnvironmentVariable("LV2_PATH").split(':', Qt::SkipEmptyParts);
    dirs << QDir::homePath() + "/.lv2" << "/usr/local/lib/lv2" << "/usr/lib/lv2";
    dirs.removeDuplicates();
    LilvWorld* w = lilv_world_new();
    LilvNode* path = lilv_new_string(w, dirs.join(':').toLocal8Bit().constData());
    lilv_world_set_option(w, LILV_OPTION_LV2_PATH, path);
    lilv_node_free(path);
    lilv_world_load_all(w);
    return w;
}
// Instances (and their UIs and state) use one world for the life of the process; scans use a fresh one to see new bundles
LilvWorld* world() {
    static LilvWorld* w = newWorld();
    return w;
}

const char* const kSupportedFeatures[] = {LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_WORKER__schedule,
    LV2_BUF_SIZE__boundedBlockLength, LV2_CORE__hardRTCapable, LV2_CORE__inPlaceBroken, LV2_CORE__isLive};
QString unsupportedFeature(const LilvPlugin* p) {
    QString missing;
    LilvNodes* req = lilv_plugin_get_required_features(p);
    LILV_FOREACH(nodes, i, req) {
        const char* f = lilv_node_as_uri(lilv_nodes_get(req, i));
        if (std::none_of(std::begin(kSupportedFeatures), std::end(kSupportedFeatures), [f](const char* s) { return !strcmp(s, f); }))
            missing = f;
    }
    lilv_nodes_free(req);
    return missing;
}
bool isInstrument(LilvWorld* w, const LilvPlugin* p) {
    LilvNode* type = lilv_new_uri(w, LILV_NS_RDF "type");
    LilvNode* inst = lilv_new_uri(w, LV2_CORE__InstrumentPlugin);
    LilvNodes* types = lilv_plugin_get_value(p, type);
    bool r = types && lilv_nodes_contains(types, inst);
    if (types) lilv_nodes_free(types);
    lilv_node_free(type);
    lilv_node_free(inst);
    return r;
}
const LilvUI* findX11Ui(LilvWorld* w, const LilvUIs* uis) {
    LilvNode* x11 = lilv_new_uri(w, LV2_UI__X11UI);
    const LilvUI* found = nullptr;
    LILV_FOREACH(uis, i, uis) {
        if (lilv_ui_is_a(lilv_uis_get(uis, i), x11)) {
            found = lilv_uis_get(uis, i);
            break;
        }
    }
    lilv_node_free(x11);
    return found;
}
QString filePath(const LilvNode* uri) {
    char* p = lilv_file_uri_parse(lilv_node_as_uri(uri), nullptr);
    QString s = p ? QString::fromLocal8Bit(p) : QString();
    lilv_free(p);
    return s;
}

int midiBytes(const MidiEvent& e, uint8_t b[3]) {
    switch (e.type) {
    case MidiEvent::NoteOn: b[0] = 0x90 | e.channel, b[1] = e.data1, b[2] = (uint8_t)std::clamp((int)(e.velocity * 127), 1, 127); return 3;
    case MidiEvent::NoteOff: b[0] = 0x80 | e.channel, b[1] = e.data1, b[2] = 64; return 3;
    case MidiEvent::CC: b[0] = 0xB0 | e.channel, b[1] = e.data1, b[2] = e.data2; return 3;
    case MidiEvent::PitchBend: {
        int v = e.pitchBend + 8192;
        b[0] = 0xE0 | e.channel, b[1] = v & 0x7F, b[2] = (v >> 7) & 0x7F;
        return 3;
    }
    case MidiEvent::ChannelPressure: b[0] = 0xD0 | e.channel, b[1] = e.data2; return 2;
    case MidiEvent::PolyPressure: b[0] = 0xA0 | e.channel, b[1] = e.data1, b[2] = e.data2; return 3;
    case MidiEvent::ProgramChange: b[0] = 0xC0 | e.channel, b[1] = e.data1; return 2;
    case MidiEvent::AllNotesOff: b[0] = 0xB0 | e.channel, b[1] = 123, b[2] = 0; return 3;
    default: return 0;
    }
}
// Appends one event to an input atom:Sequence; drops it when the buffer is full
void appendEvent(std::vector<uint64_t>& buf, int64_t frames, uint32_t type, uint32_t size, const void* body) {
    auto* seq = (LV2_Atom_Sequence*)buf.data();
    uint32_t used = sizeof(LV2_Atom) + seq->atom.size, need = lv2_atom_pad_size(sizeof(LV2_Atom_Event) + size);
    if (used + need > buf.size() * sizeof(uint64_t)) return;
    auto* ev = (LV2_Atom_Event*)((uint8_t*)seq + used);
    ev->time.frames = frames;
    ev->body.type = type;
    ev->body.size = size;
    if (size) std::memcpy(ev + 1, body, size);
    seq->atom.size += need;
}
}   // namespace

// ---------------------------------------------------------------- ring
void Lv2Ring::copyIn(size_t pos, const void* src, size_t n) {
    if (!n) return;
    size_t at = pos % m_buf.size(), first = std::min(n, m_buf.size() - at);
    std::memcpy(m_buf.data() + at, src, first);
    std::memcpy(m_buf.data(), (const uint8_t*)src + first, n - first);
}
void Lv2Ring::copyOut(size_t pos, void* dst, size_t n) const {
    if (!n) return;
    size_t at = pos % m_buf.size(), first = std::min(n, m_buf.size() - at);
    std::memcpy(dst, m_buf.data() + at, first);
    std::memcpy((uint8_t*)dst + first, m_buf.data(), n - first);
}
bool Lv2Ring::push(uint32_t tag, const void* data, uint32_t size) {
    size_t w = m_write.load(std::memory_order_relaxed), r = m_read.load(std::memory_order_acquire);
    uint32_t hdr[2] = {tag, size};
    if (sizeof hdr + size > m_buf.size() - (w - r)) return false;
    copyIn(w, hdr, sizeof hdr);
    copyIn(w + sizeof hdr, data, size);
    m_write.store(w + sizeof hdr + size, std::memory_order_release);
    return true;
}
bool Lv2Ring::pop(uint32_t& tag, uint32_t& size, void* out) {
    size_t r = m_read.load(std::memory_order_relaxed), w = m_write.load(std::memory_order_acquire);
    if (r == w) return false;
    uint32_t hdr[2];
    copyOut(r, hdr, sizeof hdr);
    tag = hdr[0];
    size = hdr[1];
    copyOut(r + sizeof hdr, out, size);
    m_read.store(r + sizeof hdr + size, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------- scan
std::vector<PluginInfo> Lv2Plugin::scan() {
    std::vector<PluginInfo> out;
    LilvWorld* w = newWorld();
    LilvNode* audio = lilv_new_uri(w, LV2_CORE__AudioPort);
    LilvNode* input = lilv_new_uri(w, LV2_CORE__InputPort);
    LilvNode* output = lilv_new_uri(w, LV2_CORE__OutputPort);
    QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const LilvPlugins* all = lilv_world_get_all_plugins(w);
    LILV_FOREACH(plugins, it, all) {
        const LilvPlugin* p = lilv_plugins_get(all, it);
        PluginInfo info;
        info.format = "lv2";
        info.uid = lilv_node_as_uri(lilv_plugin_get_uri(p));
        info.path = QDir::cleanPath(filePath(lilv_plugin_get_bundle_uri(p)));
        info.lastScanned = now;
        LilvNode* name = lilv_plugin_get_name(p);
        info.name = name ? QString::fromUtf8(lilv_node_as_string(name)) : info.uid;
        lilv_node_free(name);
        if (!lilv_plugin_verify(p)) {
            info.failed = true;
            info.failReason = "Invalid LV2 description";
            out.push_back(info);
            continue;
        }
        if (LilvNode* author = lilv_plugin_get_author_name(p)) {
            info.vendor = QString::fromUtf8(lilv_node_as_string(author));
            lilv_node_free(author);
        }
        info.category = isInstrument(w, p) ? "Instrument" : "Effect";
        if (const LilvNode* label = lilv_plugin_class_get_label(lilv_plugin_get_class(p)))
            info.subCategories = QString::fromUtf8(lilv_node_as_string(label));
        info.numInputs = (int)lilv_plugin_get_num_ports_of_class(p, audio, input, (LilvNode*)nullptr);
        info.numOutputs = (int)lilv_plugin_get_num_ports_of_class(p, audio, output, (LilvNode*)nullptr);
        if (LilvUIs* uis = lilv_plugin_get_uis(p)) {
            info.hasEditor = findX11Ui(w, uis) != nullptr;
            lilv_uis_free(uis);
        }
        if (QString f = unsupportedFeature(p); !f.isEmpty()) {
            info.failed = true;
            info.failReason = "Requires unsupported LV2 feature " + f;
        }
        out.push_back(info);
    }
    lilv_node_free(audio);
    lilv_node_free(input);
    lilv_node_free(output);
    lilv_world_free(w);
    return out;
}

// ---------------------------------------------------------------- plugin
Lv2Plugin::Lv2Plugin() {
    const Urids& u = U();
    auto opt = [](LV2_URID key, uint32_t size, LV2_URID type, const void* value) {
        return LV2_Options_Option{LV2_OPTIONS_INSTANCE, 0, key, size, type, value};
    };
    m_options[0] = opt(u.sampleRate, sizeof(float), u.atomFloat, &m_optSampleRate);
    m_options[1] = opt(u.minBlock, sizeof(int32_t), u.atomInt, &m_optMinBlock);
    m_options[2] = opt(u.maxBlock, sizeof(int32_t), u.atomInt, &m_optMaxBlock);
    m_options[3] = opt(u.nominalBlock, sizeof(int32_t), u.atomInt, &m_optMaxBlock);
    m_options[4] = opt(u.sequenceSize, sizeof(int32_t), u.atomInt, &m_optSeqSize);
    m_options[5] = opt(u.updateRate, sizeof(float), u.atomFloat, &m_optUpdateRate);
    m_options[6] = opt(0, 0, 0, nullptr);
    m_schedule = {this, &Lv2Plugin::scheduleWork};
    m_featureList[0] = {LV2_URID__map, &g_map};
    m_featureList[1] = {LV2_URID__unmap, &g_unmap};
    m_featureList[2] = {LV2_OPTIONS__options, m_options};
    m_featureList[3] = {LV2_WORKER__schedule, &m_schedule};
    m_featureList[4] = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
    for (int i = 0; i < 5; ++i) m_features[i] = &m_featureList[i];
    m_features[5] = nullptr;
    lv2_atom_forge_init(&m_forge, &g_map);
    m_posBuf.assign(64, 0);
    m_rtMsg.resize(std::max(m_fromUi.capacity(), m_workResponses.capacity()));
}
Lv2Plugin::~Lv2Plugin() {
    if (m_uiWidget) {   // the editor can outlive us by a tick (the window rebuilds on its next timer)
        m_uiWidget->onAttach = nullptr;
        m_uiWidget->onDetach = nullptr;
        m_uiWidget->onIdle = nullptr;
        m_uiWidget->onResized = nullptr;
    }
    closeUi();
    freeInstance();
    if (m_uis) lilv_uis_free(m_uis);
}

std::unique_ptr<Lv2Plugin> Lv2Plugin::create(const QString& uri, const QString& bundlePath, QString* error) {
    LilvWorld* w = world();
    LilvNode* u = lilv_new_uri(w, uri.toUtf8().constData());
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), u);
    if (!p && !bundlePath.isEmpty()) {   // installed after the world was loaded, or outside the search path
        LilvNode* b = lilv_new_file_uri(w, nullptr, (QDir::cleanPath(bundlePath) + "/").toLocal8Bit().constData());
        lilv_world_load_bundle(w, b);
        lilv_node_free(b);
        p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), u);
    }
    lilv_node_free(u);
    QString missing = p ? unsupportedFeature(p) : QString();
    if (!p || !missing.isEmpty()) {
        if (error) *error = p ? "Requires unsupported LV2 feature " + missing : "LV2 plugin not found: " + uri;
        return nullptr;
    }
    std::unique_ptr<Lv2Plugin> self(new Lv2Plugin());
    self->init(p);
    return self;
}

void Lv2Plugin::init(const LilvPlugin* p) {
    LilvWorld* w = world();
    m_plugin = p;
    m_uri = lilv_node_as_uri(lilv_plugin_get_uri(p));
    LilvNode* name = lilv_plugin_get_name(p);
    m_name = name ? QString::fromUtf8(lilv_node_as_string(name)) : m_uri;
    lilv_node_free(name);
    m_isInstrument = isInstrument(w, p);
    m_uis = lilv_plugin_get_uis(p);
    if (m_uis) m_ui = findX11Ui(w, m_uis);
    auto node = [w](const char* uri) { return lilv_new_uri(w, uri); };
    LilvNode* audio = node(LV2_CORE__AudioPort);
    LilvNode* control = node(LV2_CORE__ControlPort);
    LilvNode* atom = node(LV2_ATOM__AtomPort);
    LilvNode* input = node(LV2_CORE__InputPort);
    LilvNode* midiEvent = node(LV2_MIDI__MidiEvent);
    LilvNode* position = node(LV2_TIME__Position);
    LilvNode* toggled = node(LV2_CORE__toggled);
    LilvNode* integer = node(LV2_CORE__integer);
    LilvNode* enumeration = node(LV2_CORE__enumeration);
    LilvNode* logarithmic = node(LV2_PORT_PROPS__logarithmic);
    LilvNode* sideChain = node(LV2_CORE_PREFIX "isSideChain");
    LilvNode* minSize = node(LV2_RESIZE_PORT__minimumSize);
    uint32_t n = lilv_plugin_get_num_ports(p);
    m_ports.resize(n);
    m_control.assign(n, 0.f);
    m_outValues = std::make_unique<std::atomic<float>[]>(n);
    m_audio.resize(n);
    m_atom.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
        Port& pt = m_ports[i];
        pt.symbol = lilv_node_as_string(lilv_port_get_symbol(p, port));
        pt.input = lilv_port_is_a(p, port, input);
        if (lilv_port_is_a(p, port, control)) {
            pt.type = PortType::Control;
            if (!pt.input) {
                m_controlOuts.push_back(i);
                continue;
            }
            LilvNode* nm = lilv_port_get_name(p, port);
            LilvNode *def = nullptr, *min = nullptr, *max = nullptr;
            lilv_port_get_range(p, port, &def, &min, &max);
            ParamDesc d;
            d.id = i;
            d.name = nm ? lilv_node_as_string(nm) : pt.symbol;
            d.min = min ? lilv_node_as_float(min) : 0.f;
            d.max = max ? lilv_node_as_float(max) : 1.f;
            if (!(d.max > d.min)) d.max = d.min + 1.f;
            d.def = std::clamp(def ? lilv_node_as_float(def) : d.min, d.min, d.max);
            if (lilv_port_has_property(p, port, toggled)) {
                d.unit = ParamUnit::Toggle;
                d.steps = 2;
            } else if (lilv_port_has_property(p, port, integer) || lilv_port_has_property(p, port, enumeration))
                d.steps = (int)std::lround(d.max - d.min) + 1;
            else d.logarithmic = d.min >= 0.f && lilv_port_has_property(p, port, logarithmic);
            lilv_node_free(nm);
            lilv_node_free(def);
            lilv_node_free(min);
            lilv_node_free(max);
            if (LilvScalePoints* sp = lilv_port_get_scale_points(p, port)) {
                LILV_FOREACH(scale_points, it, sp) {
                    const LilvScalePoint* s = lilv_scale_points_get(sp, it);
                    pt.scalePoints.emplace_back(lilv_node_as_float(lilv_scale_point_get_value(s)),
                        QString::fromUtf8(lilv_node_as_string(lilv_scale_point_get_label(s))));
                }
                lilv_scale_points_free(sp);
            }
            pt.param = m_params.count();
            m_params.add(d);
            m_controlIns.push_back(i);
        } else if (lilv_port_is_a(p, port, audio)) {
            pt.type = PortType::Audio;
            if (!pt.input) {
                m_audioOuts.push_back(i);
                LilvNode* nm = lilv_port_get_name(p, port);
                m_outNames << QString::fromUtf8(nm ? lilv_node_as_string(nm) : pt.symbol.c_str());
                if (nm) lilv_node_free(nm);
            }
            else (lilv_port_has_property(p, port, sideChain) ? m_sideIns : m_mainIns).push_back(i);
        } else if (lilv_port_is_a(p, port, atom)) {
            pt.type = PortType::Atom;
            pt.midi = lilv_port_supports_event(p, port, midiEvent);
            pt.time = pt.input && lilv_port_supports_event(p, port, position);
            m_wantsTime = m_wantsTime || pt.time;
            size_t bytes = kAtomBytes;
            if (LilvNodes* sz = lilv_port_get_value(p, port, minSize)) {
                bytes = std::max<size_t>(bytes, (size_t)std::max(0, lilv_node_as_int(lilv_nodes_get_first(sz))));
                lilv_nodes_free(sz);
            }
            m_atom[i].assign((bytes + 7) / 8, 0);
            (pt.input ? m_atomIns : m_atomOuts).push_back(i);
            if (pt.input && pt.midi && m_midiIn < 0) m_midiIn = (int)i;
        }
    }
    if (lilv_plugin_has_latency(p)) m_latencyPort = (int)lilv_plugin_get_latency_port_index(p);
    m_optSeqSize = (int32_t)kAtomBytes;
    for (LilvNode* nd : {audio, control, atom, input, midiEvent, position, toggled, integer, enumeration, logarithmic, sideChain, minSize})
        lilv_node_free(nd);
}

// Bounded: a plugin that crashed inside run() (the engine's crash guard jumps out of process()) never releases the flag
void Lv2Plugin::lockRun() {
    for (int i = 0; i < 2000 && m_busy.test_and_set(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void Lv2Plugin::prepare(double sr, int maxBlock) {
    if (m_instance && sr == m_sr && maxBlock == m_maxBlock) return;
    lockRun();
    bool reopenUi = m_uiHandle && m_uiWidget;
    closeUi();   // it holds the old instance (instance-access)
    if (m_instance) m_pendingState = stateBytes();   // carried over to the instance at the new rate / block size
    freeInstance();
    m_sr = sr;
    m_maxBlock = maxBlock;
    instantiate();
    unlockRun();
    int w, h;
    if (reopenUi && m_instance) openUi((unsigned long)m_uiWidget->winId(), w, h);
}

bool Lv2Plugin::instantiate() {
    m_optSampleRate = (float)m_sr;
    m_optMaxBlock = m_maxBlock;
    m_instance = lilv_plugin_instantiate(m_plugin, m_sr, m_features);
    if (!m_instance) {
        Log::error("LV2 instantiation failed: " + m_uri);
        return false;
    }
    for (uint32_t i = 0; i < m_ports.size(); ++i) {
        void* buf = nullptr;
        if (m_ports[i].type == PortType::Control) buf = &m_control[i];
        else if (m_ports[i].type == PortType::Atom) buf = m_atom[i].data();
        else {
            m_audio[i].assign(m_maxBlock, 0.f);
            buf = m_audio[i].data();
        }
        lilv_instance_connect_port(m_instance, i, buf);
    }
    m_worker = (const LV2_Worker_Interface*)lilv_instance_get_extension_data(m_instance, LV2_WORKER__interface);
    m_workQuit = false;
    if (m_worker) m_workThread = std::thread(&Lv2Plugin::workLoop, this);
    if (!m_pendingState.isEmpty()) applyState(m_pendingState);
    m_pendingState.clear();
    lilv_instance_activate(m_instance);
    // one silent block, so the latency port holds a value before the engine computes delay compensation
    static const MidiEventList kNoMidi{};
    std::vector<float> l(m_maxBlock), r(m_maxBlock);
    runBlock(l.data(), r.data(), m_maxBlock, kNoMidi, ProcessContext{});
    return true;
}

void Lv2Plugin::freeInstance() {
    if (!m_instance) return;
    if (m_workThread.joinable()) {
        m_workQuit = true;
        m_workSem.release();
        m_workThread.join();
    }
    uint32_t tag, size;
    while (m_workRequests.pop(tag, size, m_rtMsg.data()) || m_workResponses.pop(tag, size, m_rtMsg.data())) {
    }   // worker messages hold pointers into this instance
    lilv_instance_deactivate(m_instance);
    lilv_instance_free(m_instance);
    m_instance = nullptr;
    m_worker = nullptr;
}

void Lv2Plugin::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    bool busy = m_busy.test_and_set(std::memory_order_acquire);   // the UI thread is restoring state or re-instantiating
    if (!busy && m_instance) runBlock(L, R, n, midi, ctx);
    else if (m_isInstrument) {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    }
    if (!busy) unlockRun();
}

void Lv2Plugin::runBlock(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    const Urids& u = U();
    for (uint32_t i : m_controlIns) m_control[i] = m_params.getAt(m_ports[i].param);
    // audio in: the stereo pair; a mono plugin gets the sum
    if (m_mainIns.size() == 1)
        for (int i = 0; i < n; ++i) m_audio[m_mainIns[0]][i] = (L[i] + R[i]) * 0.5f;
    else
        for (size_t k = 0; k < m_mainIns.size(); ++k) {
            const float* src = k == 0 ? L : k == 1 ? R : nullptr;
            float* dst = m_audio[m_mainIns[k]].data();
            if (src) std::copy(src, src + n, dst);
            else std::fill(dst, dst + n, 0.f);
        }
    for (size_t k = 0; k < m_sideIns.size(); ++k) {
        const float* src = k == 0 || !ctx.sidechainR ? ctx.sidechainL : ctx.sidechainR;
        float* dst = m_audio[m_sideIns[k]].data();
        if (src) std::copy(src, src + n, dst);
        else std::fill(dst, dst + n, 0.f);
    }
    // atom inputs: transport position, UI messages, MIDI (in time order)
    for (uint32_t i : m_atomIns) {
        auto* seq = (LV2_Atom_Sequence*)m_atom[i].data();
        seq->atom.type = u.atomSequence;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad = 0;
    }
    if (m_wantsTime) {
        double barBeat = (ctx.positionBeats - ctx.barStartBeats) * ctx.tsDen / 4.0;   // in the time signature's beat unit
        lv2_atom_forge_set_buffer(&m_forge, (uint8_t*)m_posBuf.data(), m_posBuf.size() * sizeof(uint64_t));
        LV2_Atom_Forge_Frame frame;
        lv2_atom_forge_object(&m_forge, &frame, 0, u.timePosition);
        lv2_atom_forge_key(&m_forge, u.timeFrame);
        lv2_atom_forge_long(&m_forge, ctx.positionSamples);
        lv2_atom_forge_key(&m_forge, u.timeSpeed);
        lv2_atom_forge_float(&m_forge, ctx.playing ? 1.f : 0.f);
        lv2_atom_forge_key(&m_forge, u.timeBar);
        lv2_atom_forge_long(&m_forge, ctx.barNumber);
        lv2_atom_forge_key(&m_forge, u.timeBarBeat);
        lv2_atom_forge_float(&m_forge, (float)barBeat);
        lv2_atom_forge_key(&m_forge, u.timeBeatUnit);
        lv2_atom_forge_int(&m_forge, ctx.tsDen);
        lv2_atom_forge_key(&m_forge, u.timeBeatsPerBar);
        lv2_atom_forge_float(&m_forge, (float)std::max(ctx.tsNum, 1));
        lv2_atom_forge_key(&m_forge, u.timeBeatsPerMinute);
        lv2_atom_forge_float(&m_forge, (float)ctx.bpm);
        lv2_atom_forge_pop(&m_forge, &frame);
        auto* pos = (const LV2_Atom*)m_posBuf.data();
        for (uint32_t i : m_atomIns)
            if (m_ports[i].time) appendEvent(m_atom[i], 0, pos->type, pos->size, pos + 1);
    }
    uint32_t tag, size;
    while (m_fromUi.pop(tag, size, m_rtMsg.data())) {
        auto* a = (const LV2_Atom*)m_rtMsg.data();
        if (tag < m_ports.size() && m_ports[tag].type == PortType::Atom && m_ports[tag].input && size >= sizeof(LV2_Atom) &&
            a->size <= size - sizeof(LV2_Atom))
            appendEvent(m_atom[tag], 0, a->type, a->size, a + 1);
    }
    if (m_midiIn >= 0) {
        int64_t t = 0;
        for (int k = 0; k < midi.count; ++k) {
            uint8_t b[3];
            int len = midiBytes(midi.events[k], b);
            t = std::clamp<int64_t>(midi.events[k].sampleOffset, t, std::max(n - 1, 0));
            if (len) appendEvent(m_atom[m_midiIn], t, u.midiEvent, len, b);
        }
    }
    for (uint32_t i : m_atomOuts) {
        auto* a = (LV2_Atom*)m_atom[i].data();
        a->type = u.atomChunk;
        a->size = (uint32_t)(m_atom[i].size() * sizeof(uint64_t) - sizeof(LV2_Atom));
    }
    lilv_instance_run(m_instance, n);
    if (m_worker) {
        LV2_Handle h = lilv_instance_get_handle(m_instance);
        while (m_workResponses.pop(tag, size, m_rtMsg.data())) m_worker->work_response(h, size, m_rtMsg.data());
        if (m_worker->end_run) m_worker->end_run(h);
    }
    if (!m_audioOuts.empty()) {
        const float* l = m_audio[m_audioOuts[0]].data();
        const float* r = m_audioOuts.size() > 1 ? m_audio[m_audioOuts[1]].data() : l;
        std::copy(l, l + n, L);
        std::copy(r, r + n, R);
    } else if (m_isInstrument) {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    }
    for (uint32_t i : m_controlOuts) m_outValues[i].store(m_control[i], std::memory_order_relaxed);
    if (m_latencyPort >= 0) {
        float v = m_control[m_latencyPort];
        int lat = v > 0.f && v < 1e7f ? (int)v : 0;
        if (m_latency.exchange(lat) != lat) m_latencyDirty = true;
    }
    if (m_uiOpen.load(std::memory_order_acquire))
        for (uint32_t i : m_atomOuts) {
            auto* seq = (LV2_Atom_Sequence*)m_atom[i].data();
            if (seq->atom.type != u.atomSequence) continue;
            LV2_ATOM_SEQUENCE_FOREACH(seq, ev) m_toUi.push(i, &ev->body, sizeof(LV2_Atom) + ev->body.size);
        }
}

// ---------------------------------------------------------------- worker
void Lv2Plugin::workLoop() {
    std::vector<uint8_t> msg(m_workRequests.capacity());
    uint32_t tag, size;
    for (;;) {
        m_workSem.acquire();
        if (m_workQuit) return;
        while (m_workRequests.pop(tag, size, msg.data()))
            m_worker->work(lilv_instance_get_handle(m_instance), &Lv2Plugin::workRespond, this, size, msg.data());
    }
}
// Always handed to the worker thread (also outside run(), e.g. from restore): work() must never run twice at once
LV2_Worker_Status Lv2Plugin::scheduleWork(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
    auto* self = static_cast<Lv2Plugin*>(h);
    if (!self->m_worker) return LV2_WORKER_ERR_UNKNOWN;
    if (!self->m_workRequests.push(0, data, size)) return LV2_WORKER_ERR_NO_SPACE;
    self->m_workSem.release();
    return LV2_WORKER_SUCCESS;
}
LV2_Worker_Status Lv2Plugin::workRespond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) {
    return static_cast<Lv2Plugin*>(h)->m_workResponses.push(0, data, size) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
}

// ---------------------------------------------------------------- state
const void* Lv2Plugin::portValue(const char* symbol, void* user, uint32_t* size, uint32_t* type) {
    auto* self = static_cast<Lv2Plugin*>(user);
    for (uint32_t i : self->m_controlIns)
        if (self->m_ports[i].symbol == symbol) {
            self->m_portTmp = self->m_params.getAt(self->m_ports[i].param);   // lilv copies it before the next call
            *size = sizeof(float);
            *type = U().atomFloat;
            return &self->m_portTmp;
        }
    *size = *type = 0;
    return nullptr;
}
void Lv2Plugin::setPortValue(const char* symbol, void* user, const void* value, uint32_t size, uint32_t type) {
    auto* self = static_cast<Lv2Plugin*>(user);
    const Urids& u = U();
    float v;
    if (type == u.atomFloat && size == sizeof(float)) v = *(const float*)value;
    else if (type == u.atomDouble && size == sizeof(double)) v = (float)*(const double*)value;
    else if (type == u.atomInt && size == sizeof(int32_t)) v = (float)*(const int32_t*)value;
    else if (type == u.atomLong && size == sizeof(int64_t)) v = (float)*(const int64_t*)value;
    else return;
    for (uint32_t i : self->m_controlIns)
        if (self->m_ports[i].symbol == symbol) self->m_params.setAt(self->m_ports[i].param, v);
}
// lilv Turtle: control port values plus whatever the plugin saves through state:interface
QByteArray Lv2Plugin::stateBytes() {
    LilvState* s = lilv_state_new_from_instance(m_plugin, m_instance, &g_map, nullptr, nullptr, nullptr, nullptr, &Lv2Plugin::portValue,
        this, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, m_features);
    if (!s) return QByteArray();
    char* str = lilv_state_to_string(world(), &g_map, &g_unmap, s, "urn:musicthingy:state", nullptr);   // the old name, as in states saved before the rename
    QByteArray out(str);
    lilv_free(str);
    lilv_state_free(s);
    return out;
}
void Lv2Plugin::applyState(const QByteArray& state) {
    LilvState* s = lilv_state_new_from_string(world(), &g_map, state.constData());
    if (!s) {
        Log::warn("Unreadable LV2 state for " + m_name);
        return;
    }
    lilv_state_restore(s, m_instance, &Lv2Plugin::setPortValue, this, 0, m_features);
    lilv_state_free(s);
}
QJsonObject Lv2Plugin::saveState() const {
    QJsonObject o = IProcessor::saveState();
    o["format"] = "lv2";
    QByteArray st = m_instance ? const_cast<Lv2Plugin*>(this)->stateBytes() : m_pendingState;
    if (!st.isEmpty()) o["pluginState"] = QString::fromLatin1(st.toBase64());
    return o;
}
void Lv2Plugin::restoreState(const QJsonObject& s) {
    QByteArray st = QByteArray::fromBase64(s["pluginState"].toString().toLatin1());
    if (st.isEmpty()) {
        IProcessor::restoreState(s);
        return;
    }
    lockRun();   // state:interface restore() must not run concurrently with run()
    applyState(st);   // without an instance this sets the control ports only
    if (!m_instance) m_pendingState = st;
    unlockRun();
}
QString Lv2Plugin::formatValue(uint32_t id) const {
    if (id < m_ports.size()) {
        float v = m_params.get(id);
        for (auto& [value, label] : m_ports[id].scalePoints)
            if (std::fabs(value - v) < 1e-4f) return label;
    }
    return IProcessor::formatValue(id);
}

// ---------------------------------------------------------------- X11 UI
QWidget* Lv2Plugin::createEditor(QWidget* parent) {
    if (!m_ui) return nullptr;
    auto* w = new PluginEmbedWidget(parent);
    m_uiWidget = w;
    w->onAttach = [this](unsigned long xid, int& width, int& height) { return openUi(xid, width, height); };
    w->onDetach = [this] { closeUi(); };
    w->onIdle = [this] { idleUi(); };
    w->onResized = [this](int width, int height) {
        if (m_uiResize) m_uiResize->ui_resize(m_uiHandle, width, height);   // the UI's own interface takes its instance handle
    };
    return w;
}

bool Lv2Plugin::openUi(unsigned long xid, int& width, int& height) {
    if (m_uiHandle || !m_instance || !m_ui) return false;
    QString bin = filePath(lilv_ui_get_binary_uri(m_ui));
    QByteArray bundle = QDir::cleanPath(filePath(lilv_ui_get_bundle_uri(m_ui))).toLocal8Bit() + "/";
    m_uiLib = dlopen(bin.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    auto descriptor = m_uiLib ? (LV2UI_DescriptorFunction)dlsym(m_uiLib, "lv2ui_descriptor") : nullptr;
    const char* uiUri = lilv_node_as_uri(lilv_ui_get_uri(m_ui));
    for (uint32_t i = 0; descriptor && !m_uiDesc; ++i) {
        const LV2UI_Descriptor* d = descriptor(i);
        if (!d) break;
        if (!strcmp(d->URI, uiUri)) m_uiDesc = d;
    }
    LV2UI_Widget widget = nullptr;
    if (m_uiDesc) {
        m_hostResize = {this, &Lv2Plugin::uiRequestResize};
        m_dataAccess = {lilv_instance_get_descriptor(m_instance)->extension_data};
        m_uiFeatureList[0] = {LV2_UI__parent, (void*)(uintptr_t)xid};
        m_uiFeatureList[1] = {LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(m_instance)};
        m_uiFeatureList[2] = {LV2_UI__resize, &m_hostResize};
        m_uiFeatureList[3] = {LV2_DATA_ACCESS_URI, &m_dataAccess};
        m_uiFeatureList[4] = {LV2_UI__idleInterface, nullptr};
        for (int i = 0; i < 3; ++i) m_uiFeatures[i] = m_features[i];   // urid map / unmap, options
        for (int i = 0; i < 5; ++i) m_uiFeatures[3 + i] = &m_uiFeatureList[i];
        m_uiFeatures[8] = nullptr;
        m_uiW = m_uiH = 0;
        m_uiSent.assign(m_ports.size(), NAN);
        m_uiMsg.resize(m_toUi.capacity());
        uint32_t tag, size;
        while (m_toUi.pop(tag, size, m_uiMsg.data())) {}   // left over from a previous UI
        m_uiOpen.store(true, std::memory_order_release);   // already now: a UI may message the plugin while instantiating
        m_uiHandle = m_uiDesc->instantiate(
            m_uiDesc, m_uri.toUtf8().constData(), bundle.constData(), &Lv2Plugin::uiWrite, this, &widget, m_uiFeatures);
    }
    if (!m_uiHandle) {
        Log::error("LV2 UI failed to load: " + bin + (m_uiLib ? QString() : " (" + QString::fromLocal8Bit(dlerror()) + ")"));
        m_uiOpen.store(false);
        m_uiDesc = nullptr;
        if (m_uiLib) dlclose(m_uiLib);
        m_uiLib = nullptr;
        return false;
    }
    if (m_uiDesc->extension_data) {
        m_uiIdle = (const LV2UI_Idle_Interface*)m_uiDesc->extension_data(LV2_UI__idleInterface);
        m_uiResize = (const LV2UI_Resize*)m_uiDesc->extension_data(LV2_UI__resize);
    }
    width = m_uiW;
    height = m_uiH;
    auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if ((width <= 0 || height <= 0) && widget && x11) {   // the UI did not ask for a size: take its window's
        Window root;
        int x, y;
        unsigned w = 0, h = 0, border, depth;
        if (XGetGeometry(x11->display(), (Window)(uintptr_t)widget, &root, &x, &y, &w, &h, &border, &depth)) {
            width = (int)w;
            height = (int)h;
        }
    }
    idleUi();   // initial port values
    return true;
}

void Lv2Plugin::closeUi() {
    if (!m_uiHandle) return;
    m_uiOpen.store(false);
    m_uiDesc->cleanup(m_uiHandle);
    m_uiHandle = nullptr;
    m_uiDesc = nullptr;
    m_uiIdle = nullptr;
    m_uiResize = nullptr;
    dlclose(m_uiLib);
    m_uiLib = nullptr;
}

void Lv2Plugin::idleUi() {
    if (!m_uiHandle) return;
    if (m_uiDesc->port_event) {
        uint32_t port, size;
        while (m_toUi.pop(port, size, m_uiMsg.data())) m_uiDesc->port_event(m_uiHandle, port, size, U().eventTransfer, m_uiMsg.data());
        for (uint32_t i = 0; i < m_ports.size(); ++i) {
            if (m_ports[i].type != PortType::Control) continue;
            float v = m_ports[i].input ? m_params.getAt(m_ports[i].param) : m_outValues[i].load(std::memory_order_relaxed);
            if (v != m_uiSent[i]) {
                m_uiSent[i] = v;
                m_uiDesc->port_event(m_uiHandle, i, sizeof(float), 0, &v);
            }
        }
    }
    if (m_uiIdle) m_uiIdle->idle(m_uiHandle);
}

void Lv2Plugin::uiWrite(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t protocol, const void* buffer) {
    auto* self = static_cast<Lv2Plugin*>(c);
    if (port >= self->m_ports.size()) return;
    const Port& pt = self->m_ports[port];
    if (protocol == 0 && size == sizeof(float) && pt.type == PortType::Control && pt.input) {
        float v = *(const float*)buffer;
        self->m_params.setAt(pt.param, v);
        self->m_uiSent[port] = v;   // the UI already shows it
        if (self->onParamEdited) self->onParamEdited(port, v);
    } else if (protocol == U().eventTransfer && pt.type == PortType::Atom && pt.input) self->m_fromUi.push(port, buffer, size);
}

int Lv2Plugin::uiRequestResize(LV2UI_Feature_Handle h, int width, int height) {
    auto* self = static_cast<Lv2Plugin*>(h);
    self->m_uiW = width;
    self->m_uiH = height;
    if (self->m_uiHandle && self->m_uiWidget) self->m_uiWidget->requestResize(width, height);
    return 0;
}

}   // namespace mt
