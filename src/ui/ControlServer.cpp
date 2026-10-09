#include <QNativeGestureEvent>
#include "ui/ControlServer.h"
#include "ui/App.h"
#include "ui/Theme.h"
#include "ui/MainWindow.h"
#include "ui/MixerPanel.h"
#include "ui/PianoRollPanel.h"
#include "ui/PlaylistPanel.h"
#include "ui/PluginWindow.h"
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
#include <QMouseEvent>
#include <QWheelEvent>

namespace mt {

QString ControlServer::socketPath() {
    QString env = qEnvironmentVariable("MT_CONTROL_SOCKET");
    if (!env.isEmpty()) return env; /* tests: a private socket so they never talk to the user's instance */
    QString d = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (d.isEmpty()) d = QDir::tempPath();
    return d + "/musicthingy.sock";
}

ControlServer::ControlServer(App& app, MainWindow& win, QObject* parent)
    : QObject(parent)
    , m_app(app)
    , m_win(win) {
    // don't steal the socket from another running instance (e.g. an offscreen test run next to the real app)
    QString path = socketPath();
    {
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(200)) {
            probe.disconnectFromServer();
            path = path.left(path.size() - 5) + "-" + QString::number(QCoreApplication::applicationPid()) + ".sock";
        } else QLocalServer::removeServer(path);
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(path)) return;
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto* s = m_server.nextPendingConnection()) {
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                while (s->canReadLine()) {
                    QString reply = handle(QString::fromUtf8(s->readLine()).trimmed());
                    s->write((reply + "\n").toUtf8());
                    s->flush();
                }
            });
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });
}

QString ControlServer::handle(const QString& line) {
    QStringList a = line.split(' ', Qt::SkipEmptyParts);
    if (a.isEmpty()) return "err empty";
    QString c = a[0].toLower();
    if (c == "ping") return "pong";
    if (c == "play") {
        m_app.play();
        return "ok";
    }
    if (c == "stop") {
        m_app.stop();
        return "ok";
    }
    if (c == "note" && a.size() >= 2) {
        int p = a[1].toInt();
        int ms = a.size() > 2 ? a[2].toInt() : 300;
        m_app.noteOn(p, 0.9f);
        QTimer::singleShot(ms, &m_app, [this, p] { m_app.noteOff(p); });
        return "ok";
    }
    if (c == "select" && a.size() >= 2) {
        int idx = a[1].toInt();
        auto& ch = m_app.project().channels;
        if (idx < 0 || idx >= (int)ch.size()) return "err no such channel";
        m_app.selectChannel(ch[idx].id);
        return "ok";
    }
    if (c == "channels") {
        QStringList l;
        for (auto& ch : m_app.project().channels) l << ch.name;
        return l.join(" | ");
    }
    if (c == "stats")
        return QString("device=%1 playing=%2 xruns=%3 sr=%4 cpu=%5")
            .arg(m_app.engine().deviceRunning())
            .arg(m_app.engine().isPlaying())
            .arg(m_app.engine().xruns())
            .arg(m_app.engine().sampleRate())
            .arg(m_app.engine().cpuLoad(), 0, 'f', 3);
    if (c == "screenshot" && a.size() >= 2) { return m_win.saveScreenshot(a[1]) ? "ok" : "err"; }
    if (c == "editor") {
        auto& ch = m_app.project().channels;
        if (ch.empty()) return "err no channels";
        int idx = a.size() > 1 ? a[1].toInt() : (int)ch.size() - 1;
        if (idx < 0 || idx >= (int)ch.size()) return "err index";
        m_win.openEditor(ch[idx].id);
        return "ok";
    }
    if (c == "sample" && a.size() >= 3) {
        auto& ch = m_app.project().channels;
        int idx = a[1].toInt();
        if (idx < 0 || idx >= (int)ch.size()) return "err index";
        m_app.setChannelSample(ch[idx].id, a[2]);
        return "ok";
    }
    if (c == "action" && a.size() >= 2) {
        if (auto* act = m_app.commands().action(a[1])) {
            act->trigger();
            return "ok";
        }
        return "err no such action";
    }
    if (c == "params") {
        if (m_app.project().channels.empty()) return "err";
        auto* pr = m_app.processor(m_app.project().channels.back().id);
        if (!pr) return "err no processor";
        QStringList l;
        auto& ps = pr->params();
        int n = std::min(ps.count(), a.size() > 1 ? a[1].toInt() : 40);
        for (int i = 0; i < n; ++i)
            l << QString("%1:%2=%3").arg(i).arg(QString::fromStdString(ps.desc(i).name)).arg(ps.getNormalized(ps.desc(i).id), 0, 'f', 3);
        return l.join(" | ");
    }
    if (c == "param" && a.size() >= 3) {
        if (m_app.project().channels.empty()) return "err";
        Id owner = m_app.project().channels.back().id;
        auto* pr = m_app.processor(owner);
        if (!pr) return "err no processor";
        auto& ps = pr->params();
        int idx = -1;
        for (int i = 0; i < ps.count(); ++i)
            if (QString::fromStdString(ps.desc(i).name).compare(a[1], Qt::CaseInsensitive) == 0 || QString::number(i) == a[1]) {
                idx = i;
                break;
            }
        if (idx < 0) return "err no such param";
        m_app.setProcessorParam(owner, ps.desc(idx).id, a[2].toFloat(), true);
        return "ok";
    }
    if (c == "theme" && a.size() >= 2) {
        if (const theme::Palette* t = theme::preset(a[1])) {
            theme::applyTheme(*t);
            return "ok " + t->name;
        }
        return "err no such theme";
    }
    if (c == "route" && a.size() >= 3) {
        auto& tr = m_app.project().mixerTracks;
        int i = a[1].toInt(), j = a[2].toInt();
        if (i < 0 || j < 0 || i >= (int)tr.size() || j >= (int)tr.size()) return "err index";
        m_app.routeTrack(tr[i].id, tr[j].id);
        return "ok";
    }
    if (c == "send" && a.size() >= 3) {
        auto& tr = m_app.project().mixerTracks;
        int i = a[1].toInt(), j = a[2].toInt();
        if (i < 0 || j < 0 || i >= (int)tr.size() || j >= (int)tr.size()) return "err index";
        m_app.addSend(tr[i].id, tr[j].id, a.size() > 3 && a[3] == "sc");
        return "ok";
    }
    if (c == "mixsel" && a.size() >= 2) {
        auto& tr = m_app.project().mixerTracks;
        int i = a[1].toInt();
        if (i < 0 || i >= (int)tr.size()) return "err index";
        m_win.mixer()->selectTrack(tr[i].id);
        return "ok";
    }
    if (c == "scale" && a.size() >= 3) {
        m_win.pianoRoll()->view()->setScale(a[1].toInt(), a[2].toInt());
        return "ok";
    }
    if (c == "notes" && a.size() >= 5) {
        auto& ch = m_app.project().channels;
        int idx = a[1].toInt();
        if (idx < 0 || idx >= (int)ch.size()) return "err index";
        Id cid = ch[idx].id;
        int pitch = a[2].toInt(), count = a[3].toInt();
        Tick len = a[4].toLongLong();
        Id pid = m_app.project().selectedPatternId;
        m_app.editPattern(pid, "test notes", [cid, pitch, count, len](Pattern& pat) {
            pat.ensureChannel(cid);
            auto* d = pat.channelData(cid);
            for (int i = 0; i < count; ++i) {
                Note n;
                n.id = 80000 + i;
                n.pitch = pitch;
                n.start = (Tick)i * len;
                n.duration = len;
                n.velocity = 0.9f;
                d->notes.push_back(n);
            }
        });
        if (!m_app.project().playlistTracks.empty()) m_app.addPatternClip(m_app.project().playlistTracks.front().id, pid, 0);
        return "ok";
    }
    if (c == "render" && a.size() >= 2) {
        RenderOptions o;
        o.path = a[1];
        o.format = "wav";
        o.songMode = true;
        o.tailSeconds = 0.5;
        QString err;
        bool ok = m_app.exportAudio(o, nullptr, &err);
        return ok ? "ok" : "err " + err;
    }
    if (c == "preview" && a.size() >= 2) {
        m_app.previewFile(a[1]);
        return "ok";
    }
    if (c == "previewstate") {
        auto b = m_app.previewBuffer();
        if (!b) return "none";
        return QString("failed=%1 done=%2 available=%3 frames=%4 playing=%5 firstAudio=%6ms")
            .arg(b->failed.load())
            .arg(b->done.load())
            .arg(b->available.load())
            .arg(b->frames)
            .arg(m_app.engine().previewPlaying())
            .arg(b->firstAudioUs.load() < 0 ? -1.0 : (b->firstAudioUs.load() - b->createdUs) / 1000.0, 0, 'f', 1);
    }
    if (c == "stoppreview") {
        m_app.stopPreview();
        return "ok";
    }
    if (c == "droptest" && a.size() >= 2) {   // simulate a browser drop of a file on the rack (view or panel)
        auto* md = new QMimeData();
        md->setUrls({QUrl::fromLocalFile(a[1])});
        md->setData("application/x-musicthingy-item", ("audio|" + a[1]).toUtf8());
        QWidget* target = a.size() > 2 && a[2] == "panel" ? (QWidget*)m_win.rackPanel() : (QWidget*)m_win.rackPanel()->view();
        QPointF pos(target->width() / 2.0, target->height() - 5.0);
        QDragEnterEvent en(pos.toPoint(), Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &en);
        QDragMoveEvent mv(pos.toPoint(), Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &mv);
        QDropEvent dr(pos, Qt::CopyAction, md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &dr);
        delete md;
        return QString("enter=%1 move=%2 drop=%3 channels=%4")
            .arg(en.isAccepted())
            .arg(mv.isAccepted())
            .arg(dr.isAccepted())
            .arg(m_app.project().channels.size());
    }
    if (c == "prclick" && a.size() >= 3) {   // click a piano-roll cell: prclick <tick> <pitch> [right]
        auto* v = m_win.pianoRoll()->view();
        QPoint pt = v->debugPoint(a[1].toLongLong(), a[2].toInt());
        Qt::MouseButton b = a.size() > 3 && a[3] == "right" ? Qt::RightButton : Qt::LeftButton;
        QMouseEvent pr(QEvent::MouseButtonPress, pt, v->mapToGlobal(pt), b, b, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &pr);
        QMouseEvent rl(QEvent::MouseButtonRelease, pt, v->mapToGlobal(pt), b, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &rl);
        return QString("ok %1,%2").arg(pt.x()).arg(pt.y());
    }
    if (c == "prdrag" && a.size() >= 5) {   // drag in the piano roll: prdrag <tick> <pitch> <toTick> <toPitch> [xoffset of the press]
        auto* v = m_win.pianoRoll()->view();
        QPoint p1 = v->debugPoint(a[1].toLongLong(), a[2].toInt()) + QPoint(a.size() > 5 ? a[5].toInt() : 0, 0),
               p2 = v->debugPoint(a[3].toLongLong(), a[4].toInt());
        QMouseEvent pr(QEvent::MouseButtonPress, p1, v->mapToGlobal(p1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &pr);
        for (int i = 1; i <= 4; ++i) {
            QPoint m = p1 + (p2 - p1) * i / 4;
            QMouseEvent mv(QEvent::MouseMove, m, v->mapToGlobal(m), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(v, &mv);
        }
        QMouseEvent rl(QEvent::MouseButtonRelease, p2, v->mapToGlobal(p2), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &rl);
        return "ok";
    }
    if (c == "prwheel" && a.size() >= 2) {   // vertical wheel over the piano-roll grid: prwheel <ticks> [alt]
        auto* v = m_win.pianoRoll()->view();
        int n = a[1].toInt();
        QPointF pt(v->width() / 2.0, v->height() / 2.0);
        Qt::KeyboardModifiers mods = a.size() > 2 && a[2] == "alt" ? Qt::AltModifier : Qt::NoModifier;
        for (int i = 0; i < std::abs(n); ++i) {
            QWheelEvent w(pt, v->mapToGlobal(pt), QPoint(), QPoint(0, n > 0 ? 120 : -120), Qt::NoButton, mods, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(v, &w);
        }
        return v->debugScroll();
    }
    if (c == "prmouse" && a.size() >= 5) {   // raw left drag in the piano roll: prmouse <x> <y> <toX> <toY> [mods: c s a]
        auto* v = m_win.pianoRoll()->view();
        QPoint p1(a[1].toInt(), a[2].toInt()), p2(a[3].toInt(), a[4].toInt());
        QString m = a.size() > 5 ? a[5] : QString();
        Qt::KeyboardModifiers mods = (m.contains('c') ? Qt::ControlModifier : Qt::NoModifier) |
            (m.contains('s') ? Qt::ShiftModifier : Qt::NoModifier) | (m.contains('a') ? Qt::AltModifier : Qt::NoModifier);
        QMouseEvent pr(QEvent::MouseButtonPress, p1, v->mapToGlobal(p1), Qt::LeftButton, Qt::LeftButton, mods);
        QCoreApplication::sendEvent(v, &pr);
        for (int i = 1; i <= 4; ++i) {
            QPoint q = p1 + (p2 - p1) * i / 4;
            QMouseEvent mv(QEvent::MouseMove, q, v->mapToGlobal(q), Qt::LeftButton, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(v, &mv);
        }
        QMouseEvent rl(QEvent::MouseButtonRelease, p2, v->mapToGlobal(p2), Qt::LeftButton, Qt::NoButton, mods);
        QCoreApplication::sendEvent(v, &rl);
        return v->debugScroll();
    }
    if (c == "prpinch" && a.size() >= 2) {   // trackpad pinch over the piano roll: prpinch <value>
        auto* v = m_win.pianoRoll()->view();
        QPointF pt(v->width() / 2.0, v->height() / 2.0);
        QNativeGestureEvent g(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, pt, pt, v->mapToGlobal(pt), a[1].toDouble(), QPointF());
        QCoreApplication::sendEvent(v, &g);
        return v->debugScroll();
    }
    if (c == "prinfo") return m_win.pianoRoll()->view()->debugScroll();
    if (c == "rackclick" && a.size() >= 3) {   // click a rack step cell: rackclick <row> <step> [right]
        auto* v = m_win.rackPanel()->view();
        QPoint pt = v->debugStepPoint(a[1].toInt(), a[2].toInt());
        Qt::MouseButton b = a.size() > 3 && a[3] == "right" ? Qt::RightButton : Qt::LeftButton;
        QMouseEvent pr(QEvent::MouseButtonPress, pt, v->mapToGlobal(pt), b, b, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &pr);
        QMouseEvent rl(QEvent::MouseButtonRelease, pt, v->mapToGlobal(pt), b, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &rl);
        return "ok";
    }
    if (c == "patdata") {   // dump the selected pattern's per-channel steps / notes
        Pattern* p = m_app.project().pattern(m_app.project().selectedPatternId);
        if (!p) return "err no pattern";
        QString out = QString("steps=%1").arg(p->stepCount);
        for (auto& d : p->channels) {
            QString st;
            for (size_t i = 0; i < d.steps.size(); ++i) st += d.steps[i].enabled ? 'x' : '.';
            out += QString(" | ch%1 steps=%2 notes=").arg(d.id).arg(st);
            for (auto& n : d.notes) out += QString("(%1@%2+%3)").arg(n.pitch).arg(n.start).arg(n.duration);
            QStringList vel;
            for (auto& n : d.notes) vel << QString::number(qRound(n.velocity * 100));
            if (!vel.isEmpty()) out += " vel=" + vel.join(',');
        }
        return out;
    }
    if (c == "leftsplit" && a.size() >= 2) {
        m_win.debugLeftDock(a[1].toInt());
        return "ok";
    }
    if (c == "leftsizes") return m_win.debugLeftSizes();
    if (c == "dock" && a.size() >= 2) {
        m_win.debugDock(a[1] == "top" ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea);
        return "ok";
    }
    if (c == "layout") return m_win.debugLayout();
    if (c == "winsize" && a.size() >= 3) {
        m_win.resize(a[1].toInt(), a[2].toInt());
        return "ok";
    }
    if (c == "split") {
        auto ids = m_app.splitPatternByChannel(m_app.project().selectedPatternId);
        QStringList l;
        for (auto& p : m_app.project().patterns) l << p.name;
        QStringList cl;
        for (auto& cc : m_app.project().clips)
            cl << QString("%1@t%2").arg(cc.name).arg(m_app.project().mixerIndex(0) * 0 + [&] {
                int i = 0;
                for (size_t k = 0; k < m_app.project().playlistTracks.size(); ++k)
                    if (m_app.project().playlistTracks[k].id == cc.trackId) i = (int)k + 1;
                return i;
            }());
        return QString("new=%1 patterns: %2 | clips: %3").arg(ids.size()).arg(l.join(", ")).arg(cl.join(", "));
    }
    if (c == "audioclip" && a.size() >= 2) {
        Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id;
        Id id = m_app.addAudioClip(track, a[1], 0);
        return id ? "ok" : "err";
    }
    if (c == "clipfade" && a.size() >= 5) {
        auto& cl = m_app.project().clips;
        auto it = std::find_if(cl.begin(), cl.end(), [](const Clip& x) { return x.type == ClipType::Audio; });
        if (it == cl.end()) return "err no audio clip";
        Tick fi = a[1].toLongLong(), fo = a[2].toLongLong();
        float sh = a[3].toFloat(), g = a[4].toFloat();
        m_app.editClip(it->id, "test fade", [fi, fo, sh, g](Clip& x) {
            x.fadeIn = fi;
            x.fadeOut = fo;
            x.fadeInShape = sh;
            x.fadeOutShape = -sh;
            x.gain = g;
        });
        return "ok";
    }
    if (c == "pldrag" && a.size() >= 5) {   // left drag in the playlist: pldrag <track> <tick> <toTrack> <toTick> [xoffset of the press]
        auto* v = m_win.playlist()->view();
        QPoint p1 = v->debugPoint(a[1].toInt(), a[2].toLongLong()) + QPoint(a.size() > 5 ? a[5].toInt() : 0, 0), p2 = v->debugPoint(a[3].toInt(), a[4].toLongLong());
        QMouseEvent pr(QEvent::MouseButtonPress, p1, v->mapToGlobal(p1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &pr);
        QMouseEvent mv(QEvent::MouseMove, p2, v->mapToGlobal(p2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &mv);
        QMouseEvent rl(QEvent::MouseButtonRelease, p2, v->mapToGlobal(p2), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &rl);
        return "ok";
    }
    if (c == "plpinch" && a.size() >= 2) {   // trackpad pinch over the playlist: plpinch <value> -> "<ppt before> <ppt after>"
        auto* v = m_win.playlist()->view();
        double before = v->debugPpt();
        QPointF pt(v->width() / 2.0, v->height() / 2.0);
        QNativeGestureEvent g(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, pt, pt, v->mapToGlobal(pt), a[1].toDouble(), QPointF());
        QCoreApplication::sendEvent(v, &g);
        return QString("%1 %2").arg(before).arg(v->debugPpt());
    }
    if (c == "clips") {   // clips as track:start+length(loop) in project order
        QStringList out;
        auto& pr = m_app.project();
        for (auto& cl : pr.clips) {
            int ti = 0;
            while (ti < (int)pr.playlistTracks.size() && pr.playlistTracks[ti].id != cl.trackId) ++ti;
            out << QString("%1:%2+%3%4").arg(ti).arg(cl.start).arg(pr.clipLength(cl)).arg(cl.loopSource ? "L" : "");
        }
        return out.join(' ');
    }
    if (c == "clipeditor" && a.size() >= 2) {   // open the editor of the first audio clip and save a screenshot of it
        for (auto& cl : m_app.project().clips)
            if (cl.type == ClipType::Audio) {
                auto* e = new AudioClipEditor(m_app, cl.id, &m_win);
                e->show();
                QCoreApplication::processEvents();
                return e->grab().save(a[1]) ? "ok" : "err save";
            }
        return "err no audio clip";
    }
    if (c == "quit") {
        QTimer::singleShot(0, qApp, [] { qApp->quit(); });
        return "ok";
    }
    return "err unknown command (ping play stop note select channels stats screenshot editor action theme quit)";
}

}   // namespace mt
