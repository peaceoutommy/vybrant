// Plugin manager: scanning (out-of-process, asynchronous), plugin database persistence, and instance creation.
#pragma once
#include "dsp/Processor.h"
#include "project/Project.h"
#include <QObject>
#include <QStringList>
#include <QProcess>
#include <vector>
#include <memory>

namespace mt {

// Windows plugins through yabridge (https://github.com/robbert-vdh/yabridge): yabridgectl turns each Windows .dll / .vst3 into a
// Linux .so / .vst3 wrapper in ~/.vst/yabridge and ~/.vst3/yabridge, which the normal scanner then picks up like any native plugin.
namespace yabridge {
struct Status {
    QString yabridgectl, wine, version;
    QStringList winePluginDirs;
    bool installed() const { return !yabridgectl.isEmpty(); }
};
Status status();   // looks for yabridgectl / wine on PATH and in ~/.local/share/yabridge
bool isBridged(const QString& pluginPath);   // true for yabridge wrapper plugins (they boot Wine, so scans / loads take longer)
QStringList wrapperDirs();   // ~/.vst/yabridge, ~/.vst3/yabridge
}   // namespace yabridge

struct PluginInfo {
    QString format;   // "vst2" | "vst3" | "lv2"
    QString path;   // bundle dir (vst3, lv2) or .so (vst2)
    QString uid;   // class id hex (vst3) / unique id hex (vst2) / plugin URI (lv2)
    QString name, vendor, category /* Instrument|Effect */, subCategories, version;
    int numInputs = 0, numOutputs = 0;
    bool hasEditor = false;
    bool favorite = false, hidden = false, failed = false;
    QString failReason;
    QString lastScanned;
    QString key() const { return format + ":" + uid + "@" + path; }
};

class PluginDatabase : public QObject {
    Q_OBJECT
public:
    explicit PluginDatabase(QObject* parent = nullptr);
    void load();
    void save() const;
    const std::vector<PluginInfo>& plugins() const { return m_plugins; }
    const PluginInfo* find(const QString& format, const QString& uid) const;
    const PluginInfo* findByPath(const QString& path, const QString& uid = QString()) const;
    void addOrUpdate(const PluginInfo& info);
    void markFailed(const QString& path, const QString& reason);
    void setFavorite(const QString& key, bool f);
    void setHidden(const QString& key, bool h);
    void removeMissing();
    QStringList directories() const { return m_dirs; }
    void setDirectories(const QStringList& d) {
        m_dirs = d;
        save();
    }
    static QStringList defaultDirectories();
    static QString databasePath();
Q_SIGNALS:
    void changed();

private:
    std::vector<PluginInfo> m_plugins;
    QStringList m_dirs;
};

class PluginScanner : public QObject {
    Q_OBJECT
public:
    explicit PluginScanner(PluginDatabase& db, QObject* parent = nullptr);
    void scan(bool rescanAll = false);   // asynchronous; results land in the database
    void cancel();
    bool isScanning() const { return !m_queue.isEmpty() || !m_running.empty(); }
    static QStringList candidateFiles(const QStringList& dirs);
    // In-process scan used by the child process (`musicthingy --scan-plugin <path>`), prints JSON to stdout
    static int scanChildMain(const QString& path);
Q_SIGNALS:
    void progress(int done, int total, const QString& current);
    void finished();

private:
    void launchNext();
    PluginDatabase& m_db;
    QStringList m_queue;
    int m_total = 0, m_done = 0;
    std::vector<QProcess*> m_running;
    bool m_cancel = false;
};

class PluginHost {
public:
    explicit PluginHost(PluginDatabase& db)
        : m_db(db) {}
    // Create a runtime processor for a project plugin instance (returns nullptr and marks missing when unavailable)
    std::unique_ptr<IProcessor> createInstance(const PluginInstanceData& data, QString* error = nullptr);
    // Build a fresh project-level instance record from a database entry
    static PluginInstanceData makeInstanceData(const PluginInfo& info, Id id);

private:
    PluginDatabase& m_db;
};

}   // namespace mt
