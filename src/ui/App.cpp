#include <QMenu>
#include <QColorDialog>
#include "ui/App.h"
#include "audio/JackTransport.h"
#include "plugins/PluginBridge.h"
#include "ui/widgets/Controls.h"
#include "project/MidiFile.h"
#include <QRegularExpression>
#include <cstdio>
#include "audio/RenderEngine.h"
#include "dsp/NativeRegistry.h"
#include "dsp/instruments/Instruments.h"
#include "core/Log.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDirIterator>
#include <QCoreApplication>

namespace mt {

static App* s_app = nullptr;
App* App::instance() { return s_app; }

App::App(QObject* parent)
    : QObject(parent) {
    s_app = this;
    m_engine = std::make_unique<AudioEngine>(m_devices, m_cache);
    m_recorder = std::make_unique<RecordingEngine>();
    m_midi = std::make_unique<MidiEngine>(*m_engine);
    m_scanner = std::make_unique<PluginScanner>(m_pluginDb);
    m_pluginHost = std::make_unique<PluginHost>(m_pluginDb);
    m_engine->setRecordingEngine(m_recorder.get());
    m_engine->setPluginFactory([this](const PluginInstanceData& d) { return m_pluginHost->createInstance(d); });
    connect(&m_model, &ProjectModel::changed, this, &App::onModelChanged);
    connect(m_engine.get(), &AudioEngine::recordedMidi, this, &App::onRecordedMidi);
    connect(m_engine.get(), &AudioEngine::recordSegment, this, &App::onRecordSegment);
    connect(m_engine.get(), &AudioEngine::transportStateChanged, this, &App::onTransportStateChanged);
    connect(m_engine.get(), &AudioEngine::pluginCrashed, this, &App::onPluginCrashed);
    connect(m_midi.get(), &MidiEngine::ccReceived, this, &App::onCcReceived);
    BridgedPlugin::onCrashed = [this](Id instanceId, const QString& name) {
        if (auto* pd = project().plugin(instanceId)) pd->crashed = true;
        Q_EMIT statusMessage("Plugin " + name +
                " crashed in its own process - the app keeps running; turn 'Own process' off and on in its window to restart it",
            0);
    };
    connect(m_midi.get(), &MidiEngine::clockTransport, this, [this](int what, double tick) {
        if (what == 0) seek(0);
        if (what == 0 || what == 1) play();
        else if (what == 2) pause();   // MIDI Stop: a Continue resumes from here
        else seek((Tick)tick);
    });
    connect(&m_autosaveTimer, &QTimer::timeout, this, &App::autosave);
    connect(&m_stretchTimer, &QTimer::timeout, this, &App::updateSamplerStretch);
    m_stretchTimer.start(250);
    {
        auto as = autosaveSettings();
        if (as.enabled && as.intervalMinutes > 0) m_autosaveTimer.start(as.intervalMinutes * 60 * 1000);
    }
}
App::~App() {
    m_midi->stop();
    m_engine->stopDevice();
    s_app = nullptr;
}

// The device can be busy for a moment (e.g. a previous instance still closing it): keep trying for a while before giving up
void App::retryAudioDevice(int attempt) {
    QTimer::singleShot(500, this, [this, attempt] {
        if (m_engine->deviceRunning()) return;
        QString err;
        if (m_engine->startDevice(&err)) {
            m_recorder->setSampleRate(m_engine->sampleRate());
            Q_EMIT statusMessage("Audio device started", 3000);
            return;
        }
        if (attempt < 20) retryAudioDevice(attempt + 1);
        else {
            fprintf(stderr, "Audio device could not be opened: %s\n", err.toUtf8().constData());
            Q_EMIT statusMessage("No audio output: " + err + " - check Options > Audio settings", 0);
        }
    });
}
void App::initialize() {
    m_pluginDb.load();
    m_devices.initialize();
    m_devices.loadConfig();
    QString err;
    if (!m_engine->startDevice(&err)) {
        Log::warn("Audio device not started: " + err);
        fprintf(stderr, "Audio device not started: %s (retrying)\n", err.toUtf8().constData());
        retryAudioDevice(0);
    }
    m_recorder->setSampleRate(m_engine->sampleRate());
    m_midi->start();
    m_engine->setMidiClockOut(QSettings().value("midi/clockOut", false).toBool());
    m_midi->setClockFollow(QSettings().value("midi/clockIn", false).toBool());
    if (QSettings().value("sync/jack", false).toBool()) setJackSync(true);
    m_model.replaceProject(makeDefaultProject());
    m_engine->rebuild(project());
}

// ---------------------------------------------------------------- model -> engine
void App::onModelChanged(uint32_t flags) {
    if (flags & ChangeTransport) syncTransportToEngine();
    if (flags & (ChangeChannels | ChangeMixer | ChangeStructure)) syncMixerTrackNames();
    m_engine->requestRebuild();
}
// Mixer tracks the user hasn't renamed carry the name of the first channel routed to them, or "Insert N" when none is.
// Derived state, recomputed on every change (connected before any view, so they all see the new names) - undo needs nothing.
void App::syncMixerTrackNames() {
    auto& tracks = project().mixerTracks;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& t = tracks[i];
        if (t.isMaster || t.customName) continue;
        t.name = QString("Insert %1").arg(i);
        for (auto& c : project().channels)
            if (c.mixerTrackId == t.id) {
                t.name = c.name;
                break;
            }
    }
}
// Samplers in a stretch mode play a Rubber Band render of their sample (Tune, Time and tempo fit applied). Keep it current:
// a changed setting or tempo drops the stale render (resampling covers the gap) and requests the new one. The cache renders
// one file at a time, so polling while a knob moves only renders the value it settles on.
void App::updateSamplerStretch() {
    double bpm = project().transport.bpm;
    for (auto& c : project().channels) {
        auto* s = dynamic_cast<Sampler*>(processor(c.id));
        if (!s) continue;
        auto& p = s->params();
        int mode = p.getInt(Sampler::pStretchMode);
        double ratio = s->timeRatio(bpm), pitch = p.get(Sampler::pTune);
        QString path = s->samplePath();
        bool want = mode != StretchResample && !path.isEmpty() && s->sample() && (std::fabs(ratio - 1.0) > 1e-4 || std::fabs(pitch) > 1e-3);
        QString key = want ? QString("%1|%2|%3|%4").arg(path).arg(ratio, 0, 'f', 5).arg(pitch, 0, 'f', 3).arg(mode) : QString();
        if (key == s->stretchKey()) continue;
        AudioAssetPtr a = want ? m_cache.getStretched(path, ratio, pitch, mode) : nullptr;
        if (a || !want) s->setStretched(a, key);
        else if (!s->stretchKey().isEmpty()) s->setStretched(nullptr, QString());   // stale: resample until the render lands
    }
}
void App::syncTransportToEngine() {
    auto& t = project().transport;
    m_engine->setBpm(t.bpm);
    m_engine->setTimeSig(t.timeSig.numerator, t.timeSig.denominator);
    m_engine->setLoop(t.loopEnabled, t.loopStart, t.loopEnd);
    m_engine->setSongMode(t.songMode);
    m_engine->setLoopRecord(t.loopRecord);
    m_engine->setMetronome(t.metronome);
    m_engine->setCountIn(t.countIn, t.countInBars);
    m_engine->setPunch(t.punchEnabled, t.punchIn, t.punchOut);
}

// ---------------------------------------------------------------- project lifecycle
QString App::projectTitle() const {
    return m_model.filePath().isEmpty() ? m_model.project().meta.title : QFileInfo(m_model.filePath()).completeBaseName();
}
QString App::templatesDir() const {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/templates";
    QDir().mkpath(d);
    return d;
}
QString App::autosavePath() const {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave";
    QDir().mkpath(d);
    return d + "/recovery.mtp";
}
QStringList App::recentFiles() const { return QSettings().value("general/recent").toStringList(); }
void App::addRecent(const QString& path) {
    QStringList r = recentFiles();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 12) r.removeLast();
    QSettings().setValue("general/recent", r);
}
QStringList App::templates() const {
    QStringList out;
    for (auto& f : QDir(templatesDir()).entryList({"*.mtp"}, QDir::Files, QDir::Name)) out << templatesDir() + "/" + f;
    return out;
}

void App::newProject(bool demo) {
    m_engine->stop();
    m_engine->unloadProject();
    m_recorder->stop();   // the pass belongs to the project being closed: its files are left on disk, unplaced
    m_recPass = false;
    m_model.setFilePath(QString());
    m_model.replaceProject(makeDefaultProject(demo));
    m_engine->rebuild(project());
    syncTransportToEngine();
    Q_EMIT projectLoaded();
}
bool App::newFromTemplate(const QString& path) {
    if (!openProject(path)) return false;
    m_model.setFilePath(QString());
    m_model.setDirty(true);
    return true;
}
bool App::openProject(const QString& path) {
    Project p;
    QString err;
    if (!ProjectSerializer::load(path, p, &err)) {
        Q_EMIT statusMessage("Open failed: " + err, 5000);
        Log::error("Open failed: " + err);
        return false;
    }
    m_engine->stop();
    m_engine->unloadProject();
    m_recorder->stop();   // the pass belongs to the project being closed: its files are left on disk, unplaced
    m_recPass = false;
    m_model.setFilePath(path);
    m_model.replaceProject(std::move(p));
    m_recorder->setRecordDirectory(QFileInfo(path).absolutePath() + "/Recordings");
    m_engine->rebuild(project());
    syncTransportToEngine();
    m_engine->seek(project().transport.position);
    addRecent(path);
    Q_EMIT projectLoaded();
    Q_EMIT statusMessage("Opened " + path, 3000);
    return true;
}
bool App::saveProject() {
    if (m_model.filePath().isEmpty()) return false;
    return saveProjectAs(m_model.filePath());
}
bool App::saveProjectAs(const QString& path) {
    m_engine->syncStateToProject(project());
    project().meta.modified = QDateTime::currentDateTime().toString(Qt::ISODate);
    project().transport.position = (Tick)m_engine->positionTicks();
    for (auto& [id, proc] : m_engine->registry().processors()) (void)id;
    QString err;
    if (!ProjectSerializer::save(project(), path, &err)) {
        Q_EMIT statusMessage("Save failed: " + err, 5000);
        return false;
    }
    m_model.setFilePath(path);
    m_model.setDirty(false);
    m_recorder->setRecordDirectory(QFileInfo(path).absolutePath() + "/Recordings");
    addRecent(path);
    QFile::remove(autosavePath());
    Q_EMIT statusMessage("Saved " + path, 3000);
    return true;
}
bool App::saveTemplate(const QString& name) {
    m_engine->syncStateToProject(project());
    QString err;
    return ProjectSerializer::save(project(), templatesDir() + "/" + name + ".mtp", &err);
}
App::AutosaveSettings App::autosaveSettings() const {
    QSettings s;
    AutosaveSettings a;
    a.enabled = s.value("autosave/enabled", true).toBool();
    a.intervalMinutes = s.value("autosave/intervalMinutes", 5).toInt();
    a.maxCopies = s.value("autosave/maxCopies", 10).toInt();
    return a;
}
void App::setAutosaveSettings(const AutosaveSettings& a) {
    QSettings s;
    s.setValue("autosave/enabled", a.enabled);
    s.setValue("autosave/intervalMinutes", a.intervalMinutes);
    s.setValue("autosave/maxCopies", a.maxCopies);
    s.sync();
    m_autosaveTimer.stop();
    if (a.enabled && a.intervalMinutes > 0) m_autosaveTimer.start(a.intervalMinutes * 60 * 1000);
}
QString App::backupDir() const {
    if (m_model.filePath().isEmpty()) return QString();
    return QFileInfo(m_model.filePath()).absolutePath() + "/Backup";
}
QStringList App::backups() const {
    QString d = backupDir();
    if (d.isEmpty()) return {};
    QString base = QFileInfo(m_model.filePath()).completeBaseName();
    QStringList out;
    for (auto& f : QDir(d).entryList({base + "_*.mtp"}, QDir::Files, QDir::Name | QDir::Reversed)) out << d + "/" + f;
    return out;
}
bool App::writeBackupNow(QString* error) {
    if (m_model.filePath().isEmpty()) {
        if (error) *error = "Save the project first";
        return false;
    }
    QString dir = backupDir();
    QDir().mkpath(dir);
    QString base = QFileInfo(m_model.filePath()).completeBaseName();
    QString path = QString("%1/%2_%3.mtp").arg(dir, base, QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss"));
    m_engine->syncStateToProject(project());
    Project copy = project();
    copy.settings.projectDir = QFileInfo(m_model.filePath()).absolutePath();
    if (!ProjectSerializer::save(copy, path, error)) return false;
    // recycle: keep only the newest N copies
    int keep = std::max(1, autosaveSettings().maxCopies);
    QStringList all = backups();
    for (int i = keep; i < all.size(); ++i) QFile::remove(all[i]);
    return true;
}
void App::autosave() {
    if (!m_model.isDirty()) return;
    if (!m_model.filePath().isEmpty()) {
        QString err;
        if (writeBackupNow(&err))
            Q_EMIT statusMessage(
                QString("Autosaved backup (%1 kept)").arg(std::min((int)backups().size(), autosaveSettings().maxCopies)), 2500);
        else Q_EMIT statusMessage("Autosave failed: " + err, 4000);
    }
    m_engine->syncStateToProject(project());
    Project copy = project();
    copy.settings.projectDir = m_model.filePath().isEmpty() ? QString() : QFileInfo(m_model.filePath()).absolutePath();
    QJsonObject root = ProjectSerializer::toJson(copy);
    root["autosaveOf"] = m_model.filePath();
    QFile f(autosavePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        if (m_model.filePath().isEmpty()) Q_EMIT statusMessage("Autosaved recovery copy", 1500);
    }
}
bool App::hasRecoveryFile() const { return QFileInfo::exists(autosavePath()); }
bool App::recoverAutosave() {
    QFile f(autosavePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    auto root = QJsonDocument::fromJson(f.readAll()).object();
    QString orig = root["autosaveOf"].toString();
    Project p;
    p.settings.projectDir = orig.isEmpty() ? QString() : QFileInfo(orig).absolutePath();
    QString err;
    if (!ProjectSerializer::fromJson(root, p, &err)) return false;
    m_engine->stop();
    m_engine->unloadProject();
    m_model.setFilePath(orig);
    m_model.replaceProject(std::move(p));
    m_model.setDirty(true);
    m_engine->rebuild(project());
    syncTransportToEngine();
    Q_EMIT projectLoaded();
    return true;
}
std::vector<AudioSource*> App::missingSamples() {
    std::vector<AudioSource*> v;
    for (auto& a : project().audioSources) {
        a.missing = !a.filePath.isEmpty() && !QFileInfo::exists(a.filePath);
        if (a.missing) v.push_back(&a);
    }
    return v;
}
void App::relinkSample(Id id, const QString& newPath) {
    edit<AudioSource>(id, "Relink sample", [newPath](AudioSource& a) {
        a.filePath = newPath;
        a.missing = false;
    });
}
// Repoints a sample to an edited copy of its file: the audio source (every clip of it) and/or a sampler channel. The sampler
// plays its own asset, not the channel's path, so it is swapped too, both ways.
void App::replaceSampleFile(Id sourceId, Id channelId, const QString& path) {
    std::vector<std::unique_ptr<Command>> cmds;
    if (project().audioSource(sourceId)) {
        double sr = 0;
        int chans = 0;
        int64_t frames = 0;
        AudioFile::probe(path, &sr, &chans, &frames);
        cmds.push_back(std::make_unique<EditObjectCommand<AudioSource>>(
            m_model, sourceId, "",
            [=](AudioSource& a) {
                if (frames != a.frames) a.trimStart = a.trimEnd = 0, a.slices.clear();   // they counted frames of the old file
                a.filePath = path;
                a.sampleRate = sr;
                a.channels = chans;
                a.frames = frames;
                a.missing = false;
            },
            0));
    }
    if (const Channel* ch = project().channel(channelId)) {
        auto load = [this, channelId](const QString& p) {
            if (auto* s = dynamic_cast<Sampler*>(processor(channelId))) {
                auto a = m_cache.getSync(p);
                if (a) s->setSample(a, p);
                s->pendingPath = a ? QString() : p;   // a load still pending for the other file must not land after this
            }
        };
        cmds.push_back(std::make_unique<LambdaCommand>("", [load, path] { load(path); }, [load, old = ch->samplePath] { load(old); }));
        cmds.push_back(
            std::make_unique<EditObjectCommand<Channel>>(m_model, channelId, "", [path](Channel& c) { c.samplePath = path; }, 0));
    }
    m_cache.getSync(path);
    if (!cmds.empty()) m_model.macro("Edit audio", std::move(cmds));
}
int App::searchMissingIn(const QString& dir) {
    int n = 0;
    for (auto* a : missingSamples()) {
        QString name = QFileInfo(a->filePath).fileName();
        QDirIterator it(dir, {name}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            relinkSample(a->id, it.next());
            ++n;
        }
    }
    return n;
}
void App::collectSamples() {
    if (m_model.filePath().isEmpty()) {
        Q_EMIT statusMessage("Save the project first", 3000);
        return;
    }
    QString dir = QFileInfo(m_model.filePath()).absolutePath() + "/Samples";
    QDir().mkpath(dir);
    int n = 0;
    std::vector<std::unique_ptr<Command>> cmds;
    auto copyTo = [&](const QString& src) {
        if (src.isEmpty() || src.startsWith("builtin:") || src.startsWith(dir)) return src;
        QString dst = dir + "/" + QFileInfo(src).fileName();
        if (!QFileInfo::exists(dst)) QFile::copy(src, dst);
        ++n;
        return dst;
    };
    for (auto& a : project().audioSources) {
        QString d = copyTo(a.filePath);
        if (d != a.filePath)
            cmds.push_back(
                std::make_unique<EditObjectCommand<AudioSource>>(m_model, a.id, "Collect", [d](AudioSource& s) { s.filePath = d; }, 0));
    }
    for (auto& c : project().channels) {
        QString d = copyTo(c.samplePath);
        if (d != c.samplePath) {
            Id cid = c.id;
            cmds.push_back(
                std::make_unique<EditObjectCommand<Channel>>(m_model, cid, "Collect", [d](Channel& ch) { ch.samplePath = d; }, 0));
            if (auto* s = dynamic_cast<Sampler*>(processor(cid))) { s->setSample(m_cache.getSync(d), d); }
        }
    }
    if (!cmds.empty()) m_model.macro("Collect project samples", std::move(cmds));
    Q_EMIT statusMessage(QString("Collected %1 sample(s) into %2").arg(n).arg(dir), 4000);
}

// ---------------------------------------------------------------- transport
void App::play() {
    if (!m_previewPath.isEmpty()) stopPreview();
    if (m_engine->isRecording() && !m_engine->isPlaying()) beginRecordPass();
    m_engine->play();
    if (m_jack) m_jack->play();
}
void App::stop() {
    bool playing = m_engine->isPlaying();
    if (m_engine->isRecording()) record(false);
    endRecordPass();
    if (m_jack) m_jack->stop();
    if (!playing) {
        seek(0);
        return;
    }   // FL: Stop while stopped jumps to the song start
    m_engine->stop();
    if (m_jack) m_jack->locate(project().transport.songMode && project().transport.loopEnabled ? project().transport.loopStart : 0);
}
void App::pause() {
    m_engine->pause();
    endRecordPass();
    if (m_jack) m_jack->stop();
}
void App::togglePlay() {
    if (m_engine->isPlaying()) stop();
    else play();
}
// FL: Record only arms - Play starts the pass. Arming while playing punches in on the fly, disarming punches out.
void App::record(bool on) {
    if (on && !m_previewPath.isEmpty()) stopPreview();
    if (on && m_engine->isPlaying()) beginRecordPass();   // before the engine records: the recorder empties its rings
    m_engine->setRecording(on);
    if (!on) endRecordPass();
}
void App::seek(Tick t) {
    m_engine->seek(t);
    if (m_jack) m_jack->locate(t);
}
bool App::setJackSync(bool on, QString* error) {
    QSettings().setValue("sync/jack", on);
    m_jack.reset();
    if (!on) return true;
    JackTransport::Host h;
    h.playing = [this] { return m_engine->isPlaying(); };
    h.positionTicks = [this] { return m_engine->positionTicks(); };
    // without tempo points the engine runs at its live tempo, which JACK itself may have set
    auto map = [this] {
        return project().transport.tempoPoints.empty() ? TempoMap(m_engine->transport().bpm) : project().transport.tempoMap();
    };
    h.secondsAt = [map](double t) { return map().secondsAt(t); };
    h.tickAt = [map](double s) { return map().tickAt(s); };
    h.looping = [this] { return !project().transport.songMode || project().transport.loopEnabled; };
    h.setPlaying = [this](bool b) { b ? play() : pause(); };
    h.seek = [this](Tick t) { seek(t); };
    h.setBpm = [this](double b) { m_engine->setBpm(b); };
    m_jack = std::make_unique<JackTransport>(std::move(h));
    if (m_jack->open(error)) return true;
    m_jack.reset();
    return false;
}
void App::seekToStart() { seek(project().transport.songMode && project().transport.loopEnabled ? project().transport.loopStart : 0); }
void App::setBpm(double bpm, bool commit) {
    bpm = clamp(bpm, 20.0, 999.0);
    auto& tr = project().transport;
    if (!tr.tempoPoints.empty()) {   // a tempo map: edit the point in effect at the playhead, or the initial tempo before the first
        auto set = [bpm, pos = (Tick)m_engine->positionTicks()](TransportSettings& t) {
            auto it = std::upper_bound(
                t.tempoPoints.begin(), t.tempoPoints.end(), pos, [](Tick v, const TempoPoint& p) { return v < p.tick; });
            (it == t.tempoPoints.begin() ? t.bpm : std::prev(it)->bpm) = bpm;
        };
        if (!commit) {   // live drag: edit in place, one undoable command when it ends
            if (!m_tempoDragBefore) m_tempoDragBefore = tr;
            set(tr);
            m_engine->requestRebuild();
            return;
        }
        if (m_tempoDragBefore) tr = *std::exchange(m_tempoDragBefore, std::nullopt);
        editTempo("Change tempo", set, 1001);
        return;
    }
    m_engine->setBpm(bpm);
    if (commit) editTempo("Change tempo", [bpm](TransportSettings& t) { t.bpm = bpm; }, 1001);
}
double App::bpmAtPlayhead() const { return project().transport.tempoMap().bpmAt(m_engine->positionTicks()); }
void App::editTempo(const QString& desc, std::function<void(TransportSettings&)> fn, int mergeId) {
    // Audio clips and tempo edits (Fit tempo clips are locked to beats and need nothing):
    //  - Resample: the audio keeps its speed, so the clip keeps its start and its length in seconds - its length, start offset and
    //    fades are re-measured through the new map (otherwise a slower tempo leaves an empty tail, a faster one cuts the audio)
    //  - Stretch modes: the clip keeps its size in bars and the audio is re-stretched to fill it, at the same pitch
    TransportSettings after = project().transport;
    fn(after);
    TempoMap m0 = project().transport.tempoMap(), m1 = after.tempoMap();
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().clips) {
        if (c.type != ClipType::Audio || c.fitTempo) continue;
        double s0 = m0.secondsAt(c.start), s1 = m1.secondsAt(c.start);
        auto remap = [&](Tick t) { return m1.ticksFor(c.start, m0.secondsAt(t) - s0); };   // ticks from start to t's moment, new map
        Clip x = c;
        Tick end = c.start + c.length;
        if (c.stretchMode != StretchResample) x.timeStretch *= (m1.secondsAt(end) - s1) / (m0.secondsAt(end) - s0);
        else {
            x.length = std::max<Tick>(1, remap(end));
            x.sourceOffset = -remap(c.start - c.sourceOffset);
            x.fadeIn = remap(c.start + c.fadeIn);
            x.fadeOut = x.length - remap(end - c.fadeOut);
        }
        if (x == c) continue;
        cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_model, c.id, "", [x](Clip& y) { y = x; }, 0));
    }
    if (cmds.empty()) {
        m_model.editTransport(desc, [after](TransportSettings& t) { t = after; }, mergeId);
        return;
    }
    cmds.push_back(m_model.transportCommand("", [after](TransportSettings& t) { t = after; }));
    m_model.macro(desc, std::move(cmds));
}
void App::setTimeSig(int n, int d) {
    m_model.editTransport("Time signature", [n, d](TransportSettings& t) {
        t.timeSig.numerator = n;
        t.timeSig.denominator = d;
    });
}
void App::setSongMode(bool s) {
    m_model.editTransport(s ? "Song mode" : "Pattern mode", [s](TransportSettings& t) { t.songMode = s; });
}
void App::setLoop(bool on) {
    m_model.editTransport("Loop", [on](TransportSettings& t) { t.loopEnabled = on; });
}
void App::setLoopRange(Tick s, Tick e) {
    if (e <= s) return;
    m_model.editTransport(
        "Loop range",
        [s, e](TransportSettings& t) {
            t.loopStart = s;
            t.loopEnd = e;
        },
        1002);
}
void App::setMetronome(bool m) {
    m_model.editTransport("Metronome", [m](TransportSettings& t) { t.metronome = m; });
}
void App::setCountIn(bool c) {
    m_model.editTransport("Count-in", [c](TransportSettings& t) { t.countIn = c; });
}
void App::setPunch(bool on, Tick in, Tick out) {
    m_model.editTransport("Punch", [=](TransportSettings& t) {
        t.punchEnabled = on;
        t.punchIn = in;
        t.punchOut = out;
    });
}
void App::setLoopRecord(bool on) {
    m_model.editTransport("Loop record", [on](TransportSettings& t) { t.loopRecord = on; });
}
Tick App::snapTicks() const {
    const auto& t = project().transport;
    if (!t.snap || t.snapDivision == 0) return 1;
    if (t.snapDivision < 0) return t.timeSig.ticksPerBar() * -t.snapDivision;
    return PPQ / t.snapDivision;
}
Tick App::snapTick(Tick t, bool force) const {
    Tick s = snapTicks();
    if (s <= 1 && !force) return t;
    if (s <= 1) s = PPQ / 4;
    return (t + s / 2) / s * s;
}
void App::selectPattern(Id id) {
    if (project().selectedPatternId != id) {
        project().selectedPatternId = id;
        m_engine->setSelectedPattern(id);
        m_model.notify(ChangePatterns | ChangeMeta);
    }
    if (id) Q_EMIT revealInProject("pattern|" + QString::number(id));   // also when it already was: the strip may show another tab
}
void App::selectChannel(Id id) {
    if (project().selectedChannelId == id) return;
    project().selectedChannelId = id;
    m_model.notify(ChangeChannels | ChangeMeta);
}

// ---------------------------------------------------------------- channels
static const char* const kChannelPalette[] = {"#e2665c", "#e0a040", "#6dc26d", "#5aa9e6", "#c07be0", "#e6c35a", "#5ad1c9", "#e07ba0"};
Id App::addChannel(ChannelType type, const QString& key, const QString& name, const QString& samplePath, Id pluginId) {
    Channel c;
    c.id = project().newId();
    c.type = type;
    c.instrumentKey = key;
    c.pluginInstanceId = pluginId;
    c.samplePath = samplePath;
    c.name = !name.isEmpty()
        ? name
        : (!samplePath.isEmpty() ? QFileInfo(samplePath).completeBaseName()
                                 : (NativeRegistry::instance().info(key) ? NativeRegistry::instance().info(key)->name : "Channel"));
    c.color = QColor(kChannelPalette[project().channels.size() % 8]);
    // route to the first free insert (or master)
    std::set<Id> used;
    for (auto& ch : project().channels) used.insert(ch.mixerTrackId);
    c.mixerTrackId = project().masterId();
    for (auto& t : project().mixerTracks)
        if (!t.isMaster && !used.count(t.id)) {
            c.mixerTrackId = t.id;
            break;
        }
    if (type == ChannelType::DrumSampler && samplePath.isEmpty()) c.instrumentState = QJsonObject{{"builtin", "Kick"}};
    m_model.insert<Channel>(c, "Add channel");
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& p : project().patterns) {
        Id pid = p.id, cid = c.id;
        cmds.push_back(std::make_unique<EditObjectCommand<Pattern>>(m_model, pid, "", [cid](Pattern& pat) { pat.ensureChannel(cid); }, 0));
    }
    if (!cmds.empty()) m_model.macro("Add channel to patterns", std::move(cmds));
    selectChannel(c.id);
    return c.id;
}
// Adding a plugin opens its editor straight away - the window fills itself in once the engine has built the processor.
Id App::addPluginChannel(const PluginInfo& info) {
    PluginInstanceData d = PluginHost::makeInstanceData(info, project().newId());
    m_model.insert<PluginInstanceData>(d, "Add plugin");
    Id ch = addChannel(ChannelType::Plugin, "plugin", info.name, QString(), d.id);
    Q_EMIT requestOpenEditor(ch, info.name);
    return ch;
}
void App::deleteChannel(Id id) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& p : project().patterns) {
        Id pid = p.id;
        cmds.push_back(std::make_unique<EditObjectCommand<Pattern>>(
            m_model, pid, "", [id](Pattern& pat) { std::erase_if(pat.channels, [id](const PatternChannelData& c) { return c.id == id; }); },
            0));
    }
    for (auto& l : project().automationLanes)
        if ((l.target.kind == ParamTarget::ChannelVolume || l.target.kind == ParamTarget::ChannelPan ||
                l.target.kind == ParamTarget::InstrumentParam) &&
            l.target.ownerId == id)
            cmds.push_back(std::make_unique<RemoveObjectCommand<AutomationLane>>(m_model, l.id, ""));
    cmds.push_back(std::make_unique<RemoveObjectCommand<Channel>>(m_model, id, ""));
    m_model.macro("Delete channel", std::move(cmds));
    if (project().selectedChannelId == id) selectChannel(project().channels.empty() ? 0 : project().channels.front().id);
}
Id App::duplicateChannel(Id id) {
    const Channel* src = project().channel(id);
    if (!src) return 0;
    m_engine->syncStateToProject(project());
    src = project().channel(id);
    Channel c = *src;
    c.id = project().newId();
    c.name = src->name + " copy";
    int idx = 0;
    for (size_t i = 0; i < project().channels.size(); ++i)
        if (project().channels[i].id == id) idx = (int)i + 1;
    if (c.pluginInstanceId) {
        const auto* pd = project().plugin(c.pluginInstanceId);
        if (pd) {
            PluginInstanceData d = *pd;
            d.id = project().newId();
            c.pluginInstanceId = d.id;
            m_model.insert<PluginInstanceData>(d, "Duplicate plugin");
        }
    }
    m_model.insert<Channel>(c, "Duplicate channel", idx);
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& p : project().patterns) {
        Id pid = p.id, cid = c.id;
        cmds.push_back(std::make_unique<EditObjectCommand<Pattern>>(
            m_model, pid, "",
            [cid, id](Pattern& pat) {
                PatternChannelData copy;
                if (auto* s = pat.channelData(id)) copy = *s; /* copy first: ensureChannel may reallocate */
                auto& d = pat.ensureChannel(cid);
                d.notes = copy.notes;
                for (auto& n : d.notes) n.id = n.id * 31 + 7;
                d.steps = copy.steps;
                d.controls = copy.controls;
            },
            0));
    }
    if (!cmds.empty()) m_model.macro("Duplicate channel data", std::move(cmds));
    return c.id;
}
void App::renameChannel(Id id, const QString& name) {
    m_model.edit<Channel>(id, "Rename channel", [name](Channel& c) { c.name = name; });
}
void App::setChannelColor(Id id, const QColor& col) {
    m_model.edit<Channel>(id, "Channel color", [col](Channel& c) { c.color = col; });
}
void App::moveChannel(Id id, int newIndex) { m_model.reorder<Channel>(id, newIndex, "Move channel"); }
template <class T> void App::liveControl(Id id, const char* desc, int mergeId, bool commit, std::function<void(T&)> set, const ParamTarget& t) {
    if (commit) m_model.edit<T>(id, desc, set, mergeId);
    else {
        auto& v = m_model.vec<T>();
        auto it = std::find_if(v.begin(), v.end(), [id](const T& x) { return x.id == id; });
        if (it != v.end()) set(*it);
    }
    if (m_engine->isRecording() && project().transport.recordMode == 3) recordAutomationPoint(t, targetValue(t));
}
void App::setChannelVolume(Id id, float v, bool commit) {
    if (auto* c = m_engine->registry().channelIfExists(id)) c->volume.store(v);
    liveControl<Channel>(id, "Channel volume", 2001, commit, [v](Channel& c) { c.volume = v; }, {ParamTarget::ChannelVolume, id, 0, 0});
}
void App::setChannelPan(Id id, float v, bool commit) {
    if (auto* c = m_engine->registry().channelIfExists(id)) c->pan.store(v);
    liveControl<Channel>(id, "Channel pan", 2002, commit, [v](Channel& c) { c.pan = v; }, {ParamTarget::ChannelPan, id, 0, 0});
}
void App::setChannelMute(Id id, bool m) {
    m_model.edit<Channel>(id, m ? "Mute channel" : "Unmute channel", [m](Channel& c) { c.mute = m; });
}
void App::setChannelSolo(Id id, bool s) {
    m_model.edit<Channel>(id, "Solo channel", [s](Channel& c) { c.solo = s; });
}
void App::setChannelEnabled(Id id, bool e) {
    m_model.edit<Channel>(id, "Enable channel", [e](Channel& c) { c.enabled = e; });
}
void App::setChannelCutItself(Id id, bool on) {
    m_model.edit<Channel>(id, on ? "Cut itself on" : "Cut itself off", [on](Channel& c) { c.cutItself = on; });
}
void App::setChannelMixerTrack(Id id, Id trackId) {
    const Channel* ch = project().channel(id);
    const MixerTrack* t = project().mixerTrack(trackId);
    if (!ch || !t) return;
    std::vector<std::unique_ptr<Command>> cmds;
    cmds.push_back(std::make_unique<EditObjectCommand<Channel>>(m_model, id, "", [trackId](Channel& c) { c.mixerTrackId = trackId; }, 0));
    m_model.macro("Route channel", std::move(cmds));
}
void App::setChannelAuxOutput(Id id, int index, Id trackId) {
    if (!project().channel(id) || index < 0) return;
    m_model.edit<Channel>(id, "Route plugin output", [index, trackId](Channel& c) {
        if ((int)c.auxOutputTracks.size() <= index) c.auxOutputTracks.resize(index + 1, 0);
        c.auxOutputTracks[index] = trackId;
    });
}
void App::routeAuxOutputsToFreeTracks(Id id) {
    IProcessor* proc = processor(id);
    const Channel* ch = project().channel(id);
    if (!proc || !ch || proc->auxOutputCount() == 0) return;
    std::set<Id> used;
    for (auto& c : project().channels) {
        used.insert(c.mixerTrackId);
        used.insert(c.auxOutputTracks.begin(), c.auxOutputTracks.end());
    }
    std::vector<Id> aux(proc->auxOutputCount(), 0);
    std::vector<std::unique_ptr<Command>> cmds;
    auto t = project().mixerTracks.begin();
    for (int k = 0; k < (int)aux.size(); ++k) {
        while (t != project().mixerTracks.end() && (t->isMaster || used.count(t->id))) ++t;
        if (t == project().mixerTracks.end()) break;
        aux[k] = t->id;
        QString name = ch->name + " " + proc->auxOutputName(k);
        cmds.push_back(std::make_unique<EditObjectCommand<MixerTrack>>(
            m_model, t->id, "",
            [name](MixerTrack& x) {
                x.name = name;
                x.customName = true;
            },
            0));
        ++t;
    }
    cmds.push_back(std::make_unique<EditObjectCommand<Channel>>(m_model, id, "", [aux](Channel& c) { c.auxOutputTracks = aux; }, 0));
    m_model.macro("Route plugin outputs", std::move(cmds));
}
void App::setChannelSample(Id id, const QString& path) {
    auto* ch = project().channel(id);
    if (!ch) return;
    if (ch->type == ChannelType::Sampler || ch->type == ChannelType::DrumSampler) {
        auto asset = m_cache.get(path);
        if (auto* s = dynamic_cast<Sampler*>(processor(id))) {
            if (asset) s->setSample(asset, path);
            else s->pendingPath = path;
            if (double b = AudioFile::bpmFromName(path); b > 0) s->params().set(Sampler::pSourceBpm, (float)b);   // "loop 83 bpm.wav"
        }
        if (auto* d = dynamic_cast<DrumSampler*>(processor(id))) {
            if (asset) d->setPadSample(0, asset, path);
        }
        QString name = QFileInfo(path).completeBaseName();
        m_model.edit<Channel>(id, "Set sample", [path, name](Channel& c) {
            c.samplePath = path;
            c.name = name;
            if (c.type == ChannelType::DrumSampler) c.instrumentState.remove("builtin");
        });
        if (auto* d = dynamic_cast<DrumSampler*>(processor(id))) {
            QJsonObject st = d->saveState();
            QJsonArray pads = st["pads"].toArray();
            if (pads.isEmpty()) pads.append(path);
            else pads[0] = path;
            st["pads"] = pads;
            st["builtin"] = "";
            d->restoreState(st);
        }
    } else changeChannelInstrument(id, "native.sampler"), setChannelSample(id, path);
}
void App::changeChannelInstrument(Id id, const QString& key, Id pluginId) {
    ChannelType type = pluginId ? ChannelType::Plugin : channelTypeForKey(key);
    m_model.edit<Channel>(id, "Change instrument", [key, pluginId, type](Channel& c) {
        c.instrumentKey = pluginId ? "plugin" : key;
        c.pluginInstanceId = pluginId;
        c.type = type;
        c.instrumentState = QJsonObject();
    });
}

// ---------------------------------------------------------------- patterns
Id App::addPattern(const QString& name) {
    Pattern p;
    p.id = project().newId();
    p.name = name.isEmpty() ? QString("Pattern %1").arg(project().patterns.size() + 1) : name;
    static const char* pal[] = {"#8f97a1", "#f2a93b", "#6dc26d", "#c07be0", "#e2665c", "#5ad1c9"}; /* first (and default) pattern is grey */
    p.color = QColor(pal[project().patterns.size() % 6]);
    for (auto& c : project().channels) p.ensureChannel(c.id);
    m_model.insert<Pattern>(p, "Add pattern");
    selectPattern(p.id);
    return p.id;
}
void App::deletePattern(Id id) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().clips)
        if (c.type == ClipType::Pattern && c.sourceId == id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, c.id, ""));
    cmds.push_back(std::make_unique<RemoveObjectCommand<Pattern>>(m_model, id, ""));
    m_model.macro("Delete pattern", std::move(cmds));
    if (project().selectedPatternId == id) selectPattern(project().patterns.empty() ? 0 : project().patterns.front().id);
}
Id App::clonePattern(Id id) {
    const Pattern* src = project().pattern(id);
    if (!src) return 0;
    Pattern p = *src;
    p.id = project().newId();
    p.name = src->name + " (copy)";
    for (auto& c : p.channels)
        for (auto& n : c.notes) n.id = project().newId();
    m_model.insert<Pattern>(p, "Clone pattern");
    return p.id;
}
// Split by channel: every channel that has steps or notes in the pattern becomes its own pattern, named after the channel
// (a sampler channel carries its sample's name). Playlist clips of the source are replaced by one clip per new pattern, stacked
// on the following tracks, so the song still plays the same; the source pattern is then removed. One undo step.
std::vector<Id> App::splitPatternByChannel(Id id) {
    const Pattern* src = project().pattern(id);
    std::vector<Id> out;
    if (!src) return out;
    std::vector<std::unique_ptr<Command>> cmds;
    std::vector<Pattern> parts;
    for (auto& cd : src->channels) {
        bool has = !cd.notes.empty() || !cd.controls.empty();
        for (auto& st : cd.steps)
            if (st.enabled) has = true;
        if (!has) continue;
        const Channel* ch = project().channel(cd.id);
        if (!ch) continue;
        Pattern p = *src;
        p.id = project().newId();
        p.name = ch->name;
        p.color = ch->color;
        p.channels.clear();
        p.channels.push_back(cd);
        for (auto& n : p.channels[0].notes) n.id = project().newId();
        parts.push_back(p);
    }
    if (parts.size() < 2) {
        Q_EMIT statusMessage("Split by channel: the pattern has content on only one channel", 3000);
        return out;
    }
    int at = -1;
    for (size_t i = 0; i < project().patterns.size(); ++i)
        if (project().patterns[i].id == id) at = (int)i + 1;
    for (size_t i = 0; i < parts.size(); ++i) {
        cmds.push_back(std::make_unique<InsertObjectCommand<Pattern>>(m_model, parts[i], "", at < 0 ? -1 : at + (int)i));
        out.push_back(parts[i].id);
    }
    // playlist: each clip of the source becomes a stack of clips, one per part, starting on the clip's track and going down
    auto& tracks = project().playlistTracks;
    std::vector<Clip> srcClips;
    for (auto& c : project().clips)
        if (c.type == ClipType::Pattern && c.sourceId == id) srcClips.push_back(c);
    int neededTracks = 0;
    for (auto& c : srcClips) {
        int ti = 0;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == c.trackId) ti = (int)i;
        neededTracks = std::max(neededTracks, ti + (int)parts.size());
    }
    std::vector<Id> trackIds;
    for (auto& t : tracks) trackIds.push_back(t.id);
    for (int i = (int)tracks.size(); i < neededTracks; ++i) {
        PlaylistTrack t;
        t.id = project().newId();
        t.name = QString("Track %1").arg(i + 1);
        cmds.push_back(std::make_unique<InsertObjectCommand<PlaylistTrack>>(m_model, t, "", -1));
        trackIds.push_back(t.id);
    }
    for (auto& c : srcClips) {
        int ti = 0;
        for (size_t i = 0; i < trackIds.size(); ++i)
            if (trackIds[i] == c.trackId) ti = (int)i;
        for (size_t i = 0; i < parts.size(); ++i) {
            Clip nc = c;
            nc.id = project().newId();
            nc.sourceId = parts[i].id;
            nc.name = parts[i].name;
            nc.trackId = trackIds[std::min<size_t>(ti + i, trackIds.size() - 1)];
            cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, nc, "", -1));
        }
        cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, c.id, ""));
    }
    cmds.push_back(std::make_unique<RemoveObjectCommand<Pattern>>(m_model, id, ""));
    m_model.macro("Split pattern by channel", std::move(cmds));
    if (!out.empty()) selectPattern(out.front());
    return out;
}
void App::renamePattern(Id id, const QString& name) {
    m_model.edit<Pattern>(id, "Rename pattern", [name](Pattern& p) { p.name = name; });
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().clips)
        if (c.type == ClipType::Pattern && c.sourceId == id)
            cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_model, c.id, "", [name](Clip& cl) { cl.name = name; }, 0));
    if (!cmds.empty()) m_model.macro("Rename clips", std::move(cmds));
}
void App::setPatternColor(Id id, const QColor& c) {
    m_model.edit<Pattern>(id, "Pattern color", [c](Pattern& p) { p.color = c; });
}
void App::setStep(Id patternId, Id channelId, int step, bool enabled) {
    TimeSignature ts = project().transport.timeSig;
    m_model.edit<Pattern>(patternId, enabled ? "Set step" : "Clear step", [=](Pattern& p) {
        auto& d = p.ensureChannel(channelId);
        if ((int)d.steps.size() <= step) d.steps.resize(step + 1);
        d.steps[step].enabled = enabled;
        int spb = std::max(1, (int)(ts.ticksPerBar() / p.stepTicks()));
        // the pattern length follows the steps in whole bars: grows when a step is set beyond it, shrinks back when steps are cleared
        int last = -1;
        for (auto& c : p.channels)
            for (size_t i = 0; i < c.steps.size(); ++i)
                if (c.steps[i].enabled) last = std::max(last, (int)i);
        Tick notesEnd = 0;
        for (auto& c : p.channels)
            for (auto& n : c.notes) notesEnd = std::max(notesEnd, n.start + n.duration);
        int need = std::max((last / spb + 1) * spb, (int)((notesEnd + p.stepTicks() - 1) / p.stepTicks() + spb - 1) / spb * spb);
        need = std::max(need, spb);
        if (need != p.stepCount) {
            p.stepCount = need;
            for (auto& c : p.channels)
                if ((int)c.steps.size() < need) c.steps.resize(need);
        }
    });
}
void App::setStepProperty(Id patternId, Id channelId, int step, int which, float value, bool commit) {
    auto fn = [=](Pattern& p) {
        auto& d = p.ensureChannel(channelId);
        if ((int)d.steps.size() <= step) d.steps.resize(step + 1);
        Step& s = d.steps[step];
        switch (which) {
        case 0: s.velocity = clamp(value, 0.f, 1.f); break;
        case 1: s.pan = clamp(value, -1.f, 1.f); break;
        case 2: s.pitch = clamp(value, -24.f, 24.f); break;
        case 3: s.probability = clamp(value, 0.f, 1.f); break;
        case 4: s.offset = clamp(value, -0.5f, 0.5f); break;
        }
    };
    if (commit) m_model.edit<Pattern>(patternId, "Edit step", fn, 3000 + which);
    else {
        if (auto* p = project().pattern(patternId)) {
            fn(*p);
            m_model.notify(ChangePatterns);
        }
    }
}

// ---------------------------------------------------------------- playlist
Id App::addPlaylistTrack(const QString& name) {
    PlaylistTrack t;
    t.id = project().newId();
    t.name = name.isEmpty() ? QString("Track %1").arg(project().playlistTracks.size() + 1) : name;
    m_model.insert<PlaylistTrack>(t, "Add track");
    return t.id;
}
Id App::firstPlaylistTrack() { return project().playlistTracks.empty() ? addPlaylistTrack() : project().playlistTracks.front().id; }
void App::deletePlaylistTrack(Id id) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().clips)
        if (c.trackId == id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, c.id, ""));
    cmds.push_back(std::make_unique<RemoveObjectCommand<PlaylistTrack>>(m_model, id, ""));
    m_model.macro("Delete track", std::move(cmds));
}
Id App::addPatternClip(Id trackId, Id patternId, Tick start, Tick length, bool loopSource) {
    const Pattern* p = project().pattern(patternId);
    if (!p) return 0;
    Clip c;
    c.id = project().newId();
    c.type = ClipType::Pattern;
    c.trackId = trackId;
    c.sourceId = patternId;
    c.start = std::max<Tick>(0, start);
    c.length = length > 0 ? length : 0; /* 0 = follows the pattern length */
    c.loopSource = loopSource;
    c.name = p->name;
    m_model.insert<Clip>(c, "Add pattern clip");
    return c.id;
}
Id App::ensureAudioSource(const QString& path) {
    for (auto& a : project().audioSources)
        if (a.filePath == path) return a.id;
    AudioSource a;
    a.id = project().newId();
    a.filePath = path;
    a.name = QFileInfo(path).completeBaseName();
    AudioFile::probe(path, &a.sampleRate, &a.channels, &a.frames);
    m_model.insert<AudioSource>(a, "Add audio reference");
    m_cache.get(path);
    return a.id;
}
Id App::addAudioClip(Id trackId, const QString& file, Tick start) {
    Id src = ensureAudioSource(file);
    const AudioSource* a = project().audioSource(src);
    if (!a) return 0;
    double secs = a->sampleRate > 0 ? a->frames / a->sampleRate : 1.0;
    TempoMap tempo = project().transport.tempoMap();
    Clip c;
    c.id = project().newId();
    c.type = ClipType::Audio;
    c.trackId = trackId;
    c.sourceId = src;
    c.start = std::max<Tick>(0, start);
    c.length = std::max<Tick>(tempo.ticksFor(c.start, secs), PPQ / 4);
    c.name = a->name;
    c.loopSource = false;
    if (a->hasPlaceDefaults) {   // settings made in the project panel before the sample was placed
        copyClipSettings(a->placeDefaults, c);
        double trimmed = (a->trimEnd > 0 ? a->trimEnd : a->frames) - a->trimStart;
        if (a->sampleRate > 0) {   // fit-tempo clips are locked to beats, the rest last 'sec' through the tempo map
            double bpm = project().transport.bpm, sec = trimmed / a->sampleRate / clipSourceSpeed(c, bpm);
            c.length = std::max<Tick>(PPQ / 4, c.fitTempo && c.sourceBpm > 0 ? secondsToTicks(sec, bpm) : tempo.ticksFor(c.start, sec));
        }
    }
    m_model.insert<Clip>(c, "Add audio clip");
    return c.id;
}
Id App::addAutomationClip(Id trackId, Id laneId, Tick start, Tick length) {
    const AutomationLane* l = project().lane(laneId);
    if (!l) return 0;
    Clip c;
    c.id = project().newId();
    c.type = ClipType::Automation;
    c.trackId = trackId;
    c.sourceId = laneId;
    c.start = std::max<Tick>(0, start);
    c.length = length > 0 ? length : l->length;
    c.name = l->name;
    m_model.insert<Clip>(c, "Add automation clip");
    return c.id;
}
void App::deleteClips(const std::vector<Id>& ids) {
    if (ids.empty()) return;
    std::vector<std::unique_ptr<Command>> cmds;
    for (Id id : ids) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, id, ""));
    m_model.macro(ids.size() == 1 ? "Delete clip" : QString("Delete %1 clips").arg(ids.size()), std::move(cmds));
}
// c (clen ticks long) keeps the part before 'at'; returns the part after it (with c's id)
static Clip cutClip(Clip& c, Tick clen, Tick at) {
    Clip right = c;
    Tick leftLen = at - c.start;
    right.start = at;
    right.length = clen - leftLen;
    right.sourceOffset = c.sourceOffset + leftLen;
    right.fadeIn = 0;
    c.length = leftLen;
    c.fadeOut = 0;
    return right;
}
void App::splitClip(Id id, Tick at) {
    const Clip* c = project().clip(id);
    Tick clen = c ? project().clipLength(*c) : 0;
    if (!c || at <= c->start || at >= c->start + clen) return;
    Clip left = *c;
    Clip right = cutClip(left, clen, at);
    right.id = project().newId();
    std::vector<std::unique_ptr<Command>> cmds;
    cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_model, id, "", [left](Clip& cl) { cl = left; }, 0));
    cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, right, "", -1));
    m_model.macro("Split clip", std::move(cmds));
}
// The takes of a recording are audio clips of one source on stacked tracks. Each take overlapping [from, to) is split at the
// bounds; inside, the parts on the clicked take's track play and the rest are muted. Outside nothing changes. One undo step.
void App::useTake(Id clipId, Tick from, Tick to) {
    const Clip* k = project().clip(clipId);
    if (!k || k->type != ClipType::Audio || to <= from) return;
    const Id src = k->sourceId, lane = k->trackId;
    std::vector<std::unique_ptr<Command>> cmds;
    for (const Clip& c : project().clips) {
        Tick end = c.start + c.length;
        if (c.type != ClipType::Audio || c.sourceId != src || end <= from || c.start >= to) continue;
        Clip left = c, mid = c, right;
        bool hasLeft = from > c.start, hasRight = to < end;
        if (hasLeft) mid = cutClip(left, c.length, from);
        if (hasRight) right = cutClip(mid, mid.length, to);
        mid.muted = c.trackId != lane;
        Clip first = hasLeft ? left : mid;   // keeps the clip's id
        cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_model, c.id, "", [first](Clip& x) { x = first; }, 0));
        for (Clip* part : {hasLeft ? &mid : nullptr, hasRight ? &right : nullptr})
            if (part) {
                part->id = project().newId();
                cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, *part, "", -1));
            }
    }
    if (!cmds.empty()) m_model.macro("Use take", std::move(cmds));
}
Id App::duplicateClip(Id id, Tick newStart, Id newTrack) {
    const Clip* c = project().clip(id);
    if (!c) return 0;
    Clip d = *c;
    d.id = project().newId();
    d.start = std::max<Tick>(0, newStart);
    if (newTrack) d.trackId = newTrack;
    m_model.insert<Clip>(d, "Duplicate clip");
    return d.id;
}
void App::makeClipUnique(Id clipId) {
    const Clip* c = project().clip(clipId);
    if (!c || c->type != ClipType::Pattern) return;
    Id np = clonePattern(c->sourceId);
    if (!np) return;
    QString name = project().pattern(np)->name;
    m_model.edit<Clip>(clipId, "Make unique", [np, name](Clip& cl) {
        cl.sourceId = np;
        cl.name = name;
    });
}
Id App::addMarker(Tick pos, const QString& name) {
    Marker m;
    m.id = project().newId();
    m.position = pos;
    m.name = name.isEmpty() ? QString("Marker %1").arg(project().markers.size() + 1) : name;
    m_model.insert<Marker>(m, "Add marker");
    return m.id;
}

// ---------------------------------------------------------------- mixer
Id App::addMixerTrack(const QString& name) {
    MixerTrack t;
    t.id = project().newId();
    t.name = name.isEmpty() ? QString("Insert %1").arg(project().mixerTracks.size()) : name;
    t.outputTrackId = project().masterId();
    m_model.insert<MixerTrack>(t, "Add mixer track");
    return t.id;
}
void App::deleteMixerTrack(Id id) {
    const MixerTrack* t = project().mixerTrack(id);
    if (!t || t->isMaster) return;
    Id master = project().masterId();
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().channels)
        if (c.mixerTrackId == id)
            cmds.push_back(
                std::make_unique<EditObjectCommand<Channel>>(m_model, c.id, "", [master](Channel& ch) { ch.mixerTrackId = master; }, 0));
    for (auto& o : project().mixerTracks)
        if (o.outputTrackId == id || std::any_of(o.sends.begin(), o.sends.end(), [id](const Send& s) { return s.targetTrackId == id; }))
            cmds.push_back(std::make_unique<EditObjectCommand<MixerTrack>>(
                m_model, o.id, "",
                [id, master](MixerTrack& m) {
                    if (m.outputTrackId == id) m.outputTrackId = master;
                    std::erase_if(m.sends, [id](const Send& s) { return s.targetTrackId == id; });
                },
                0));
    cmds.push_back(std::make_unique<RemoveObjectCommand<MixerTrack>>(m_model, id, ""));
    m_model.macro("Delete mixer track", std::move(cmds));
}
void App::setMixerVolume(Id id, float v, bool commit) {
    m_engine->registry().track(id).volume.store(v);
    liveControl<MixerTrack>(id, "Mixer volume", 2003, commit, [v](MixerTrack& t) { t.volume = v; }, {ParamTarget::MixerVolume, id, 0, 0});
}
void App::setMixerPan(Id id, float v, bool commit) {
    m_engine->registry().track(id).pan.store(v);
    liveControl<MixerTrack>(id, "Mixer pan", 2004, commit, [v](MixerTrack& t) { t.pan = v; }, {ParamTarget::MixerPan, id, 0, 0});
}
void App::setSendLevel(Id from, Id to, float level, bool commit) {
    auto& lv = m_engine->registry().track(from).sendLevels[to];
    if (!lv) lv = std::make_unique<std::atomic<float>>(level);
    else lv->store(level);
    liveControl<MixerTrack>(from, "Send level", 2005, commit, [to, level](MixerTrack& t) {
        for (auto& s : t.sends)
            if (s.targetTrackId == to) s.level = level;
    }, {ParamTarget::MixerSend, from, to, 0});
}
Id App::addEffect(Id trackId, const QString& key, int slot) {
    EffectSlot e;
    e.id = project().newId();
    e.processorKey = key;
    if (auto* info = NativeRegistry::instance().info(key)) e.name = info->name;
    m_model.edit<MixerTrack>(trackId, "Add effect", [e, slot](MixerTrack& t) {
        if (slot < 0 || slot >= (int)t.effects.size()) t.effects.push_back(e);
        else t.effects.insert(t.effects.begin() + slot, e);
    });
    return e.id;
}
Id App::addPluginEffect(Id trackId, const PluginInfo& info, int slot) {
    PluginInstanceData d = PluginHost::makeInstanceData(info, project().newId());
    m_model.insert<PluginInstanceData>(d, "Add plugin");
    EffectSlot e;
    e.id = project().newId();
    e.processorKey = "plugin";
    e.pluginInstanceId = d.id;
    e.name = info.name;
    m_model.edit<MixerTrack>(trackId, "Add plugin effect", [e, slot](MixerTrack& t) {
        if (slot < 0 || slot >= (int)t.effects.size()) t.effects.push_back(e);
        else t.effects.insert(t.effects.begin() + slot, e);
    });
    Q_EMIT requestOpenEditor(e.id, info.name);
    return e.id;
}
void App::removeEffect(Id trackId, Id slotId) {
    m_engine->syncStateToProject(project());
    m_model.edit<MixerTrack>(trackId, "Remove effect",
        [slotId](MixerTrack& t) { std::erase_if(t.effects, [slotId](const EffectSlot& e) { return e.id == slotId; }); });
}
void App::moveEffect(Id trackId, Id slotId, int newIndex) {
    m_model.edit<MixerTrack>(trackId, "Move effect", [slotId, newIndex](MixerTrack& t) {
        for (size_t i = 0; i < t.effects.size(); ++i)
            if (t.effects[i].id == slotId) {
                EffectSlot e = t.effects[i];
                t.effects.erase(t.effects.begin() + i);
                int ni = clamp(newIndex, 0, (int)t.effects.size());
                t.effects.insert(t.effects.begin() + ni, e);
                break;
            }
    });
}
void App::setEffectBypass(Id trackId, Id slotId, bool b) {
    m_model.edit<MixerTrack>(trackId, b ? "Bypass effect" : "Enable effect", [slotId, b](MixerTrack& t) {
        for (auto& e : t.effects)
            if (e.id == slotId) e.bypass = b;
    });
}
void App::setEffectMix(Id trackId, Id slotId, float mix) {
    m_model.edit<MixerTrack>(trackId, "Effect mix", [slotId, mix](MixerTrack& t) {
        for (auto& e : t.effects)
            if (e.id == slotId) e.mix = mix;
    }, 2006);
}
bool App::routeTrack(Id from, Id to) {
    if (from == to || project().routingWouldCycle(from, to)) {
        Q_EMIT statusMessage("Routing would create a feedback loop", 3000);
        return false;
    }
    m_model.edit<MixerTrack>(from, "Route track", [to](MixerTrack& t) { t.outputTrackId = to; });
    return true;
}
void App::addSend(Id from, Id to, bool sidechain) {
    if (from == to || project().routingWouldCycle(from, to)) {
        Q_EMIT statusMessage("Send would create a feedback loop", 3000);
        return;
    }
    m_model.edit<MixerTrack>(from, sidechain ? "Add sidechain" : "Add send", [to, sidechain](MixerTrack& t) {
        for (auto& s : t.sends)
            if (s.targetTrackId == to) {
                s.sidechain = sidechain;
                return;
            }
        Send s;
        s.targetTrackId = to;
        s.level = sidechain ? 1.f : 0.5f;
        s.sidechain = sidechain;
        t.sends.push_back(s);
    });
}
void App::removeSend(Id from, Id to) {
    m_model.edit<MixerTrack>(
        from, "Remove send", [to](MixerTrack& t) { std::erase_if(t.sends, [to](const Send& s) { return s.targetTrackId == to; }); });
}

// ---------------------------------------------------------------- processors
void App::setProcessorParam(Id ownerId, uint32_t paramId, float normalized, bool commit) {
    IProcessor* p = processor(ownerId);
    if (!p) return;
    uint64_t key = ((uint64_t)ownerId << 32) ^ paramId;
    (void)key;
    float before = p->params().getNormalized(paramId);
    auto it = m_paramDragStart.find(paramId ^ (uint32_t)(ownerId * 2654435761u));
    if (it == m_paramDragStart.end()) m_paramDragStart[paramId ^ (uint32_t)(ownerId * 2654435761u)] = before;
    p->params().setNormalized(paramId, normalized);
    if (m_engine->isRecording() && project().transport.recordMode == 3) {
        ParamTarget t;
        bool isInstrument = project().channel(ownerId) != nullptr;
        t.kind = isInstrument ? ParamTarget::InstrumentParam : ParamTarget::EffectParam;
        t.ownerId = isInstrument ? ownerId : 0;
        t.slotId = isInstrument ? 0 : ownerId;
        if (!isInstrument) {
            for (auto& mt : project().mixerTracks)
                for (auto& e : mt.effects)
                    if (e.id == ownerId) t.ownerId = mt.id;
        }
        t.paramId = paramId;
        recordAutomationPoint(t, normalized);
    }
    if (commit) {
        float start = m_paramDragStart[paramId ^ (uint32_t)(ownerId * 2654435761u)];
        m_paramDragStart.erase(paramId ^ (uint32_t)(ownerId * 2654435761u));
        auto setter = [this, ownerId, paramId](float v) {
            if (auto* pp = processor(ownerId)) pp->params().setNormalized(paramId, v);
        };
        const ParamDesc* d = p->params().descById(paramId);
        QString name = d ? QString::fromStdString(d->name) : "Parameter";
        m_model.push(std::make_unique<ProcessorParamCommand>("Change " + name, setter, start, normalized,
            4000 + (int)(paramId % 1000) + (int)(ownerId % 97) * 1000, [this] { m_model.notify(ChangeNone); }));
    }
}
QString App::presetDir(const QString& key) const {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets/" + QString(key).replace(':', '_');
    QDir().mkpath(d);
    return d;
}
QStringList App::presetsFor(const QString& key) const {
    QStringList out;
    for (auto& f : QDir(presetDir(key)).entryList({"*.json"}, QDir::Files, QDir::Name)) out << presetDir(key) + "/" + f;
    return out;
}
void App::savePreset(Id ownerId, const QString& name) {
    IProcessor* p = processor(ownerId);
    if (!p) return;
    QFile f(presetDir(p->key()) + "/" + name + ".json");
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(p->saveState()).toJson());
}
bool App::loadPreset(Id ownerId, const QString& file) {
    IProcessor* p = processor(ownerId);
    if (!p) return false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    p->restoreState(QJsonDocument::fromJson(f.readAll()).object());
    m_model.setDirty(true);
    return true;
}

// ---------------------------------------------------------------- automation / MIDI mapping
QString App::targetName(const ParamTarget& t) const {
    auto paramName = [&](Id owner, uint32_t pid) {
        auto* p = m_engine->registry().processor(owner);
        if (!p) return QString("Param %1").arg(pid);
        auto* d = p->params().descById(pid);
        return QString("%1 - %2").arg(p->displayName(), d ? QString::fromStdString(d->name) : QString::number(pid));
    };
    switch (t.kind) {
    case ParamTarget::ChannelVolume: {
        auto* c = project().channel(t.ownerId);
        return (c ? c->name : "Channel") + " Volume";
    }
    case ParamTarget::ChannelPan: {
        auto* c = project().channel(t.ownerId);
        return (c ? c->name : "Channel") + " Pan";
    }
    case ParamTarget::MixerVolume: {
        auto* m = project().mixerTrack(t.ownerId);
        return (m ? m->name : "Track") + " Volume";
    }
    case ParamTarget::MixerPan: {
        auto* m = project().mixerTrack(t.ownerId);
        return (m ? m->name : "Track") + " Pan";
    }
    case ParamTarget::MixerSend: {
        auto* m = project().mixerTrack(t.ownerId);
        auto* d = project().mixerTrack(t.slotId);
        return (m ? m->name : "Track") + " Send > " + (d ? d->name : "?");
    }
    case ParamTarget::EffectParam: return paramName(t.slotId, t.paramId);
    case ParamTarget::InstrumentParam: {
        auto* c = project().channel(t.ownerId);
        return (c ? c->name + ": " : "") + paramName(t.ownerId, t.paramId);
    }
    default: return "None";
    }
}
float App::targetValue(const ParamTarget& t) const {
    switch (t.kind) {
    case ParamTarget::ChannelVolume: {
        auto* c = project().channel(t.ownerId);
        return c ? c->volume : 0;
    }
    case ParamTarget::ChannelPan: {
        auto* c = project().channel(t.ownerId);
        return c ? (c->pan + 1) * 0.5f : 0.5f;
    }
    case ParamTarget::MixerVolume: {
        auto* m = project().mixerTrack(t.ownerId);
        return m ? m->volume / 1.25f : 0;
    }
    case ParamTarget::MixerPan: {
        auto* m = project().mixerTrack(t.ownerId);
        return m ? (m->pan + 1) * 0.5f : 0.5f;
    }
    case ParamTarget::MixerSend: {
        auto* m = project().mixerTrack(t.ownerId);
        if (m)
            for (auto& s : m->sends)
                if (s.targetTrackId == t.slotId) return s.level;
        return 0;
    }
    case ParamTarget::EffectParam: {
        auto* p = m_engine->registry().processor(t.slotId);
        return p ? p->params().getNormalized(t.paramId) : 0;
    }
    case ParamTarget::InstrumentParam: {
        auto* p = m_engine->registry().processor(t.ownerId);
        return p ? p->params().getNormalized(t.paramId) : 0;
    }
    default: return 0;
    }
}
Id App::createAutomationLane(const ParamTarget& target, const QString& name, bool placeClip, Id patternId) {
    AutomationLane l;
    l.id = project().newId();
    l.name = name.isEmpty() ? targetName(target) : name;
    l.target = target;
    l.patternId = patternId;
    l.length = PPQ * 4 * 4;
    float v = targetValue(target);
    l.points.push_back({0, v, CurveType::Linear, 0});
    l.points.push_back({l.length, v, CurveType::Linear, 0});
    m_model.insert<AutomationLane>(l, "Create automation clip");
    if (placeClip && !patternId) {
        Id track = 0;
        for (auto& t : project().playlistTracks) {
            bool free = true;
            for (auto& c : project().clips)
                if (c.trackId == t.id) free = false;
            if (free) {
                track = t.id;
                break;
            }
        }
        if (!track) track = addPlaylistTrack(l.name);
        TempoMap tempo = project().transport.tempoMap();
        addAutomationClip(
            track, l.id, project().transport.songMode ? tempo.barStart(tempo.barAt((Tick)m_engine->positionTicks())) : 0, l.length);
    }
    Q_EMIT statusMessage("Automation clip created: " + l.name, 3000);
    return l.id;
}
void App::deleteLane(Id id) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& c : project().clips)
        if (c.type == ClipType::Automation && c.sourceId == id)
            cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, c.id, ""));
    cmds.push_back(std::make_unique<RemoveObjectCommand<AutomationLane>>(m_model, id, ""));
    m_model.macro("Delete automation", std::move(cmds));
}
void App::recordAutomationPoint(const ParamTarget& t, float normalized) {
    Tick pos = (Tick)m_engine->positionTicks();
    bool song = project().transport.songMode;
    Id selPat = project().selectedPatternId;
    AutomationLane* lane = nullptr;
    for (auto& l : project().automationLanes)
        if (l.target == t && (song ? l.patternId == 0 : l.patternId == selPat)) {
            lane = &l;
            break;
        }
    if (!lane) {
        Id id = createAutomationLane(t, QString(), song, song ? 0 : selPat);
        lane = project().lane(id);
        if (!lane) return;
    }
    Tick local = pos;
    if (song) {
        for (auto& c : project().clips)
            if (c.type == ClipType::Automation && c.sourceId == lane->id && pos >= c.start && pos < c.start + c.length) {
                local = pos - c.start + c.sourceOffset;
                break;
            }
    }
    Id lid = lane->id;
    auto* l = project().lane(lid);
    // erase points within a small window and insert the new one (direct edit, one undo entry per recording pass is impractical; mark dirty)
    std::erase_if(l->points, [local](const AutomationPoint& p) { return std::llabs(p.time - local) < PPQ / 32; });
    AutomationPoint ap;
    ap.time = local;
    ap.value = clamp(normalized, 0.f, 1.f);
    l->points.push_back(ap);
    std::sort(l->points.begin(), l->points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
    if (l->length < local + PPQ) l->length = local + PPQ;
    m_model.notify(ChangeAutomation);
}
void App::startMidiLearn(const ParamTarget& target, const QString& name) {
    m_learnActive = true;
    m_learnTarget = target;
    m_learnName = name;
    m_midi->setLearning(true);
    Q_EMIT midiLearnChanged(true, name);
    Q_EMIT statusMessage("Move a MIDI controller to link '" + name + "' (Esc cancels)", 0);
}
void App::cancelMidiLearn() {
    m_learnActive = false;
    m_midi->setLearning(false);
    Q_EMIT midiLearnChanged(false, QString());
    Q_EMIT statusMessage("", 1);
}
void App::onCcReceived(int cc, int channel, int) {
    if (!m_learnActive) return;
    MidiMapping m;
    m.cc = cc;
    m.midiChannel = -1;
    m.target = m_learnTarget;
    m.name = m_learnName;
    ParamTarget t = m_learnTarget;
    m_model.push(std::make_unique<LambdaCommand>(
        "Link MIDI controller",
        [this, m, t] {
            std::erase_if(project().midiMappings, [&](const MidiMapping& x) { return x.target == t; });
            project().midiMappings.push_back(m);
            m_model.notify(ChangeStructure);
        },
        [this, t] {
            std::erase_if(project().midiMappings, [&](const MidiMapping& x) { return x.target == t; });
            m_model.notify(ChangeStructure);
        }));
    QString name = m_learnName;
    cancelMidiLearn();
    Q_EMIT statusMessage(QString("Linked CC %1 (ch %2) to %3").arg(cc).arg(channel + 1).arg(name), 4000);
}
void App::removeMidiMapping(int index) {
    if (index < 0 || index >= (int)project().midiMappings.size()) return;
    MidiMapping m = project().midiMappings[index];
    m_model.push(std::make_unique<LambdaCommand>(
        "Remove MIDI link",
        [this, index] {
            if (index < (int)project().midiMappings.size()) project().midiMappings.erase(project().midiMappings.begin() + index);
            m_model.notify(ChangeStructure);
        },
        [this, m, index] {
            project().midiMappings.insert(project().midiMappings.begin() + std::min(index, (int)project().midiMappings.size()), m);
            m_model.notify(ChangeStructure);
        }));
}

// ---------------------------------------------------------------- live input
void App::noteOn(int pitch, float vel) {
    MidiEvent e;
    e.type = MidiEvent::NoteOn;
    e.data1 = (uint8_t)clamp(pitch, 0, 127);
    e.velocity = vel;
    e.data2 = (uint8_t)(vel * 127);
    m_engine->pushUiMidi(e);
}
void App::noteOff(int pitch) {
    MidiEvent e;
    e.type = MidiEvent::NoteOff;
    e.data1 = (uint8_t)clamp(pitch, 0, 127);
    m_engine->pushUiMidi(e);
}
void App::previewFile(const QString& path) {
    if (!AudioFile::isAudioFile(path)) return;
    m_engine->previewStop();
    QString err;
    auto b = m_previewLoader.load(path, &err);
    if (!b) {
        m_previewPath.clear();
        m_previewBuffer = nullptr;
        Q_EMIT previewChanged(QString());
        Q_EMIT statusMessage("Cannot preview " + path + ": " + err, 4000);
        return;
    }
    playPreview(b);
}
void App::playPreview(std::shared_ptr<PreviewBuffer> b) {
    m_previewPath = b->path;
    m_previewBuffer = b;
    m_engine->previewPlay(b);
    Q_EMIT previewChanged(b->path);
}
void App::stopPreview() {
    m_engine->previewStop();
    m_previewLoader.cancel();
    m_previewPath.clear();
    m_previewBuffer = nullptr;
    Q_EMIT previewChanged(QString());
}
void App::panic() { m_engine->allNotesOff(); }

// ---------------------------------------------------------------- recording
// A pass runs while the transport plays with Record armed. Like FL Studio: audio goes into the playlist (song mode) on the
// track linked to its mixer insert, every loop pass a take stacked under the last; notes go into the selected pattern, in song
// mode with a pattern clip placed where the pass started; and the whole pass is one undo step.
void App::beginRecordPass() {
    if (m_recPass) return;
    m_recPass = true;
    m_recSegments.clear();
    m_recNoteIds.clear();
    m_recControlsBefore.clear();
    m_pendingNotes.clear();
    auto& tr = project().transport;
    m_recSong = tr.songMode;
    m_recPatternId = (tr.recordMode == 0 || tr.recordMode == 1) ? project().selectedPatternId : 0;
    Tick at = (Tick)m_engine->positionTicks();
    if (tr.loopEnabled && tr.loopRecord && at >= tr.loopStart && at < tr.loopEnd) at = tr.loopStart;   // the loop wraps back before 'at'
    TempoMap tm = tr.tempoMap();
    m_recClipStart = tm.barStart(tm.barAt(at));   // on the bar line, also after a time-signature change
    if (tr.recordMode != 0 && tr.recordMode != 2) return;
    std::vector<std::pair<Id, int>> armed;
    for (auto& t : project().mixerTracks)
        if (t.armed && t.inputChannel >= 0) armed.push_back({t.id, t.inputStereo ? 2 : 1});
    if (armed.empty()) return;
    if (m_recorder->recordDirectory().isEmpty())
        m_recorder->setRecordDirectory(QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Vybrant Recordings");
    m_recorder->setSampleRate(m_engine->sampleRate());
    m_recorder->start(armed);
}
void App::onRecordSegment(double tick, int64_t frame, bool contiguous) {
    if (m_recPass) m_recSegments.push_back({tick, frame, contiguous});
}
void App::onTransportStateChanged() {
    if (!m_engine->isPlaying()) endRecordPass();
}
void App::endRecordPass() {
    if (!m_recPass) return;
    m_engine->pollMessages();   // a loop wrap just before the stop: its marker may still be queued
    m_recPass = false;
    m_pendingNotes.clear();
    std::vector<RecordedFile> files = m_recorder->stop();
    std::vector<std::unique_ptr<Command>> cmds;
    std::vector<Clip> placed;   // clips of this pass, not in the project yet
    std::vector<PlaylistTrack> newTracks;
    auto busy = [&](Id track, Tick s, Tick e) {
        auto hit = [&](const Clip& c) { return c.trackId == track && c.start < e && s < c.start + project().clipLength(c); };
        return std::any_of(project().clips.begin(), project().clips.end(), hit) || std::any_of(placed.begin(), placed.end(), hit);
    };
    // the first track with room for [s, e) - for audio one routed to the recording insert (FL's linked audio track), made if needed
    auto trackFor = [&](const MixerTrack* mt, Tick s, Tick e) -> Id {
        for (auto* v : {&project().playlistTracks, &newTracks})
            for (auto& t : *v)
                if ((!mt || t.mixerTrackId == mt->id) && !busy(t.id, s, e)) return t.id;
        PlaylistTrack t;
        t.id = project().newId();
        t.name = mt ? mt->name : QString("Track %1").arg(project().playlistTracks.size() + newTracks.size() + 1);
        if (mt) {
            t.mixerTrackId = mt->id;
            t.color = QColor("#d9775c");
        }
        newTracks.push_back(t);
        return t.id;
    };
    // ---- notes: back out what the pass added so restoring it is the undo step
    Pattern* pat = m_recNoteIds.empty() && m_recControlsBefore.empty() ? nullptr : project().pattern(m_recPatternId);
    if (pat) {
        Pattern after = *pat;
        for (auto& cd : pat->channels) {
            std::erase_if(cd.notes, [&](const Note& n) { return m_recNoteIds.count(n.id) > 0; });
            if (auto it = m_recControlsBefore.find(cd.id); it != m_recControlsBefore.end()) cd.controls = it->second;
        }
        cmds.push_back(std::make_unique<EditObjectCommand<Pattern>>(m_model, pat->id, "", [after](Pattern& p) { p = after; }, 0));
        bool covered = std::any_of(project().clips.begin(), project().clips.end(), [&](const Clip& c) {
            return c.type == ClipType::Pattern && c.sourceId == pat->id && m_recClipStart >= c.start &&
                m_recClipStart < c.start + project().clipLength(c);
        });
        if (m_recSong && !covered) {
            Clip c;
            c.id = project().newId();
            c.type = ClipType::Pattern;
            c.sourceId = pat->id;
            c.start = m_recClipStart;
            c.length = 0;   // follows the pattern, so it grows with what was recorded
            c.name = pat->name;
            c.trackId = trackFor(nullptr, c.start, c.start + after.effectiveLength(project().transport.timeSig));
            placed.push_back(c);
        }
    }
    // ---- audio: each file is the whole pass; segments (loop passes, punch-ins) become takes. The device's reported buffer
    // latency is cut from the front (FL's latency compensation): what was played to the beat arrived that much later.
    int takes = 0;
    std::vector<std::pair<QString, Id>> channelTakes;   // pattern mode: (file, insert)
    for (auto& f : files) {
        const MixerTrack* mt = project().mixerTrack(f.mixerTrackId);
        if (f.frames <= 0 || m_recSegments.empty()) {
            QFile::remove(f.filePath);
            continue;
        }
        if (!m_recSong) {   // FL: pattern mode records into a channel (playlist audio only plays in song mode)
            channelTakes.push_back({f.filePath, f.mixerTrackId});
            continue;
        }
        double sr = f.sampleRate;
        TempoMap tempo = project().transport.tempoMap();
        int64_t latency = std::llround((m_devices.reportedInputLatency() + m_devices.reportedOutputLatency()) * sr);
        AudioSource a;
        a.id = project().newId();
        a.filePath = f.filePath;
        a.name = QFileInfo(f.filePath).completeBaseName();
        a.sampleRate = sr;
        a.channels = f.channels;
        a.frames = f.frames;
        size_t first = placed.size();
        int64_t base = m_recSegments.front().frame;
        for (size_t i = 0; i < m_recSegments.size(); ++i) {
            int64_t from = m_recSegments[i].frame - base + latency;
            // the next segment's first frames still belong to this take only when recording ran on (a loop wrap, not a punch-in)
            int64_t to = i + 1 < m_recSegments.size() ? m_recSegments[i + 1].frame - base + (m_recSegments[i + 1].contiguous ? latency : 0) : f.frames;
            to = std::min(f.frames, to);
            if (to - from < sr / 10) continue;   // a sliver after the last wrap: not a take
            Clip c;
            c.id = project().newId();
            c.type = ClipType::Audio;
            c.sourceId = a.id;
            c.start = std::llround(m_recSegments[i].tick);
            // through the tempo map: the clip's anchor (start - offset) is where the file's first frame would play
            c.sourceOffset = c.start - (Tick)std::llround(tempo.tickAt(tempo.secondsAt(c.start) - from / sr));
            c.length = std::max<Tick>(tempo.ticksFor(c.start, (to - from) / sr), 1);
            c.loopSource = false;
            c.name = QString("%1 take %2").arg(mt ? mt->name : "Rec").arg(placed.size() - first + 1);
            c.trackId = trackFor(mt, c.start, c.start + c.length);
            placed.push_back(c);
        }
        if (placed.size() == first) {
            QFile::remove(f.filePath);
            continue;
        }
        takes += (int)(placed.size() - first);
        // Blend recording off: of takes that overlap only the newest is heard, the earlier ones stay muted under it
        for (size_t i = first; i < placed.size(); ++i)
            for (size_t j = i + 1; j < placed.size(); ++j)
                if (placed[j].start < placed[i].start + placed[i].length && placed[i].start < placed[j].start + placed[j].length)
                    placed[i].muted = true;
        cmds.push_back(std::make_unique<InsertObjectCommand<AudioSource>>(m_model, a, "", -1));
        m_cache.get(f.filePath);
    }
    for (auto& t : newTracks) cmds.push_back(std::make_unique<InsertObjectCommand<PlaylistTrack>>(m_model, t, "", -1));
    for (auto& c : placed) cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, c, "", -1));
    if (!cmds.empty()) m_model.macro("Record", std::move(cmds));
    for (auto& [path, insert] : channelTakes) {
        Id ch = addChannel(ChannelType::Sampler, "native.sampler", QString(), path);
        m_model.edit<Channel>(
            ch, "Route recording", [insert](Channel& c) { c.mixerTrackId = insert; });   // plays through the insert it was monitored on
    }
    if (takes || channelTakes.size()) Q_EMIT statusMessage(QString("Recorded %1 take(s)").arg(takes + (int)channelTakes.size()), 4000);
}
void App::onRecordedMidi(const MidiEvent& e, double tick) {
    if (!m_recPass || project().transport.recordMode == 2 || project().transport.recordMode == 3) return;
    Id pid = m_recPatternId;
    Id cid = project().selectedChannelId;
    if (!pid || !cid || !project().pattern(pid)) return;
    Tick patLen = project().patternLength(*project().pattern(pid));
    Tick local = (Tick)tick;
    if (m_recSong) {
        auto clip = std::find_if(project().clips.begin(), project().clips.end(), [&](const Clip& c) {
            return c.type == ClipType::Pattern && c.sourceId == pid && local >= c.start && local < c.start + project().clipLength(c);
        });
        if (clip != project().clips.end()) local = (local - clip->start + clip->sourceOffset) % std::max<Tick>(patLen, 1);
        else local -= m_recClipStart;   // into the clip the pass places on stop
        if (local < 0) return;
    }
    if (e.type == MidiEvent::CC || e.type == MidiEvent::PitchBend || e.type == MidiEvent::ChannelPressure) {
        int ctrl = e.type == MidiEvent::CC ? e.data1 : e.type == MidiEvent::PitchBend ? kCtrlPitchBend : kCtrlPressure;
        auto& cd = project().pattern(pid)->ensureChannel(cid);
        m_recControlsBefore.try_emplace(cid, cd.controls);
        setControl(cd.controls, local, ctrl, e.type == MidiEvent::PitchBend ? e.pitchBend + 8192 : e.data2);
        if (!m_recNotifyPending) {   // a wheel sends hundreds of values a second
            m_recNotifyPending = true;
            QTimer::singleShot(100, this, [this] {
                m_recNotifyPending = false;
                m_model.notify(ChangePatterns);
            });
        }
        return;
    }
    if (e.type == MidiEvent::NoteOn) {
        m_pendingNotes[e.data1] = {(double)local, e};
        return;
    }
    if (e.type != MidiEvent::NoteOff) return;
    auto it = m_pendingNotes.find(e.data1);
    if (it == m_pendingNotes.end()) return;
    Tick start = (Tick)it->second.first;
    Tick dur = local - start;
    if (dur <= 0) dur = (local + patLen) - start;
    if (dur <= 0) dur = PPQ / 8;
    float vel = it->second.second.velocity;
    m_pendingNotes.erase(it);
    Note n;
    n.id = project().newId();
    n.pitch = e.data1;
    n.start = snapTick(start);
    n.duration = std::max<Tick>(dur, PPQ / 16);
    n.velocity = vel;
    project().pattern(pid)->ensureChannel(cid).notes.push_back(n);
    m_recNoteIds.insert(n.id);
    m_model.notify(ChangePatterns);
}
void App::setPluginBridged(Id pluginInstanceId, bool on) {
    m_engine->syncStateToProject(project());   // the new processor starts from the current state
    m_model.edit<PluginInstanceData>(pluginInstanceId, on ? "Run plugin in its own process" : "Run plugin in the app",
        [on](PluginInstanceData& d) { d.bridged = on, d.crashed = false; });
}
void App::onPluginCrashed(Id slotId) {
    QString name = "plugin";
    for (auto& t : project().mixerTracks)
        for (auto& e : t.effects)
            if (e.id == slotId) {
                name = e.name;
                if (auto* pd = project().plugin(e.pluginInstanceId)) pd->crashed = true;
            }
    for (auto& c : project().channels)
        if (c.id == slotId) {
            name = c.name;
            if (auto* pd = project().plugin(c.pluginInstanceId)) pd->crashed = true;
        }
    Log::error("Plugin crashed and was bypassed: " + name);
    Q_EMIT statusMessage("Plugin crashed and was bypassed: " + name + " - save your project", 0);
}

// ---------------------------------------------------------------- MIDI files
// Import, FL style: every part of the file (a track, or a MIDI channel of a type-0 file) with notes becomes a new synth channel
// routed like a channel added by hand, and they all go into one new pattern named after the file. The project's tempo and time
// signature stay as they are (the status bar says when the file's tempo differs).
Id App::importMidiFile(const QString& path, Id clipTrack, Tick clipAt) {
    QString file = QFileInfo(path).fileName(), err;
    MidiFileData f;
    if (!MidiFile::load(path, f, &err)) {
        Q_EMIT statusMessage(QString("Could not import %1: %2").arg(file, err), 5000);
        return 0;
    }
    std::erase_if(f.tracks, [](const MidiFileTrack& t) { return t.notes.empty(); });
    if (f.tracks.empty()) {
        Q_EMIT statusMessage(file + " has no notes", 4000);
        return 0;
    }
    std::vector<std::unique_ptr<Command>> cmds;
    Pattern pat;
    pat.id = project().newId();
    pat.name = QFileInfo(path).completeBaseName();
    for (auto& c : project().channels) pat.ensureChannel(c.id);
    std::vector<Id> ids;
    std::set<Id> used;
    for (auto& c : project().channels) used.insert(c.mixerTrackId);
    for (auto& t : f.tracks) {
        Channel c;
        c.id = project().newId();
        c.name = t.name;
        c.type = ChannelType::Synth;
        c.instrumentKey = "native.synth";
        c.color = QColor(kChannelPalette[(project().channels.size() + ids.size()) % 8]);
        c.mixerTrackId = project().masterId();
        for (auto& mt : project().mixerTracks)
            if (!mt.isMaster && !used.count(mt.id)) {
                c.mixerTrackId = mt.id;
                break;
            }
        used.insert(c.mixerTrackId);
        cmds.push_back(std::make_unique<InsertObjectCommand<Channel>>(m_model, c, "", -1));
        auto& cd = pat.ensureChannel(c.id);
        cd.notes = t.notes;
        for (auto& n : cd.notes) n.id = project().newId();
        cd.controls = t.controls;
        ids.push_back(c.id);
    }
    for (auto& p : project().patterns)
        cmds.push_back(std::make_unique<EditObjectCommand<Pattern>>(
            m_model, p.id, "",
            [ids](Pattern& x) {
                for (Id id : ids) x.ensureChannel(id);
            },
            0));
    cmds.push_back(std::make_unique<InsertObjectCommand<Pattern>>(m_model, pat, "", -1));
    if (clipTrack) {
        Clip c;
        c.id = project().newId();
        c.type = ClipType::Pattern;
        c.trackId = clipTrack;
        c.sourceId = pat.id;
        c.start = std::max<Tick>(0, clipAt);
        c.length = 0;
        c.loopSource = false;
        c.name = pat.name;
        cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, c, "", -1));
    }
    m_model.macro("Import MIDI file", std::move(cmds));
    selectPattern(pat.id);
    selectChannel(ids.front());
    QString msg = QString("Imported %1: %2 channel(s)").arg(file).arg(ids.size());
    if (f.hasTempo && std::fabs(f.bpm - project().transport.bpm) > 0.01)
        msg +=
            QString(" - the file is at %1 BPM, the project stays at %2 BPM").arg(f.bpm, 0, 'g', 5).arg(project().transport.bpm, 0, 'g', 5);
    Q_EMIT statusMessage(msg, 6000);
    return pat.id;
}
// Export: one track per channel with notes or controller points, plus the tempo and time signature. The song is every audible
// pattern clip flattened onto the timeline the way song mode plays it (the selected pattern when the playlist has none).
bool App::exportMidiFile(const QString& path, Id patternId) {
    const Project& p = project();
    MidiFileData f;
    f.bpm = p.transport.bpm;
    f.timeSig = p.transport.timeSig;
    std::map<Id, MidiFileTrack> parts;
    // what the pattern plays from its ticks [from, to) - piano roll notes and step-sequencer steps - moved by 'at'
    auto add = [&](const Pattern& pat, Tick at, Tick from, Tick to) {
        for (auto& cd : pat.channels) {
            const Channel* ch = p.channel(cd.id);
            if (!ch) continue;
            auto& part = parts[cd.id];
            std::vector<Note> notes;
            for (auto& n : cd.notes)
                if (!n.muted) notes.push_back(n);
            Tick st = pat.stepTicks();
            for (size_t i = 0; i < cd.steps.size() && (int)i < pat.stepCount; ++i) {
                const Step& sp = cd.steps[i];
                if (!sp.enabled) continue;
                Note n;   // as SnapshotBuilder compiles a step
                n.pitch = clamp((int)std::lround(ch->rootNote + sp.pitch), 0, 127);
                n.start = (Tick)i * st + (Tick)(sp.offset * st) + ((i % 2) ? (Tick)(pat.swing * st * 0.5f) : 0);
                n.duration = st;
                n.velocity = sp.velocity;
                notes.push_back(n);
            }
            for (auto& n : notes)
                if (n.start >= from && n.start < to) {
                    part.notes.push_back(n);
                    part.notes.back().start += at;
                }
            for (auto& c : cd.controls)
                if (c.tick >= from && c.tick < to) part.controls.push_back({c.tick + at, c.controller, c.value});
        }
    };
    const Pattern* single = p.pattern(patternId);
    bool song = !single && std::any_of(p.clips.begin(), p.clips.end(), [](const Clip& c) { return c.type == ClipType::Pattern; });
    if (song) {
        for (auto& c : p.clips) {
            const Pattern* pat = p.pattern(c.sourceId);
            const PlaylistTrack* t = p.track(c.trackId);
            if (c.type != ClipType::Pattern || !pat || !t || c.muted || t->mute) continue;
            // the clip plays pattern ticks [sourceOffset, sourceOffset + length), repeating the pattern when it loops
            Tick len = std::max<Tick>(p.patternLength(*pat), 1), end = c.sourceOffset + p.clipLength(c);
            for (Tick k = c.sourceOffset / len; k * len < end && (c.loopSource || k == 0); ++k)
                add(*pat, c.start - c.sourceOffset + k * len, std::max<Tick>(c.sourceOffset - k * len, 0),
                    std::min<Tick>(end - k * len, len));
        }
    } else if (const Pattern* pat = single ? single : p.pattern(p.selectedPatternId))
        add(*pat, 0, 0, p.patternLength(*pat));
    for (auto& ch : p.channels)
        if (auto it = parts.find(ch.id); it != parts.end() && (!it->second.notes.empty() || !it->second.controls.empty())) {
            it->second.name = ch.name;
            f.tracks.push_back(std::move(it->second));
        }
    QString err;
    if (!MidiFile::save(path, f, &err)) {
        Q_EMIT statusMessage("MIDI export failed: " + err, 5000);
        return false;
    }
    Q_EMIT statusMessage(QString("Exported %1 track(s) to %2").arg(f.tracks.size()).arg(QFileInfo(path).fileName()), 4000);
    return true;
}

// ---------------------------------------------------------------- audio config & export
bool App::applyAudioConfig(const AudioConfig& cfg, QString* error) {
    m_engine->stop();
    m_engine->stopDevice();
    m_devices.editableConfig() = cfg;
    m_devices.saveConfig();
    bool ok = m_engine->startDevice(error);
    m_recorder->setSampleRate(m_engine->sampleRate());
    m_engine->rebuild(project());
    return ok;
}
bool App::exportAudio(const RenderOptions& opt, std::function<bool(double)> progress, QString* error) {
    m_engine->stop();
    m_engine->syncStateToProject(project());
    if (!opt.stems) {
        bool ok = RenderEngine::render(*m_engine, project(), opt, std::move(progress), error);
        syncTransportToEngine();
        return ok;
    }
    // FL's "split mixer tracks": every insert something plays into, as heard there (post effects and fader), plus the master
    auto& p = project();
    std::set<Id> used{p.masterId()};
    for (auto& c : p.channels) used.insert(c.mixerTrackId);
    for (auto& c : p.clips)
        if (auto* t = p.track(c.trackId); t && c.type == ClipType::Audio) used.insert(t->mixerTrackId);
    for (auto& t : p.mixerTracks) {
        if (t.inputChannel >= 0) used.insert(t.id);
        for (auto& s : t.sends) used.insert(s.targetTrackId);
        if (t.outputTrackId) used.insert(t.outputTrackId);
    }
    std::vector<const MixerTrack*> tracks;
    for (auto& t : p.mixerTracks)
        if (used.count(t.id)) tracks.push_back(&t);
    QFileInfo fi(opt.path);
    bool ok = true;
    for (size_t i = 0; i < tracks.size() && ok; ++i) {
        RenderOptions o = opt;
        o.tapTrack = tracks[i]->id;
        QString name = tracks[i]->isMaster ? "Master" : tracks[i]->name;
        name.replace(QRegularExpression("[^A-Za-z0-9 _.-]"), "_");
        o.path = QString("%1/%2_%3 %4.%5")
                     .arg(fi.absolutePath(), fi.completeBaseName())
                     .arg(p.mixerIndex(o.tapTrack), 2, 10, QChar('0'))
                     .arg(name, o.format);
        ok = RenderEngine::render(*m_engine, p, o, [&](double v) { return !progress || progress((i + v) / tracks.size()); }, error);
    }
    syncTransportToEngine();
    return ok;
}

bool App::freezeMixerTrack(Id id, std::function<bool(double)> progress, QString* error) {
    const MixerTrack* mt = project().mixerTrack(id);
    if (!mt || mt->isMaster || mt->freezeTrackId) return false;
    QString dir = m_model.filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Vybrant Recordings"
                                               : QFileInfo(m_model.filePath()).absolutePath() + "/Frozen";
    QDir().mkpath(dir);
    RenderOptions o;
    o.path = QString("%1/%2 frozen %3.wav")
                 .arg(dir, QString(mt->name).replace(QRegularExpression("[^A-Za-z0-9 _.-]"), "_"),
                     QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    o.bitDepth = 32;
    o.dither = false;
    o.sampleRate = m_engine->sampleRate();
    o.tapTrack = id;
    o.tapPreFader = true;
    if (!exportAudio(o, std::move(progress), error)) return false;
    double sr = 0;
    int ch = 0;
    int64_t frames = 0;
    if (!AudioFile::probe(o.path, &sr, &ch, &frames) || frames <= 0) {
        if (error) *error = "The render is empty";
        return false;
    }
    AudioSource a;
    a.id = project().newId();
    a.filePath = o.path;
    a.name = QFileInfo(o.path).completeBaseName();
    a.sampleRate = sr;
    a.channels = ch;
    a.frames = frames;
    PlaylistTrack pt;
    pt.id = project().newId();
    pt.name = mt->name + " (frozen)";
    pt.color = mt->color;
    pt.mixerTrackId = id;
    Clip c;
    c.id = project().newId();
    c.type = ClipType::Audio;
    c.trackId = pt.id;
    c.sourceId = a.id;
    c.length = std::max<Tick>(project().transport.tempoMap().ticksFor(0, frames / sr), 1);
    c.loopSource = false;
    c.name = pt.name;
    std::vector<std::unique_ptr<Command>> cmds;
    cmds.push_back(std::make_unique<InsertObjectCommand<AudioSource>>(m_model, a, "", -1));
    cmds.push_back(std::make_unique<InsertObjectCommand<PlaylistTrack>>(m_model, pt, "", -1));
    cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, c, "", -1));
    cmds.push_back(
        std::make_unique<EditObjectCommand<MixerTrack>>(m_model, id, "", [t = pt.id](MixerTrack& x) { x.freezeTrackId = t; }, 0));
    m_cache.getSync(o.path);   // loaded before the clip exists, so it plays at once instead of after an async load
    m_model.macro("Freeze " + mt->name, std::move(cmds));
    return true;
}
void App::unfreezeMixerTrack(Id id) {
    const MixerTrack* mt = project().mixerTrack(id);
    if (!mt || !mt->freezeTrackId) return;
    std::vector<std::unique_ptr<Command>> cmds;
    std::set<Id> sources;
    for (auto& c : project().clips)
        if (c.trackId == mt->freezeTrackId) {
            cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, c.id, ""));
            if (c.type == ClipType::Audio) sources.insert(c.sourceId);
        }
    for (Id s : sources) cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_model, s, ""));
    if (project().track(mt->freezeTrackId))
        cmds.push_back(std::make_unique<RemoveObjectCommand<PlaylistTrack>>(m_model, mt->freezeTrackId, ""));
    cmds.push_back(std::make_unique<EditObjectCommand<MixerTrack>>(m_model, id, "", [](MixerTrack& x) { x.freezeTrackId = 0; }, 0));
    m_model.macro("Unfreeze " + mt->name, std::move(cmds));
}
Id App::consolidateClips(const std::vector<Id>& clipIds, std::function<bool(double)> progress, QString* error, Tail tail) {
    auto& p = project();
    std::set<Id> sel;
    Tick from = std::numeric_limits<Tick>::max(), to = 0;
    const Clip* first = nullptr;
    for (Id id : clipIds)
        if (const Clip* c = p.clip(id); c && c->type == ClipType::Pattern) {
            sel.insert(id);
            from = std::min(from, c->start);
            to = std::max(to, c->start + p.clipLength(*c));
            if (!first || c->start < first->start) first = c;
        }
    if (sel.empty()) return 0;
    // the render is what reaches the master, so the clip must play straight into it (not through an insert a second time)
    auto freeAt = [&](const PlaylistTrack& t) {
        const MixerTrack* m = p.mixerTrack(t.mixerTrackId);
        return (!m || m->isMaster) && std::none_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) {
            return c.trackId == t.id && !sel.count(c.id) && c.start < to && c.start + p.clipLength(c) > from;
        });
    };
    Id track = 0;
    if (const PlaylistTrack* t = p.track(first->trackId); t && freeAt(*t)) track = t->id;
    for (auto& t : p.playlistTracks)
        if (!track && freeAt(t)) track = t.id;
    const Pattern* pat = p.pattern(first->sourceId);
    QString name = pat ? pat->name : first->name;
    QString dir = m_model.filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Vybrant Recordings"
                                               : QFileInfo(m_model.filePath()).absolutePath() + "/Consolidated";
    QDir().mkpath(dir);
    RenderOptions o;
    o.path = QString("%1/%2 consolidated %3.wav")
                 .arg(dir, QString(name).replace(QRegularExpression("[^A-Za-z0-9 _.-]"), "_"),
                     QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));
    o.bitDepth = 32;
    o.dither = false;
    o.sampleRate = m_engine->sampleRate();
    o.start = from;
    o.end = to;
    o.tapTrack = p.masterId();
    o.tapInput = true;
    o.playTail = true;
    m_engine->stop();
    m_engine->syncStateToProject(p);
    Project only = p;   // the selected clips alone; automation clips keep running
    for (auto& c : only.clips)
        if (!sel.count(c.id) && c.type != ClipType::Automation) c.muted = true;
    m_engine->rebuild(only);
    bool ok = RenderEngine::render(*m_engine, only, o, std::move(progress), error);
    m_engine->rebuild(p);
    syncTransportToEngine();
    if (!ok) return 0;
    double sr = 0;
    int ch = 0;
    int64_t frames = 0;
    if (!AudioFile::probe(o.path, &sr, &ch, &frames) || frames <= 0) {
        if (error) *error = "The render is empty";
        return 0;
    }
    TempoMap tm = p.transport.tempoMap();
    int64_t selFrames = (int64_t)std::ceil((tm.secondsAt(to) - tm.secondsAt(from)) * sr - 1e-6);   // playback reaches into the last sample
    if (tail != Tail::Leave && selFrames > 0 && frames > selFrames) {   // drop (Cut) or fold back (Wrap) what rings on past the end
        auto f = AudioFile::load(o.path, 0, error);
        if (!f) return 0;
        // one frame more than the selection: playback interpolates the last sample with the next (the cut-off one, or the loop's start)
        std::vector<float> out((size_t)(selFrames + 1) * f->channels, 0.f);
        for (int64_t i = 0; i < f->frames && (tail == Tail::Wrap || i <= selFrames); ++i) {
            int64_t j = (tail == Tail::Wrap ? i % selFrames : i) * f->channels;
            out[j] += f->L()[i];
            if (f->channels > 1) out[j + 1] += f->R()[i];
        }
        if (tail == Tail::Wrap)
            for (int k = 0; k < f->channels; ++k) out[(size_t)selFrames * f->channels + k] = out[k];
        if (!AudioFile::write(o.path, out.data(), f->channels, selFrames + 1, sr, 32, "wav", error)) return 0;
        frames = selFrames + 1;
    }
    std::vector<std::unique_ptr<Command>> cmds;
    for (Id id : sel) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_model, id, ""));
    AudioSource a;
    a.id = p.newId();
    a.filePath = o.path;
    a.name = name + " (consolidated)";   // the file name carries a timestamp so renders never collide; the sample needn't
    a.sampleRate = sr;
    a.channels = ch;
    a.frames = frames;
    cmds.push_back(std::make_unique<InsertObjectCommand<AudioSource>>(m_model, a, "", -1));
    if (!track) {   // every track routed to the master is busy there
        PlaylistTrack pt;
        pt.id = track = p.newId();
        pt.name = QString("Track %1").arg(p.playlistTracks.size() + 1);
        cmds.push_back(std::make_unique<InsertObjectCommand<PlaylistTrack>>(m_model, pt, "", -1));
    }
    Clip c;
    c.id = p.newId();
    c.type = ClipType::Audio;
    c.trackId = track;
    c.sourceId = a.id;
    c.start = from;
    c.length = tail == Tail::Leave ? std::max<Tick>(tm.ticksFor(from, frames / sr), 1) : to - from;
    c.loopSource = false;
    c.name = a.name;
    cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_model, c, "", -1));
    m_cache.getSync(o.path);   // loaded before the clip exists, so it plays at once instead of after an async load
    m_model.macro("Consolidate", std::move(cmds));
    return c.id;
}

void addPatternActions(QMenu* menu, App& app, QWidget* parent, const std::vector<Id>& ids) {
    if (ids.empty()) return;
    QString many = ids.size() > 1 ? QString(" (%1)").arg(ids.size()) : QString();
    const Pattern* first = app.project().pattern(ids[0]);
    if (ids.size() == 1) {
        menu->addAction("Rename...", [&app, parent, id = ids[0]] {
            const Pattern* p = app.project().pattern(id);
            if (auto n = askName(parent, "Rename pattern", p ? p->name : QString())) app.renamePattern(id, *n);
        });
        menu->addAction("Split by channel", [&app, id = ids[0]] { app.splitPatternByChannel(id); })
            ->setToolTip("Every channel with content becomes its own pattern, named after the channel");
    }
    menu->addAction("Color..." + many, [&app, parent, ids, c0 = first ? first->color : QColor(Qt::white)] {
        QColor c = QColorDialog::getColor(c0, parent);
        if (c.isValid())
            for (Id id : ids) app.setPatternColor(id, c);
    });
    menu->addAction("Clone" + many, [&app, ids] {
        for (Id id : ids) app.clonePattern(id);
    });
}
void fillMixerTrackMenu(QMenu* menu, const Project& p, Id current, std::function<void(Id)> pick) {
    for (auto& t : p.mixerTracks) {
        auto* a = menu->addAction(t.name, [pick, id = t.id] { pick(id); });
        a->setCheckable(true);
        a->setChecked(t.id == current);
    }
}

}   // namespace mt
