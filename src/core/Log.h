#pragma once
#include <QDebug>
#include <QString>
#include <functional>
namespace mt {
// Central log sink; UI can subscribe to show messages in a status/log panel.
struct Log {
    enum Level { Info, Warn, Error };
    static void write(Level lvl, const QString& msg) {
        if (lvl == Error) qWarning().noquote() << "[ERROR]" << msg; else if (lvl == Warn) qWarning().noquote() << "[WARN]" << msg; else qInfo().noquote() << "[INFO]" << msg;
        if (sink()) sink()(lvl, msg);
    }
    static std::function<void(Level, const QString&)>& sink() { static std::function<void(Level, const QString&)> s; return s; }
    static void info(const QString& m) { write(Info, m); }
    static void warn(const QString& m) { write(Warn, m); }
    static void error(const QString& m) { write(Error, m); }
};
}
