// Built-in self test (`musicthingy --selftest`): exercises the project model, serializer, undo, DSP and engine.
#include "ui/App.h"
#include "project/ProjectSerializer.h"
#include "dsp/NativeRegistry.h"
#include "dsp/effects/Effects.h"
#include "dsp/instruments/Instruments.h"
#include "audio/RenderEngine.h"
#include "ui/widgets/Controls.h"
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

namespace mt {

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                                                   \
    do {                                                                                                                                   \
        if (cond) std::printf("  ok   %s\n", msg);                                                                                         \
        else {                                                                                                                             \
            std::printf("  FAIL %s\n", msg);                                                                                               \
            ++g_fail;                                                                                                                      \
        }                                                                                                                                  \
    } while (0)

int runSelfTest(App& app) {
    std::printf("MusicThingy self test\n");
    // ---- project round trip
    Project p = makeDefaultProject(true);
    QJsonObject j1 = ProjectSerializer::toJson(p);
    Project p2;
    QString err;
    bool ok = ProjectSerializer::fromJson(j1, p2, &err);
    QJsonObject j2 = ProjectSerializer::toJson(p2);
    CHECK(ok && QJsonDocument(j1).toJson() == QJsonDocument(j2).toJson(), "serializer round trip is lossless");
    QTemporaryDir tmp;
    QString path = tmp.path() + "/t.mtp";
    CHECK(ProjectSerializer::save(p, path, &err), "save project file");
    Project p3;
    CHECK(
        ProjectSerializer::load(path, p3, &err) && p3.channels.size() == p.channels.size() && p3.patterns.size() == 1, "load project file");
    QJsonObject old = j1;
    old["formatVersion"] = 0;
    Project p4;
    CHECK(!ProjectSerializer::fromJson(old, p4, &err), "rejects invalid format version");
    // ---- undo / redo through the model
    auto& model = app.model();
    size_t nChan = app.project().channels.size();
    Id cid = app.addChannel(ChannelType::Synth, "native.synth", "UndoTest");
    CHECK(app.project().channels.size() == nChan + 1 && app.project().channel(cid), "add channel command");
    model.undoStack().undo();
    model.undoStack().undo();
    CHECK(app.project().channels.size() == nChan, "undo restores channel list");
    model.undoStack().redo();
    model.undoStack().redo();
    CHECK(app.project().channels.size() == nChan + 1, "redo re-applies");
    app.setChannelVolume(cid, 0.3f, true);
    app.setChannelVolume(cid, 0.4f, true);
    CHECK(std::fabs(app.project().channel(cid)->volume - 0.4f) < 1e-6f, "volume command applied");
    size_t before = model.undoStack().count();
    model.undoStack().undo();
    CHECK(std::fabs(app.project().channel(cid)->volume - 0.8f) < 1e-6f && model.undoStack().count() == before - 1,
        "merged knob edits undo in one step");
    // ---- routing cycle detection
    Id master = app.project().masterId();
    Id a = app.project().mixerTracks[1].id, b = app.project().mixerTracks[2].id;
    CHECK(app.routeTrack(a, b), "route insert 1 -> insert 2");
    CHECK(!app.project().routingWouldCycle(a, master), "routing to master is acyclic");
    CHECK(app.project().routingWouldCycle(b, a), "cycle detected (2 -> 1 while 1 -> 2)");
    app.routeTrack(a, master);
    // ---- automation evaluation
    AutomationLane lane;
    lane.points = {{0, 0.f, CurveType::Linear, 0}, {PPQ, 1.f, CurveType::Stepped, 0}, {2 * PPQ, 0.f, CurveType::Linear, 0}};
    CHECK(std::fabs(lane.valueAt(PPQ / 2) - 0.5f) < 1e-5f, "linear automation interpolation");
    CHECK(std::fabs(lane.valueAt(PPQ + PPQ / 2) - 1.f) < 1e-5f, "stepped automation holds value");
    // ---- every native effect processes finite output; every instrument makes sound
    int fxOk = 0, fxTotal = 0, instOk = 0, instTotal = 0;
    std::vector<float> L(512), R(512);
    dsp::Rng rng;
    MidiEventList midi;
    ProcessContext ctx;
    ctx.sampleRate = 44100;
    ctx.blockSize = 512;
    ctx.bpm = 120;
    for (auto& info : NativeRegistry::instance().all()) {
        auto proc = info.factory();
        proc->prepare(44100, 512);
        if (info.kind == ProcessorKind::Effect) {
            ++fxTotal;
            bool finite = true;
            for (int blk = 0; blk < 20; ++blk) {
                for (int i = 0; i < 512; ++i) {
                    L[i] = rng.bipolar() * 0.5f;
                    R[i] = L[i];
                }
                proc->process(L.data(), R.data(), 512, midi, ctx);
                for (int i = 0; i < 512; ++i)
                    if (!std::isfinite(L[i]) || !std::isfinite(R[i]) || std::fabs(L[i]) > 100.f) finite = false;
            }
            if (finite) ++fxOk;
            else std::printf("  FAIL effect %s produced non-finite output\n", info.name.toUtf8().constData());
        } else if (info.key != "native.midiout") {
            ++instTotal;
            if (auto* ds = dynamic_cast<DrumSampler*>(proc.get())) ds->setBuiltin("Kick");
            if (auto* s = dynamic_cast<Sampler*>(proc.get())) s->setSample(DrumSampler::synthesizeDrum("Snare", 44100), "test");
            midi.clear();
            MidiEvent e;
            e.type = MidiEvent::NoteOn;
            e.data1 = 60;
            e.velocity = 1.f;
            e.data2 = 127;
            midi.add(e);
            float energy = 0;
            for (int blk = 0; blk < 8; ++blk) {
                std::fill(L.begin(), L.end(), 0.f);
                std::fill(R.begin(), R.end(), 0.f);
                proc->process(L.data(), R.data(), 512, midi, ctx);
                midi.clear();
                for (int i = 0; i < 512; ++i) energy += L[i] * L[i];
            }
            if (energy > 1e-4f) ++instOk;
            else std::printf("  FAIL instrument %s is silent\n", info.name.toUtf8().constData());
        }
    }
    CHECK(fxOk == fxTotal, QString("all %1 native effects process cleanly").arg(fxTotal).toUtf8().constData());
    CHECK(instOk == instTotal, QString("all %1 native instruments produce audio on note-on").arg(instTotal).toUtf8().constData());
    // ---- plugin delay compensation: limiter with lookahead on one insert
    {
        Id t1 = app.project().mixerTracks[1].id;
        Id slot = app.addEffect(t1, "native.limiter");
        app.engine().rebuild(app.project());
        int lat = app.processor(slot) ? app.processor(slot)->latencySamples() : 0;
        auto& tc2 = app.engine().registry().track(app.project().mixerTracks[2].id);
        CHECK(lat > 0 && tc2.generatorDelay.delay == lat, "PDC delays parallel tracks by the reported plugin latency");
        app.removeEffect(t1, slot);
        app.engine().rebuild(app.project());
        CHECK(app.engine().registry().track(app.project().mixerTracks[2].id).generatorDelay.delay == 0,
            "PDC updates when the effect is removed");
    }
    // ---- offline render of the default beat
    {
        RenderOptions o;
        o.path = tmp.path() + "/r.wav";
        o.format = "wav";
        o.tailSeconds = 0.5;
        o.songMode = true;
        bool r = app.exportAudio(o, nullptr, &err);
        double sr = 0;
        int ch = 0;
        int64_t frames = 0;
        AudioFile::probe(o.path, &sr, &ch, &frames);
        CHECK(r && frames > 44100 && ch == 2, "offline render produces a stereo file of the song length");
        auto asset = AudioFile::load(o.path, 0);
        CHECK(asset && asset->peakValue > 0.1f, "rendered audio is not silent");
    }
    // ---- sample modes: tempo from names, clip speed, stretch flavours, sampler tempo fit
    {
        CHECK(AudioFile::bpmFromName("/x/PIOTR FIGIEL ENSEMBLE - To an unfaithful girl 89 bpm.wav") == 89 &&
                AudioFile::bpmFromName("кольхида - Криницы 83 bpm.wav") == 83 && AudioFile::bpmFromName("loop_120BPM.wav") == 120 &&
                AudioFile::bpmFromName("kick.wav") == 0,
            "sample tempo is read from file names");
        Clip c;
        c.fitTempo = true;
        c.sourceBpm = 83;
        c.pitch = 12;
        CHECK(std::fabs(clipSourceSpeed(c, 120) - 2.0 * 120 / 83) < 1e-9, "resample: tempo fit and pitch both change the clip's speed");
        c.stretchMode = StretchGeneral;
        CHECK(std::fabs(clipSourceSpeed(c, 120) - 120.0 / 83) < 1e-9, "stretch modes: pitch leaves the clip's speed alone");
        auto tone = std::make_shared<AudioAsset>();
        tone->sampleRate = 44100;
        tone->channels = 2;
        tone->frames = 44100;
        for (int i = 0; i < 44100; ++i) tone->left.push_back(0.5f * std::sin(i * 0.0627f) * (i % 11025 < 2000 ? 1.f : 0.3f));
        tone->right = tone->left;
        bool lengthsOk = true;
        for (int m = StretchGeneral; m <= StretchVocals; ++m) {
            auto st = AudioFile::stretch(*tone, 2.0, 0.0, m);
            if (std::abs(st->frames - 88200) > 1800) {
                lengthsOk = false;
                std::printf("  stretch mode %d: %lld frames\n", m, (long long)st->frames);
            }
        }
        CHECK(lengthsOk, "every stretch flavour renders the requested length");
        // sampler: how long one note sounds (seconds) for a given setup
        auto dc = std::make_shared<AudioAsset>();
        dc->sampleRate = 44100;
        dc->channels = 1;
        dc->frames = 44100;
        dc->left.assign(44100, 0.5f);
        auto noteLength = [&](std::function<void(Sampler&)> setup) {
            Sampler smp;
            smp.prepare(44100, 512);
            smp.setSample(dc, "dc.wav");
            setup(smp);
            MidiEventList ev;
            MidiEvent on;
            on.type = MidiEvent::NoteOn;
            on.data1 = 60;
            ev.add(on);
            ProcessContext cx;
            cx.bpm = 120;
            std::vector<float> l(512), r(512);
            int lastLoud = 0;
            for (int b = 0; b < 300; ++b) {
                std::fill(l.begin(), l.end(), 0.f);
                std::fill(r.begin(), r.end(), 0.f);
                smp.process(l.data(), r.data(), 512, ev, cx);
                ev.clear();
                for (int i = 0; i < 512; ++i)
                    if (std::fabs(l[i]) > 0.01f) lastLoud = b * 512 + i;
            }
            return lastLoud / 44100.0;
        };
        double plain = noteLength([](Sampler&) {});
        double fitted = noteLength([](Sampler& s) {
            s.params().set(Sampler::pFitTempo, 1);
            s.params().set(Sampler::pSourceBpm, 60);
        });
        CHECK(std::fabs(plain - 1.0) < 0.02 && std::fabs(fitted - 0.5) < 0.02,
            QString("resample: a 60 BPM sample at 120 BPM plays twice as fast (%1 s -> %2 s)").arg(plain).arg(fitted).toUtf8().constData());
        auto twoSec = std::make_shared<AudioAsset>(*dc);
        twoSec->frames = 88200;
        twoSec->left.assign(88200, 0.5f);
        double rendered = noteLength([&](Sampler& s) {
            s.params().set(Sampler::pStretchMode, StretchGeneral);
            s.params().set(Sampler::pTune, 12);
            s.setStretched(twoSec, "k");
        });
        CHECK(std::fabs(rendered - 2.0) < 0.02, "stretch mode plays the render as is (Tune is already in it, not applied twice)");
    }
    // ---- a sample's placement settings (edited in the project panel before placing) carry over to new clips and saves
    {
        auto& pr = app.project();
        AudioSource src;
        src.id = pr.newId();
        src.filePath = "/nonexistent/placed_later.wav";
        src.sampleRate = 44100;
        src.frames = 44100;
        src.hasPlaceDefaults = true;
        src.placeDefaults.gain = 0.5f;
        src.placeDefaults.pitch = 3;
        src.placeDefaults.stretchMode = StretchVocals;
        pr.audioSources.push_back(src);
        Id track = pr.playlistTracks.empty() ? app.addPlaylistTrack() : pr.playlistTracks.front().id;
        Id cid = app.addAudioClip(track, src.filePath, 0);
        const Clip* c = pr.clip(cid);
        CHECK(c && c->gain == 0.5f && c->pitch == 3 && c->stretchMode == StretchVocals, "a new clip starts from its sample's placement settings");
        Project p6;
        CHECK(ProjectSerializer::fromJson(ProjectSerializer::toJson(pr), p6, &err) && p6.audioSource(src.id) &&
                p6.audioSource(src.id)->hasPlaceDefaults && p6.audioSource(src.id)->placeDefaults.pitch == 3,
            "placement settings survive save/load");
        std::erase_if(pr.clips, [&](const Clip& x) { return x.id == cid; });
        std::erase_if(pr.audioSources, [&](const AudioSource& x) { return x.id == src.id; });
    }
    // ---- tempo change: audio clips that don't follow the tempo keep their length in seconds
    {
        auto& pr = app.project();
        double bpm0 = pr.transport.bpm;
        Clip a, f;
        a.id = pr.newId();
        a.type = ClipType::Audio;
        a.length = PPQ * 4;
        a.fadeIn = PPQ;
        f = a;
        f.id = pr.newId();
        f.fitTempo = true;
        Clip st = a;
        st.id = pr.newId();
        st.stretchMode = StretchGeneral;
        pr.clips.push_back(a);
        pr.clips.push_back(f);
        pr.clips.push_back(st);
        app.setBpm(bpm0 / 2);
        CHECK(pr.clip(a.id)->length == PPQ * 2 && pr.clip(a.id)->fadeIn == PPQ / 2 && pr.clip(f.id)->length == PPQ * 4,
            "resample: halving the tempo halves the clip's beats (same seconds); a tempo-fit clip keeps its beats");
        CHECK(pr.clip(st.id)->length == PPQ * 4 && std::fabs(pr.clip(st.id)->timeStretch - 2.0) < 1e-9,
            "stretch mode: halving the tempo keeps the clip's bars and stretches the audio 2x to fill them");
        app.model().undoStack().undo();
        CHECK(pr.transport.bpm == bpm0 && pr.clip(a.id)->length == PPQ * 4, "one undo restores the tempo and the clip");
        std::erase_if(pr.clips, [&](const Clip& c) { return c.id == a.id || c.id == f.id || c.id == st.id; });
    }
    // ---- typing a knob value: double-click the value label, type, Enter applies / Escape cancels
    {
        QWidget host;
        auto* lbl = new QLabel("0.00 st", &host);
        double got = 1e9;
        int calls = 0;
        makeValueEditable(lbl, [&](double v) {
            got = v;
            ++calls;
        });
        auto typeInto = [&](const QString& text, int key) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(lbl, &dbl);
            QLineEdit* ed = host.findChild<QLineEdit*>();
            if (!ed) return false;
            ed->setText(text);
            QKeyEvent k(QEvent::KeyPress, key, Qt::NoModifier);
            QCoreApplication::sendEvent(ed, &k);
            delete ed;   // deleteLater would wait for the event loop
            return true;
        };
        bool shown = typeInto("+1,2 st", Qt::Key_Return);
        CHECK(shown && calls == 1 && std::fabs(got - 1.2) < 1e-9, "double-click a knob value and type it (\"+1,2 st\" -> 1.2)");
        typeInto("5", Qt::Key_Escape);
        CHECK(calls == 1, "Escape cancels a typed knob value");
    }
    // ---- semitone knobs: gentle detent at whole semitones that a slow drag can still leave
    {
        auto snap = Knob::wholeStepSnap(-24, 24);
        auto st = [](float n) { return n * 48 - 24; };
        auto pos = [](float v) { return (v + 24) / 48; };
        CHECK(std::fabs(st(snap(pos(1.1f))) - 1.f) < 1e-4f && std::fabs(st(snap(pos(1.2f))) - 1.2f) < 1e-4f,
            "semitone snap lands 1.1 on 1 but leaves 1.2 alone");
        Knob k;
        k.snap = snap;
        k.setValue(pos(1.f));
        QMouseEvent pr(QEvent::MouseButtonPress, QPointF(10, 50), QPointF(10, 50), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&k, &pr);
        for (int y = 49; y >= 40; --y) {   // one pixel at a time
            QMouseEvent mv(QEvent::MouseMove, QPointF(10, y), QPointF(10, y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&k, &mv);
        }
        float v = st(k.value());
        CHECK(v > 1.2f && v < 4.f, QString("a slow drag moves past the detent (reached %1 st)").arg(v).toUtf8().constData());
    }
    // ---- mixer track names follow the routed channel unless the user renamed the track
    {
        Id pc = app.addChannel(ChannelType::Synth, "native.synth", "Piano");
        Id t1 = app.addMixerTrack(), t2 = app.addMixerTrack();
        auto name = [&](Id t) { return app.project().mixerTrack(t)->name; };
        QString t1Default = name(t1);
        app.setChannelMixerTrack(pc, t1);
        CHECK(name(t1) == "Piano", "mixer track takes the name of the channel routed to it");
        app.setChannelMixerTrack(pc, t2);
        CHECK(name(t2) == "Piano" && name(t1) == t1Default, "moving the channel moves the name; the old track gets its default back");
        app.editMixerTrack(t1, "Rename", [](MixerTrack& x) { x.name = "Strings bus"; x.customName = true; });
        app.setChannelMixerTrack(pc, t1);
        CHECK(name(t1) == "Strings bus" && name(t2) != "Piano", "a renamed track keeps its name when a channel is routed to it");
        app.renameChannel(pc, "Grand");
        app.setChannelMixerTrack(pc, t2);
        CHECK(name(t2) == "Grand" && name(t1) == "Strings bus", "renaming the channel renames its unrenamed track");
        QJsonObject j = ProjectSerializer::toJson(app.project());
        Project p5;
        CHECK(ProjectSerializer::fromJson(j, p5, &err) && p5.mixerTrack(t1)->customName && !p5.mixerTrack(t2)->customName,
            "custom mixer names survive save/load");
        app.deleteChannel(pc);
        app.deleteMixerTrack(t2);
        app.deleteMixerTrack(t1);
    }
    // ---- live note-on + note-off in the same block must not leave the note stuck
    {
        Id prevSel = app.project().selectedChannelId;
        Id sid = app.addChannel(ChannelType::Synth, "native.synth", "LiveTest");
        app.selectChannel(sid);
        app.engine().rebuild(app.project());
        auto& eng = app.engine();
        eng.offlineBegin(44100, 512);
        std::vector<float> oL(512), oR(512);
        auto blocks = [&](int k) {
            float e = 0;
            for (int b = 0; b < k; ++b) {
                eng.offlineProcess(oL.data(), oR.data(), 512);
                for (int i = 0; i < 512; ++i) e += oL[i] * oL[i];
            }
            return e;
        };
        app.noteOn(60);
        app.noteOff(60);
        float heard = blocks(4);
        blocks(400);   // ~4.6 s: far longer than any release
        float tail = blocks(4);
        eng.offlineEnd();
        CHECK(heard > 1e-4f && tail < 1e-6f, "live tap (note-on + note-off in one block) sounds and releases");
        app.selectChannel(prevSel);
        app.deleteChannel(sid);
    }
    // ---- sample cache & waveform peaks
    {
        auto a = DrumSampler::synthesizeDrum("Kick", 44100);
        CHECK(a->peaks.size() == 4 && a->levelFor(100).samplesPerBin == 64 && a->levelFor(5000).samplesPerBin == 4096,
            "multiresolution waveform cache levels");
    }
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

}   // namespace mt

#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
namespace mt {
// `musicthingy --preview-bench <dir>`: measures click-to-play latency of the browser preview for every file in a folder
int runPreviewBench(App& app, const QString& dir) {
    for (auto& f : QDir(dir).entryList({"*.wav", "*.flac", "*.ogg", "*.mp3"}, QDir::Files, QDir::Name)) {
        QString path = dir + "/" + f;
        app.stopPreview();
        {
            QElapsedTimer w;
            w.start();
            while (app.engine().previewPlaying() && w.elapsed() < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        QElapsedTimer t;
        t.start();
        app.previewFile(path);
        while (!app.engine().previewPlaying() && t.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::printf("%-14s  %6lld ms\n", f.toUtf8().constData(), (long long)t.elapsed());
        QElapsedTimer hold;
        hold.start();
        while (hold.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return 0;
}
}   // namespace mt

#include <QMouseEvent>
#include <QWheelEvent>
#include <QMainWindow>
#include <QPixmap>
namespace mt {
// `musicthingy --uitest <png>`: Ctrl+drag a selection rectangle in the playlist and screenshot the result
int runUiTest(QMainWindow* win, const QString& png) {
    QWidget* view = nullptr;
    for (auto* w : win->findChildren<QWidget*>())
        if (QString(w->metaObject()->className()) == "mt::PlaylistView") view = w;
    if (!view) {
        std::printf("no playlist view\n");
        return 1;
    }
    if (png.contains("zoom")) {
        for (int i = 0; i < 14; ++i) {
            QWheelEvent w(QPointF(600, 300), view->mapToGlobal(QPoint(600, 300)), QPoint(), QPoint(0, -120), Qt::NoButton,
                Qt::ControlModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(view, &w);
        }
        QCoreApplication::processEvents();
        view->repaint();
        view->grab().save(png);
        return 0;
    }
    QPoint a(520, 300), b(760, 420);
    QMouseEvent press(QEvent::MouseButtonPress, a, view->mapToGlobal(a), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(view, &press);
    QMouseEvent move(QEvent::MouseMove, b, view->mapToGlobal(b), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(view, &move);
    QCoreApplication::processEvents();
    view->repaint();
    view->grab().save(png);
    QMouseEvent rel(QEvent::MouseButtonRelease, b, view->mapToGlobal(b), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(view, &rel);
    return 0;
}
}   // namespace mt
namespace mt {
int runDuplicateTest(App& app) {
    Id id = app.project().channels[2].id;
    for (int i = 0; i < 5; ++i) {
        Id d = app.duplicateChannel(id);
        if (!d || !app.project().channel(d)) {
            std::printf("duplicate failed\n");
            return 1;
        }
        id = d;
    }
    auto* pat = app.project().pattern(app.project().selectedPatternId);
    std::printf("duplicated 5x: %zu channels, pattern has %zu channel rows\n", app.project().channels.size(), pat->channels.size());
    return 0;
}
}   // namespace mt
#include <QThread>
namespace mt {
int runBackupTest(App& app) {
    QTemporaryDir tmp;
    QString path = tmp.path() + "/song.mtp";
    if (!app.saveProjectAs(path)) {
        std::printf("save failed\n");
        return 1;
    }
    App::AutosaveSettings s;
    s.enabled = true;
    s.intervalMinutes = 5;
    s.maxCopies = 3;
    app.setAutosaveSettings(s);
    for (int i = 0; i < 5; ++i) {
        app.model().setDirty(true);
        QString err;
        if (!app.writeBackupNow(&err)) {
            std::printf("backup failed: %s\n", err.toUtf8().constData());
            return 1;
        }
        QThread::msleep(1100);
    }
    auto b = app.backups();
    std::printf("backups kept: %d (limit 3), dir %s\n", (int)b.size(), app.backupDir().toUtf8().constData());
    for (auto& f : b) std::printf("  %s\n", QFileInfo(f).fileName().toUtf8().constData());
    return b.size() == 3 ? 0 : 1;
}
}   // namespace mt

#include <QDockWidget>
namespace mt {
// `--docktest`: drag the floating Channel Rack by its title bar towards the bottom of the main window and report
int runDockTest(QMainWindow* win) {
    QDockWidget* rack = nullptr;
    for (auto* d : win->findChildren<QDockWidget*>())
        if (d->objectName() == "dockRack") rack = d;
    if (!rack) {
        std::printf("no rack\n");
        return 1;
    }
    std::printf("start: floating=%d geom=%d,%d %dx%d main=%d,%d %dx%d\n", rack->isFloating(), rack->x(), rack->y(), rack->width(),
        rack->height(), win->x(), win->y(), win->width(), win->height());
    QPoint title(rack->width() / 2, 8);   // title bar
    QMouseEvent press(QEvent::MouseButtonPress, title, rack->mapToGlobal(title), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(rack, &press);
    QCoreApplication::processEvents();
    int targetBottom = win->geometry().bottom();
    int steps = 12;
    for (int i = 1; i <= steps; ++i) {
        int dy = (targetBottom - rack->frameGeometry().bottom()) * i / steps;
        QPoint p = title + QPoint(0, dy);
        QMouseEvent mv(QEvent::MouseMove, p, rack->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(rack, &mv);
        QCoreApplication::processEvents();
        std::printf("move %d: floating=%d rack bottom=%d main bottom=%d\n", i, rack->isFloating(), rack->frameGeometry().bottom(),
            win->geometry().bottom());
    }
    QPoint last = title + QPoint(0, targetBottom - rack->frameGeometry().bottom());
    QMouseEvent rel(QEvent::MouseButtonRelease, last, rack->mapToGlobal(last), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(rack, &rel);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    std::printf("after release: floating=%d area=%d\n", rack->isFloating(), (int)win->dockWidgetArea(rack));
    return 0;
}
}   // namespace mt
