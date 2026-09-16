// Built-in self test (`musicthingy --selftest`): exercises the project model, serializer, undo, DSP and engine.
#include "ui/App.h"
#include "project/ProjectSerializer.h"
#include "dsp/NativeRegistry.h"
#include "dsp/effects/Effects.h"
#include "dsp/instruments/Instruments.h"
#include "audio/RenderEngine.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

namespace mt {

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("  ok   %s\n", msg); else { std::printf("  FAIL %s\n", msg); ++g_fail; } } while (0)

int runSelfTest(App& app) {
    std::printf("MusicThingy self test\n");
    // ---- project round trip
    Project p = makeDefaultProject(); QJsonObject j1 = ProjectSerializer::toJson(p); Project p2; QString err; bool ok = ProjectSerializer::fromJson(j1, p2, &err); QJsonObject j2 = ProjectSerializer::toJson(p2);
    CHECK(ok && QJsonDocument(j1).toJson() == QJsonDocument(j2).toJson(), "serializer round trip is lossless");
    QTemporaryDir tmp; QString path = tmp.path() + "/t.mtp"; CHECK(ProjectSerializer::save(p, path, &err), "save project file"); Project p3; CHECK(ProjectSerializer::load(path, p3, &err) && p3.channels.size() == p.channels.size() && p3.patterns.size() == 1, "load project file");
    QJsonObject old = j1; old["formatVersion"] = 0; Project p4; CHECK(!ProjectSerializer::fromJson(old, p4, &err), "rejects invalid format version");
    // ---- undo / redo through the model
    auto& model = app.model(); size_t nChan = app.project().channels.size(); Id cid = app.addChannel(ChannelType::Synth, "native.synth", "UndoTest");
    CHECK(app.project().channels.size() == nChan + 1 && app.project().channel(cid), "add channel command");
    model.undoStack().undo(); model.undoStack().undo(); CHECK(app.project().channels.size() == nChan, "undo restores channel list");
    model.undoStack().redo(); model.undoStack().redo(); CHECK(app.project().channels.size() == nChan + 1, "redo re-applies");
    app.setChannelVolume(cid, 0.3f, true); app.setChannelVolume(cid, 0.4f, true); CHECK(std::fabs(app.project().channel(cid)->volume - 0.4f) < 1e-6f, "volume command applied"); size_t before = model.undoStack().count(); model.undoStack().undo(); CHECK(std::fabs(app.project().channel(cid)->volume - 0.8f) < 1e-6f && model.undoStack().count() == before - 1, "merged knob edits undo in one step");
    // ---- routing cycle detection
    Id master = app.project().masterId(); Id a = app.project().mixerTracks[1].id, b = app.project().mixerTracks[2].id; CHECK(app.routeTrack(a, b), "route insert 1 -> insert 2"); CHECK(!app.project().routingWouldCycle(a, master), "routing to master is acyclic"); CHECK(app.project().routingWouldCycle(b, a), "cycle detected (2 -> 1 while 1 -> 2)"); app.routeTrack(a, master);
    // ---- automation evaluation
    AutomationLane lane; lane.points = {{0, 0.f, CurveType::Linear, 0}, {PPQ, 1.f, CurveType::Stepped, 0}, {2 * PPQ, 0.f, CurveType::Linear, 0}}; CHECK(std::fabs(lane.valueAt(PPQ / 2) - 0.5f) < 1e-5f, "linear automation interpolation"); CHECK(std::fabs(lane.valueAt(PPQ + PPQ / 2) - 1.f) < 1e-5f, "stepped automation holds value");
    // ---- every native effect processes finite output; every instrument makes sound
    int fxOk = 0, fxTotal = 0, instOk = 0, instTotal = 0; std::vector<float> L(512), R(512); dsp::Rng rng; MidiEventList midi; ProcessContext ctx; ctx.sampleRate = 44100; ctx.blockSize = 512; ctx.bpm = 120;
    for (auto& info : NativeRegistry::instance().all()) {
        auto proc = info.factory(); proc->prepare(44100, 512);
        if (info.kind == ProcessorKind::Effect) { ++fxTotal; bool finite = true; for (int blk = 0; blk < 20; ++blk) { for (int i = 0; i < 512; ++i) { L[i] = rng.bipolar() * 0.5f; R[i] = L[i]; } proc->process(L.data(), R.data(), 512, midi, ctx); for (int i = 0; i < 512; ++i) if (!std::isfinite(L[i]) || !std::isfinite(R[i]) || std::fabs(L[i]) > 100.f) finite = false; } if (finite) ++fxOk; else std::printf("  FAIL effect %s produced non-finite output\n", info.name.toUtf8().constData()); }
        else if (info.key != "native.midiout") { ++instTotal; if (auto* ds = dynamic_cast<DrumSampler*>(proc.get())) ds->setBuiltin("Kick"); if (auto* s = dynamic_cast<Sampler*>(proc.get())) s->setSample(DrumSampler::synthesizeDrum("Snare", 44100), "test"); midi.clear(); MidiEvent e; e.type = MidiEvent::NoteOn; e.data1 = 60; e.velocity = 1.f; e.data2 = 127; midi.add(e); float energy = 0; for (int blk = 0; blk < 8; ++blk) { std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f); proc->process(L.data(), R.data(), 512, midi, ctx); midi.clear(); for (int i = 0; i < 512; ++i) energy += L[i] * L[i]; } if (energy > 1e-4f) ++instOk; else std::printf("  FAIL instrument %s is silent\n", info.name.toUtf8().constData()); }
    }
    CHECK(fxOk == fxTotal, QString("all %1 native effects process cleanly").arg(fxTotal).toUtf8().constData()); CHECK(instOk == instTotal, QString("all %1 native instruments produce audio on note-on").arg(instTotal).toUtf8().constData());
    // ---- plugin delay compensation: limiter with lookahead on one insert
    { Id t1 = app.project().mixerTracks[1].id; Id slot = app.addEffect(t1, "native.limiter"); app.engine().rebuild(app.project()); int lat = app.processor(slot) ? app.processor(slot)->latencySamples() : 0; auto& tc2 = app.engine().registry().track(app.project().mixerTracks[2].id); CHECK(lat > 0 && tc2.generatorDelay.delay == lat, "PDC delays parallel tracks by the reported plugin latency"); app.removeEffect(t1, slot); app.engine().rebuild(app.project()); CHECK(app.engine().registry().track(app.project().mixerTracks[2].id).generatorDelay.delay == 0, "PDC updates when the effect is removed"); }
    // ---- offline render of the default beat
    { RenderOptions o; o.path = tmp.path() + "/r.wav"; o.format = "wav"; o.tailSeconds = 0.5; o.songMode = true; bool r = app.exportAudio(o, nullptr, &err); double sr = 0; int ch = 0; int64_t frames = 0; AudioFile::probe(o.path, &sr, &ch, &frames); CHECK(r && frames > 44100 && ch == 2, "offline render produces a stereo file of the song length"); auto asset = AudioFile::load(o.path, 0); CHECK(asset && asset->peakValue > 0.1f, "rendered audio is not silent"); }
    // ---- sample cache & waveform peaks
    { auto a = DrumSampler::synthesizeDrum("Kick", 44100); CHECK(a->peaks.size() == 4 && a->levelFor(100).samplesPerBin == 64 && a->levelFor(5000).samplesPerBin == 4096, "multiresolution waveform cache levels"); }
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

} // namespace mt

#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
namespace mt {
// `musicthingy --preview-bench <dir>`: measures click-to-play latency of the browser preview for every file in a folder
int runPreviewBench(App& app, const QString& dir) {
    for (auto& f : QDir(dir).entryList({"*.wav", "*.flac", "*.ogg", "*.mp3"}, QDir::Files, QDir::Name)) {
        QString path = dir + "/" + f; app.stopPreview(); { QElapsedTimer w; w.start(); while (app.engine().previewPlaying() && w.elapsed() < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5); } QElapsedTimer t; t.start(); app.previewFile(path);
        while (!app.engine().previewPlaying() && t.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::printf("%-14s  %6lld ms\n", f.toUtf8().constData(), (long long)t.elapsed());
        QElapsedTimer hold; hold.start(); while (hold.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return 0;
}
}

#include <QMouseEvent>
#include <QWheelEvent>
#include <QMainWindow>
#include <QPixmap>
namespace mt {
// `musicthingy --uitest <png>`: Ctrl+drag a selection rectangle in the playlist and screenshot the result
int runUiTest(QMainWindow* win, const QString& png) {
    QWidget* view = nullptr; for (auto* w : win->findChildren<QWidget*>()) if (QString(w->metaObject()->className()) == "mt::PlaylistView") view = w;
    if (!view) { std::printf("no playlist view\n"); return 1; }
    if (png.contains("zoom")) { for (int i = 0; i < 14; ++i) { QWheelEvent w(QPointF(600, 300), view->mapToGlobal(QPoint(600, 300)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false); QCoreApplication::sendEvent(view, &w); } QCoreApplication::processEvents(); view->repaint(); view->grab().save(png); return 0; }
    QPoint a(520, 300), b(760, 420);
    QMouseEvent press(QEvent::MouseButtonPress, a, view->mapToGlobal(a), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier); QCoreApplication::sendEvent(view, &press);
    QMouseEvent move(QEvent::MouseMove, b, view->mapToGlobal(b), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier); QCoreApplication::sendEvent(view, &move);
    QCoreApplication::processEvents(); view->repaint(); view->grab().save(png);
    QMouseEvent rel(QEvent::MouseButtonRelease, b, view->mapToGlobal(b), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier); QCoreApplication::sendEvent(view, &rel);
    return 0;
}
}
namespace mt { int runDuplicateTest(App& app) { Id id = app.project().channels[2].id; for (int i = 0; i < 5; ++i) { Id d = app.duplicateChannel(id); if (!d || !app.project().channel(d)) { std::printf("duplicate failed\n"); return 1; } id = d; } auto* pat = app.project().pattern(app.project().selectedPatternId); std::printf("duplicated 5x: %zu channels, pattern has %zu channel rows\n", app.project().channels.size(), pat->channels.size()); return 0; } }
#include <QThread>
namespace mt { int runBackupTest(App& app) {
    QTemporaryDir tmp; QString path = tmp.path() + "/song.mtp"; if (!app.saveProjectAs(path)) { std::printf("save failed\n"); return 1; }
    App::AutosaveSettings s; s.enabled = true; s.intervalMinutes = 5; s.maxCopies = 3; app.setAutosaveSettings(s);
    for (int i = 0; i < 5; ++i) { app.model().setDirty(true); QString err; if (!app.writeBackupNow(&err)) { std::printf("backup failed: %s\n", err.toUtf8().constData()); return 1; } QThread::msleep(1100); }
    auto b = app.backups(); std::printf("backups kept: %d (limit 3), dir %s\n", (int)b.size(), app.backupDir().toUtf8().constData()); for (auto& f : b) std::printf("  %s\n", QFileInfo(f).fileName().toUtf8().constData());
    return b.size() == 3 ? 0 : 1; } }
