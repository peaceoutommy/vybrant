#include "plugins/PluginHost.h"
#include "plugins/Vst3Plugin.h"
#include "plugins/Vst2Plugin.h"
#include "plugins/ClapPlugin.h"
#include "plugins/Lv2Plugin.h"
#include "plugins/PluginBridge.h"
#include <QSettings>
#include "core/Log.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>
#include "core/DynLib.h"
#include <QTimer>
#include <QFileInfo>
#include <QSignalBlocker>
#include <functional>

namespace mt {

// ---------------------------------------------------------------- yabridge
namespace yabridge {
static QString findExe(const QString& name) {
    QString p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty()) return p;
    for (auto& d :
        {QDir::homePath() + "/.local/share/yabridge", QDir::homePath() + "/.local/bin", QString("/usr/bin"), QString("/usr/local/bin")})
        if (QFileInfo(d + "/" + name).isExecutable()) return d + "/" + name;
    return QString();
}
Status status() {
    Status st;
    st.yabridgectl = findExe("yabridgectl");
    st.wine = findExe("wine");
    if (st.wine.isEmpty()) st.wine = findExe("wine64");
    if (!st.yabridgectl.isEmpty()) {
        QProcess p;
        p.start(st.yabridgectl, {"--version"});
        if (p.waitForFinished(3000)) st.version = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        QProcess l;
        l.start(st.yabridgectl, {"list"});
        if (l.waitForFinished(3000))
            for (auto& line : QString::fromUtf8(l.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts))
                if (line.startsWith('/')) st.winePluginDirs << line.trimmed();
    }
    return st;
}
QStringList wrapperDirs() { return {QDir::homePath() + "/.vst/yabridge", QDir::homePath() + "/.vst3/yabridge"}; }
bool isBridged(const QString& path) {
    for (auto& d : wrapperDirs())
        if (path.startsWith(d)) return true;
    // the wrappers are copies of yabridge's chainloader: cheap to recognise by the strings inside
    QString so = path;
    if (QFileInfo(path).isDir()) {
        QDir bin(path + "/Contents/x86_64-linux");
        auto l = bin.entryList({"*.so"}, QDir::Files);
        so = l.isEmpty() ? QString() : bin.filePath(l.first());
    }
    QFile f(so);
    if (so.isEmpty() || !f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024) return false;
    return f.read(4 * 1024 * 1024).contains("yabridge");
}
}   // namespace yabridge

// ---------------------------------------------------------------- database
PluginDatabase::PluginDatabase(QObject* parent)
    : QObject(parent) {
    m_dirs = defaultDirectories();
}
QString PluginDatabase::databasePath() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d + "/plugins.json";
}
static QStringList clapDirectories() {
    QStringList d = {QDir::homePath() + "/.clap", "/usr/lib/clap", "/usr/local/lib/clap"};
    for (auto& p : qEnvironmentVariable("CLAP_PATH").split(':', Qt::SkipEmptyParts)) d << p;
    return d;
}
QStringList PluginDatabase::defaultDirectories() {
    QString home = QDir::homePath();
    QStringList d = {home + "/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3", home + "/.vst", "/usr/lib/vst", "/usr/local/lib/vst",
        home + "/.lxvst", "/usr/lib/lxvst", "/usr/local/lib/lxvst"};
    QString env = qEnvironmentVariable("VST_PATH");
    for (auto& p : env.split(':', Qt::SkipEmptyParts)) d << p;
    env = qEnvironmentVariable("VST3_PATH");
    for (auto& p : env.split(':', Qt::SkipEmptyParts)) d << p;
    d << clapDirectories();
    d.removeDuplicates();
    return d;
}
void PluginDatabase::load() {
    QFile f(databasePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    auto root = doc.object();
    m_plugins.clear();
    for (auto v : root["plugins"].toArray()) {
        auto o = v.toObject();
        PluginInfo p;
        p.format = o["format"].toString();
        p.path = o["path"].toString();
        p.uid = o["uid"].toString();
        p.name = o["name"].toString();
        p.vendor = o["vendor"].toString();
        p.category = o["category"].toString();
        p.subCategories = o["subCategories"].toString();
        p.version = o["version"].toString();
        p.numInputs = o["numInputs"].toInt();
        p.numOutputs = o["numOutputs"].toInt();
        p.hasEditor = o["hasEditor"].toBool();
        p.favorite = o["favorite"].toBool();
        p.hidden = o["hidden"].toBool();
        p.failed = o["failed"].toBool();
        p.failReason = o["failReason"].toString();
        p.lastScanned = o["lastScanned"].toString();
        m_plugins.push_back(p);
    }
    auto dirs = root["directories"].toArray();
    if (!dirs.isEmpty()) {
        m_dirs.clear();
        for (auto v : dirs) m_dirs << v.toString();
    }
    if (root["version"].toInt() < 2)   // saved before CLAP support: add its default folders once
        for (auto& d : clapDirectories())
            if (!m_dirs.contains(d)) m_dirs << d;
    Q_EMIT changed();
}
void PluginDatabase::save() const {
    QJsonArray arr;
    for (auto& p : m_plugins)
        arr.append(QJsonObject{{"format", p.format}, {"path", p.path}, {"uid", p.uid}, {"name", p.name}, {"vendor", p.vendor},
            {"category", p.category}, {"subCategories", p.subCategories}, {"version", p.version}, {"numInputs", p.numInputs},
            {"numOutputs", p.numOutputs}, {"hasEditor", p.hasEditor}, {"favorite", p.favorite}, {"hidden", p.hidden}, {"failed", p.failed},
            {"failReason", p.failReason}, {"lastScanned", p.lastScanned}});
    QJsonArray dirs;
    for (auto& d : m_dirs) dirs.append(d);
    QFile f(databasePath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"version", 2}, {"plugins", arr}, {"directories", dirs}}).toJson());
}
const PluginInfo* PluginDatabase::find(const QString& format, const QString& uid) const {
    for (auto& p : m_plugins)
        if (p.format == format && p.uid == uid) return &p;
    return nullptr;
}
const PluginInfo* PluginDatabase::findByPath(const QString& path, const QString& uid) const {
    for (auto& p : m_plugins)
        if (p.path == path && (uid.isEmpty() || p.uid == uid)) return &p;
    return nullptr;
}
void PluginDatabase::addOrUpdate(const PluginInfo& info) {
    for (auto& p : m_plugins)
        if (p.format == info.format && p.path == info.path && p.uid == info.uid) {
            bool fav = p.favorite, hid = p.hidden;
            p = info;
            p.favorite = fav;
            p.hidden = hid;
            Q_EMIT changed();
            return;
        }
    m_plugins.push_back(info);
    Q_EMIT changed();
}
void PluginDatabase::markFailed(const QString& path, const QString& reason) {
    bool found = false;
    for (auto& p : m_plugins)
        if (p.path == path) {
            p.failed = true;
            p.failReason = reason;
            found = true;
        }
    if (!found) {
        PluginInfo p;
        p.path = path;
        p.name = QFileInfo(path).completeBaseName();
        p.format = path.endsWith(".vst3") ? "vst3" : path.endsWith(".clap") ? "clap" : "vst2";
        p.failed = true;
        p.failReason = reason;
        p.lastScanned = QDateTime::currentDateTime().toString(Qt::ISODate);
        m_plugins.push_back(p);
    }
    Q_EMIT changed();
}
void PluginDatabase::setFavorite(const QString& key, bool f) {
    for (auto& p : m_plugins)
        if (p.key() == key) p.favorite = f;
    save();
    Q_EMIT changed();
}
void PluginDatabase::setHidden(const QString& key, bool h) {
    for (auto& p : m_plugins)
        if (p.key() == key) p.hidden = h;
    save();
    Q_EMIT changed();
}
void PluginDatabase::removeMissing() {
    std::erase_if(m_plugins, [](const PluginInfo& p) { return !QFileInfo::exists(p.path); });
    save();
    Q_EMIT changed();
}

// ---------------------------------------------------------------- scanner
PluginScanner::PluginScanner(PluginDatabase& db, QObject* parent)
    : QObject(parent)
    , m_db(db) {}

QStringList PluginScanner::candidateFiles(const QStringList& dirs) {
    QStringList out;
    std::function<void(const QString&, int)> walk = [&](const QString& dir, int depth) {
        QDir d(dir);
        if (!d.exists() || depth > 6) return;
        for (auto& fi : d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
            if (fi.isDir()) {
                if (fi.fileName().endsWith(".vst3", Qt::CaseInsensitive)) out << fi.absoluteFilePath();
                else if (!fi.fileName().endsWith(".lv2")) walk(fi.absoluteFilePath(), depth + 1);   // LV2 bundles: Lv2Plugin::scan
            } else if (fi.suffix() == "so" && !dir.contains("/Contents/")) out << fi.absoluteFilePath();
            else if (fi.suffix().compare("vst3", Qt::CaseInsensitive) == 0) out << fi.absoluteFilePath();
            else if (fi.suffix().compare("clap", Qt::CaseInsensitive) == 0) out << fi.absoluteFilePath();
        }
    };
    for (auto& d : dirs) walk(d, 0);
    out.removeDuplicates();
    return out;
}

void PluginScanner::scan(bool rescanAll) {
    if (isScanning()) return;
    m_cancel = false;
    m_queue.clear();
    m_done = 0;
    {   // LV2: lilv only reads the bundles' Turtle (no plugin code runs), so this is done in-process; one change signal for all
        const QSignalBlocker block(&m_db);
        for (auto& p : Lv2Plugin::scan()) m_db.addOrUpdate(p);
    }
    Q_EMIT m_db.changed();
    for (auto& f : candidateFiles(m_db.directories())) {
        if (!rescanAll) {
            if (auto* p = m_db.findByPath(f)) {
                if (!p->failed || true) {
                    if (QFileInfo(f).lastModified().toString(Qt::ISODate) <= p->lastScanned) continue;
                }
            }
        }
        m_queue << f;
    }
    m_total = m_queue.size();
    if (m_queue.isEmpty()) {
        m_db.save();
        Q_EMIT finished();
        return;
    }
    for (int i = 0; i < 4; ++i) launchNext();
}
void PluginScanner::cancel() {
    m_cancel = true;
    m_queue.clear();
    for (auto* p : m_running) p->kill();
}

void PluginScanner::launchNext() {
    if (m_queue.isEmpty()) {
        if (m_running.empty()) {
            m_db.save();
            Q_EMIT finished();
        }
        return;
    }
    QString path = m_queue.takeFirst();
    auto* proc = new QProcess(this);
    m_running.push_back(proc);
    Q_EMIT progress(m_done, m_total, path);
    connect(proc, &QProcess::finished, this, [this, proc, path](int code, QProcess::ExitStatus st) {
        QString out = QString::fromUtf8(proc->readAllStandardOutput());
        QString err = QString::fromUtf8(proc->readAllStandardError());
        if (st != QProcess::NormalExit || code != 0) {
            m_db.markFailed(path, st != QProcess::NormalExit ? "Crashed during scan" : ("Scan failed: " + err.trimmed().left(200)));
        } else {
            int n = 0;
            for (auto& line : out.split('\n', Qt::SkipEmptyParts)) {
                auto doc = QJsonDocument::fromJson(line.toUtf8());
                if (!doc.isObject()) continue;
                auto o = doc.object();
                PluginInfo p;
                p.format = o["format"].toString();
                p.path = path;
                p.uid = o["uid"].toString();
                p.name = o["name"].toString();
                p.vendor = o["vendor"].toString();
                p.category = o["category"].toString();
                p.subCategories = o["subCategories"].toString();
                p.version = o["version"].toString();
                p.numInputs = o["numInputs"].toInt();
                p.numOutputs = o["numOutputs"].toInt();
                p.hasEditor = o["hasEditor"].toBool();
                p.lastScanned = QDateTime::currentDateTime().toString(Qt::ISODate);
                m_db.addOrUpdate(p);
                ++n;
            }
            if (n == 0) m_db.markFailed(path, "No plugin classes found");
        }
        std::erase(m_running, proc);
        proc->deleteLater();
        m_done++;
        if (!m_cancel) launchNext();
        else if (m_running.empty()) Q_EMIT finished();
    });
    proc->start(QCoreApplication::applicationFilePath(), {"--scan-plugin", path});
    QTimer::singleShot(yabridge::isBridged(path) ? 180000 : 45000, proc, [proc] {
        if (proc->state() == QProcess::Running) proc->kill();
    });   // bridged plugins boot Wine first
}

int PluginScanner::scanChildMain(const QString& path) {
    QTextStream out(stdout);
    auto emitJson = [&](const QJsonObject& o) {
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        out.flush();
    };
    if (path.endsWith(".clap", Qt::CaseInsensitive)) {
        QString err;
        auto mod = ClapModule::load(path, &err);
        if (!mod) {
            QTextStream(stderr) << err << "\n";
            return 2;
        }
        for (auto* d : mod->descriptors()) {
            QStringList features;
            for (auto* f = d->features; f && *f; ++f) features << QString::fromUtf8(*f);
            auto inst = ClapPlugin::create(path, QString::fromUtf8(d->id), &err);   // instantiate to validate & learn editor info
            if (!inst) {
                QTextStream(stderr) << err << "\n";
                continue;
            }
            bool synth = inst->kind() == ProcessorKind::Instrument;
            emitJson(QJsonObject{{"format", "clap"}, {"uid", QString::fromUtf8(d->id)}, {"name", QString::fromUtf8(d->name)},
                {"vendor", QString::fromUtf8(d->vendor)}, {"category", synth ? "Instrument" : "Effect"},
                {"subCategories", features.join('|')}, {"version", QString::fromUtf8(d->version)}, {"hasEditor", inst->hasCustomEditor()},
                {"numInputs", synth ? 0 : 2}, {"numOutputs", 2}});
        }
        return 0;
    }
    bool isVst3 = path.endsWith(".vst3", Qt::CaseInsensitive) || QFileInfo(path).isDir();
    if (isVst3) {
        QString err;
        auto mod = Vst3Module::load(path, &err);
        if (!mod) {
            QTextStream(stderr) << err << "\n";
            return 2;
        }
        auto classes = mod->classes();
        if (classes.empty()) {
            QTextStream(stderr) << "No classes\n";
            return 3;
        }
        for (auto& c : classes) {
            QJsonObject o{{"format", "vst3"}, {"uid", c.cid}, {"name", c.name}, {"vendor", c.vendor}, {"category", c.category},
                {"subCategories", c.subCategories}, {"version", c.version}};
            // Instantiate to validate & learn bus/editor info
            QString ierr;
            auto inst = Vst3Plugin::create(path, c.cid, &ierr);
            if (inst) {
                o["hasEditor"] = inst->hasCustomEditor();
                o["numInputs"] = inst->wantsSidechain() ? 4 : (inst->kind() == ProcessorKind::Instrument ? 0 : 2);
                o["numOutputs"] = 2;
            } else {
                QTextStream(stderr) << ierr << "\n";
                continue;
            }
            emitJson(o);
        }
        return 0;
    }
    // Try VST2; a .so exporting GetPluginFactory is also accepted as VST3
    void* h = dynlib::open(path);
    bool hasVst3 = h && dynlib::symbol(h, "GetPluginFactory");
    if (h) dynlib::close(h);
    if (hasVst3) {
        QString err;
        auto mod = Vst3Module::load(path, &err);
        if (mod) {
            for (auto& c : mod->classes())
                emitJson(QJsonObject{{"format", "vst3"}, {"uid", c.cid}, {"name", c.name}, {"vendor", c.vendor}, {"category", c.category},
                    {"subCategories", c.subCategories}, {"version", c.version}, {"hasEditor", true}, {"numInputs", 2}, {"numOutputs", 2}});
            return 0;
        }
    }
    Vst2Info info;
    QString err;
    if (!Vst2Plugin::probe(path, info, &err)) {
        QTextStream(stderr) << err << "\n";
        return 4;
    }
    emitJson(
        QJsonObject{{"format", "vst2"}, {"uid", QString::number((uint32_t)info.uniqueId, 16)}, {"name", info.name}, {"vendor", info.vendor},
            {"category", info.isSynth ? "Instrument" : "Effect"}, {"subCategories", ""}, {"version", QString::number(info.version)},
            {"numInputs", info.numInputs}, {"numOutputs", info.numOutputs}, {"hasEditor", info.hasEditor}});
    return 0;
}

// ---------------------------------------------------------------- host
PluginInstanceData PluginHost::makeInstanceData(const PluginInfo& info, Id id) {
    PluginInstanceData d;
    d.id = id;
    d.format = info.format;
    d.identifier = info.uid;
    d.name = info.name;
    d.manufacturer = info.vendor;
    d.type = info.category == "Instrument" ? "instrument" : "effect";
    d.path = info.path;
    d.bridged = QSettings().value("plugins/bridge", false).toBool();
    return d;
}

std::unique_ptr<IProcessor> PluginHost::createInstance(const PluginInstanceData& data, QString* error) {
    QString path = data.path;
    if (!QFileInfo::exists(path)) {
        if (auto* p = m_db.find(data.format, data.identifier)) path = p->path;
    }
    if (!QFileInfo::exists(path)) {
        if (error) *error = "Plugin binary not found: " + data.path;
        Log::warn("Missing plugin: " + data.name + " (" + data.path + ")");
        return nullptr;
    }
    if (data.bridged) {
        PluginInstanceData d = data;
        d.path = path;
        return BridgedPlugin::create(d, error);
    }
    QString err;
    std::unique_ptr<IProcessor> proc;
    if (data.format == "vst3") {
        auto p = Vst3Plugin::create(path, data.identifier, &err);
        if (p) {
            QJsonObject st;
            if (!data.state.isEmpty()) st["pluginState"] = QString::fromLatin1(data.state.toBase64());
            if (!data.controllerState.isEmpty()) st["controllerState"] = QString::fromLatin1(data.controllerState.toBase64());
            if (!st.isEmpty()) p->restoreState(st);
            proc = std::move(p);
        }
    } else if (data.format == "vst2") {
        auto p = Vst2Plugin::create(path, &err);
        if (p) {
            QJsonObject st;
            if (!data.state.isEmpty()) st["pluginState"] = QString::fromLatin1(data.state.toBase64());
            if (!st.isEmpty()) p->restoreState(st);
            proc = std::move(p);
        }
    } else if (data.format == "clap") {
        auto p = ClapPlugin::create(path, data.identifier, &err);
        if (p && !data.state.isEmpty()) p->restoreState(QJsonObject{{"pluginState", QString::fromLatin1(data.state.toBase64())}});
        proc = std::move(p);
    } else if (data.format == "lv2") {
        auto p = Lv2Plugin::create(data.identifier, path, &err);
        if (p) {
            QJsonObject st;
            if (!data.state.isEmpty()) st["pluginState"] = QString::fromLatin1(data.state.toBase64());
            if (!st.isEmpty()) p->restoreState(st);
            proc = std::move(p);
        }
    } else err = "Unknown plugin format " + data.format;
    if (!proc) {
        if (error) *error = err;
        Log::error("Failed to load plugin " + data.name + ": " + err);
    } else Log::info("Loaded plugin " + data.name + " (" + data.format + ")");
    return proc;
}

}   // namespace mt
