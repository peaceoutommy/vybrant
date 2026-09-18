#include "ui/ControlServer.h"
#include "ui/App.h"
#include "ui/Theme.h"
#include "ui/MainWindow.h"
#include "ui/MixerPanel.h"
#include "ui/PianoRollPanel.h"
#include "ui/ChannelRackPanel.h"
#include "audio/RenderEngine.h"
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QDir>
#include <QApplication>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QUrl>
#include <QMimeData>

namespace mt {

QString ControlServer::socketPath() { QString env = qEnvironmentVariable("MT_CONTROL_SOCKET"); if (!env.isEmpty()) return env; /* tests: a private socket so they never talk to the user's instance */ QString d = qEnvironmentVariable("XDG_RUNTIME_DIR"); if (d.isEmpty()) d = QDir::tempPath(); return d + "/musicthingy.sock"; }

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
    if (c == "editor") { auto& ch = m_app.project().channels; if (ch.empty()) return "err no channels"; int idx = a.size() > 1 ? a[1].toInt() : (int)ch.size() - 1; if (idx < 0 || idx >= (int)ch.size()) return "err index"; m_win.openEditor(ch[idx].id); return "ok"; }
    if (c == "sample" && a.size() >= 3) { auto& ch = m_app.project().channels; int idx = a[1].toInt(); if (idx < 0 || idx >= (int)ch.size()) return "err index"; m_app.setChannelSample(ch[idx].id, a[2]); return "ok"; }
    if (c == "action" && a.size() >= 2) { if (auto* act = m_app.commands().action(a[1])) { act->trigger(); return "ok"; } return "err no such action"; }
    if (c == "params") { if (m_app.project().channels.empty()) return "err"; auto* pr = m_app.processor(m_app.project().channels.back().id); if (!pr) return "err no processor"; QStringList l; auto& ps = pr->params(); int n = std::min(ps.count(), a.size() > 1 ? a[1].toInt() : 40); for (int i = 0; i < n; ++i) l << QString("%1:%2=%3").arg(i).arg(QString::fromStdString(ps.desc(i).name)).arg(ps.getNormalized(ps.desc(i).id), 0, 'f', 3); return l.join(" | "); }
    if (c == "param" && a.size() >= 3) { if (m_app.project().channels.empty()) return "err"; Id owner = m_app.project().channels.back().id; auto* pr = m_app.processor(owner); if (!pr) return "err no processor"; auto& ps = pr->params(); int idx = -1; for (int i = 0; i < ps.count(); ++i) if (QString::fromStdString(ps.desc(i).name).compare(a[1], Qt::CaseInsensitive) == 0 || QString::number(i) == a[1]) { idx = i; break; } if (idx < 0) return "err no such param"; m_app.setProcessorParam(owner, ps.desc(idx).id, a[2].toFloat(), true); return "ok"; }
    if (c == "theme" && a.size() >= 2) { if (const theme::Palette* t = theme::preset(a[1])) { theme::applyTheme(*t); return "ok " + t->name; } return "err no such theme"; }
    if (c == "route" && a.size() >= 3) { auto& tr = m_app.project().mixerTracks; int i = a[1].toInt(), j = a[2].toInt(); if (i < 0 || j < 0 || i >= (int)tr.size() || j >= (int)tr.size()) return "err index"; m_app.routeTrack(tr[i].id, tr[j].id); return "ok"; }
    if (c == "send" && a.size() >= 3) { auto& tr = m_app.project().mixerTracks; int i = a[1].toInt(), j = a[2].toInt(); if (i < 0 || j < 0 || i >= (int)tr.size() || j >= (int)tr.size()) return "err index"; m_app.addSend(tr[i].id, tr[j].id, a.size() > 3 && a[3] == "sc"); return "ok"; }
    if (c == "mixsel" && a.size() >= 2) { auto& tr = m_app.project().mixerTracks; int i = a[1].toInt(); if (i < 0 || i >= (int)tr.size()) return "err index"; m_win.mixer()->selectTrack(tr[i].id); return "ok"; }
    if (c == "scale" && a.size() >= 3) { m_win.pianoRoll()->view()->setScale(a[1].toInt(), a[2].toInt()); return "ok"; }
    if (c == "notes" && a.size() >= 5) { auto& ch = m_app.project().channels; int idx = a[1].toInt(); if (idx < 0 || idx >= (int)ch.size()) return "err index"; Id cid = ch[idx].id; int pitch = a[2].toInt(), count = a[3].toInt(); Tick len = a[4].toLongLong(); Id pid = m_app.project().selectedPatternId; m_app.editPattern(pid, "test notes", [cid, pitch, count, len](Pattern& pat) { pat.ensureChannel(cid); auto* d = pat.channelData(cid); for (int i = 0; i < count; ++i) { Note n; n.id = 80000 + i; n.pitch = pitch; n.start = (Tick)i * len; n.duration = len; n.velocity = 0.9f; d->notes.push_back(n); } }); if (!m_app.project().playlistTracks.empty()) m_app.addPatternClip(m_app.project().playlistTracks.front().id, pid, 0); return "ok"; }
    if (c == "render" && a.size() >= 2) { RenderOptions o; o.path = a[1]; o.format = "wav"; o.songMode = true; o.tailSeconds = 0.5; QString err; bool ok = m_app.exportAudio(o, nullptr, &err); return ok ? "ok" : "err " + err; }
    if (c == "preview" && a.size() >= 2) { m_app.previewFile(a[1]); return "ok"; }
    if (c == "previewstate") { auto b = m_app.previewBuffer(); if (!b) return "none"; return QString("failed=%1 done=%2 available=%3 frames=%4 playing=%5 firstAudio=%6ms").arg(b->failed.load()).arg(b->done.load()).arg(b->available.load()).arg(b->frames).arg(m_app.engine().previewPlaying()).arg(b->firstAudioUs.load() < 0 ? -1.0 : (b->firstAudioUs.load() - b->createdUs) / 1000.0, 0, 'f', 1); }
    if (c == "stoppreview") { m_app.stopPreview(); return "ok"; }
    if (c == "droptest" && a.size() >= 2) { // simulate a browser drop of a file on the rack (view or panel)
        auto* md = new QMimeData(); md->setUrls({QUrl::fromLocalFile(a[1])}); md->setData("application/x-musicthingy-item", ("audio|" + a[1]).toUtf8()); QWidget* target = a.size() > 2 && a[2] == "panel" ? (QWidget*)m_win.rackPanel() : (QWidget*)m_win.rackPanel()->view(); QPointF pos(target->width() / 2.0, target->height() - 5.0);
        QDragEnterEvent en(pos.toPoint(), Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(target, &en); QDragMoveEvent mv(pos.toPoint(), Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(target, &mv); QDropEvent dr(pos, Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(target, &dr); delete md; return QString("enter=%1 move=%2 drop=%3 channels=%4").arg(en.isAccepted()).arg(mv.isAccepted()).arg(dr.isAccepted()).arg(m_app.project().channels.size()); }
    if (c == "leftsplit" && a.size() >= 2) { m_win.debugLeftDock(a[1].toInt()); return "ok"; }
    if (c == "leftsizes") return m_win.debugLeftSizes();
    if (c == "winsize" && a.size() >= 3) { m_win.resize(a[1].toInt(), a[2].toInt()); return "ok"; }
    if (c == "quit") { QTimer::singleShot(0, qApp, [] { qApp->quit(); }); return "ok"; }
    return "err unknown command (ping play stop note select channels stats screenshot editor action theme quit)";
}

} // namespace mt
