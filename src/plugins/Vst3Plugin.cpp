#include "plugins/Vst3Plugin.h"
#include "plugins/PluginHost.h"
#include "plugins/PluginEmbedWidget.h"
#include "core/Log.h"
#include <dlfcn.h>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSocketNotifier>
#include <QTimer>
#include <QPointer>
#include <map>
#include <cstring>

namespace mt {
using namespace vst3;

static QString uidToHex(const TUID id) {
    QString s;
    for (int i = 0; i < 16; ++i) s += QString("%1").arg((uint8_t)id[i], 2, 16, QChar('0')).toUpper();
    return s;
}
static bool hexToUid(const QString& hex, TUID out) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        bool ok;
        out[i] = (char)hex.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok) return false;
    }
    return true;
}
static QString u16(const TChar* s) { return QString::fromUtf16(s); }
static void toU16(const QString& s, TChar* out, int cap) {
    int n = std::min(cap - 1, (int)s.size());
    for (int i = 0; i < n; ++i) out[i] = (TChar)s[i].unicode();
    out[n] = 0;
}

// ---------------------------------------------------------------- module
QString Vst3Module::resolveBinary(const QString& p) {
    QFileInfo fi(p);
    if (fi.isDir()) {
        QDir d(p);
        QString arch = "x86_64-linux";
#if defined(__aarch64__)
        arch = "aarch64-linux";
#elif defined(__i386__)
        arch = "i386-linux";
#endif
        QDir bin(d.filePath("Contents/" + arch));
        if (bin.exists()) {
            auto sos = bin.entryList({"*.so"}, QDir::Files);
            if (!sos.isEmpty()) return bin.filePath(sos.first());
        }
        return QString();
    }
    return p;
}
std::shared_ptr<Vst3Module> Vst3Module::load(const QString& path, QString* error) {
    QString bin = resolveBinary(path);
    if (bin.isEmpty()) {
        if (error) *error = "No binary found in bundle";
        return nullptr;
    }
    void* h = dlopen(bin.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (error) *error = QString::fromUtf8(dlerror());
        return nullptr;
    }
    auto m = std::shared_ptr<Vst3Module>(new Vst3Module());
    m->m_handle = h;
    m->m_path = path;
    auto entry = (ModuleEntryProc)dlsym(h, "ModuleEntry");
    if (entry) entry(h);
    m->m_exit = (ModuleExitProc)dlsym(h, "ModuleExit");
    auto gf = (GetFactoryProc)dlsym(h, "GetPluginFactory");
    if (!gf) {
        if (error) *error = "GetPluginFactory not exported (not a VST3 module)";
        return nullptr;
    }
    m->m_factory = gf();
    if (!m->m_factory) {
        if (error) *error = "GetPluginFactory returned null";
        return nullptr;
    }
    return m;
}
Vst3Module::~Vst3Module() {
    if (m_factory) m_factory->release();
    if (m_exit) m_exit();
    if (m_handle) dlclose(m_handle);
}
std::vector<Vst3ClassInfo> Vst3Module::classes() const {
    std::vector<Vst3ClassInfo> v;
    if (!m_factory) return v;
    IPluginFactory2* f2 = nullptr;
    m_factory->queryInterface(IPluginFactory2_iid.id, (void**)&f2);
    int32 n = m_factory->countClasses();
    for (int32 i = 0; i < n; ++i) {
        Vst3ClassInfo ci;
        if (f2) {
            PClassInfo2 info{};
            if (f2->getClassInfo2(i, &info) != kResultOk) continue;
            if (strcmp(info.category, kVstAudioEffectClass) != 0) continue;
            ci.cid = uidToHex(info.cid);
            ci.name = QString::fromUtf8(info.name);
            ci.vendor = QString::fromUtf8(info.vendor);
            ci.subCategories = QString::fromUtf8(info.subCategories);
            ci.version = QString::fromUtf8(info.version);
            ci.sdkVersion = QString::fromUtf8(info.sdkVersion);
        } else {
            PClassInfo info{};
            if (m_factory->getClassInfo(i, &info) != kResultOk) continue;
            if (strcmp(info.category, kVstAudioEffectClass) != 0) continue;
            ci.cid = uidToHex(info.cid);
            ci.name = QString::fromUtf8(info.name);
        }
        if (ci.vendor.isEmpty()) {
            PFactoryInfo fi{};
            if (m_factory->getFactoryInfo(&fi) == kResultOk) ci.vendor = QString::fromUtf8(fi.vendor);
        }
        ci.category = ci.subCategories.contains("Instrument", Qt::CaseInsensitive) ? "Instrument" : "Effect";
        v.push_back(ci);
    }
    if (f2) f2->release();
    return v;
}

// ---------------------------------------------------------------- host-side objects
template <class Base> struct RefCounted : Base {
    std::atomic<uint32> rc{1};
    uint32 addRef() override { return ++rc; }
    uint32 release() override {
        uint32 r = --rc;
        if (r == 0) delete this;
        return r;
    }
    virtual ~RefCounted() = default;
};

struct HostAttributeList : RefCounted<IAttributeList> {
    struct Val {
        int64 i = 0;
        double f = 0;
        std::u16string s;
        std::vector<uint8> bin;
        int type = 0;
    };
    std::map<std::string, Val> vals;
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IAttributeList_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult setInt(AttrID id, int64 v) override {
        auto& x = vals[id];
        x.i = v;
        x.type = 1;
        return kResultOk;
    }
    tresult getInt(AttrID id, int64& v) override {
        auto it = vals.find(id);
        if (it == vals.end()) return kResultFalse;
        v = it->second.i;
        return kResultOk;
    }
    tresult setFloat(AttrID id, double v) override {
        auto& x = vals[id];
        x.f = v;
        x.type = 2;
        return kResultOk;
    }
    tresult getFloat(AttrID id, double& v) override {
        auto it = vals.find(id);
        if (it == vals.end()) return kResultFalse;
        v = it->second.f;
        return kResultOk;
    }
    tresult setString(AttrID id, const TChar* s) override {
        auto& x = vals[id];
        x.s = s ? std::u16string((const char16_t*)s) : u"";
        x.type = 3;
        return kResultOk;
    }
    tresult getString(AttrID id, TChar* s, uint32 bytes) override {
        auto it = vals.find(id);
        if (it == vals.end()) return kResultFalse;
        size_t n = std::min<size_t>(it->second.s.size(), bytes / 2 - 1);
        std::memcpy(s, it->second.s.data(), n * 2);
        s[n] = 0;
        return kResultOk;
    }
    tresult setBinary(AttrID id, const void* d, uint32 n) override {
        auto& x = vals[id];
        x.bin.assign((const uint8*)d, (const uint8*)d + n);
        x.type = 4;
        return kResultOk;
    }
    tresult getBinary(AttrID id, const void*& d, uint32& n) override {
        auto it = vals.find(id);
        if (it == vals.end()) return kResultFalse;
        d = it->second.bin.data();
        n = (uint32)it->second.bin.size();
        return kResultOk;
    }
};
struct HostMessage : RefCounted<IMessage> {
    std::string id;
    HostAttributeList* attrs = new HostAttributeList();
    ~HostMessage() override { attrs->release(); }
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IMessage_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    FIDString getMessageID() override { return id.c_str(); }
    void setMessageID(FIDString s) override { id = s ? s : ""; }
    IAttributeList* getAttributes() override { return attrs; }
};
struct HostApplication : RefCounted<IHostApplication> {
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IHostApplication_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult getName(String128 name) override {
        toU16("MusicThingy", name, 128);
        return kResultOk;
    }
    tresult createInstance(TUID cid, TUID iid, void** obj) override {
        if (uidEq(cid, IMessage_iid.id) && uidEq(iid, IMessage_iid.id)) {
            *obj = new HostMessage();
            return kResultOk;
        }
        if (uidEq(cid, IAttributeList_iid.id) && uidEq(iid, IAttributeList_iid.id)) {
            *obj = new HostAttributeList();
            return kResultOk;
        }
        *obj = nullptr;
        return kNotImplemented;
    }
};
struct MemoryStream : RefCounted<IBStream> {
    std::vector<uint8> data;
    int64 pos = 0;
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IBStream_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult read(void* buf, int32 n, int32* got) override {
        int64 avail = std::max<int64>(0, (int64)data.size() - pos);
        int32 k = (int32)std::min<int64>(n, avail);
        if (k > 0) std::memcpy(buf, data.data() + pos, k);
        pos += k;
        if (got) *got = k;
        return kResultOk;
    }
    tresult write(void* buf, int32 n, int32* wrote) override {
        if (pos + n > (int64)data.size()) data.resize(pos + n);
        std::memcpy(data.data() + pos, buf, n);
        pos += n;
        if (wrote) *wrote = n;
        return kResultOk;
    }
    tresult seek(int64 p, int32 mode, int64* res) override {
        int64 np = mode == kIBSeekSet ? p : mode == kIBSeekCur ? pos + p : (int64)data.size() + p;
        if (np < 0) return kInvalidArgument;
        pos = np;
        if (res) *res = pos;
        return kResultOk;
    }
    tresult tell(int64* p) override {
        if (p) *p = pos;
        return kResultOk;
    }
};
struct ParamQueue : IParamValueQueue {   // RT-safe fixed capacity; lifetime managed by ParamChanges
    ParamID id = 0;
    static constexpr int kCap = 8;
    int32 offsets[kCap];
    ParamValue values[kCap];
    int count = 0;
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IParamValueQueue_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 addRef() override { return 1; }
    uint32 release() override { return 1; }
    ParamID getParameterId() override { return id; }
    int32 getPointCount() override { return count; }
    tresult getPoint(int32 i, int32& off, ParamValue& v) override {
        if (i < 0 || i >= count) return kInvalidArgument;
        off = offsets[i];
        v = values[i];
        return kResultOk;
    }
    tresult addPoint(int32 off, ParamValue v, int32& index) override {
        if (count >= kCap) {
            values[kCap - 1] = v;
            offsets[kCap - 1] = off;
            index = kCap - 1;
            return kResultOk;
        }
        offsets[count] = off;
        values[count] = v;
        index = count++;
        return kResultOk;
    }
};
struct ParamChanges : IParameterChanges {
    std::vector<ParamQueue> queues;
    int used = 0;
    void reserve(int n) { queues.resize(std::max(n, 16)); }
    void clear() { used = 0; }
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IParameterChanges_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 addRef() override { return 1; }
    uint32 release() override { return 1; }
    int32 getParameterCount() override { return used; }
    IParamValueQueue* getParameterData(int32 i) override { return (i >= 0 && i < used) ? &queues[i] : nullptr; }
    IParamValueQueue* addParameterData(const ParamID& id, int32& index) override {
        for (int i = 0; i < used; ++i)
            if (queues[i].id == id) {
                index = i;
                return &queues[i];
            }
        if (used >= (int)queues.size()) return nullptr;
        auto& q = queues[used];
        q.id = id;
        q.count = 0;
        index = used++;
        return &q;
    }
};
struct EventList : IEventList {
    std::vector<Event> events;
    int count = 0;
    void reserve(int n) { events.resize(n); }
    void clear() { count = 0; }
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IEventList_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 addRef() override { return 1; }
    uint32 release() override { return 1; }
    int32 getEventCount() override { return count; }
    tresult getEvent(int32 i, Event& e) override {
        if (i < 0 || i >= count) return kInvalidArgument;
        e = events[i];
        return kResultOk;
    }
    tresult addEvent(Event& e) override {
        if (count >= (int)events.size()) return kOutOfMemory;
        events[count++] = e;
        return kResultOk;
    }
};
struct ComponentHandler : RefCounted<IComponentHandler> {
    Vst3Plugin* plugin = nullptr;
    struct Handler2 : IComponentHandler2 {
        ComponentHandler* owner;
        explicit Handler2(ComponentHandler* o)
            : owner(o) {}
        tresult queryInterface(const TUID iid, void** obj) override { return owner->queryInterface(iid, obj); }
        uint32 addRef() override { return owner->addRef(); }
        uint32 release() override { return owner->release(); }
        tresult setDirty(TBool) override { return kResultOk; }
        tresult requestOpenEditor(FIDString) override { return kNotImplemented; }
        tresult startGroupEdit() override { return kResultOk; }
        tresult finishGroupEdit() override { return kResultOk; }
    } h2{this};
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IComponentHandler_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        if (uidEq(iid, IComponentHandler2_iid.id)) {
            addRef();
            *obj = &h2;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult beginEdit(ParamID) override { return kResultOk; }
    tresult performEdit(ParamID id, ParamValue v) override;
    tresult endEdit(ParamID) override { return kResultOk; }
    tresult restartComponent(int32 flags) override;
};
// Linux run loop + plug frame implemented with Qt on the UI thread
struct PlugFrame : RefCounted<IPlugFrame> {
    QPointer<PluginEmbedWidget> widget;
    struct RunLoop;
    RunLoop* runLoop;
    struct RunLoop : IRunLoop {
        PlugFrame* owner;
        // one handler may register several fds (JUCE does): a plain map lost the earlier notifiers, which kept firing after
        // the editor had freed the handler
        std::multimap<IEventHandler*, QSocketNotifier*> fds;
        std::multimap<ITimerHandler*, QTimer*> timers;
        tresult queryInterface(const TUID iid, void** obj) override { return owner->queryInterface(iid, obj); }
        uint32 addRef() override { return owner->addRef(); }
        uint32 release() override { return owner->release(); }
        tresult registerEventHandler(IEventHandler* h, FileDescriptor fd) override {
            if (!h) return kInvalidArgument;
            auto* n = new QSocketNotifier(fd, QSocketNotifier::Read);
            QObject::connect(n, &QSocketNotifier::activated, [h, fd] { h->onFDIsSet(fd); });
            fds.emplace(h, n);
            return kResultOk;
        }
        tresult unregisterEventHandler(IEventHandler* h) override {
            auto [b, e] = fds.equal_range(h);
            if (b == e) return kResultFalse;
            for (auto it = b; it != e; ++it) delete it->second;
            fds.erase(b, e);
            return kResultOk;
        }
        tresult registerTimer(ITimerHandler* h, TimerInterval ms) override {
            if (!h) return kInvalidArgument;
            auto* t = new QTimer();
            t->setInterval((int)std::max<TimerInterval>(ms, 1));
            QObject::connect(t, &QTimer::timeout, [h] { h->onTimer(); });
            t->start();
            timers.emplace(h, t);
            return kResultOk;
        }
        tresult unregisterTimer(ITimerHandler* h) override {
            auto [b, e] = timers.equal_range(h);
            if (b == e) return kResultFalse;
            for (auto it = b; it != e; ++it) delete it->second;
            timers.erase(b, e);
            return kResultOk;
        }
        void clearAll() {
            for (auto& [h, n] : fds) delete n;
            fds.clear();
            for (auto& [h, t] : timers) delete t;
            timers.clear();
        }
    };
    PlugFrame() {
        runLoop = new RunLoop();
        runLoop->owner = this;
    }
    ~PlugFrame() override {
        runLoop->clearAll();
        delete runLoop;
    }
    tresult queryInterface(const TUID iid, void** obj) override {
        if (uidEq(iid, IPlugFrame_iid.id) || uidEq(iid, FUnknown_iid.id)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        if (uidEq(iid, IRunLoop_iid.id)) {
            addRef();
            *obj = runLoop;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult resizeView(IPlugView* view, ViewRect* r) override {
        if (widget && r) {
            widget->requestResize(r->getWidth(), r->getHeight());
            view->onSize(r);
        }
        return kResultOk;
    }
};

struct Vst3HostObjects {
    HostApplication* app = new HostApplication();
    ComponentHandler* handler = new ComponentHandler();
    ParamChanges inChanges, outChanges;
    EventList inEvents, outEvents;
    vst3::ProcessContext ctx{};
    IPlugView* view = nullptr;
    PlugFrame* frame = nullptr;
    ~Vst3HostObjects() {
        if (view) {
            view->setFrame(nullptr);
            view->release();
        }
        if (frame) frame->release();
        handler->release();
        app->release();
    }
};

tresult ComponentHandler::performEdit(ParamID id, ParamValue v) {
    if (plugin) {
        plugin->m_params.set(id, (float)v);
        for (size_t i = 0; i < plugin->m_paramIds.size() && i < plugin->m_lastPushed.size(); ++i)
            if (plugin->m_paramIds[i] == id) plugin->m_lastPushed[i] = (float)v; /* the GUI already has it */
        if (plugin->onParamEdited) plugin->onParamEdited(id, (float)v);
    }
    return kResultOk;
}
tresult ComponentHandler::restartComponent(int32 flags) {
    if (!plugin) return kResultOk;
    if (flags & kLatencyChanged) plugin->m_latencyDirty.store(true);
    if (flags & kParamValuesChanged) plugin->refreshParamsFromController();
    return kResultOk;
}

// ---------------------------------------------------------------- plugin
Vst3Plugin::Vst3Plugin()
    : m_host(std::make_unique<Vst3HostObjects>()) {
    m_host->handler->plugin = this;
}
Vst3Plugin::~Vst3Plugin() {
    if (m_host->view) { m_host->view->removed(); }
    setActive(false);
    if (m_controller && !m_controllerIsComponent) {
        IConnectionPoint *a = nullptr, *b = nullptr;
        if (m_component) m_component->queryInterface(IConnectionPoint_iid.id, (void**)&a);
        m_controller->queryInterface(IConnectionPoint_iid.id, (void**)&b);
        if (a && b) {
            a->disconnect(b);
            b->disconnect(a);
        }
        if (a) a->release();
        if (b) b->release();
        m_controller->setComponentHandler(nullptr);
        m_controller->terminate();
        m_controller->release();
    }
    if (m_midiMapping) m_midiMapping->release();
    if (m_processor) m_processor->release();
    if (m_component) {
        m_component->terminate();
        m_component->release();
    }
    m_host.reset();
    m_module.reset();
}

std::unique_ptr<Vst3Plugin> Vst3Plugin::create(const QString& path, const QString& classId, QString* error) {
    std::unique_ptr<Vst3Plugin> p(new Vst3Plugin());
    if (!p->init(path, classId, error)) return nullptr;
    return p;
}

bool Vst3Plugin::init(const QString& path, const QString& classId, QString* error) {
    m_module = Vst3Module::load(path, error);
    if (!m_module) return false;
    IPluginFactory* f = m_module->factory();
    IPluginFactory3* f3 = nullptr;
    if (f->queryInterface(IPluginFactory3_iid.id, (void**)&f3) == kResultOk && f3) {
        f3->setHostContext(m_host->app);
        f3->release();
    }
    TUID cid;
    QString wanted = classId;
    auto classes = m_module->classes();
    if (classes.empty()) {
        if (error) *error = "No audio module classes";
        return false;
    }
    if (wanted.isEmpty()) wanted = classes.front().cid;
    const Vst3ClassInfo* info = nullptr;
    for (auto& c : classes)
        if (c.cid == wanted) info = &c;
    if (!info) {
        if (error) *error = "Class not found in module";
        return false;
    }
    hexToUid(info->cid, cid);
    m_classId = info->cid;
    m_name = info->name;
    m_vendor = info->vendor;
    m_isInstrument = info->category == "Instrument";
    if (f->createInstance(cid, IComponent_iid.id, (void**)&m_component) != kResultOk || !m_component) {
        if (error) *error = "createInstance(IComponent) failed";
        return false;
    }
    if (m_component->initialize(m_host->app) != kResultOk) {
        if (error) *error = "IComponent::initialize failed";
        return false;
    }
    if (m_component->queryInterface(IAudioProcessor_iid.id, (void**)&m_processor) != kResultOk || !m_processor) {
        if (error) *error = "No IAudioProcessor";
        return false;
    }
    // controller: same object or separate class
    if (m_component->queryInterface(IEditController_iid.id, (void**)&m_controller) == kResultOk && m_controller)
        m_controllerIsComponent = true;
    else {
        TUID ccid;
        if (m_component->getControllerClassId(ccid) == kResultOk &&
            f->createInstance(ccid, IEditController_iid.id, (void**)&m_controller) == kResultOk && m_controller) {
            if (m_controller->initialize(m_host->app) != kResultOk) {
                m_controller->release();
                m_controller = nullptr;
            }
        }
    }
    if (m_controller) {
        m_controller->setComponentHandler(m_host->handler);
        if (!m_controllerIsComponent) {
            IConnectionPoint *a = nullptr, *b = nullptr;
            m_component->queryInterface(IConnectionPoint_iid.id, (void**)&a);
            m_controller->queryInterface(IConnectionPoint_iid.id, (void**)&b);
            if (a && b) {
                a->connect(b);
                b->connect(a);
            }
            if (a) a->release();
            if (b) b->release();
            // hand the component state to the controller so both start in sync
            MemoryStream* ms = new MemoryStream();
            if (m_component->getState(ms) == kResultOk) {
                ms->pos = 0;
                m_controller->setComponentState(ms);
            }
            ms->release();
        }
        m_controller->queryInterface(IMidiMapping_iid.id, (void**)&m_midiMapping);
        IPlugView* v = m_controller->createView(kEditorViewType);
        if (v) {
            m_hasEditor = v->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue;
            v->release();
        }
    }
    setupBuses();
    buildParameterMirror();
    m_host->inEvents.reserve(1024);
    m_host->outEvents.reserve(1024);
    m_host->inChanges.reserve(std::max(64, (int)m_paramIds.size()));
    m_host->outChanges.reserve(std::max(64, (int)m_paramIds.size()));
    return true;
}

void Vst3Plugin::setupBuses() {
    m_numInputBuses = m_component->getBusCount(kAudio, kInput);
    m_numOutputBuses = m_component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> ins(m_numInputBuses, kStereo), outs(m_numOutputBuses, kStereo);
    for (int i = 0; i < m_numInputBuses; ++i) {
        BusInfo bi{};
        m_component->getBusInfo(kAudio, kInput, i, bi);
        if (bi.channelCount == 1) ins[i] = kMono;
    }
    m_outNames.clear();
    for (int i = 0; i < m_numOutputBuses; ++i) {
        BusInfo bi{};
        m_component->getBusInfo(kAudio, kOutput, i, bi);
        if (bi.channelCount == 1) outs[i] = kMono;
        m_outNames << u16(bi.name);
    }
    if (m_processor->setBusArrangements(ins.data(), m_numInputBuses, outs.data(), m_numOutputBuses) != kResultOk) {
        for (int i = 0; i < m_numInputBuses; ++i) m_processor->getBusArrangement(kInput, i, ins[i]);
        for (int i = 0; i < m_numOutputBuses; ++i) m_processor->getBusArrangement(kOutput, i, outs[i]);
    }
    auto chans = [](SpeakerArrangement a) {
        int n = 0;
        for (int b = 0; b < 64; ++b)
            if (a & (1ull << b)) ++n;
        return std::max(n, 1);
    };
    m_inChans.clear();
    m_outChans.clear();
    for (auto a : ins) m_inChans.push_back(chans(a));
    for (auto a : outs) m_outChans.push_back(chans(a));
    m_auxBuses.clear();
    for (int i = 1; i < m_numOutputBuses; ++i)
        if (outs[i] != 0) m_auxBuses.push_back(i);   // an empty arrangement (setBusArrangements refused) has no channels
    for (int i = 0; i < m_numInputBuses; ++i) m_component->activateBus(kAudio, kInput, i, i < 2);
    for (int i = 0; i < m_numOutputBuses; ++i) m_component->activateBus(kAudio, kOutput, i, true);   // extra outs: multi-out routing
    int evIn = m_component->getBusCount(kEvent, kInput), evOut = m_component->getBusCount(kEvent, kOutput);
    for (int i = 0; i < evIn; ++i) m_component->activateBus(kEvent, kInput, i, i == 0);
    for (int i = 0; i < evOut; ++i) m_component->activateBus(kEvent, kOutput, i, i == 0);
}

void Vst3Plugin::buildParameterMirror() {
    m_paramIds.clear();
    m_lastSent.clear();
    m_lastPushed.clear();
    if (!m_controller) return;
    int32 n = m_controller->getParameterCount();
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo pi{};
        if (m_controller->getParameterInfo(i, pi) != kResultOk) continue;
        ParamDesc d;
        d.id = pi.id;
        d.name = u16(pi.title).toStdString();
        d.min = 0;
        d.max = 1;
        d.def = (float)pi.defaultNormalizedValue;
        d.automatable = (pi.flags & kCanAutomate) && !(pi.flags & kIsReadOnly);
        d.steps = pi.stepCount > 0 ? pi.stepCount + 1 : 0;
        if (pi.flags & kIsHidden) d.automatable = false;
        if (pi.stepCount == 1) d.unit = ParamUnit::Toggle;
        if (pi.flags & kIsBypass) m_bypassParamIndex = (int)m_paramIds.size();
        m_params.add(d);
        m_paramIds.push_back(pi.id);
        // m_lastPushed mirrors what the edit controller already holds, so the first syncControllerFromParams() tick
        // has nothing to send back - without it that tick pushes every parameter over IPC for no reason
        float v = (float)m_controller->getParamNormalized(pi.id);
        m_params.set(pi.id, v);
        m_lastSent.push_back(v);
        m_lastPushed.push_back(v);
    }
    m_reported = std::make_unique<std::atomic<float>[]>(m_paramIds.size());
    for (size_t i = 0; i < m_paramIds.size(); ++i) m_reported[i].store(-1.f);   // normalized values: -1 = nothing reported
}
void Vst3Plugin::refreshParamsFromController() {
    if (!m_controller) return;
    m_lastPushed.assign(m_paramIds.size(), 0.f);
    for (size_t i = 0; i < m_paramIds.size(); ++i) {
        float v = (float)m_controller->getParamNormalized(m_paramIds[i]);
        m_params.set(m_paramIds[i], v);
        m_lastPushed[i] = v;
    }
}
// Push host-side parameter changes (automation, generic knobs, MIDI) to the edit controller so a custom GUI follows them.
// Only values that changed since the last push are sent: every controller call is an IPC round trip for bridged plugins.
void Vst3Plugin::syncControllerFromParams() {
    if (!m_controller) return;
    if (m_lastPushed.size() != m_paramIds.size()) { m_lastPushed.assign(m_paramIds.size(), -1.f); }
    for (size_t i = 0; i < m_paramIds.size(); ++i) {
        float v = m_params.get(m_paramIds[i]);
        if (std::fabs(m_lastPushed[i] - v) > 1e-6f) {
            m_lastPushed[i] = v;
            // a value the plugin reported itself is not a host change: echoing it back makes some plugins act on it as one
            // (Surge XT applies a loaded state to its processor a moment late, reports the old values meanwhile, and getting
            // them back resets the state)
            if (m_reported && m_reported[i].load(std::memory_order_relaxed) == v) continue;
            m_controller->setParamNormalized(m_paramIds[i], v);
        }
    }
}
QString Vst3Plugin::formatValue(uint32_t id) const {
    if (!m_controller) return IProcessor::formatValue(id);
    String128 s{};
    if (m_controller->getParamStringByValue(id, const_cast<Vst3Plugin*>(this)->m_params.get(id), s) == kResultOk) return u16(s);
    return IProcessor::formatValue(id);
}

void Vst3Plugin::setActive(bool a) {
    if (a == m_active) return;
    if (a) {
        m_component->setActive(1);
        m_processor->setProcessing(1);
        m_active = true;
        m_latency.store((int)m_processor->getLatencySamples());
        m_tail = (int)m_processor->getTailSamples();
    } else {
        m_processor->setProcessing(0);
        m_component->setActive(0);
        m_active = false;
    }
}
void Vst3Plugin::prepare(double sr, int maxBlock) {
    bool was = m_active;
    setActive(false);
    m_sr = sr;
    m_maxBlock = maxBlock;
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlock;
    setup.sampleRate = sr;
    m_processor->setupProcessing(setup);
    // bus buffers
    m_inL.assign(maxBlock, 0.f);
    m_inR.assign(maxBlock, 0.f);
    m_scratch.clear();
    m_inBuses.assign(m_numInputBuses, AudioBusBuffers{});
    m_outBuses.assign(m_numOutputBuses, AudioBusBuffers{});
    m_inPtrs.assign(m_numInputBuses, {});
    m_outPtrs.assign(m_numOutputBuses, {});
    auto scratch = [&]() {
        m_scratch.emplace_back(maxBlock, 0.f);
        return m_scratch.back().data();
    };
    m_scratch.reserve(64);
    for (int b = 0; b < m_numInputBuses; ++b) {
        m_inPtrs[b].resize(m_inChans[b]);
        for (int c = 0; c < m_inChans[b]; ++c) m_inPtrs[b][c] = scratch();
    }
    for (int b = 0; b < m_numOutputBuses; ++b) {
        m_outPtrs[b].resize(m_outChans[b]);
        for (int c = 0; c < m_outChans[b]; ++c) m_outPtrs[b][c] = scratch();
    }
    for (int b = 0; b < m_numInputBuses; ++b) {
        m_inBuses[b].numChannels = m_inChans[b];
        m_inBuses[b].silenceFlags = 0;
        m_inBuses[b].channelBuffers32 = m_inPtrs[b].data();
    }
    for (int b = 0; b < m_numOutputBuses; ++b) {
        m_outBuses[b].numChannels = m_outChans[b];
        m_outBuses[b].silenceFlags = 0;
        m_outBuses[b].channelBuffers32 = m_outPtrs[b].data();
    }
    setActive(true);
    (void)was;
}
void Vst3Plugin::reset() {}

void Vst3Plugin::process(float* L, float* R, int n, const MidiEventList& midi, const mt::ProcessContext& ctx) {
    if (!m_active) return;
    auto& h = *m_host;
    h.inChanges.clear();
    h.outChanges.clear();
    h.inEvents.clear();
    h.outEvents.clear();
    m_outMidi.clear();
    // parameter changes from automation / UI
    for (size_t i = 0; i < m_paramIds.size(); ++i) {
        float v = m_params.getAt((int)i);
        if (v != m_lastSent[i]) {
            m_lastSent[i] = v;
            int32 idx;
            if (auto* q = h.inChanges.addParameterData(m_paramIds[i], idx)) {
                int32 pi;
                q->addPoint(0, v, pi);
            }
        }
    }
    // events
    for (int i = 0; i < midi.count; ++i) {
        const MidiEvent& e = midi.events[i];
        Event ev{};
        ev.busIndex = 0;
        ev.sampleOffset = e.sampleOffset;
        ev.flags = kIsLive;
        switch (e.type) {
        case MidiEvent::NoteOn:
            ev.type = kNoteOnEvent;
            ev.noteOn = {(int16)e.channel, (int16)e.data1, e.finePitch, e.velocity, 0, e.noteId};
            h.inEvents.addEvent(ev);
            break;
        case MidiEvent::NoteOff:
            ev.type = kNoteOffEvent;
            ev.noteOff = {(int16)e.channel, (int16)e.data1, e.velocity, e.noteId, 0.f};
            h.inEvents.addEvent(ev);
            break;
        case MidiEvent::AllNotesOff:
            for (int p = 0; p < 128; ++p) {
                ev.type = kNoteOffEvent;
                ev.noteOff = {(int16)e.channel, (int16)p, 0.f, -1, 0.f};
                h.inEvents.addEvent(ev);
            }
            break;
        case MidiEvent::PolyPressure:
            ev.type = kPolyPressureEvent;
            ev.polyPressure = {(int16)e.channel, (int16)e.data1, e.data2 / 127.f, -1};
            h.inEvents.addEvent(ev);
            break;
        case MidiEvent::CC:
        case MidiEvent::PitchBend:
        case MidiEvent::ChannelPressure: {
            if (!m_midiMapping) break;
            CtrlNumber cn = e.type == MidiEvent::CC ? e.data1 : (e.type == MidiEvent::PitchBend ? 129 : 128);
            ParamID pid;
            if (m_midiMapping->getMidiControllerAssignment(0, e.channel, cn, pid) == kResultOk) {
                double v = e.type == MidiEvent::PitchBend ? (e.pitchBend + 8192) / 16383.0 : e.data2 / 127.0;
                int32 idx;
                if (auto* q = h.inChanges.addParameterData(pid, idx)) {
                    int32 pi;
                    q->addPoint(e.sampleOffset, v, pi);
                }
                int pidx = m_params.indexOf(pid);
                if (pidx >= 0) {
                    m_params.setAt(pidx, (float)v);
                    m_lastSent[pidx] = (float)v;
                }
            }
            break;
        }
        default: break;
        }
    }
    // audio buffers
    bool hasIn = m_numInputBuses > 0;
    if (hasIn) {
        if (m_inChans[0] >= 2) {
            std::copy(L, L + n, m_inPtrs[0][0]);
            std::copy(R, R + n, m_inPtrs[0][1]);
        } else
            for (int i = 0; i < n; ++i) m_inPtrs[0][0][i] = (L[i] + R[i]) * 0.5f;
        if (m_numInputBuses > 1) {
            const float* sl = ctx.sidechainL;
            const float* sr = ctx.sidechainR ? ctx.sidechainR : ctx.sidechainL;
            if (sl) {
                if (m_inChans[1] >= 2) {
                    std::copy(sl, sl + n, m_inPtrs[1][0]);
                    std::copy(sr, sr + n, m_inPtrs[1][1]);
                } else
                    for (int i = 0; i < n; ++i) m_inPtrs[1][0][i] = (sl[i] + sr[i]) * 0.5f;
            } else
                for (int c = 0; c < m_inChans[1]; ++c) std::fill(m_inPtrs[1][c], m_inPtrs[1][c] + n, 0.f);
        }
    }
    for (int b = 0; b < m_numOutputBuses; ++b)
        for (int c = 0; c < m_outChans[b]; ++c) std::fill(m_outPtrs[b][c], m_outPtrs[b][c] + n, 0.f);
    // context
    auto& pc = h.ctx;
    pc = vst3::ProcessContext{};
    pc.state = kTempoValid | kTimeSigValid | kProjectTimeMusicValid | kBarPositionValid | kContTimeValid | (ctx.playing ? kPlaying : 0) |
        (ctx.recording ? kRecording : 0) | (ctx.looping ? (kCycleActive | kCycleValid) : 0);
    pc.sampleRate = m_sr;
    pc.projectTimeSamples = ctx.positionSamples;
    pc.continousTimeSamples = ctx.positionSamples;
    pc.projectTimeMusic = ctx.positionBeats;
    pc.barPositionMusic = ctx.barStartBeats;
    pc.cycleStartMusic = ctx.loopStartBeats;
    pc.cycleEndMusic = ctx.loopEndBeats;
    pc.tempo = ctx.bpm;
    pc.timeSigNumerator = ctx.tsNum;
    pc.timeSigDenominator = ctx.tsDen;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = n;
    data.numInputs = m_numInputBuses;
    data.numOutputs = m_numOutputBuses;
    data.inputs = m_inBuses.data();
    data.outputs = m_outBuses.data();
    data.inputParameterChanges = &h.inChanges;
    data.outputParameterChanges = &h.outChanges;
    data.inputEvents = &h.inEvents;
    data.outputEvents = &h.outEvents;
    data.processContext = &pc;
    m_processor->process(data);
    if (m_numOutputBuses > 0) {
        if (m_outChans[0] >= 2) {
            std::copy(m_outPtrs[0][0], m_outPtrs[0][0] + n, L);
            std::copy(m_outPtrs[0][1], m_outPtrs[0][1] + n, R);
        } else {
            std::copy(m_outPtrs[0][0], m_outPtrs[0][0] + n, L);
            std::copy(m_outPtrs[0][0], m_outPtrs[0][0] + n, R);
        }
    } else {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    }
    // parameter changes emitted by the plugin
    for (int i = 0; i < h.outChanges.getParameterCount(); ++i) {
        auto* q = h.outChanges.getParameterData(i);
        if (!q || q->getPointCount() == 0) continue;
        int32 off;
        ParamValue v;
        q->getPoint(q->getPointCount() - 1, off, v);
        int idx = m_params.indexOf(q->getParameterId());
        if (idx >= 0) {
            m_params.setAt(idx, (float)v);
            m_lastSent[idx] = (float)v;
            if (m_reported) m_reported[idx].store(m_params.getAt(idx), std::memory_order_relaxed);
        }
    }
    // MIDI out
    for (int i = 0; i < h.outEvents.getEventCount(); ++i) {
        Event ev;
        h.outEvents.getEvent(i, ev);
        MidiEvent me;
        me.sampleOffset = ev.sampleOffset;
        if (ev.type == kNoteOnEvent) {
            me.type = MidiEvent::NoteOn;
            me.channel = (uint8_t)ev.noteOn.channel;
            me.data1 = (uint8_t)ev.noteOn.pitch;
            me.velocity = ev.noteOn.velocity;
            me.data2 = (uint8_t)(me.velocity * 127);
            m_outMidi.add(me);
        } else if (ev.type == kNoteOffEvent) {
            me.type = MidiEvent::NoteOff;
            me.channel = (uint8_t)ev.noteOff.channel;
            me.data1 = (uint8_t)ev.noteOff.pitch;
            m_outMidi.add(me);
        }
    }
}

QJsonObject Vst3Plugin::saveState() const {
    QJsonObject o;
    o["key"] = key();
    o["format"] = "vst3";
    MemoryStream* ms = new MemoryStream();
    if (m_component->getState(ms) == kResultOk)
        o["pluginState"] = QString::fromLatin1(QByteArray((const char*)ms->data.data(), (int)ms->data.size()).toBase64());
    ms->release();
    if (m_controller) {
        MemoryStream* cs = new MemoryStream();
        if (m_controller->getState(cs) == kResultOk)
            o["controllerState"] = QString::fromLatin1(QByteArray((const char*)cs->data.data(), (int)cs->data.size()).toBase64());
        cs->release();
    }
    QJsonObject params;
    for (size_t i = 0; i < m_paramIds.size(); ++i) params[QString::number(m_paramIds[i])] = m_params.getAt((int)i);
    o["params"] = params;
    return o;
}
void Vst3Plugin::restoreState(const QJsonObject& s) {
    bool applied = false;
    QByteArray st = QByteArray::fromBase64(s["pluginState"].toString().toLatin1());
    if (!st.isEmpty()) {
        MemoryStream* ms = new MemoryStream();
        ms->data.assign(st.begin(), st.end());
        ms->pos = 0;
        bool was = m_active;
        setActive(false);
        m_component->setState(ms);
        ms->pos = 0;
        if (m_controller && !m_controllerIsComponent) m_controller->setComponentState(ms);
        ms->release();
        if (was) setActive(true);
        applied = true;
    }
    QByteArray cst = QByteArray::fromBase64(s["controllerState"].toString().toLatin1());
    if (!cst.isEmpty() && m_controller) {
        MemoryStream* cs = new MemoryStream();
        cs->data.assign(cst.begin(), cst.end());
        cs->pos = 0;
        m_controller->setState(cs);
        cs->release();
        applied = true;
    }
    // Nothing was written, so the mirror built at load time is still current: re-reading it would cost one blocking
    // controller call per parameter (2000+ IPC round trips for a bridged plugin) to learn values we already have.
    if (!applied) return;
    // the processor already holds this state: only parameters the host changes from here on are sent as parameter changes
    // (re-sending all 2000+ parameters of a big plugin in one block can trigger momentary/trigger parameters and reset voices)
    auto settle = [this] {
        refreshParamsFromController();
        for (size_t i = 0; i < m_paramIds.size() && i < m_lastSent.size(); ++i) m_lastSent[i] = m_params.getAt((int)i);
    };
    settle();
    // JUCE plugins (Surge XT) apply a loaded state on their message thread a moment later and don't announce the values it
    // changes (an effect-type switch resets its parameters), so the mirror is read once more after that
    QTimer::singleShot(300, [settle, alive = std::weak_ptr<int>(m_alive)] {
        if (alive.lock()) settle();
    });
}

QWidget* Vst3Plugin::createEditor(QWidget* parent) {
    if (!m_controller || !m_hasEditor) return nullptr;
    auto* w = new PluginEmbedWidget(parent);
    w->onAttach = [this, w](unsigned long xid, int& width, int& height) {
        if (m_host->view) return false;
        IPlugView* v = m_controller->createView(kEditorViewType);
        if (!v) return false;
        if (!m_host->frame) { m_host->frame = new PlugFrame(); }
        m_host->frame->widget = w;
        v->setFrame(m_host->frame);
        ViewRect r{};
        if (v->getSize(&r) == kResultOk) {
            width = r.getWidth();
            height = r.getHeight();
        }
        if (v->attached((void*)xid, kPlatformTypeX11EmbedWindowID) != kResultOk) {
            v->setFrame(nullptr);
            v->release();
            return false;
        }
        // HiDPI: native plugins get the real scale factor. Bridged (yabridge / Wine) plugins don't: Wine has no fractional
        // HiDPI support, so a factor like 1.25 makes the editor draw at two sizes at once and its hit-testing no longer
        // matches what you see (the same thing yabridge's own editor_disable_host_scaling option works around).
        if (!yabridge::isBridged(m_module ? m_module->path() : QString())) {
            IPlugViewContentScaleSupport* sc = nullptr;
            if (v->queryInterface(IPlugViewContentScaleSupport_iid.id, (void**)&sc) == kResultOk && sc) {
                sc->setContentScaleFactor((float)w->devicePixelRatioF());
                sc->release();
            }
        }
        m_host->view = v;
        if (v->getSize(&r) == kResultOk && r.getWidth() > 0 && r.getHeight() > 0) {
            width = r.getWidth();
            height = r.getHeight();
        }   // the view may have picked its final size while attaching (scaling, bridged plugins)
        return true;
    };
    w->onDetach = [this] {
        if (m_host->view) {
            m_host->view->removed();
            m_host->view->setFrame(nullptr);
            m_host->view->release();
            m_host->view = nullptr;
        }
        if (m_host->frame) m_host->frame->runLoop->clearAll();
    };
    w->onResized = [this, w](int width, int height) {
        if (!m_host->view || m_host->view->canResize() != kResultTrue) return;
        ViewRect r{0, 0, width, height};
        m_host->view->checkSizeConstraint(&r);   // snaps r to a size the editor allows
        m_host->view->onSize(&r);
        if (r.getWidth() != width || r.getHeight() != height) w->snapTo(r.getWidth(), r.getHeight());
    };
    // Linux editors that don't take X focus themselves (VSTGUI) get their keys from the host
    w->onKey = [this](QKeyEvent* e, bool down) {
        if (!m_host->view) return false;
        static const std::pair<int, int16> keys[] = {{Qt::Key_Backspace, 1}, {Qt::Key_Tab, 2}, {Qt::Key_Return, 4}, {Qt::Key_Escape, 6},
            {Qt::Key_Space, 7}, {Qt::Key_End, 9}, {Qt::Key_Home, 10}, {Qt::Key_Left, 11}, {Qt::Key_Up, 12}, {Qt::Key_Right, 13},
            {Qt::Key_Down, 14}, {Qt::Key_PageUp, 15}, {Qt::Key_PageDown, 16}, {Qt::Key_Enter, 19}, {Qt::Key_Insert, 21},
            {Qt::Key_Delete, 22}};   // VST3 VirtualKeyCodes
        int16 code = 0;
        for (auto [qt, vk] : keys)
            if (e->key() == qt) code = vk;
        char16_t ch = code || e->text().isEmpty() ? 0 : e->text().at(0).unicode();
        if (!code && !ch) return false;
        Qt::KeyboardModifiers m = e->modifiers();
        int16 mods = (m & Qt::ShiftModifier ? 1 : 0) | (m & Qt::AltModifier ? 2 : 0) |
            (m & Qt::ControlModifier ? 4 : 0);   // shift, alt, command (= Ctrl off macOS)
        return (down ? m_host->view->onKeyDown(ch, code, mods) : m_host->view->onKeyUp(ch, code, mods)) == kResultTrue;
    };
    return w;
}

}   // namespace mt
