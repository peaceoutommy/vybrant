#include "plugins/PluginBridge.h"
#include "plugins/PluginHost.h"
#include "plugins/Vst2Plugin.h"
#include "plugins/Vst3Plugin.h"
#include "plugins/ClapPlugin.h"
#include "plugins/Lv2Plugin.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace mt {

namespace bridge {
constexpr int kMaxBlock = 4096, kMaxParams = 16384, kMaxMidi = MidiEventList::kCapacity, kMaxAux = 64;   // ~2 MB per plugin
struct Shm {
    sem_t request, done;
    std::atomic<int> quit, reset, edited;   // edited: the plugin's editor moved a parameter (marks the project dirty)
    int frames, numMidi, numOutMidi, numParams, latency;
    bool hasSidechain;
    ProcessContext ctx;   // its sidechain pointers are re-pointed into this block on the plugin side
    MidiEvent midi[kMaxMidi], outMidi[kMaxMidi];
    float params[kMaxParams], paramsOut[kMaxParams];   // host -> plugin every block; plugin -> host when its editor moved one
    uint8_t paramsOutSet[kMaxParams];
    float audio[2 + 2 * kMaxAux][kMaxBlock];   // the main pair in place, then the extra output pairs
    float sidechain[2][kMaxBlock];
};
static_assert(std::is_trivially_copyable_v<MidiEvent> && std::is_trivially_copyable_v<ProcessContext>);
static_assert(std::atomic<int>::is_always_lock_free);   // so the atomics work across processes

static Shm* map(const QString& name, bool create) {
    int fd = shm_open(name.toLocal8Bit().constData(), create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR, 0600);
    if (fd < 0) return nullptr;
    if (create && ftruncate(fd, sizeof(Shm)) != 0) {   // zero-filled
        close(fd);
        return nullptr;
    }
    void* p = mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? nullptr : (Shm*)p;
}
static QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }
static QJsonObject descJson(const ParamDesc& d) {
    QJsonArray choices;
    for (auto& c : d.choices) choices.append(QString::fromStdString(c));
    return {{"id", (double)d.id}, {"name", QString::fromStdString(d.name)}, {"min", d.min}, {"max", d.max}, {"def", d.def},
        {"unit", (int)d.unit}, {"auto", d.automatable}, {"log", d.logarithmic}, {"steps", d.steps}, {"choices", choices}};
}
static ParamDesc descFrom(const QJsonObject& o) {
    ParamDesc d;
    d.id = (uint32_t)o["id"].toDouble();
    d.name = o["name"].toString().toStdString();
    d.min = (float)o["min"].toDouble();
    d.max = (float)o["max"].toDouble();
    d.def = (float)o["def"].toDouble();
    d.unit = (ParamUnit)o["unit"].toInt();
    d.automatable = o["auto"].toBool();
    d.logarithmic = o["log"].toBool();
    d.steps = o["steps"].toInt();
    for (auto c : o["choices"].toArray()) d.choices.push_back(c.toString().toStdString());
    return d;
}
static QJsonObject dataJson(const PluginInstanceData& d) {
    return {{"id", (double)d.id}, {"format", d.format}, {"identifier", d.identifier}, {"name", d.name}, {"path", d.path},
        {"state", QString::fromLatin1(d.state.toBase64())}, {"controllerState", QString::fromLatin1(d.controllerState.toBase64())}};
}
static PluginInstanceData dataFrom(const QJsonObject& o) {
    PluginInstanceData d;   // not bridged: the plugin process hosts it directly
    d.id = (Id)o["id"].toDouble();
    d.format = o["format"].toString();
    d.identifier = o["identifier"].toString();
    d.name = o["name"].toString();
    d.path = o["path"].toString();
    d.state = QByteArray::fromBase64(o["state"].toString().toLatin1());
    d.controllerState = QByteArray::fromBase64(o["controllerState"].toString().toLatin1());
    return d;
}
}   // namespace bridge

// ---------------------------------------------------------------- app side
std::unique_ptr<BridgedPlugin> BridgedPlugin::create(const PluginInstanceData& data, QString* error) {
    static std::atomic<int> counter{0};
    std::unique_ptr<BridgedPlugin> b(new BridgedPlugin());
    auto fail = [&](const QString& e) -> std::unique_ptr<BridgedPlugin> {
        if (error) *error = e;
        Log::error("Plugin process for " + data.name + ": " + e);
        return nullptr;
    };
    b->m_data = data;
    QString tag = QString("mt-bridge-%1-%2").arg(QCoreApplication::applicationPid()).arg(counter++);
    b->m_shmName = "/" + tag;
    shm_unlink(b->m_shmName.toLocal8Bit().constData());   // left over by an earlier run that had the same pid
    if (!(b->m_shm = bridge::map(b->m_shmName, true))) return fail("cannot create shared memory");
    sem_init(&b->m_shm->request, 1, 0);
    sem_init(&b->m_shm->done, 1, 0);
    b->m_server = new QLocalServer(b.get());
    QLocalServer::removeServer(tag);
    if (!b->m_server->listen(tag)) return fail("cannot listen on " + tag);
    b->m_proc = new QProcess(b.get());
    b->m_proc->setProcessChannelMode(QProcess::ForwardedChannels);
    b->m_proc->start(QCoreApplication::applicationFilePath(), {"--plugin-bridge", b->m_server->fullServerName(), b->m_shmName});
    if (!b->m_proc->waitForStarted(10000)) return fail("the plugin process did not start");
    b->m_pid = (int)b->m_proc->processId();
    b->m_pidfd = (int)syscall(SYS_pidfd_open, (pid_t)b->m_pid, 0);
    int timeout = yabridge::isBridged(data.path) ? 120000 : 30000;   // Wine has to boot first
    if (!b->m_server->waitForNewConnection(timeout)) return fail("the plugin process did not connect");
    b->m_sock = b->m_server->nextPendingConnection();
    QJsonObject r = b->call({{"op", "create"}, {"plugin", bridge::dataJson(data)}}, timeout);
    if (!r["ok"].toBool()) return fail(r.isEmpty() ? "the plugin process did not answer" : r["error"].toString());
    b->m_key = r["key"].toString();
    b->m_name = r["name"].toString();
    b->m_format = r["format"].toString();
    b->m_kind = (ProcessorKind)r["kind"].toInt();
    b->m_hasEditor = r["editor"].toBool();
    b->m_wantsSidechain = r["sidechain"].toBool();
    b->m_tail = r["tail"].toInt();
    b->m_latency = b->m_reportedLatency = r["latency"].toInt();
    for (auto a : r["aux"].toArray()) b->m_auxNames << a.toString();
    QJsonArray descs = r["params"].toArray(), values = r["values"].toArray();
    for (int i = 0; i < descs.size() && i < bridge::kMaxParams; ++i) {
        b->m_params.add(bridge::descFrom(descs[i].toObject()));
        b->m_params.setAt(i, (float)values[i].toDouble());   // the plugin's state, not the defaults
    }
    BridgedPlugin* self = b.get();
    connect(b->m_sock, &QLocalSocket::readyRead, self, [self] { self->pump(); });
    connect(b->m_proc, &QProcess::finished, self, [self] {
        if (!self->m_shuttingDown) self->died();
    });
    b->m_editTimer.setInterval(200);
    connect(&b->m_editTimer, &QTimer::timeout, self, [self] {
        if (self->m_edited.exchange(false) && self->onParamEdited) self->onParamEdited(0, 0.f);
    });
    b->m_editTimer.start();
    Log::info("Plugin " + b->m_name + " runs in its own process (pid " + QString::number(b->m_pid) + ")");
    return b;
}
BridgedPlugin::~BridgedPlugin() {
    m_shuttingDown = true;
    if (m_shm) {
        m_shm->quit = 1;
        sem_post(&m_shm->request);
    }
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        send({{"op", "quit"}});
        if (!m_proc->waitForFinished(2000)) m_proc->kill();
        m_proc->waitForFinished(1000);
    }
    if (m_pidfd >= 0) close(m_pidfd);
    if (m_shm) {
        sem_destroy(&m_shm->request);
        sem_destroy(&m_shm->done);
        munmap(m_shm, sizeof(bridge::Shm));
        shm_unlink(m_shmName.toLocal8Bit().constData());
    }
}
void BridgedPlugin::send(const QJsonObject& message) const {
    if (!m_sock || m_dead.load()) return;
    m_sock->write(bridge::line(message));
    m_sock->flush();
}
void BridgedPlugin::pump() const {
    m_inbox += m_sock->readAll();
    for (qsizetype nl; (nl = m_inbox.indexOf('\n')) >= 0;) {
        QJsonObject o = QJsonDocument::fromJson(m_inbox.left(nl)).object();
        m_inbox.remove(0, nl + 1);
        if (o.contains("re")) {
            if (o["re"].toString() == m_waitingFor) m_answer = o;
        } else if (o["ev"].toString() == "size" && m_editorSized) m_editorSized(o["w"].toInt(), o["h"].toInt());
    }
}
QJsonObject BridgedPlugin::call(const QJsonObject& request, int timeoutMs) const {
    if (!m_sock || m_dead.load()) return {};
    m_waitingFor = request["op"].toString();
    m_answer = {};
    send(request);
    QElapsedTimer t;
    t.start();
    while (m_answer.isEmpty() && t.elapsed() < timeoutMs && m_sock->state() == QLocalSocket::ConnectedState) {
        m_sock->waitForReadyRead(50);
        pump();
    }
    m_waitingFor.clear();
    return m_answer;
}
bool BridgedPlugin::childExited() const {
    if (m_pidfd < 0) return kill(m_pid, 0) != 0;
    pollfd p{m_pidfd, POLLIN, 0};
    return poll(&p, 1, 0) > 0;   // a pidfd turns readable when the process ends
}
void BridgedPlugin::died() {
    m_dead = true;
    if (m_reported.exchange(true)) return;
    Log::error("The process of plugin " + m_name + " ended: the plugin is bypassed, the app keeps running");
    if (onCrashed) onCrashed(m_data.id, m_name);
}

void BridgedPlugin::prepare(double sampleRate, int maxBlockSize) {
    QJsonObject r = call({{"op", "prepare"}, {"sr", sampleRate}, {"block", std::min(maxBlockSize, bridge::kMaxBlock)}});
    if (r.contains("latency"))
        m_latency = r["latency"].toInt();   // known once activated: delay compensation needs it before the first block
}
void BridgedPlugin::reset() {
    if (m_shm) m_shm->reset = 1;
}
void BridgedPlugin::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    bridge::Shm* s = m_shm;
    m_outMidi.clear();
    if (m_dead.load() || n > bridge::kMaxBlock) {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
        return;
    }
    s->frames = n;
    std::copy(L, L + n, s->audio[0]);
    std::copy(R, R + n, s->audio[1]);
    s->numMidi = std::min(midi.count, bridge::kMaxMidi);
    std::copy(midi.events, midi.events + s->numMidi, s->midi);
    s->ctx = ctx;
    s->hasSidechain = ctx.sidechainL && ctx.sidechainR;
    if (s->hasSidechain) {
        std::copy(ctx.sidechainL, ctx.sidechainL + n, s->sidechain[0]);
        std::copy(ctx.sidechainR, ctx.sidechainR + n, s->sidechain[1]);
    }
    int np = m_params.count();
    s->numParams = np;
    for (int i = 0; i < np; ++i) s->params[i] = m_params.getAt(i);
    sem_post(&s->request);
    // wait in short slices: a plugin process that died mid-block never answers, and that must be noticed at once
    for (int waited = 0;; waited += 20) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 20'000'000;
        if (ts.tv_nsec >= 1'000'000'000) {
            ts.tv_nsec -= 1'000'000'000;
            ++ts.tv_sec;
        }
        if (sem_clockwait(&s->done, CLOCK_MONOTONIC, &ts) == 0) break;
        if (childExited() || waited >= 2000) {   // gone, or hung for two seconds
            m_dead = true;
            kill(m_pid, SIGKILL);
            std::fill(L, L + n, 0.f);
            std::fill(R, R + n, 0.f);
            return;
        }
    }
    std::copy(s->audio[0], s->audio[0] + n, L);
    std::copy(s->audio[1], s->audio[1] + n, R);
    for (int i = 0; i < np; ++i)
        if (s->paramsOutSet[i]) {
            s->paramsOutSet[i] = 0;
            m_params.setAt(i, s->paramsOut[i]);
        }
    if (s->edited.exchange(0)) m_edited = true;
    m_latency.store(s->latency);
    for (int i = 0; i < s->numOutMidi; ++i) m_outMidi.add(s->outMidi[i]);
}
const float* BridgedPlugin::auxOutput(int index, int side) const { return m_shm->audio[2 + 2 * index + side]; }
QJsonObject BridgedPlugin::saveState() const {
    QJsonObject r = call({{"op", "save"}});
    if (r.contains("state")) m_lastState = r["state"].toObject();
    if (m_lastState.isEmpty())   // gone before it ever saved: what it was created from
        m_lastState = QJsonObject{{"pluginState", QString::fromLatin1(m_data.state.toBase64())},
            {"controllerState", QString::fromLatin1(m_data.controllerState.toBase64())}};
    return m_lastState;
}
void BridgedPlugin::restoreState(const QJsonObject& s) {
    m_lastState = s;
    call({{"op", "restore"}, {"state", s}});   // the changed values come back through the next block
}
QString BridgedPlugin::formatValue(uint32_t paramId) const {
    float v = m_params.get(paramId);
    auto it = m_valueText.find(paramId);
    if (it != m_valueText.end() && it->second.first == v) return it->second.second;
    QString text = call({{"op", "format"}, {"id", (double)paramId}})["text"].toString();
    m_valueText[paramId] = {v, text};
    return text;
}
QWidget* BridgedPlugin::createEditor(QWidget* parent) {
    QJsonObject r = call({{"op", "editor"}});
    if (!r["ok"].toBool()) return nullptr;
    // the editor is a window of the plugin process; Qt embeds a foreign X11 window through a window container
    auto* holder = new QWidget(parent);
    auto* lay = new QVBoxLayout(holder);
    lay->setContentsMargins(0, 0, 0, 0);
    QWidget* c = QWidget::createWindowContainer(QWindow::fromWinId((WId)r["xid"].toDouble()), holder);
    c->setMinimumSize(r["w"].toInt(), r["h"].toInt());
    lay->addWidget(c);
    m_editorSized = [holder, c](int w, int h) {
        c->setMinimumSize(w, h);
        holder->updateGeometry();
        if (QWidget* top = holder->window()) {
            if (top->layout()) top->layout()->activate();
            top->adjustSize();
        }
    };
    struct Resize : QObject {
        std::function<void(QSize)> fn;
        bool eventFilter(QObject* o, QEvent* e) override {
            if (e->type() == QEvent::Resize) fn(static_cast<QWidget*>(o)->size());
            return false;
        }
    };
    auto* rf = new Resize;
    rf->setParent(c);
    rf->fn = [this](QSize s) { send({{"op", "editorResize"}, {"w", s.width()}, {"h", s.height()}}); };
    c->installEventFilter(rf);
    connect(holder, &QObject::destroyed, this, [this] {
        m_editorSized = nullptr;
        onParamEdited = nullptr;   // set by the window this editor lived in
        call({{"op", "editorClose"}});
    });
    QTimer::singleShot(0, holder, [this] { send({{"op", "editorShow"}}); });   // shown (and attached) once it sits inside ours
    return holder;
}

// ---------------------------------------------------------------- plugin process
namespace {
struct Bridge {
    std::unique_ptr<IProcessor> proc;
    bridge::Shm* shm = nullptr;
    QLocalSocket sock;
    QByteArray inbox;
    QWidget* editor = nullptr;
    std::thread audio;
    std::vector<float> lastIn;   // what the app sent: a parameter the plugin changed itself differs from it
    MidiEventList midi;

    void reply(const QString& op, QJsonObject o) {
        o["re"] = op;
        sock.write(bridge::line(o));
        sock.flush();
    }
    QJsonArray values() const {
        QJsonArray v;
        for (int i = 0; i < proc->params().count(); ++i) v.append(proc->params().getAt(i));
        return v;
    }
    void audioLoop() {
        sched_param sp{};
        sp.sched_priority = 70;   // it runs inside the app's audio callback; ignored where realtime isn't allowed
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        bridge::Shm* s = shm;
        for (;;) {
            if (sem_wait(&s->request) != 0) continue;
            if (s->quit) return;
            if (s->reset.exchange(0)) proc->reset();
            ParameterSet& ps = proc->params();
            int np = std::min(s->numParams, ps.count());
            for (int i = 0; i < np; ++i)
                if (s->params[i] != lastIn[i]) ps.setAt(i, lastIn[i] = s->params[i]);
            midi.count = s->numMidi;
            std::copy(s->midi, s->midi + s->numMidi, midi.events);
            ProcessContext ctx = s->ctx;
            ctx.sidechainL = s->hasSidechain ? s->sidechain[0] : nullptr;
            ctx.sidechainR = s->hasSidechain ? s->sidechain[1] : nullptr;
            int n = s->frames;
            proc->process(s->audio[0], s->audio[1], n, midi, ctx);
            for (int k = 0; k < std::min(proc->auxOutputCount(), bridge::kMaxAux); ++k)
                for (int side = 0; side < 2; ++side)
                    std::copy(proc->auxOutput(k, side), proc->auxOutput(k, side) + n, s->audio[2 + 2 * k + side]);
            for (int i = 0; i < np; ++i)
                if (float v = ps.getAt(i); v != lastIn[i]) {   // its own editor moved it: tell the app
                    s->paramsOut[i] = lastIn[i] = v;
                    s->paramsOutSet[i] = 1;
                }
            s->latency = proc->latencySamples();
            const MidiEventList* out = proc->outputMidi();
            s->numOutMidi = out ? std::min(out->count, bridge::kMaxMidi) : 0;
            if (out) std::copy(out->events, out->events + s->numOutMidi, s->outMidi);
            sem_post(&s->done);
        }
    }
    void handle(const QJsonObject& o) {
        QString op = o["op"].toString();
        if (op == "create") {
            PluginDatabase db;
            db.load();
            PluginHost host(db);
            QString err;
            proc = host.createInstance(bridge::dataFrom(o["plugin"].toObject()), &err);
            if (!proc) return reply(op, {{"ok", false}, {"error", err}});
            QJsonArray descs, aux;
            for (int i = 0; i < proc->params().count(); ++i) descs.append(bridge::descJson(proc->params().desc(i)));
            for (int k = 0; k < std::min(proc->auxOutputCount(), bridge::kMaxAux); ++k) aux.append(proc->auxOutputName(k));
            for (int i = 0; i < proc->params().count(); ++i) lastIn.push_back(proc->params().getAt(i));
            auto edited = [s = shm](uint32_t, float) { s->edited = 1; };
            if (auto* v3 = dynamic_cast<Vst3Plugin*>(proc.get())) v3->onParamEdited = edited;
            if (auto* v2 = dynamic_cast<Vst2Plugin*>(proc.get())) v2->onParamEdited = edited;
            if (auto* c = dynamic_cast<ClapPlugin*>(proc.get())) c->onParamEdited = edited;
            if (auto* l2 = dynamic_cast<Lv2Plugin*>(proc.get())) l2->onParamEdited = edited;
            reply(op,
                {{"ok", true}, {"key", proc->key()}, {"name", proc->displayName()}, {"format", proc->pluginFormat()},
                    {"kind", (int)proc->kind()}, {"editor", proc->hasCustomEditor()}, {"sidechain", proc->wantsSidechain()},
                    {"tail", proc->tailSamples()}, {"latency", proc->latencySamples()}, {"aux", aux}, {"params", descs},
                    {"values", values()}});
            audio = std::thread([this] { audioLoop(); });
        } else if (op == "prepare") {
            proc->prepare(o["sr"].toDouble(), o["block"].toInt());
            reply(op, {{"latency", proc->latencySamples()}});
        } else if (op == "save") reply(op, {{"state", proc->saveState()}});
        else if (op == "restore") {
            proc->restoreState(o["state"].toObject());
            reply(op, {});
        } else if (op == "format") reply(op, {{"text", proc->formatValue((uint32_t)o["id"].toDouble())}});
        else if (op == "editor") {
            delete editor;
            editor = proc->createEditor(nullptr);
            if (!editor) return reply(op, {{"ok", false}});
            editor->setWindowFlags(Qt::FramelessWindowHint);
            QSize sz = editor->sizeHint();
            editor->resize(sz);
            struct Resize : QObject {
                Bridge* b;
                bool eventFilter(QObject* w, QEvent* e) override {
                    if (e->type() == QEvent::Resize) {
                        QSize s = static_cast<QWidget*>(w)->size();
                        b->sock.write(bridge::line({{"ev", "size"}, {"w", s.width()}, {"h", s.height()}}));
                        b->sock.flush();
                    }
                    return false;
                }
            };
            auto* rf = new Resize;
            rf->b = this;
            rf->setParent(editor);
            editor->installEventFilter(rf);
            reply(op, {{"ok", true}, {"xid", (double)editor->winId()}, {"w", sz.width()}, {"h", sz.height()}});
        } else if (op == "editorShow" && editor) editor->show();
        else if (op == "editorResize" && editor) editor->resize(o["w"].toInt(), o["h"].toInt());
        else if (op == "editorClose") {
            delete editor;
            editor = nullptr;
            reply(op, {});
        } else if (op == "quit") qApp->quit();
    }
};
}   // namespace

int pluginBridgeMain(const QString& socketName, const QString& shmName) {
    Bridge b;
    if (!(b.shm = bridge::map(shmName, false))) return 2;
    b.sock.connectToServer(socketName);
    if (!b.sock.waitForConnected(10000)) return 3;
    QObject::connect(&b.sock, &QLocalSocket::readyRead, [&b] {
        b.inbox += b.sock.readAll();
        for (qsizetype nl; (nl = b.inbox.indexOf('\n')) >= 0;) {
            QJsonObject o = QJsonDocument::fromJson(b.inbox.left(nl)).object();
            b.inbox.remove(0, nl + 1);
            b.handle(o);
        }
    });
    QObject::connect(&b.sock, &QLocalSocket::disconnected, qApp, &QCoreApplication::quit);   // the app is gone
    QTimer ui;   // the main-thread upkeep PluginWindow does for in-process plugins: the editor follows automation
    QObject::connect(&ui, &QTimer::timeout, [&b] {
        if (auto* v3 = dynamic_cast<Vst3Plugin*>(b.proc.get())) v3->syncControllerFromParams();
    });
    ui.start(33);
    int rc = qApp->exec();
    b.shm->quit = 1;
    sem_post(&b.shm->request);
    if (b.audio.joinable()) b.audio.join();
    delete b.editor;
    b.proc.reset();
    return rc;
}

}   // namespace mt
