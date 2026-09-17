#include "ui/ControlServer.h"
#include "ui/App.h"
#include "ui/Theme.h"
#include "ui/MainWindow.h"
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QDir>
#include <QApplication>

namespace mt {

QString ControlServer::socketPath() { QString d = qEnvironmentVariable("XDG_RUNTIME_DIR"); if (d.isEmpty()) d = QDir::tempPath(); return d + "/musicthingy.sock"; }

ControlServer::ControlServer(App& app, MainWindow& win, QObject* parent) : QObject(parent), m_app(app), m_win(win) {
    // don't steal the socket from another running instance (e.g. an offscreen test run next to the real app)
    QString path = socketPath(); { QLocalSocket probe; probe.connectToServer(path); if (probe.waitForConnected(200)) { probe.disconnectFromServer(); path = path.left(path.size() - 5) + "-" + QString::number(QCoreApplication::applicationPid()) + ".sock"; } else QLocalServer::removeServer(path); }
    m_server.setSocketOptions(QLocalServer::UserAccessOption); if (!m_server.listen(path)) return;
    connect(&m_server, &QLocalServer::newConnection, this, [this] { while (auto* s = m_server.nextPendingConnection()) { connect(s, &QLocalSocket::readyRead, this, [this, s] { while (s->canReadLine()) { QString reply = handle(QString::fromUtf8(s->readLine()).trimmed()); s->write((reply + "\n").toUtf8()); s->flush(); } }); connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater); } });
}

QString ControlServer::handle(const QString& line) {
    QStringList a = line.split(' ', Qt::SkipEmptyParts); if (a.isEmpty()) return "err empty"; QString c = a[0].toLower();
    if (c == "ping") return "pong";
    if (c == "play") { m_app.play(); return "ok"; }
    if (c == "stop") { m_app.stop(); return "ok"; }
    if (c == "note" && a.size() >= 2) { int p = a[1].toInt(); int ms = a.size() > 2 ? a[2].toInt() : 300; m_app.noteOn(p, 0.9f); QTimer::singleShot(ms, &m_app, [this, p] { m_app.noteOff(p); }); return "ok"; }
    if (c == "select" && a.size() >= 2) { int idx = a[1].toInt(); auto& ch = m_app.project().channels; if (idx < 0 || idx >= (int)ch.size()) return "err no such channel"; m_app.selectChannel(ch[idx].id); return "ok"; }
    if (c == "channels") { QStringList l; for (auto& ch : m_app.project().channels) l << ch.name; return l.join(" | "); }
    if (c == "stats") return QString("device=%1 playing=%2 xruns=%3 sr=%4 cpu=%5").arg(m_app.engine().deviceRunning()).arg(m_app.engine().isPlaying()).arg(m_app.engine().xruns()).arg(m_app.engine().sampleRate()).arg(m_app.engine().cpuLoad(), 0, 'f', 3);
    if (c == "screenshot" && a.size() >= 2) { return m_win.saveScreenshot(a[1]) ? "ok" : "err"; }
    if (c == "editor") { if (m_app.project().channels.empty()) return "err no channels"; m_win.openEditor(m_app.project().channels.back().id); return "ok"; }
    if (c == "action" && a.size() >= 2) { if (auto* act = m_app.commands().action(a[1])) { act->trigger(); return "ok"; } return "err no such action"; }
    if (c == "params") { if (m_app.project().channels.empty()) return "err"; auto* pr = m_app.processor(m_app.project().channels.back().id); if (!pr) return "err no processor"; QStringList l; auto& ps = pr->params(); int n = std::min(ps.count(), a.size() > 1 ? a[1].toInt() : 40); for (int i = 0; i < n; ++i) l << QString("%1:%2=%3").arg(i).arg(QString::fromStdString(ps.desc(i).name)).arg(ps.getNormalized(ps.desc(i).id), 0, 'f', 3); return l.join(" | "); }
    if (c == "param" && a.size() >= 3) { if (m_app.project().channels.empty()) return "err"; Id owner = m_app.project().channels.back().id; auto* pr = m_app.processor(owner); if (!pr) return "err no processor"; auto& ps = pr->params(); int idx = -1; for (int i = 0; i < ps.count(); ++i) if (QString::fromStdString(ps.desc(i).name).compare(a[1], Qt::CaseInsensitive) == 0 || QString::number(i) == a[1]) { idx = i; break; } if (idx < 0) return "err no such param"; m_app.setProcessorParam(owner, ps.desc(idx).id, a[2].toFloat(), true); return "ok"; }
    if (c == "theme" && a.size() >= 2) { if (const theme::Palette* t = theme::preset(a[1])) { theme::applyTheme(*t); return "ok " + t->name; } return "err no such theme"; }
    if (c == "quit") { QTimer::singleShot(0, qApp, [] { qApp->quit(); }); return "ok"; }
    return "err unknown command (ping play stop note select channels stats screenshot editor action theme quit)";
}

} // namespace mt
