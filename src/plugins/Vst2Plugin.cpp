#include "plugins/Vst2Plugin.h"
#include "plugins/PluginEmbedWidget.h"
#include "core/Log.h"
#include <dlfcn.h>
#include <QJsonObject>
#include <QJsonArray>
#include <cstring>
#include <QFileInfo>

namespace mt {
using namespace vst2;

static thread_local Vst2Plugin* t_loading = nullptr;   // the instance being constructed (callbacks arrive before AEffect->user is set)

intptr_t Vst2Plugin::hostCallback(AEffect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    Vst2Plugin* self = (e && e->user) ? static_cast<Vst2Plugin*>(e->user) : t_loading;
    switch (opcode) {
    case audioMasterAutomate:
        if (self && index >= 0 && index < self->m_params.count()) {
            self->m_params.setAt(index, opt);
            self->m_lastSent[index] = opt;
            if (self->onParamEdited) self->onParamEdited((uint32_t)index, opt);
        }
        return 0;
    case audioMasterVersion: return 2400;
    case audioMasterCurrentId: return self ? self->m_info.uniqueId : 0;
    case audioMasterIdle: return 0;
    case audioMasterGetTime: return self ? (intptr_t)&self->m_timeInfo : 0;
    case audioMasterProcessEvents: {
        if (!self || !ptr) return 0;
        auto* evs = (VstEvents*)ptr;
        for (int i = 0; i < evs->numEvents; ++i) {
            auto* me = (VstMidiEvent*)evs->events[i];
            if (me->type != kVstMidiType) continue;
            uint8_t st = (uint8_t)me->midiData[0] & 0xF0, ch = (uint8_t)me->midiData[0] & 0x0F;
            MidiEvent m;
            m.channel = ch;
            m.data1 = (uint8_t)me->midiData[1];
            m.data2 = (uint8_t)me->midiData[2];
            m.sampleOffset = me->deltaFrames;
            if (st == 0x90 && m.data2 > 0) {
                m.type = MidiEvent::NoteOn;
                m.velocity = m.data2 / 127.f;
            } else if (st == 0x80 || st == 0x90) m.type = MidiEvent::NoteOff;
            else if (st == 0xB0) m.type = MidiEvent::CC;
            else continue;
            self->m_outMidi.add(m);
        }
        return 1;
    }
    case audioMasterIOChanged:
        if (self) self->m_latencyDirty.store(true);
        return 1;
    case audioMasterSizeWindow: return 1;
    case audioMasterGetSampleRate: return self ? (intptr_t)self->m_sr : 44100;
    case audioMasterGetBlockSize: return self ? self->m_maxBlock : 512;
    case audioMasterGetInputLatency:
    case audioMasterGetOutputLatency: return 0;
    case audioMasterGetCurrentProcessLevel: return 0;
    case audioMasterGetAutomationState: return 1;
    case audioMasterGetVendorString:
        if (ptr) strncpy((char*)ptr, "Vybrant", 64);
        return 1;
    case audioMasterGetProductString:
        if (ptr) strncpy((char*)ptr, "Vybrant DAW", 64);
        return 1;
    case audioMasterGetVendorVersion: return 1000;
    case audioMasterCanDo: {
        const char* s = (const char*)ptr;
        if (!s) return 0;
        static const char* yes[] = {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents", "receiveVstMidiEvent",
            "sizeWindow", "supplyIdle", "acceptIOChanges", "startStopProcess", "shellCategory"};
        for (auto* y : yes)
            if (strcmp(s, y) == 0) return 1;
        return 0;
    }
    case audioMasterGetLanguage: return 1;
    case audioMasterUpdateDisplay: return 1;
    case audioMasterBeginEdit:
    case audioMasterEndEdit: return 1;
    default: return 0;
    }
}

intptr_t Vst2Plugin::dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) const {
    return m_effect ? m_effect->dispatcher(m_effect, opcode, index, value, ptr, opt) : 0;
}

std::unique_ptr<Vst2Plugin> Vst2Plugin::create(const QString& path, QString* error) {
    std::unique_ptr<Vst2Plugin> p(new Vst2Plugin());
    if (!p->init(path, error)) return nullptr;
    return p;
}

bool Vst2Plugin::init(const QString& path, QString* error) {
    m_path = path;
    void* h = dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (error) *error = QString::fromUtf8(dlerror());
        return false;
    }
    auto entry = (MainProc)dlsym(h, "VSTPluginMain");
    if (!entry) entry = (MainProc)dlsym(h, "main_plugin");
    if (!entry) entry = (MainProc)dlsym(h, "main");
    if (!entry) {
        dlclose(h);
        if (error) *error = "VSTPluginMain not exported (not a VST2 plugin)";
        return false;
    }
    m_handle = h;
    t_loading = this;
    AEffect* e = entry(&Vst2Plugin::hostCallback);
    t_loading = nullptr;
    if (!e || e->magic != kMagic) {
        if (error) *error = "Plugin entry returned an invalid AEffect";
        return false;
    }
    m_effect = e;
    e->user = this;
    dispatch(effOpen);
    char buf[256] = {};
    dispatch(effGetEffectName, 0, 0, buf);
    m_info.name = QString::fromUtf8(buf);
    if (m_info.name.isEmpty()) {
        std::memset(buf, 0, sizeof buf);
        dispatch(effGetProductString, 0, 0, buf);
        m_info.name = QString::fromUtf8(buf);
    }
    if (m_info.name.isEmpty()) m_info.name = QFileInfo(path).completeBaseName();
    std::memset(buf, 0, sizeof buf);
    dispatch(effGetVendorString, 0, 0, buf);
    m_info.vendor = QString::fromUtf8(buf);
    m_info.uniqueId = e->uniqueID;
    m_info.version = e->version;
    m_info.isSynth = (e->flags & effFlagsIsSynth) != 0 || dispatch(effGetPlugCategory) == kPlugCategSynth;
    m_info.hasEditor = (e->flags & effFlagsHasEditor) != 0;
    m_info.numInputs = e->numInputs;
    m_info.numOutputs = e->numOutputs;
    m_info.numParams = e->numParams;
    for (int i = 0; i < e->numParams; ++i) {
        char nm[128] = {};
        dispatch(effGetParamName, i, 0, nm);
        ParamDesc d;
        d.id = (uint32_t)i;
        d.name = nm[0] ? nm : ("Param " + std::to_string(i + 1));
        d.min = 0;
        d.max = 1;
        d.def = e->getParameter(e, i);
        d.automatable = dispatch(effCanBeAutomated, i) != 0 || true;
        m_params.add(d);
        m_params.setAt(i, d.def);
        m_lastSent.push_back(d.def);
    }
    return true;
}

bool Vst2Plugin::probe(const QString& path, Vst2Info& info, QString* error) {
    auto p = create(path, error);
    if (!p) return false;
    info = p->m_info;
    return true;
}

Vst2Plugin::~Vst2Plugin() {
    if (m_effect) {
        setActive(false);
        dispatch(effClose);
        m_effect = nullptr;
    }
    if (m_handle) dlclose(m_handle);
}

void Vst2Plugin::setActive(bool a) {
    if (!m_effect || a == m_active) return;
    if (a) {
        dispatch(effMainsChanged, 0, 1);
        dispatch(effStartProcess);
    } else {
        dispatch(effStopProcess);
        dispatch(effMainsChanged, 0, 0);
    }
    m_active = a;
}
void Vst2Plugin::prepare(double sr, int maxBlock) {
    setActive(false);
    m_sr = sr;
    m_maxBlock = maxBlock;
    dispatch(effSetSampleRate, 0, 0, nullptr, (float)sr);
    dispatch(effSetBlockSize, 0, maxBlock);
    int ni = std::max(m_effect->numInputs, 2), no = std::max(m_effect->numOutputs, 2);
    m_inBufs.assign(ni, std::vector<float>(maxBlock, 0.f));
    m_outBufs.assign(no, std::vector<float>(maxBlock, 0.f));
    m_inPtrs.resize(ni);
    m_outPtrs.resize(no);
    for (int i = 0; i < ni; ++i) m_inPtrs[i] = m_inBufs[i].data();
    for (int i = 0; i < no; ++i) m_outPtrs[i] = m_outBufs[i].data();
    m_midiEvents.resize(1024);
    m_eventsMem.resize(sizeof(VstEvents) + sizeof(VstEvent*) * 1024);
    setActive(true);
}
void Vst2Plugin::reset() {}

void Vst2Plugin::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    if (!m_effect || !m_active) return;
    m_outMidi.clear();
    for (int i = 0; i < m_params.count(); ++i) {
        float v = m_params.getAt(i);
        if (v != m_lastSent[i]) {
            m_lastSent[i] = v;
            m_effect->setParameter(m_effect, i, v);
        }
    }
    // time info
    auto& t = m_timeInfo;
    t.samplePos = (double)ctx.positionSamples;
    t.sampleRate = m_sr;
    t.ppqPos = ctx.positionBeats;
    t.tempo = ctx.bpm;
    t.barStartPos = ctx.barStartBeats;
    t.cycleStartPos = ctx.loopStartBeats;
    t.cycleEndPos = ctx.loopEndBeats;
    t.timeSigNumerator = ctx.tsNum;
    t.timeSigDenominator = ctx.tsDen;
    t.flags = kVstPpqPosValid | kVstTempoValid | kVstBarsValid | kVstTimeSigValid | kVstCyclePosValid |
        (ctx.playing ? kVstTransportPlaying : 0) | (ctx.recording ? kVstTransportRecording : 0) |
        (ctx.looping ? kVstTransportCycleActive : 0);
    // MIDI
    if (midi.count > 0) {
        auto* evs = (VstEvents*)m_eventsMem.data();
        int cnt = 0;
        for (int i = 0; i < midi.count && cnt < 1024; ++i) {
            const MidiEvent& e = midi.events[i];
            VstMidiEvent& me = m_midiEvents[cnt];
            std::memset(&me, 0, sizeof me);
            me.type = kVstMidiType;
            me.byteSize = sizeof(VstMidiEvent);
            me.deltaFrames = e.sampleOffset;
            switch (e.type) {
            case MidiEvent::NoteOn:
                me.midiData[0] = (char)(0x90 | e.channel);
                me.midiData[1] = (char)e.data1;
                me.midiData[2] = (char)std::max(1, (int)(e.velocity * 127));
                break;
            case MidiEvent::NoteOff:
                me.midiData[0] = (char)(0x80 | e.channel);
                me.midiData[1] = (char)e.data1;
                me.midiData[2] = (char)64;
                break;
            case MidiEvent::CC:
                me.midiData[0] = (char)(0xB0 | e.channel);
                me.midiData[1] = (char)e.data1;
                me.midiData[2] = (char)e.data2;
                break;
            case MidiEvent::PitchBend: {
                int v = e.pitchBend + 8192;
                me.midiData[0] = (char)(0xE0 | e.channel);
                me.midiData[1] = (char)(v & 0x7F);
                me.midiData[2] = (char)((v >> 7) & 0x7F);
                break;
            }
            case MidiEvent::ChannelPressure:
                me.midiData[0] = (char)(0xD0 | e.channel);
                me.midiData[1] = (char)e.data2;
                break;
            case MidiEvent::PolyPressure:
                me.midiData[0] = (char)(0xA0 | e.channel);
                me.midiData[1] = (char)e.data1;
                me.midiData[2] = (char)e.data2;
                break;
            case MidiEvent::ProgramChange:
                me.midiData[0] = (char)(0xC0 | e.channel);
                me.midiData[1] = (char)e.data1;
                break;
            case MidiEvent::AllNotesOff:
                me.midiData[0] = (char)(0xB0 | e.channel);
                me.midiData[1] = 123;
                me.midiData[2] = 0;
                break;
            default: continue;
            }
            evs->events[cnt] = (VstEvent*)&me;
            ++cnt;
        }
        evs->numEvents = cnt;
        evs->reserved = 0;
        if (cnt) dispatch(effProcessEvents, 0, 0, evs);
    }
    // audio
    int ni = m_effect->numInputs, no = m_effect->numOutputs;
    if (ni >= 1) std::copy(L, L + n, m_inPtrs[0]);
    if (ni >= 2) std::copy(R, R + n, m_inPtrs[1]);
    for (int c = 2; c < ni; ++c) std::fill(m_inPtrs[c], m_inPtrs[c] + n, 0.f);
    for (int c = 0; c < no; ++c) std::fill(m_outPtrs[c], m_outPtrs[c] + n, 0.f);
    if (m_effect->flags & effFlagsCanReplacing && m_effect->processReplacing)
        m_effect->processReplacing(m_effect, m_inPtrs.data(), m_outPtrs.data(), n);
    else if (m_effect->process) m_effect->process(m_effect, m_inPtrs.data(), m_outPtrs.data(), n);
    if (no >= 2) {
        std::copy(m_outPtrs[0], m_outPtrs[0] + n, L);
        std::copy(m_outPtrs[1], m_outPtrs[1] + n, R);
    } else if (no == 1) {
        std::copy(m_outPtrs[0], m_outPtrs[0] + n, L);
        std::copy(m_outPtrs[0], m_outPtrs[0] + n, R);
    }
}

QJsonObject Vst2Plugin::saveState() const {
    QJsonObject o;
    o["key"] = key();
    o["format"] = "vst2";
    if (m_effect->flags & effFlagsProgramChunks) {
        void* data = nullptr;
        intptr_t n = dispatch(effGetChunk, 0, 0, &data);
        if (n > 0 && data) o["pluginState"] = QString::fromLatin1(QByteArray((const char*)data, (int)n).toBase64());
    }
    QJsonObject params;
    for (int i = 0; i < m_params.count(); ++i) params[QString::number(i)] = m_effect->getParameter(m_effect, i);
    o["params"] = params;
    o["program"] = (int)dispatch(effGetProgram);
    return o;
}
void Vst2Plugin::restoreState(const QJsonObject& s) {
    QByteArray st = QByteArray::fromBase64(s["pluginState"].toString().toLatin1());
    if (!st.isEmpty() && (m_effect->flags & effFlagsProgramChunks)) dispatch(effSetChunk, 0, st.size(), st.data());
    else {
        auto p = s["params"].toObject();
        for (auto it = p.begin(); it != p.end(); ++it) {
            int i = it.key().toInt();
            if (i >= 0 && i < m_params.count()) m_effect->setParameter(m_effect, i, (float)it.value().toDouble());
        }
    }
    if (s.contains("program")) dispatch(effSetProgram, 0, s["program"].toInt());
    syncFromPlugin();
}
void Vst2Plugin::syncFromPlugin() {
    for (int i = 0; i < m_params.count(); ++i) {
        float v = m_effect->getParameter(m_effect, i);
        m_params.setAt(i, v);
        m_lastSent[i] = v;
    }
}
QString Vst2Plugin::formatValue(uint32_t id) const {
    char disp[128] = {}, label[64] = {};
    dispatch(effGetParamDisplay, (int)id, 0, disp);
    dispatch(effGetParamLabel, (int)id, 0, label);
    QString s = QString::fromUtf8(disp).trimmed();
    if (label[0]) s += " " + QString::fromUtf8(label).trimmed();
    return s.isEmpty() ? IProcessor::formatValue(id) : s;
}

QWidget* Vst2Plugin::createEditor(QWidget* parent) {
    if (!m_info.hasEditor) return nullptr;
    auto* w = new PluginEmbedWidget(parent);
    w->onAttach = [this](unsigned long xid, int& width, int& height) {
        ERect* r = nullptr;
        dispatch(effEditGetRect, 0, 0, &r);
        if (dispatch(effEditOpen, 0, 0, (void*)(uintptr_t)xid) == 0 && !r) return false;
        dispatch(effEditGetRect, 0, 0, &r);
        if (r) {
            width = r->right - r->left;
            height = r->bottom - r->top;
        }
        return true;
    };
    w->onDetach = [this] { dispatch(effEditClose); };
    w->onIdle = [this] { dispatch(effEditIdle); };
    return w;
}

}   // namespace mt
