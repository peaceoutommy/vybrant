// Built-in self test (`vybrant --selftest`): exercises the project model, serializer, undo, DSP and engine.
#include "ui/App.h"
#include "project/ProjectSerializer.h"
#include "dsp/NativeRegistry.h"
#include "dsp/Fft.h"
#include "dsp/effects/Effects.h"
#include "dsp/instruments/Instruments.h"
#include "audio/RenderEngine.h"
#include "plugins/PluginHost.h"
#include "plugins/PluginBridge.h"
#include "project/MidiFile.h"
#include "ui/PianoRollPanel.h"
#include "ui/widgets/Controls.h"
#include "ui/dialogs/Dialogs.h"
#include "ui/Theme.h"
#include "ui/ProcessorPicker.h"
#include "License.h"
#include "core/ProcessUsage.h"
#include "AppImage.h"
#include <QSlider>
#include <QPushButton>
#include <QSettings>
#include "ui/AudioEditor.h"
#include "ui/PlaylistPanel.h"
#include "ui/PluginWindow.h"
#ifndef _WIN32
#include "plugins/Lv2Plugin.h"
#endif
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QMenu>
#include <QInputDialog>
#include <QApplication>
#include <QTimer>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <cstdio>
#include <cstring>

namespace mt {
QWidget* dynamicsHandle(QWidget* editor, int i, QPoint& pos);   // ui/editors/DynamicsEditors.cpp

static int g_fail = 0;
#ifdef _WIN32   // what the test CLAP plugin's editor window needs
static const QString kGuiLib = "-luser32";
#else
static const QString kGuiLib = "-lX11";
#endif
#define CHECK(cond, msg)                                                                                                                   \
    do {                                                                                                                                   \
        if (cond) std::printf("  ok   %s\n", msg);                                                                                         \
        else {                                                                                                                             \
            std::printf("  FAIL %s\n", msg);                                                                                               \
            ++g_fail;                                                                                                                      \
        }                                                                                                                                  \
    } while (0)

bool eqHandleDragSelfTest(IProcessor* eq);   // ui/editors/EqEditors.cpp

int runSelfTest(App& app) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash keeps the lines before it (stdout to a pipe is buffered)
    std::printf("Vybrant self test\n");
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
#ifndef _WIN32
    // ---- LV2 hosting: the gain plugin in tests/lv2, built with the system C compiler into a temporary bundle
    {
        QString src = QCoreApplication::applicationDirPath() + "/../tests/lv2/test-gain";
        QString lv2Dir = tmp.path() + "/lv2", bundle = lv2Dir + "/mt-test-gain.lv2";
        QString cc = QStandardPaths::findExecutable("cc");
        bool built = !cc.isEmpty() && QDir().mkpath(bundle) && QFile::copy(src + "/manifest.ttl", bundle + "/manifest.ttl") &&
            QFile::copy(src + "/plugin.ttl", bundle + "/plugin.ttl") &&
            QProcess::execute(cc, {"-shared", "-fPIC", "-O2", "-o", bundle + "/mt-test-gain.so", src + "/gain.c"}) == 0;
        if (!built) std::printf("  skip LV2 checks (no C compiler or LV2 headers, or tests/lv2 not next to bin/)\n");
        else {
            qputenv("LV2_PATH", lv2Dir.toLocal8Bit());
            PluginInfo info;
            for (auto& p : Lv2Plugin::scan())
                if (p.uid == "urn:vybrant:test-gain") info = p;
            CHECK(info.format == "lv2" && info.category == "Effect" && info.numInputs == 2 && info.numOutputs == 2 && !info.hasEditor &&
                    !info.failed && info.path == bundle,
                "LV2 scan finds the test plugin");
            auto gainOut = [&](IProcessor* p) {
                std::fill(L.begin(), L.end(), 0.5f);
                std::fill(R.begin(), R.end(), -0.5f);
                midi.clear();
                p->process(L.data(), R.data(), 512, midi, ctx);
                return std::fabs(L[100] - 0.25f) < 1e-6f && std::fabs(R[100] + 0.25f) < 1e-6f;
            };
            auto a = Lv2Plugin::create(info.uid, info.path, &err);
            if (a) {
                a->prepare(44100, 512);
                a->params().set(4, 0.5f);   // the gain port
            }
            CHECK(a && gainOut(a.get()), "LV2 plugin applies its control port (gain 0.5)");
            CHECK(a && a->latencySamples() == 64, "LV2 latency port is read");
            auto b = Lv2Plugin::create(info.uid, info.path, &err);
            if (a && b) {
                b->restoreState(QJsonObject{{"pluginState", a->saveState()["pluginState"]}});   // as PluginHost::createInstance does
                b->prepare(44100, 512);
            }
            CHECK(a && b && std::fabs(b->params().get(4) - 0.5f) < 1e-6f && gainOut(b.get()), "LV2 state round-trips into a new instance");
            app.pluginDb().addOrUpdate(info);
            Id t1 = app.project().mixerTracks[1].id;
            Id slot = app.addPluginEffect(t1, info);
            app.engine().rebuild(app.project());
            IProcessor* fx = app.processor(slot);
            CHECK(fx && fx->pluginFormat() == "lv2" &&
                    app.engine().registry().track(app.project().mixerTracks[2].id).generatorDelay.delay == 64,
                "an LV2 effect on a mixer track drives delay compensation");
            app.removeEffect(t1, slot);
            app.engine().rebuild(app.project());
        }
    }
#endif
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
        o.stems = true;
        o.path = tmp.path() + "/stems/s.wav";
        QDir().mkpath(tmp.path() + "/stems");
        r = app.exportAudio(o, nullptr, &err);
        QStringList files = QDir(tmp.path() + "/stems").entryList({"*.wav"}, QDir::Files, QDir::Name);
        auto peak = [&](const QString& part) {
            for (auto& f : files)
                if (f.contains(part))
                    if (auto a = AudioFile::load(tmp.path() + "/stems/" + f, 0)) return a->peakValue;
            return -1.f;
        };
        CHECK(r && files.size() >= 5 && peak("_00 Master") > 0.1f && peak(" Kick") > 0.1f && peak(" Snare") >= 0 && peak(" Snare") < 1e-4f,
            "stems: one file per used mixer track, each holding only what plays there (the demo snare has no steps)");
    }
    // ---- theme dialog: the corner radius previews live; closing without Save puts it back, Save keeps it
    {
        double r0 = theme::radius();
        auto saved = [] { return QSettings().value("theme/radius", -1.0).toDouble(); };
        double s0 = saved();
        ThemeDialog d;
        auto* slider = d.findChild<QSlider*>();
        slider->setValue((int)r0 + 3);
        bool live = theme::radius() == r0 + 3 && saved() == s0;
        d.reject();
        bool reverted = theme::radius() == r0 && saved() == s0;
        ThemeDialog d2;
        d2.findChild<QSlider*>()->setValue((int)r0 + 2);
        for (auto* b : d2.findChildren<QPushButton*>())
            if (b->text() == "Save") b->click();
        bool kept = theme::radius() == r0 + 2 && saved() == r0 + 2;
        CHECK(live && reverted && kept, "theme radius: previews live, Cancel puts it back, only Save keeps it");
        theme::setRadius(r0);
        if (s0 < 0) QSettings().remove("theme/radius");
        else QSettings().setValue("theme/radius", s0);
        theme::applyTheme(theme::current());
    }
    // ---- freeze: the insert's render plays in its place, so the song sounds the same - also with a latency effect on it
    {
        auto& pr = app.project();
        Id kick = pr.channels[0].id, ins = pr.channels[0].mixerTrackId;
        model.edit<Channel>(kick, "", [](Channel& c) { c.solo = true; });   // only the kick: other instruments use noise
        Id slot = app.addEffect(ins, "native.limiter");
        QString oldPath = model.filePath();
        model.setFilePath(tmp.path() + "/f.mtp");   // the render goes next to the project
        RenderOptions o;
        o.tailSeconds = 0.5;
        o.bitDepth = 32;
        o.dither = false;
        o.path = tmp.path() + "/pre.wav";
        app.exportAudio(o, nullptr, &err);
        bool fz = app.freezeMixerTrack(ins, nullptr, &err);
        o.path = tmp.path() + "/post.wav";
        app.exportAudio(o, nullptr, &err);
        auto a = AudioFile::load(tmp.path() + "/pre.wav", 0), b = AudioFile::load(tmp.path() + "/post.wav", 0);
        float diff = 1.f;
        if (a && b && a->frames > 1000 && b->frames >= a->frames) {   // the frozen clip keeps its tail, so the song can get longer
            diff = 0.f;
            for (int64_t i = 0; i < std::min(a->frames, b->frames); ++i) diff = std::max(diff, std::fabs(a->L()[i] - b->L()[i]));
        }
        const MixerTrack* mt = pr.mixerTrack(ins);
        CHECK(fz && mt && mt->freezeTrackId && pr.track(mt->freezeTrackId) && diff < 1e-3f && a->peakValue > 0.1f,
            QString("a frozen insert sounds the same, lined up through its effect's latency (max diff %1)").arg(diff).toUtf8().constData());
        app.unfreezeMixerTrack(ins);
        CHECK(!pr.mixerTrack(ins)->freezeTrackId &&
                std::none_of(
                    pr.playlistTracks.begin(), pr.playlistTracks.end(), [](const PlaylistTrack& t) { return t.name.endsWith("(frozen)"); }),
            "unfreeze removes the render and its track");
        model.undoStack().undo();
        model.undoStack().undo();
        CHECK(!pr.mixerTrack(ins)->freezeTrackId, "undo steps back through unfreeze and freeze");
        model.setFilePath(oldPath);
        app.removeEffect(ins, slot);
        model.edit<Channel>(kick, "", [](Channel& c) { c.solo = false; });
    }
    // ---- consolidate: a pattern clip becomes one audio clip that sounds the same - the master's effects (a latency one and a
    // phase flip, which would cancel if applied twice) run once, and an unselected clip playing alongside stays out of the render
    {
        auto& pr = app.project();
        Id kick = pr.channels[0].id, master = pr.masterId(), tr1 = pr.playlistTracks[0].id, tr2 = pr.playlistTracks[1].id, src = 0;
        model.edit<Channel>(kick, "", [](Channel& c) { c.solo = true; });
        Id lim = app.addEffect(master, "native.limiter"), flip = app.addEffect(master, "native.phaseinvert");
        app.engine().rebuild(pr);   // creates the effects' processors
        IProcessor* f = app.processor(flip);
        if (f) {
            f->params().set(PhaseInvert::pLeft, 1);
            f->params().set(PhaseInvert::pRight, 1);
        }
        QString oldPath = model.filePath();
        model.setFilePath(tmp.path() + "/c.mtp");
        for (auto& c : pr.clips)
            if (c.type == ClipType::Pattern) src = c.id;
        Tick at = (pr.songLength() / PPQ + 4) * PPQ;
        Id sel = app.duplicateClip(src, at, tr1);
        Id pat = pr.clip(sel)->sourceId;
        Tick len = pr.clipLength(*pr.clip(sel));
        RenderOptions o;
        o.tailSeconds = 0.5;
        o.bitDepth = 32;
        o.dither = false;
        o.start = at;
        o.end = at + len * 2;   // the transport runs past the clip, as in the song: a render's tail stops audio clips
        o.path = tmp.path() + "/cpre.wav";
        app.exportAudio(o, nullptr, &err);
        Id other = app.duplicateClip(src, at, tr2);   // plays along, on the same instrument: must stay out of the render
        Id nc = app.consolidateClips({sel}, nullptr, &err);
        const Clip* c = pr.clip(nc);
        CHECK(c && c->type == ClipType::Audio && c->start == at && c->trackId != tr1 && c->length == len && pr.clip(sel) &&
                pr.clip(sel)->muted && pr.clip(other) && !pr.clip(other)->muted && pr.pattern(pat),
            "consolidate: one audio clip at the clip's start and of its length on a free track; the original is muted and kept (as in FL Studio)");
        app.deleteClips({other});
        o.path = tmp.path() + "/cpost.wav";
        app.exportAudio(o, nullptr, &err);
        auto a = AudioFile::load(tmp.path() + "/cpre.wav", 0), b = AudioFile::load(tmp.path() + "/cpost.wav", 0);
        float diff = 1.f;
        int64_t clipFrames = (int64_t)((pr.transport.tempoMap().secondsAt(at + len) - pr.transport.tempoMap().secondsAt(at)) * 44100);
        if (a && b && a->frames > 1000) {   // over the clip: past its end the tail is cut, as in FL Studio
            diff = 0.f;
            for (int64_t i = 0; i < std::min({a->frames, b->frames, clipFrames}); ++i)
                diff = std::max(diff, std::fabs(a->L()[i] - b->L()[i]));
        }
        const AudioSource* cs = c ? pr.audioSource(c->sourceId) : nullptr;
        CHECK(f && diff < 1e-3f && a->peakValue > 0.1f && cs && std::llabs(cs->frames - clipFrames) <= 2,
            QString("consolidate (tail: cut): the clip sounds the same over its length (max diff %1) and its file ends there")
                .arg(diff)
                .toUtf8()
                .constData());
        auto cut = cs ? AudioFile::load(cs->filePath, 0) : nullptr;
        model.undoStack().undo();   // the delete
        model.undoStack().undo();
        CHECK(pr.clip(sel) && !pr.clip(sel)->muted && pr.clip(other) && !pr.clip(nc), "consolidate is one undo step");
        {   // Leave lets the tail ring on (a longer clip and file); Wrap folds it onto the start (same length, a different start)
            Id l = app.consolidateClips({sel}, nullptr, &err, App::Tail::Leave);
            const Clip* lc = pr.clip(l);
            const AudioSource* ls = lc ? pr.audioSource(lc->sourceId) : nullptr;
            bool leave = lc && lc->length > len && ls && ls->frames > clipFrames;
            model.undoStack().undo();
            Id w = app.consolidateClips({sel}, nullptr, &err, App::Tail::Wrap);
            const Clip* wc = pr.clip(w);
            const AudioSource* ws = wc ? pr.audioSource(wc->sourceId) : nullptr;
            auto wrap = ws ? AudioFile::load(ws->filePath, 0) : nullptr;
            float folded = 0.f;
            for (int64_t i = 0; wrap && cut && i < std::min(wrap->frames, cut->frames); ++i)
                folded = std::max(folded, std::fabs(wrap->L()[i] - cut->L()[i]));
            bool wrapped = wc && wc->length == len && wrap && cut && wrap->frames == cut->frames && folded > 1e-4f;
            model.undoStack().undo();
            CHECK(leave && wrapped && pr.clip(sel) && !pr.clip(sel)->muted,
                "consolidate tails: Leave rings on past the selection, Wrap folds it onto the start");
        }
        if (nc) {   // the playlist's Ctrl+Alt+C does it to its selection
            PlaylistView v(app);
            v.revealClips({sel});
            QTimer::singleShot(0, [] {   // the Tail prompt: keep its default
                if (auto* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) d->accept();
            });
            QKeyEvent k(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
            QCoreApplication::sendEvent(&v, &k);
            CHECK(pr.clip(sel) && pr.clip(sel)->muted && pr.clip(other) && pr.clips.back().type == ClipType::Audio &&
                    pr.clips.back().start == at,
                "Ctrl+Alt+C in the playlist consolidates the selected pattern clips");
            model.undoStack().undo();
        }
        app.deleteClips({sel, other});
        model.setFilePath(oldPath);
        app.removeEffect(master, flip);
        app.removeEffect(master, lim);
        model.edit<Channel>(kick, "", [](Channel& c) { c.solo = false; });
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
        Id track = app.firstPlaylistTrack();
        Id cid = app.addAudioClip(track, src.filePath, 0);
        const Clip* c = pr.clip(cid);
        CHECK(c && c->gain == 0.5f && c->pitch == 3 && c->stretchMode == StretchVocals,
            "a new clip starts from its sample's placement settings");
        Project p6;
        CHECK(ProjectSerializer::fromJson(ProjectSerializer::toJson(pr), p6, &err) && p6.audioSource(src.id) &&
                p6.audioSource(src.id)->hasPlaceDefaults && p6.audioSource(src.id)->placeDefaults.pitch == 3,
            "placement settings survive save/load");
        std::erase_if(pr.clips, [&](const Clip& x) { return x.id == cid; });
        std::erase_if(pr.audioSources, [&](const AudioSource& x) { return x.id == src.id; });
    }
    // ---- recording in 'Automation' mode: every live control records (mixer pan and sends used to be left out)
    {
        auto& pr = app.project();
        auto& eng = app.engine();
        Id t1 = app.addMixerTrack(), t2 = app.addMixerTrack();
        app.addSend(t1, t2, false);
        int mode = pr.transport.recordMode;
        pr.transport.recordMode = 3;
        eng.offlineBegin(44100, 512);
        std::vector<float> l(512), r(512);
        eng.setRecording(true);
        eng.offlineProcess(l.data(), r.data(), 512);   // the engine picks the flag up on its next block
        auto recorded = [&](ParamTarget::Kind k, Id owner) {
            return std::any_of(pr.automationLanes.begin(), pr.automationLanes.end(),
                [&](const AutomationLane& x) { return x.target.kind == k && x.target.ownerId == owner && !x.points.empty(); });
        };
        app.setMixerVolume(t1, 0.5f, false);
        app.setMixerPan(t1, -0.5f, false);
        app.setSendLevel(t1, t2, 0.3f, false);
        bool vol = recorded(ParamTarget::MixerVolume, t1), pan = recorded(ParamTarget::MixerPan, t1),
             snd = recorded(ParamTarget::MixerSend, t1);
        CHECK(vol && pan && snd,
            QString("automation recording catches mixer volume, pan and send moves (%1 %2 %3)")
                .arg(vol)
                .arg(pan)
                .arg(snd)
                .toUtf8()
                .constData());
        eng.setRecording(false);
        eng.offlineProcess(l.data(), r.data(), 512);
        eng.offlineEnd();
        pr.transport.recordMode = mode;
        std::vector<Id> lanes;
        for (auto& x : pr.automationLanes)
            if (x.target.ownerId == t1) lanes.push_back(x.id);
        for (Id id : lanes) app.deleteLane(id);
        app.deleteMixerTrack(t2);
        app.deleteMixerTrack(t1);
    }
    // ---- recording passes, FL style: Record only arms and Play starts the pass; in song mode every loop pass of the input is a
    // take of one file, stacked on the insert's playlist tracks with the older ones muted; notes recorded outside the pattern's
    // clips get a clip where the pass started; each pass is one undo step
    {
        auto& pr = app.project();
        auto& eng = app.engine();
        QTemporaryDir recDir;
        app.recorder().setRecordDirectory(recDir.path());
        Id ins = app.addMixerTrack("Mic");
        model.edit<MixerTrack>(ins, "", [](MixerTrack& t) { t.inputChannel = 0, t.armed = true; });
        TransportSettings saved = pr.transport;
        Tick bar = pr.transport.timeSig.ticksPerBar();
        model.editTransport("", [bar](TransportSettings& t) {
            t.songMode = t.loopEnabled = t.loopRecord = true;
            t.punchEnabled = t.countIn = false;
            t.recordMode = 2;
            t.loopStart = 0;
            t.loopEnd = bar;
        });
        const int B = 512;
        eng.offlineBegin(44100, B);
        std::vector<float> l(B), r(B), in(B * 2, 0.25f);
        auto run = [&](int blocks) {
            for (int i = 0; i < blocks; ++i) {
                eng.offlineProcess(l.data(), r.data(), B, in.data(), 2);
                eng.poll();
            }
        };
        size_t clips0 = pr.clips.size(), tracks0 = pr.playlistTracks.size(), undo0 = model.undoStack().count();
        app.record(true);
        run(1);
        CHECK(eng.isRecording() && !eng.isPlaying(), "Record only arms the transport");
        app.play();
        double barFrames = (double)bar / PPQ * 60.0 / pr.transport.bpm * 44100;
        int blocks = (int)(barFrames * 2.5 / B);
        run(blocks);
        app.stop();
        std::vector<const Clip*> takes;
        for (auto& c : pr.clips)
            if (c.type == ClipType::Audio && pr.track(c.trackId) && pr.track(c.trackId)->mixerTrackId == ins) takes.push_back(&c);
        const AudioSource* src = takes.empty() ? nullptr : pr.audioSource(takes[0]->sourceId);
        CHECK(src && src->frames == (int64_t)blocks * B, "the pass is one file with every recorded frame");
        CHECK(takes.size() == 3, QString("2.5 loop passes make 3 takes (%1)").arg(takes.size()).toUtf8().constData());
        if (takes.size() == 3) {
            bool atLoop = true, offsets = true, tracks = takes[0]->trackId != takes[1]->trackId && takes[1]->trackId != takes[2]->trackId;
            for (int i = 0; i < 3; ++i) {
                atLoop = atLoop && takes[i]->start == 0;
                if (i) offsets = offsets && std::abs(takes[i]->sourceOffset - takes[i - 1]->sourceOffset - bar) <= 1;
            }
            CHECK(atLoop && offsets, "each take starts at the loop start, a loop length further into the file");
            CHECK(tracks && takes[0]->muted && takes[1]->muted && !takes[2]->muted,
                "takes stack on their own tracks, only the newest audible");
        }
        model.undoStack().undo();
        CHECK(pr.clips.size() == clips0 && pr.playlistTracks.size() == tracks0 && model.undoStack().count() == undo0,
            "one undo removes the whole pass");
        model.edit<MixerTrack>(ins, "", [](MixerTrack& t) { t.inputStereo = false; });
        app.record(true);
        run(1);
        app.play();
        run(40);
        app.stop();
        const AudioSource* mono = pr.audioSources.empty() ? nullptr : &pr.audioSources.back();
        CHECK(mono && mono->channels == 1 && mono->frames == 40 * B, "a mono input records a mono file");
        model.undoStack().undo();
        // across a tempo jump inside a 2-bar loop: each take still starts its audio where its loop pass began in the file
        model.editTransport("", [bar](TransportSettings& t) {
            t.tempoPoints = {{bar, t.bpm * 0.75, false}};
            t.loopEnd = bar * 2;
        });
        TempoMap tm = pr.transport.tempoMap();
        double pass = tm.secondsAt(bar * 2) - tm.secondsAt(0);
        app.record(true);
        run(1);
        app.play();
        run((int)(pass * 44100 * 2.5 / B));
        app.stop();
        std::vector<const Clip*> tt;
        for (auto& c : pr.clips)
            if (c.type == ClipType::Audio && pr.track(c.trackId) && pr.track(c.trackId)->mixerTrackId == ins) tt.push_back(&c);
        bool mapped = tt.size() == 3;
        for (size_t i = 1; i < tt.size(); ++i)
            mapped = mapped &&
                std::fabs(clipSourceSeconds(*tt[i], tt[i]->start, tm) - clipSourceSeconds(*tt[i - 1], tt[i - 1]->start, tm) - pass) < 1e-3;
        CHECK(mapped && std::abs(tt[0]->length - bar * 2) <= 2, "takes across a tempo change line up through the tempo map");
        model.undoStack().undo();
        model.editTransport("", [](TransportSettings& t) { t.tempoPoints.clear(); });
        // notes in song mode, starting mid-bar outside any clip of the pattern
        Id pat = app.addPattern("RecTest");
        app.selectPattern(pat);
        model.editTransport("", [](TransportSettings& t) { t.recordMode = 1, t.loopEnabled = false; });
        app.seek(bar * 2 + PPQ);
        app.record(true);
        run(1);
        app.play();
        run(4);
        MidiEvent on;
        on.type = MidiEvent::NoteOn;
        on.data1 = 64;
        eng.pushUiMidi(on);
        run(20);
        MidiEvent bend, mod;
        bend.type = MidiEvent::PitchBend;
        bend.pitchBend = 4000;
        mod.type = MidiEvent::CC;
        mod.data1 = 1;
        mod.data2 = 90;
        eng.pushUiMidi(bend);
        eng.pushUiMidi(mod);
        run(2);
        MidiEvent off = on;
        off.type = MidiEvent::NoteOff;
        eng.pushUiMidi(off);
        run(4);
        size_t undo1 = model.undoStack().count();
        app.stop();
        const Pattern* p = pr.pattern(pat);
        int notes = 0;
        for (auto& cd : p->channels) notes += (int)cd.notes.size();
        auto clip = std::find_if(
            pr.clips.begin(), pr.clips.end(), [&](const Clip& c) { return c.type == ClipType::Pattern && c.sourceId == pat; });
        CHECK(notes == 1 && clip != pr.clips.end() && clip->start == bar * 2,
            "song-mode notes get a pattern clip at the bar the pass started in");
        std::vector<ControlPoint> ctl;
        for (auto& cd : p->channels) ctl.insert(ctl.end(), cd.controls.begin(), cd.controls.end());
        CHECK(ctl.size() == 2 && ctl[0].tick == ctl[1].tick && ctl[0].controller == kCtrlPitchBend && ctl[0].value == 12192 &&
                ctl[1].controller == 1 && ctl[1].value == 90,
            "pitch bend and CC played during the pass are recorded into the controller lanes");
        model.undoStack().undo();
        notes = 0;
        for (auto& cd : pr.pattern(pat)->channels) notes += (int)cd.notes.size() + (int)cd.controls.size();
        CHECK(notes == 0 && model.undoStack().count() == undo1 &&
                std::none_of(pr.clips.begin(), pr.clips.end(), [&](const Clip& c) { return c.sourceId == pat; }),
            "one undo removes the recorded notes, controller points and their clip");
        eng.offlineEnd();
        app.deletePattern(pat);
        model.editTransport("", [saved](TransportSettings& t) { t = saved; });
        app.deleteMixerTrack(ins);
    }
    // ---- take comping: 'use this take here' over the middle of 3 stacked takes splits each at the bounds, plays the clicked take
    // there and mutes the others; outside the range nothing changes; one undo puts the takes back
    {
        auto& pr = app.project();
        Tick bar = pr.transport.timeSig.ticksPerBar();
        AudioSource src;
        src.id = pr.newId();
        src.filePath = "/nonexistent/takes.wav";
        pr.audioSources.push_back(src);
        std::vector<Clip> takes;
        for (int i = 0; i < 3; ++i) {
            Clip c;
            c.id = pr.newId();
            c.type = ClipType::Audio;
            c.trackId = app.addPlaylistTrack();
            c.sourceId = src.id;
            c.length = bar * 4;
            c.sourceOffset = bar * 4 * i;
            c.fadeIn = c.fadeOut = PPQ / 4;
            c.muted = i < 2;   // as recorded: only the newest heard
            pr.clips.push_back(c);
            takes.push_back(c);
        }
        size_t clips0 = pr.clips.size(), undo0 = model.undoStack().count();
        app.useTake(takes[0].id, bar, bar * 2);
        bool parts = pr.clips.size() == clips0 + 6, comped = true;
        for (auto& t : takes) {
            std::vector<const Clip*> v;
            for (auto& c : pr.clips)
                if (c.trackId == t.trackId) v.push_back(&c);
            std::sort(v.begin(), v.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
            if (v.size() != 3) {
                parts = false;
                continue;
            }
            for (auto* c : v) comped = comped && c->sourceOffset - c->start == t.sourceOffset && c->sourceId == src.id;
            comped = comped && v[0]->start == 0 && v[1]->start == bar && v[2]->start == bar * 2 && v[2]->length == bar * 2;
            comped = comped && v[0]->muted == t.muted && v[1]->muted == (t.id != takes[0].id) && v[2]->muted == t.muted;
            comped = comped && v[0]->fadeIn == t.fadeIn && v[0]->fadeOut == 0 && v[1]->fadeIn == 0 && v[1]->fadeOut == 0 &&
                v[2]->fadeIn == 0 && v[2]->fadeOut == t.fadeOut;
        }
        CHECK(parts, "use this take: every overlapping take is split at the selection bounds");
        CHECK(comped, "use this take: the clicked take plays inside the selection, the others are muted there, outside is unchanged");
        model.undoStack().undo();
        bool restored = pr.clips.size() == clips0 && model.undoStack().count() == undo0;
        for (auto& t : takes) restored = restored && pr.clip(t.id) && *pr.clip(t.id) == t;
        CHECK(restored, "use this take: one undo restores the takes");
        for (auto& t : takes) {
            std::erase_if(pr.clips, [&](const Clip& x) { return x.id == t.id; });
            app.deletePlaylistTrack(t.trackId);
        }
        std::erase_if(pr.audioSources, [&](const AudioSource& x) { return x.id == src.id; });
    }
    // ---- sample editor: the edit operations, and Apply repoints the sample (and its sampler channel) to the new file in one undo step
    {
        AudioAsset a;
        a.channels = 2;
        a.frames = 8;
        a.left = {0.1f, 0.2f, -0.5f, 0.25f, 0.f, 0.f, 0.3f, 0.4f};
        a.right = a.left;
        AudioAsset t = a, d = a, n = a, fi = a, fo = a, r = a;
        sampleedit::trim(t, 2, 6);
        CHECK(t.frames == 4 && t.left.size() == 4 && t.right.size() == 4 && t.left[0] == -0.5f && t.right[3] == 0.f,
            "sample editor: trim keeps only the selection");
        sampleedit::erase(d, 2, 6);
        CHECK(
            d.frames == 4 && d.right.size() == 4 && d.left[1] == 0.2f && d.left[2] == 0.3f, "sample editor: delete cuts the selection out");
        sampleedit::normalize(n, 0, 8);
        CHECK(
            std::fabs(n.left[2] + 1.f) < 1e-6f && std::fabs(n.right[0] - 0.2f) < 1e-6f, "sample editor: normalize raises the peak to 0 dB");
        sampleedit::fade(fi, 0, 8, true);
        sampleedit::fade(fo, 4, 8, false);
        CHECK(fi.left[0] == 0.f && fi.left[7] == 0.4f && std::fabs(fi.left[3] - 0.25f * 3 / 7) < 1e-6f && fo.left[7] == 0.f &&
                std::fabs(fo.right[6] - 0.1f) < 1e-6f && fo.left[3] == 0.25f,
            "sample editor: fades ramp from / to silence over the selection only");
        sampleedit::reverse(r, 0, 8);
        CHECK(r.left[0] == 0.4f && r.left[7] == 0.1f && r.right[5] == -0.5f, "sample editor: reverse");
        AudioAsset m;   // mono: 'right' stays empty
        m.channels = 1;
        m.left = {0.f, 0.0001f, 0.5f, -0.5f, 0.f};
        m.frames = 5;
        sampleedit::trimSilence(m, dbToGain(-60.f));
        AudioAsset dc = m;
        sampleedit::removeDc(dc, 0, 1);
        CHECK(m.frames == 2 && m.right.empty() && m.left[0] == 0.5f && dc.left[0] == 0.f && dc.left[1] == -0.5f,
            "sample editor: trim silence and DC removal");

        auto& pr = app.project();
        QTemporaryDir dir;
        std::vector<float> s(200, 0.25f);
        QString orig = dir.path() + "/orig.wav", edited = dir.path() + "/orig edit 1.wav";
        bool written = AudioFile::write(orig, s.data(), 1, 200, 44100, 32, "wav") &&
            AudioFile::write(edited, s.data(), 1, 100, 44100, 32, "wav");
        Id sid = app.ensureAudioSource(orig);
        Id ch = app.addChannel(ChannelType::Sampler, "native.sampler", QString(), orig);
        app.engine().rebuild(pr);
        auto* sampler = dynamic_cast<Sampler*>(app.processor(ch));
        size_t undo0 = model.undoStack().count();
        app.replaceSampleFile(sid, ch, edited);
        CHECK(written && sampler && pr.audioSource(sid)->filePath == edited && pr.audioSource(sid)->frames == 100 &&
                pr.channel(ch)->samplePath == edited && sampler->samplePath() == edited && model.undoStack().count() == undo0 + 1,
            "sample editor: apply repoints the source and the sampler channel to the edited file");
        model.undoStack().undo();
        CHECK(sampler && pr.audioSource(sid)->filePath == orig && pr.audioSource(sid)->frames == 200 &&
                pr.channel(ch)->samplePath == orig && sampler->samplePath() == orig,
            "sample editor: one undo restores the original file");
        app.deleteChannel(ch);
        model.remove<AudioSource>(sid, "");
    }
    // ---- controller lanes and MIDI files: project -> SMF -> project, PPQ conversion, running status, type-0 splitting, song
    // export, and a lane point reaching the instrument at its sample offset
    {
        auto& pr = app.project();
        QTemporaryDir midDir;
        auto note = [](int pitch, Tick start, Tick dur, float vel) {
            Note n;
            n.pitch = pitch;
            n.start = start;
            n.duration = dur;
            n.velocity = vel;
            return n;
        };
        Id lead = app.addChannel(ChannelType::Synth, "native.synth", "Lead");
        Id pat = app.addPattern("MidiRoundTrip");
        std::vector<Note> notes = {note(60, 0, 480, 0.8f), note(64, 480, 240, 0.5f), note(60, 960, 960, 1.f), note(67, 960, 120, 0.3f),
            note(72, 1920, 480, 0.6f), note(72, 2400, 480, 0.7f)};   // a chord, a repeated pitch, back-to-back notes of one pitch
        std::vector<ControlPoint> ctl;
        for (auto& cp : std::vector<ControlPoint>{{0, kCtrlPitchBend, 8192}, {480, kCtrlPitchBend, 16383}, {960, kCtrlPitchBend, 0},
                 {0, 1, 0}, {960, 1, 100}, {480, 64, 127}, {1920, 64, 0}, {240, kCtrlPressure, 50}})
            setControl(ctl, cp.tick, cp.controller, cp.value);   // sorted by tick, as the lanes keep them
        model.edit<Pattern>(pat, "", [&](Pattern& x) {
            x.ensureChannel(lead).notes = notes;
            x.ensureChannel(lead).controls = ctl;
        });
        Project pj;
        CHECK(ProjectSerializer::fromJson(ProjectSerializer::toJson(pr), pj, &err) && pj.pattern(pat)->channelData(lead)->controls == ctl,
            "controller lanes survive save/load");
        QString mid = midDir.path() + "/round.mid";
        size_t undo0 = model.undoStack().count(), chans0 = pr.channels.size();
        Id imported = app.exportMidiFile(mid, pat) ? app.importMidiFile(mid) : 0;
        const Channel* ic = pr.channel(pr.selectedChannelId);
        const PatternChannelData* icd = imported && ic ? pr.pattern(imported)->channelData(ic->id) : nullptr;
        bool same = icd && icd->notes.size() == notes.size();
        for (size_t i = 0; same && i < notes.size(); ++i)
            same = icd->notes[i].pitch == notes[i].pitch && icd->notes[i].start == notes[i].start &&
                icd->notes[i].duration == notes[i].duration && std::fabs(icd->notes[i].velocity - notes[i].velocity) < 0.005f;
        CHECK(same && ic->name == "Lead" && ic->instrumentKey == "native.synth" && pr.channels.size() == chans0 + 1,
            "MIDI export + import gives a synth channel named after the track with the same notes");
        CHECK(icd && icd->controls == ctl, "MIDI export + import keeps CC, sustain, channel pressure and pitch bend points exactly");
        CHECK(model.undoStack().count() == undo0 + 1, "a MIDI import is one undo step");
        model.undoStack().undo();
        CHECK(!pr.pattern(imported) && pr.channels.size() == chans0, "undoing the import removes its pattern and channels");
        // a type-0 file at 480 PPQ written by hand: running status, note-on velocity 0 as note-off, two MIDI channels
        const unsigned char type0[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, 60, 0x00, 0xff,
            0x03, 0x04, 'S', 'o', 'n', 'g',   // track name
            0x00, 0xff, 0x51, 0x03, 0x09, 0x27, 0xc0,   // 600000 us per quarter: 100 BPM
            0x00, 0xff, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08,   // 3/4
            0x00, 0x90, 0x3c, 0x64,   // ch 1: C4 on
            0x00, 0x3e, 0x50,   // running status: D4 on
            0x00, 0x91, 0x30, 0x7f,   // ch 2: C3 on
            0x00, 0xb1, 0x01, 0x40,   // ch 2: mod wheel 64
            0x81, 0x70, 0x80, 0x3c, 0x00,   // 240 ticks later: C4 off
            0x00, 0x90, 0x3e, 0x00,   // D4 note-on velocity 0 = off
            0x81, 0x70, 0xe1, 0x00, 0x60,   // ch 2: pitch bend 12288
            0x00, 0x81, 0x30, 0x40,   // C3 off
            0x00, 0xff, 0x2f, 0x00};
        MidiFileData f;
        bool readOk = MidiFile::read(QByteArray((const char*)type0, sizeof(type0)), f, &err);
        CHECK(readOk && f.hasTempo && std::fabs(f.bpm - 100) < 1e-9 && f.timeSig.numerator == 3 && f.timeSig.denominator == 4,
            "SMF reader picks up tempo and time signature");
        CHECK(readOk && f.tracks.size() == 2 && f.tracks[0].name == "Channel 1" && f.tracks[1].name == "Channel 2",
            "a type-0 file splits into one part per MIDI channel");
        CHECK(f.tracks.size() == 2 && f.tracks[0].notes.size() == 2 && f.tracks[0].notes[1].pitch == 62 &&
                std::fabs(f.tracks[0].notes[1].velocity - 80 / 127.f) < 1e-6f && f.tracks[0].notes[0].duration == 480 &&
                f.tracks[0].notes[1].duration == 480,
            "running status and note-on velocity 0 read as notes (480 PPQ ticks doubled to 960)");
        std::vector<ControlPoint> want = {{0, 1, 64}, {960, kCtrlPitchBend, 12288}};
        CHECK(f.tracks.size() == 2 && f.tracks[1].notes.size() == 1 && f.tracks[1].notes[0].duration == 960 && f.tracks[1].controls == want,
            "CC and pitch bend of a 480 PPQ file land on the converted ticks");
        // song export: pattern clips flattened onto the timeline, a looping clip repeating the pattern
        Tick len = pr.pattern(pat)->effectiveLength(pr.transport.timeSig), at = pr.transport.timeSig.ticksPerBar() * 40;
        Id clip = app.addPatternClip(app.firstPlaylistTrack(), pat, at, len * 2, true);
        MidiFileData song;
        bool songOk = app.exportMidiFile(mid) && MidiFile::load(mid, song, &err);
        auto leadTrack = std::find_if(song.tracks.begin(), song.tracks.end(), [](const MidiFileTrack& t) { return t.name == "Lead"; });
        CHECK(songOk && leadTrack != song.tracks.end() && leadTrack->notes.size() == notes.size() * 2 && leadTrack->notes[0].start == at &&
                leadTrack->notes[notes.size()].start == at + len,
            "song export flattens a looping pattern clip onto the timeline");
        app.deleteClips({clip});
        // the engine: a CC point reaches the instrument (a MIDI out channel, whose output we can read) at its sample offset
        Id out = app.addChannel(ChannelType::MidiOut, "native.midiout", "CcOut");
        model.edit<Pattern>(pat, "", [&](Pattern& x) {
            setControl(x.ensureChannel(out).controls, PPQ, 1, 99);
            setControl(x.ensureChannel(out).controls, PPQ * 2, kCtrlPitchBend, 12000);
        });
        TransportSettings saved = pr.transport;
        model.editTransport("", [](TransportSettings& t) { t.songMode = false; });
        app.selectPattern(pat);
        auto& eng = app.engine();
        const int B = 512;
        std::vector<float> l(B), r(B);
        eng.offlineBegin(44100, B);
        const MidiEventList* outMidi = app.processor(out) ? app.processor(out)->outputMidi() : nullptr;
        eng.stop();
        eng.seek(0);
        eng.play();
        int64_t ccAt = -1, pbAt = -1;
        int ccVal = -1, pbVal = 0;
        for (int b = 0; b < 1000 && outMidi && pbAt < 0; ++b) {
            eng.offlineProcess(l.data(), r.data(), B);
            for (int i = 0; i < outMidi->count; ++i) {
                const MidiEvent& ev = outMidi->events[i];
                if (ev.type == MidiEvent::CC && ev.data1 == 1 && ccAt < 0) ccAt = (int64_t)b * B + ev.sampleOffset, ccVal = ev.data2;
                if (ev.type == MidiEvent::PitchBend && pbAt < 0) pbAt = (int64_t)b * B + ev.sampleOffset, pbVal = ev.pitchBend;
            }
        }
        double spt = 44100 * 60.0 / (pr.transport.bpm * PPQ);
        CHECK(ccVal == 99 && std::llabs(ccAt - (int64_t)(PPQ * spt)) <= 1,
            QString("a CC lane point reaches the instrument at its sample offset (%1, expected %2)")
                .arg(ccAt)
                .arg((int64_t)(PPQ * spt))
                .toUtf8()
                .constData());
        CHECK(pbVal == 12000 - 8192 && std::llabs(pbAt - (int64_t)(2 * PPQ * spt)) <= 1,
            "a pitch-bend lane point arrives as a pitch bend at its sample offset");
        eng.stop();
        eng.seek(PPQ + PPQ / 2);
        eng.play();
        eng.offlineProcess(l.data(), r.data(), B);
        bool chased = false;
        for (int i = 0; outMidi && i < outMidi->count; ++i)
            chased = chased ||
                (outMidi->events[i].type == MidiEvent::CC && outMidi->events[i].data2 == 99 && outMidi->events[i].sampleOffset == 0);
        CHECK(chased, "playback starting after a lane point first sends the held value");
        eng.stop();
        eng.offlineProcess(l.data(), r.data(), B);
        eng.offlineEnd();
        model.editTransport("", [saved](TransportSettings& t) { t = saved; });
        app.deletePattern(pat);
        app.deleteChannel(out);
        app.deleteChannel(lead);
    }
    // ---- piano roll controller lane (FL event editor): a drag draws grid points along the line, a right-drag erases them
    {
        Id cc = app.addChannel(ChannelType::Synth, "native.synth", "LaneTest");
        PianoRollView v(app);
        v.resize(800, 500);
        v.setChannel(cc);
        v.setLane(5);   // mod wheel
        QString lane = v.debugScroll().section("velY=", 1).section(' ', 0, 0);   // "top..bottom"
        int top = lane.section("..", 0, 0).toInt(), bottom = lane.section("..", 1).toInt();
        auto y = [&](float v01) { return (int)std::lround(bottom - 3 - v01 * (bottom - top + 1 - 6)); };
        auto x = [&](Tick t) { return v.debugPoint(t, 60).x() - 2; };   // a pixel right of the tick
        auto send = [&](QEvent::Type type, Qt::MouseButton b, int px, int py) {
            QMouseEvent e(type, QPointF(px, py), v.mapToGlobal(QPointF(px, py)), type == QEvent::MouseMove ? Qt::NoButton : b,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : b, Qt::NoModifier);
            QCoreApplication::sendEvent(&v, &e);
        };
        auto points = [&] { return app.project().pattern(app.project().selectedPatternId)->ensureChannel(cc).controls; };
        size_t undo0 = model.undoStack().count();
        send(QEvent::MouseButtonPress, Qt::LeftButton, x(0), y(0.5f));
        send(QEvent::MouseMove, Qt::LeftButton, x(PPQ), y(1.f));
        send(QEvent::MouseButtonRelease, Qt::LeftButton, x(PPQ), y(1.f));
        auto drawn = points();
        bool line = drawn.size() == 5 && drawn.front().value == 64 && drawn.back().value == 127;
        for (size_t i = 0; line && i < drawn.size(); ++i)
            line = drawn[i].controller == 1 && drawn[i].tick == (Tick)i * PPQ / 4 && (i == 0 || drawn[i].value > drawn[i - 1].value);
        CHECK(line && model.undoStack().count() == undo0 + 1,
            "mod wheel lane: a drag draws a rising line of points on the grid, one undo step");
        send(QEvent::MouseButtonPress, Qt::RightButton, x(PPQ / 4), y(0));
        send(QEvent::MouseMove, Qt::RightButton, x(PPQ * 3 / 4), y(0));
        send(QEvent::MouseButtonRelease, Qt::RightButton, x(PPQ * 3 / 4), y(0));
        auto left = points();
        CHECK(left.size() == 2 && left[0].tick == 0 && left[1].tick == PPQ && model.undoStack().count() == undo0 + 2,
            "a right-drag in the lane erases the points under it");
        app.deleteChannel(cc);
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
    // ---- tempo map: tempo points (jumps and ramps) and time-signature changes along the song
    {
        TempoMap m(120, {{4 * PPQ, 60, false}, {8 * PPQ, 60, true}, {12 * PPQ, 120, false}});   // jump to 60, then glide back to 120
        double ramp = 4 * std::log(2.0);   // 4 beats gliding from 60 to 120 BPM: the integral of 60 / bpm
        bool trip = true;
        for (Tick t : {(Tick)0, (Tick)PPQ * 3, (Tick)PPQ * 4, (Tick)PPQ * 5 + 17, (Tick)PPQ * 9 + 123, (Tick)PPQ * 12, (Tick)PPQ * 20})
            trip = trip && std::fabs(m.tickAt(m.secondsAt((double)t)) - t) < 1e-6;
        CHECK(std::fabs(m.secondsAt(4 * PPQ) - 2) < 1e-9 && std::fabs(m.secondsAt(8 * PPQ) - 6) < 1e-9 &&
                std::fabs(m.secondsAt(12 * PPQ) - (6 + ramp)) < 1e-9 && std::fabs(m.secondsAt(14 * PPQ) - (7 + ramp)) < 1e-9 &&
                std::fabs(m.bpmAt(10 * PPQ) - 90) < 1e-9 && trip,
            "tempo map: ticks <-> seconds through a jump and a linear ramp, and back");
        TempoMap sig(120, {}, {4, 4}, {{2, {7, 8}}});
        CHECK(sig.barStart(2) == 2 * 3840 && sig.barStart(3) == 2 * 3840 + 3360 && sig.barAt(2 * 3840 + 3359) == 2 &&
                sig.barAt(2 * 3840 + 3360) == 3 && sig.sigAt(2 * 3840).numerator == 7,
            "a time-signature change (4/4 -> 7/8 at bar 3) moves every bar line after it");
    }
    {
        auto& pr = app.project();
        auto& eng = app.engine();
        TransportSettings saved = pr.transport;
        pr.transport.bpm = 120;
        pr.transport.songMode = true;
        pr.transport.loopEnabled = pr.transport.metronome = pr.transport.countIn = false;
        // FL resample semantics: a tempo point under an audio clip keeps the clip's audio in seconds; one undo restores both
        Clip a;
        a.id = pr.newId();
        a.type = ClipType::Audio;
        a.length = 8 * PPQ;   // 4 s at 120 BPM
        pr.clips.push_back(a);
        app.editTempo("Add tempo point", [](TransportSettings& t) { t.tempoPoints.push_back({4 * PPQ, 60, false}); });
        bool kept = pr.clip(a.id)->length == 6 * PPQ && pr.transport.tempoPoints.size() == 1;   // 2 s at 120, then 2 s at 60
        app.model().undoStack().undo();
        CHECK(kept && pr.clip(a.id)->length == 8 * PPQ && pr.transport.tempoPoints.empty(),
            "a tempo point re-measures a resample clip under it (same seconds); one undo restores the map and the clip");
        std::erase_if(pr.clips, [&](const Clip& c) { return c.id == a.id; });
        // 1 s at 120 BPM, 2 s at 60, then a ramp back to 120 over two beats; 7/8 from bar 3
        pr.transport.tempoPoints = {{2 * PPQ, 60, false}, {4 * PPQ, 60, true}, {6 * PPQ, 120, false}};
        pr.transport.timeSigChanges = {{2, {7, 8}}};
        Project p7;
        QJsonObject j = ProjectSerializer::toJson(pr);
        CHECK(ProjectSerializer::fromJson(j, p7, &err) &&
                QJsonDocument(ProjectSerializer::toJson(p7)).toJson() == QJsonDocument(j).toJson() &&
                p7.transport.tempoPoints.size() == 3 && p7.transport.tempoPoints[1].ramp && p7.transport.tempoPoints[0].bpm == 60 &&
                p7.transport.timeSigChanges.size() == 1 && p7.transport.timeSigChanges[0].bar == 2 &&
                p7.transport.timeSigChanges[0].sig.denominator == 8,
            "tempo points and time-signature changes survive save/load");
        TempoMap tm = pr.transport.tempoMap();
        // an audio clip across the changes playing a 4 s ramp, so each output sample is the source position played
        QString rampPath = tmp.path() + "/ramp.wav";
        std::vector<float> rampData(176400);
        for (size_t i = 0; i < rampData.size(); ++i) rampData[i] = (float)i / rampData.size();
        AudioFile::write(rampPath, rampData.data(), 1, (int64_t)rampData.size(), 44100, 32, "wav", &err);
        eng.offlineBegin(44100, 512);
        app.cache().getSync(rampPath);
        Id track = app.addPlaylistTrack("Tempo test");
        pr.track(track)->solo = true;   // only this clip plays
        AudioSource src;
        src.id = pr.newId();
        src.filePath = rampPath;
        src.sampleRate = 44100;
        src.channels = 1;
        src.frames = (int64_t)rampData.size();
        pr.audioSources.push_back(src);
        Clip c;
        c.id = pr.newId();
        c.type = ClipType::Audio;
        c.trackId = track;
        c.sourceId = src.id;
        c.length = 12 * PPQ;
        c.loopSource = false;
        pr.clips.push_back(c);
        eng.rebuild(pr);
        std::vector<float> l(512), r(512);
        auto run = [&](int64_t frames) {
            for (int n; frames > 0; frames -= n) eng.offlineProcess(l.data(), r.data(), n = (int)std::min<int64_t>(512, frames));
        };
        eng.seek(0);
        eng.play();
        run(88200 - 1);
        run(1);
        double at2 = eng.positionTicks();
        float out2 = l[0];   // the output sample at 2 s
        run(88200);
        double at4 = eng.positionTicks();
        CHECK(std::fabs(at2 - 3 * PPQ) < 0.01 && std::fabs(at4 - tm.tickAt(4.0)) < 0.01 &&
                std::fabs(eng.transport().bpm - tm.bpmAt(at4)) < 1e-6,
            QString("the transport advances block by block through a jump and a ramp (%1 / %2 ticks after 2 / 4 s)")
                .arg(at2)
                .arg(at4)
                .toUtf8()
                .constData());
        CHECK(std::fabs(out2 - 88199.f / 176400) < 1e-3, "an audio clip plays on at its own speed across a tempo change");
        eng.seek(3 * PPQ);
        run(1);
        CHECK(std::fabs(clipSourceSeconds(c, 3 * PPQ, tm) - 2.0) < 1e-9 && std::fabs(l[0] - 0.5f) < 1e-3,
            QString("an audio clip entered after the change plays the source position the map gives (2 s at beat 4: %1)")
                .arg(l[0] * 4)
                .toUtf8()
                .constData());
        eng.stop();
        eng.seek(2 * 3840 + 3360 + 480);
        run(1);
        auto tv = eng.transport();
        CHECK(tv.bar == 4 && tv.beat == 2 && tv.tick == 0 && tv.tsNum == 7,
            "the BBT position follows the time-signature change (4:2:000 in 7/8)");
        // the transport bar BPM edits the tempo in effect at the playhead (here the jump at beat 2), the initial tempo before it
        eng.seek(3 * PPQ);
        run(1);
        app.setBpm(90);
        bool point = pr.transport.tempoPoints[0].bpm == 90 && pr.transport.bpm == 120;
        eng.seek(PPQ);
        run(1);
        app.setBpm(100);
        CHECK(point && pr.transport.bpm == 100 && pr.transport.tempoPoints[0].bpm == 90,
            "with a tempo map the BPM box edits the point in effect at the playhead (or the initial tempo)");
        eng.offlineEnd();
        std::erase_if(pr.clips, [&](const Clip& x) { return x.id == c.id; });
        std::erase_if(pr.audioSources, [&](const AudioSource& x) { return x.id == src.id; });
        std::erase_if(pr.playlistTracks, [&](const PlaylistTrack& x) { return x.id == track; });
        pr.transport = saved;
        eng.rebuild(pr);
    }
    {   // the channel rack's play position in a pattern clip follows the engine: clip length, repeat or play once
        Project pr;
        Pattern pat;
        pat.id = pr.newId();
        pat.stepCount = 16;   // empty: one bar
        pr.patterns.push_back(pat);
        Clip c;
        c.id = pr.newId();
        c.type = ClipType::Pattern;
        c.sourceId = pat.id;
        c.start = 4 * PPQ;
        c.length = 0;   // follows the pattern: one bar
        Tick bar = 4 * PPQ;
        bool follows = pr.patternPosAt(c, 4 * PPQ + PPQ) == PPQ && pr.patternPosAt(c, 4 * PPQ + bar) == -1 && pr.patternPosAt(c, 0) == -1;
        c.length = 4 * bar;
        c.loopSource = true;
        pr.clips = {c};
        bool repeats = pr.patternPosAt(c, 4 * PPQ + 2 * bar + PPQ) == PPQ;
        bool kept = pr.patternLength(pr.patterns[0]) == bar;   // a repeating clip leaves the pattern one bar long
        c.loopSource = false;
        pr.clips = {c};
        bool stretched = pr.patternLength(pr.patterns[0]) == 4 * bar && pr.patternPosAt(c, 4 * PPQ + 2 * bar + PPQ) == 2 * bar + PPQ &&
            pr.patternPosAt(c, 4 * PPQ + 4 * bar) == -1;
        CHECK(follows && repeats && kept && stretched,
            "a pattern's play position: in a clip of its own length and a repeating clip; a clip stretched to 4 bars that plays it "
            "once makes the pattern 4 bars");
    }
    // ---- song mode with no loop area repeats the song from its start at the last clip's whole bar; an export runs straight on
    {
        auto& pr = app.project();
        auto& eng = app.engine();
        TransportSettings saved = pr.transport;
        auto savedClips = pr.clips;
        pr.transport.bpm = 120;
        pr.transport.songMode = true;
        pr.transport.loopEnabled = pr.transport.metronome = pr.transport.countIn = false;
        pr.transport.tempoPoints.clear();
        pr.transport.timeSigChanges.clear();
        Clip a;
        a.id = pr.newId();
        a.type = ClipType::Audio;
        a.length = 6 * PPQ;   // one and a half bars: the song loops at the end of bar 2
        pr.clips = {a};
        eng.rebuild(pr);
        eng.offlineBegin(44100, 512);
        std::vector<float> l(512), r(512);
        auto run = [&](int64_t frames) {   // 120 BPM: one beat is 22050 frames
            for (int n; frames > 0; frames -= n) eng.offlineProcess(l.data(), r.data(), n = (int)std::min<int64_t>(512, frames));
        };
        eng.seek(6 * PPQ);
        eng.play();
        run(3 * 22050);
        double looped = eng.positionTicks();
        eng.stop();
        eng.setSongLoop(false);
        eng.seek(6 * PPQ);
        eng.play();
        run(3 * 22050);
        double straight = eng.positionTicks();
        eng.stop();
        eng.setSongLoop(true);
        eng.offlineEnd();
        CHECK(std::fabs(looped - PPQ) < 2 && straight > 8 * PPQ,
            QString(
                "song mode loops at the end of the last clip's bar (%1 ticks after running 3 beats from beat 6); an export does not (%2)")
                .arg(looped)
                .arg(straight)
                .toUtf8()
                .constData());
        pr.clips = savedClips;
        pr.transport = saved;
        eng.rebuild(pr);
    }
    // ---- the playlist tempo lane: double-click adds a point, dragging it up raises its BPM, right-click > Delete removes it
    {
        auto& pr = app.project();
        TransportSettings saved = pr.transport;
        pr.transport.bpm = 120;
        pr.transport.tempoPoints.clear();
        PlaylistView v(app);
        v.resize(1200, 500);
        int x = v.debugPoint(0, 4 * PPQ).x() - 2, y = 34;   // the tempo lane runs under the 24 px ruler
        auto mouse = [&](QEvent::Type t, int px, int py, Qt::MouseButton b) {
            QMouseEvent e(
                t, QPointF(px, py), v.mapToGlobal(QPointF(px, py)), b, t == QEvent::MouseButtonRelease ? Qt::NoButton : b, Qt::NoModifier);
            QCoreApplication::sendEvent(&v, &e);
        };
        mouse(QEvent::MouseButtonPress, 40, 12, Qt::LeftButton);   // the lane starts closed: "Tempo" beside the ruler opens it
        mouse(QEvent::MouseButtonRelease, 40, 12, Qt::LeftButton);
        mouse(QEvent::MouseButtonDblClick, x + 10, y, Qt::LeftButton);   // snaps to the bar line at beat 4
        bool added = pr.transport.tempoPoints.size() == 1 && pr.transport.tempoPoints[0].tick == 4 * PPQ;
        double b0 = added ? pr.transport.tempoPoints[0].bpm : 0;
        mouse(QEvent::MouseButtonPress, x, y, Qt::LeftButton);
        mouse(QEvent::MouseMove, x, y - 20, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, x, y - 20, Qt::LeftButton);
        bool raised = added && pr.transport.tempoPoints[0].bpm == b0 + 10 && pr.transport.tempoPoints[0].tick == 4 * PPQ;
        app.model().undoStack().undo();
        bool undone = pr.transport.tempoPoints.size() == 1 && pr.transport.tempoPoints[0].bpm == b0;
        QTimer::singleShot(0, [] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (auto* act : m->actions())
                    if (act->text() == "Delete tempo point") act->trigger();
                m->close();
            }
        });
        mouse(QEvent::MouseButtonPress, x, y, Qt::RightButton);
        CHECK(added && raised && undone && pr.transport.tempoPoints.empty(),
            "tempo lane: double-click adds a point on the bar, dragging it up raises its BPM (one undo step), right-click deletes it");
        QTimer::singleShot(0, [] {   // ruler right-click > Insert time signature change, typed into its prompt
            auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!m) return;
            QTimer::singleShot(0, [] {
                if (auto* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    d->setTextValue("7/8");
                    d->accept();
                }
            });
            for (auto* act : m->actions())
                if (act->text().startsWith("Insert time signature change")) act->trigger();
            m->close();
        });
        mouse(QEvent::MouseButtonPress, x + 10, 10, Qt::RightButton);
        auto& sc = pr.transport.timeSigChanges;
        bool inserted = sc.size() == 1 && sc[0].bar == 1 && sc[0].sig.numerator == 7 && sc[0].sig.denominator == 8;
        app.model().undoStack().undo();
        CHECK(inserted && sc.empty(), "ruler right-click inserts a time signature change at the nearest bar; undo removes it");
        pr.transport = saved;
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
    // ---- one name prompt for every rename / save-as: trimmed, empty refused unless allowed
    {
        auto answer = [](const QString& typed) {   // types into the dialog as soon as it opens, then presses OK
            QTimer::singleShot(0, [typed] {
                if (auto* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    d->setTextValue(typed);
                    d->accept();
                }
            });
        };
        answer("  Bass  ");
        auto a1 = askName(nullptr, "t", "x");
        answer("");
        auto a2 = askName(nullptr, "t", "x");
        answer("   ");
        auto a3 = askName(nullptr, "t", "x", true);
        CHECK(a1 == QString("Bass") && !a2 && a3 == QString(""), "name prompt trims, refuses empty names unless allowed");
        QMenu one, two;
        addPatternActions(&one, app, nullptr, {app.project().patterns.front().id});
        addPatternActions(&two, app, nullptr, {app.project().patterns.front().id, app.project().patterns.front().id});
        CHECK(one.actions().size() == 4 && two.actions().size() == 2 && two.actions().front()->text() == "Color... (2)",
            "pattern menus: rename / split / color / clone for one, color / clone for several");
    }
    // ---- the shared mixer-track picker menu (rack and playlist routing)
    {
        auto& tracks = app.project().mixerTracks;
        Id cur = tracks.size() > 2 ? tracks[2].id : tracks.back().id, picked = 0;
        QMenu m;
        fillMixerTrackMenu(&m, app.project(), cur, [&](Id id) { picked = id; });
        auto acts = m.actions();
        bool ok = acts.size() == (int)tracks.size();
        for (int i = 0; ok && i < acts.size(); ++i) ok = acts[i]->text() == tracks[i].name && acts[i]->isChecked() == (tracks[i].id == cur);
        if (ok) acts.last()->trigger();
        CHECK(ok && picked == tracks.back().id, "mixer-track menu lists every track, checks the current one, reports the pick");
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
        app.edit<MixerTrack>(t1, "Rename", [](MixerTrack& x) {
            x.name = "Strings bus";
            x.customName = true;
        });
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
        CHECK(heard > 1e-4f && tail < 1e-6f, "live tap (note-on + note-off in one block) sounds and releases");
        // a held note is released by Stop, and by Panic (both go through AudioEngine::releaseAllNotes)
        for (int how = 0; how < 2; ++how) {
            app.noteOn(60);
            float held = blocks(20);
            if (how == 0) eng.stop();
            else eng.allNotesOff();
            blocks(400);
            float after = blocks(4);
            CHECK(held > 1e-4f && after < 1e-6f, how == 0 ? "Stop releases a held note" : "Panic releases a held note");
            app.noteOff(60);
            blocks(1);
        }
        eng.offlineEnd();
        app.selectChannel(prevSel);
        app.deleteChannel(sid);
    }
    // ---- sample cache & waveform peaks
    {
        auto a = DrumSampler::synthesizeDrum("Kick", 44100);
        CHECK(a->peaks.size() == 4 && a->levelFor(100).samplesPerBin == 64 && a->levelFor(5000).samplesPerBin == 4096,
            "multiresolution waveform cache levels");
    }
    // ---- CLAP hosting: build the test gain plugin (tests/clap_gain.cpp), scan it out of process, run it, save and restore it
    {
        QString root = QCoreApplication::applicationDirPath() + "/..", src = root + "/tests/clap_gain.cpp";
        QString cxx = QStandardPaths::findExecutable("c++");
        QTemporaryDir dir;
        QString so = dir.path() + "/clap/MT Test Gain.clap";
        QDir().mkpath(dir.path() + "/clap");
        if (cxx.isEmpty() || !QFileInfo::exists(src)) std::printf("  skip CLAP hosting checks (no c++ compiler or tests/clap_gain.cpp)\n");
        else if (QProcess::execute(cxx, {"-std=c++17", "-shared", "-fPIC", "-I" + root + "/src", src, "-o", so, kGuiLib}) != 0)
            CHECK(false, "compile the CLAP test plugin");
        else {
            CHECK(PluginScanner::candidateFiles({dir.path()}).contains(so), "plugin scan finds .clap files");
            QProcess scan;
            scan.start(QCoreApplication::applicationFilePath(), {"--scan-plugin", so});
            scan.waitForFinished(30000);
            QJsonObject o = QJsonDocument::fromJson(scan.readAllStandardOutput().split('\n').first()).object();
            CHECK(
                o["format"].toString() == "clap" && o["uid"].toString() == "org.vybrant.test-gain" && o["category"].toString() == "Effect",
                "out-of-process scan reports the CLAP plugin");
            PluginInfo info;
            info.format = "clap";
            info.uid = "org.vybrant.test-gain";
            info.name = "MT Test Gain";
            info.path = so;
            PluginHost host(app.pluginDb());
            PluginInstanceData d = PluginHost::makeInstanceData(info, 1);
            auto proc = host.createInstance(d);
            CHECK(proc && proc->kind() == ProcessorKind::Effect && proc->params().count() == 1,
                "CLAP plugin instantiates with its parameter");
            auto run = [&](IProcessor& p) {
                std::fill(L.begin(), L.end(), 0.5f);
                std::fill(R.begin(), R.end(), 0.5f);
                midi.clear();
                p.process(L.data(), R.data(), 512, midi, ctx);
                return L[100];
            };
            if (proc) {
                proc->prepare(44100, 512);
                CHECK(proc->latencySamples() == 64, "CLAP plugin reports its latency");
                uint32_t gain = proc->params().desc(0).id;
                proc->params().set(gain, 0.5f);
                CHECK(std::fabs(run(*proc) - 0.25f) < 1e-6f, "CLAP plugin processes audio with the parameter applied");
                d.state = QByteArray::fromBase64(proc->saveState()["pluginState"].toString().toLatin1());
                auto copy = host.createInstance(d);
                if (copy) copy->prepare(44100, 512);
                CHECK(copy && std::fabs(copy->params().get(gain) - 0.5f) < 1e-6f && std::fabs(run(*copy) - 0.25f) < 1e-6f,
                    "CLAP plugin state round-trips");
                // a latency change makes the plugin request a restart: handed over by the audio thread while blocks run,
                // taken over by the UI thread when none come
                auto restarted = [&](float g, int lat, bool blocks) {
                    proc->params().set(gain, g);
                    run(*proc);
                    proc->latencyChanged();
                    for (int i = 0; i < 40 && proc->latencySamples() != lat; ++i) {
                        QThread::msleep(20);
                        QCoreApplication::processEvents();
                        if (blocks) run(*proc);
                    }
                    return proc->latencySamples() == lat && proc->latencyChanged() && std::fabs(run(*proc) - 0.5f * g) < 1e-6f;
                };
                CHECK(restarted(1.8f, 128, true), "CLAP restart request re-activates the plugin while it is processing");
                CHECK(restarted(1.f, 64, false), "CLAP restart request re-activates the plugin when no blocks come");
            }
            // multi-out: the extra output pair of an instrument plays on a mixer track of its own
            PluginInfo mi = info;
            mi.uid = "org.vybrant.test-multi";
            mi.name = "MT Test Multi";
            mi.category = "Instrument";
            PluginInstanceData md = PluginHost::makeInstanceData(mi, app.project().newId());
            auto multi = host.createInstance(md);
            if (multi) multi->prepare(44100, 512);
            float mainOut = multi ? run(*multi) : 0.f;
            CHECK(multi && multi->kind() == ProcessorKind::Instrument && multi->auxOutputCount() == 1 &&
                    multi->auxOutputName(0) == "Kick" && std::fabs(mainOut - 0.25f) < 1e-6f &&
                    std::fabs(multi->auxOutput(0, 1)[100] - 0.5f) < 1e-6f,
                "a multi-out instrument shows its extra output pair next to the main one");
            model.insert<PluginInstanceData>(md, "");
            Id ch = app.addChannel(ChannelType::Plugin, "plugin", "Multi", QString(), md.id);
            app.engine().rebuild(app.project());
            app.routeAuxOutputsToFreeTracks(ch);
            const Channel* mc = app.project().channel(ch);
            Id mainT = mc->mixerTrackId, auxT = mc->auxOutputTracks.empty() ? 0 : mc->auxOutputTracks[0];
            auto level = [&](Id track) {
                RenderOptions o;
                o.path = dir.path() + "/tap.wav";
                o.tapTrack = track;
                o.tailSeconds = 0;
                o.bitDepth = 32;
                o.dither = false;
                app.exportAudio(o, nullptr, &err);
                auto a = AudioFile::load(o.path, 0);
                return a && a->frames > 1000 ? a->L()[a->frames / 2] : -1.f;
            };
            float lm = level(mainT), la = level(auxT);
            CHECK(auxT && auxT != mainT && app.project().mixerTrack(auxT)->name == "Multi Kick" && std::fabs(lm - 0.25f) < 1e-4f &&
                    std::fabs(la - 0.5f) < 1e-4f,
                QString("the extra output plays on its own mixer track, named after it (main %1, Kick %2)")
                    .arg(lm)
                    .arg(la)
                    .toUtf8()
                    .constData());
            // freezing the insert the extra output plays on: its render plays there once, the main output stays live
            QString oldPath = model.filePath(), fe;
            model.setFilePath(dir.path() + "/f.mtp");
            bool fz = app.freezeMixerTrack(auxT, nullptr, &fe);
            float la2 = level(auxT), lm2 = level(mainT);
            CHECK(fz && std::fabs(la2 - 0.5f) < 1e-3f && std::fabs(lm2 - 0.25f) < 1e-4f,
                QString("freezing an extra output's insert plays its render once, the main output stays live (%1, %2)")
                    .arg(la2)
                    .arg(lm2)
                    .toUtf8()
                    .constData());
            app.unfreezeMixerTrack(auxT);
            model.setFilePath(oldPath);
            app.deleteChannel(ch);
#ifndef _WIN32   // no own-process plugins on Windows yet
            // out of process: the same plugins through the bridge; a crash silences only the plugin
            PluginInstanceData bd = d;
            bd.bridged = true;
            bd.state.clear();
            auto bp = host.createInstance(bd);
            auto* bridged = dynamic_cast<BridgedPlugin*>(bp.get());
            if (bp) bp->prepare(44100, 512);
            CHECK(bridged && bp->params().count() == 1 && bp->latencySamples() == 64,
                "a bridged plugin starts in its own process with its parameter and latency");
            if (bridged) {
                uint32_t gain = bp->params().desc(0).id;
                bp->params().set(gain, 0.5f);
                CHECK(std::fabs(run(*bp) - 0.25f) < 1e-6f, "it processes audio in the other process with the parameter applied");
                bd.state = QByteArray::fromBase64(bp->saveState()["pluginState"].toString().toLatin1());
                auto copy = host.createInstance(bd);
                if (copy) copy->prepare(44100, 512);
                CHECK(copy && std::fabs(copy->params().get(gain) - 0.5f) < 1e-6f && std::fabs(run(*copy) - 0.25f) < 1e-6f,
                    "a bridged plugin's state round-trips");
                PluginInstanceData bm = md;
                bm.bridged = true;
                auto bmulti = host.createInstance(bm);
                if (bmulti) bmulti->prepare(44100, 512);
                float bmain = bmulti ? run(*bmulti) : 0.f;
                CHECK(bmulti && bmulti->auxOutputCount() == 1 && std::fabs(bmain - 0.25f) < 1e-6f &&
                        std::fabs(bmulti->auxOutput(0, 0)[100] - 0.5f) < 1e-6f,
                    "a bridged multi-out instrument hands over its extra outputs");
                QString crashedName;
                auto prevHandler = BridgedPlugin::onCrashed;
                BridgedPlugin::onCrashed = [&](Id, const QString& n) { crashedName = n; };
                bp->params().set(gain, 1.99f);   // the test plugin crashes on purpose
                float out = run(*bp);
                for (int i = 0; i < 100 && crashedName.isEmpty(); ++i) {
                    QThread::msleep(10);
                    QCoreApplication::processEvents();
                }
                CHECK(bridged->dead() && out == 0.f && run(*bp) == 0.f && crashedName == "MT Test Gain" &&
                        std::fabs(run(*copy) - 0.25f) < 1e-6f,
                    "a crashing plugin only silences itself: the app and the other plugins keep running");
                BridgedPlugin::onCrashed = prevHandler;
                // in a project: the window's 'Own process' toggle rebuilds it bridged; the engine renders through the bridge
                PluginInstanceData pm = PluginHost::makeInstanceData(mi, app.project().newId());
                model.insert<PluginInstanceData>(pm, "");
                Id bch = app.addChannel(ChannelType::Plugin, "plugin", "Multi", QString(), pm.id);
                app.setPluginBridged(pm.id, true);
                app.engine().rebuild(app.project());
                app.routeAuxOutputsToFreeTracks(bch);
                const Channel* bc = app.project().channel(bch);
                Id bt = bc->auxOutputTracks.empty() ? 0 : bc->auxOutputTracks[0];
                float bl = level(bc->mixerTrackId), ba = level(bt);
                CHECK(dynamic_cast<BridgedPlugin*>(app.processor(bch)) && std::fabs(bl - 0.25f) < 1e-4f && std::fabs(ba - 0.5f) < 1e-4f,
                    QString("'Own process' rebuilds the plugin bridged and the song renders through it (main %1, Kick %2)")
                        .arg(bl)
                        .arg(ba)
                        .toUtf8()
                        .constData());
                app.deleteChannel(bch);
            }
#endif
        }
    }
#ifdef _WIN32
    {   // WinMM: a CC sent out comes back in unchanged through a loopback device (under Wine: ALSA's "Midi Through")
        MidiEngine& midi = app.midi();
        QString in, out;
        for (auto& p : midi.ports())
            if (p.name.contains("Midi Through")) (p.input ? in : out) = p.id();
        if (in.isEmpty() || out.isEmpty()) std::printf("  skip WinMM loopback check (no MIDI loopback device)\n");
        else {
            const QStringList oldIn = midi.enabledInputs(), oldOut = midi.enabledOutputs();
            int got[3] = {-1, -1, -1};
            auto conn = QObject::connect(
                &midi, &MidiEngine::ccReceived, [&](int cc, int ch, int v) { got[0] = cc, got[1] = ch, got[2] = v; });
            midi.setEnabledInputs({in});
            midi.setEnabledOutputs({out});
            midi.setLearning(true);
            MidiEvent e;
            e.type = MidiEvent::CC, e.channel = 3, e.data1 = 74, e.data2 = 99;
            app.engine().midiOutQueue().push({e, 0, 0});   // the transport is stopped: nothing else feeds the queue now
            QElapsedTimer t;
            t.start();
            while (got[0] < 0 && t.elapsed() < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            CHECK(got[0] == 74 && got[1] == 3 && got[2] == 99, "a CC goes out and comes back in through WinMM (loopback device)");
            midi.setLearning(false);
            QObject::disconnect(conn);
            midi.setEnabledInputs(oldIn);
            midi.setEnabledOutputs(oldOut);
        }
    }
#endif
    {   // four effect / synth bugs stay fixed: Vibrato's right channel follows the Shape, Tremolo's Smooth softens a square's
        // steps, Echo's left-panned taps carry a right-only source, FM Synth's Glide slides from the previous note
        const int B = 512;
        MidiEventList none;
        ProcessContext pc;
        std::vector<float> L(B), R(B);
        auto run = [&](IProcessor& p, int blocks, auto fill, auto look) {
            p.prepare(44100, B);
            for (int b = 0, n = 0; b < blocks; ++b) {
                for (int i = 0; i < B; ++i, ++n) fill(n, L[i], R[i]);
                p.process(L.data(), R.data(), B, none, pc);
                for (int i = 0; i < B; ++i) look(L[i], R[i]);
            }
        };
        Vibrato vib;
        vib.params().set(Vibrato::pShape, 3);   // square, no stereo offset: both sides must move together
        float vibDiff = 0;
        run(
            vib, 40, [](int n, float& l, float& r) { l = r = std::sin(n * 0.05f); },
            [&](float l, float r) { vibDiff = std::max(vibDiff, std::fabs(l - r)); });
        auto tremoloJump = [&](float smooth) {   // the biggest sample-to-sample step on a steady signal
            Tremolo tr;
            tr.params().set(Tremolo::pShape, 3), tr.params().set(Tremolo::pDepth, 1), tr.params().set(Tremolo::pSmooth, smooth);
            float prev = 1, jump = 0;
            run(
                tr, 60, [](int, float& l, float& r) { l = r = 1; },
                [&](float l, float) {
                    jump = std::max(jump, std::fabs(l - prev));
                    prev = l;
                });
            return jump;
        };
        Echo echo;
        echo.params().set(Echo::pSync, 0), echo.params().set(Echo::pTime, 50), echo.params().set(Echo::pSpread, 1),
            echo.params().set(Echo::pMix, 1);
        float echoLeft = 0;
        run(
            echo, 40, [](int n, float& l, float& r) { l = 0, r = n == 0 ? 1.f : 0.f; },
            [&](float l, float) { echoLeft = std::max(echoLeft, std::fabs(l)); });
        FMSynth fm;   // op 1 alone in the additive algorithm: a plain sine at the note's pitch
        fm.params().set(FMSynth::pAlgorithm, 6), fm.params().set(FMSynth::pGlide, 0.5f);
        for (int o = 1; o < 4; ++o) fm.params().set(FMSynth::opParam(o, 1), 0);
        fm.prepare(44100, B);
        std::vector<float> out;
        for (int b = 0; b < 40; ++b) {   // note 48, then 60 from block 20: with glide the new note starts near 48's pitch
            MidiEventList ev;
            MidiEvent on;
            on.data1 = b == 0 ? 48 : 60, on.velocity = 1;
            if (b == 0 || b == 20) ev.add(on);
            std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
            fm.process(L.data(), R.data(), B, ev, pc);
            if (b >= 20) out.insert(out.end(), L.begin(), L.end());
        }
        int crossings = 0;   // over the first 50 ms of note 60
        for (int i = 1; i < 2205; ++i) crossings += (out[i - 1] < 0) != (out[i] < 0);
        const double hz = crossings / 2.0 / 0.05;
        CHECK(vibDiff < 1e-6f && tremoloJump(1) < 0.02f && tremoloJump(0) > 0.5f && echoLeft > 0.01f && hz < 200,
            QString("Vibrato R follows the shape (%1), Tremolo Smooth softens steps (%2 vs %3), Echo carries R into L (%4), FM glides "
                    "(starts at %5 Hz)")
                .arg(vibDiff)
                .arg(tremoloJump(1))
                .arg(tremoloJump(0))
                .arg(echoLeft)
                .arg(hz)
                .toUtf8()
                .constData());
    }
    {   // our FFT matches a direct DFT, and a sine lands in its own bin
        const int n = 64;
        std::vector<float> x(n);
        for (int i = 0; i < n; ++i) x[i] = std::sin(0.37f * i * i) + 0.5f * std::cos(2.f * i);   // nothing periodic
        std::vector<std::complex<float>> got(n / 2 + 1);
        dsp::Fft(n).forwardReal(x.data(), got.data());
        double err = 0;
        for (int k = 0; k <= n / 2; ++k) {
            std::complex<double> want;
            for (int i = 0; i < n; ++i) want += (double)x[i] * std::polar(1.0, -2 * M_PI * k * i / n);
            err = std::max(err, std::abs(want - std::complex<double>(got[k])));
        }
        const int N = 2048, bin = 100;
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i) s[i] = std::sin(2 * M_PI * bin * i / N);
        std::vector<std::complex<float>> spec(N / 2 + 1);
        dsp::Fft(N).forwardReal(s.data(), spec.data());
        int peak = 0;
        for (int k = 1; k <= N / 2; ++k)
            if (std::abs(spec[k]) > std::abs(spec[peak])) peak = k;
        CHECK(err < 1e-3 && peak == bin && std::abs(std::abs(spec[bin]) - N / 2.0) < 0.01 * N,
            QString("FFT matches a direct DFT (max error %1) and puts a sine in its bin").arg(err).toUtf8().constData());
    }
    {   // every native processor with a registered editor builds it, sized and drawable; the others report none (generic knobs)
        QStringList broken;
        int custom = 0;
        for (auto& info : NativeRegistry::instance().all()) {
            auto proc = NativeRegistry::instance().create(info.key);
            if (!proc) continue;
            proc->prepare(44100, 512);
            const bool registered = nativeEditors().count(info.key) > 0;
            custom += registered;
            std::unique_ptr<QWidget> ed(proc->createEditor(nullptr));
            if (registered != proc->hasCustomEditor() || registered != (ed != nullptr)) broken << info.key;
            else if (ed) {
                ed->resize(ed->sizeHint().expandedTo(QSize(300, 200)));
                if (ed->grab().isNull()) broken << info.key;
            }
        }
        CHECK(broken.isEmpty() && custom >= 5,
            QString("native editors build for every registered processor (%1 custom; broken: %2)")
                .arg(custom)
                .arg(broken.join(", "))
                .toUtf8()
                .constData());
    }
    {   // Qt's own buttons get the hand once polished, a button that picked its own cursor keeps it; the usage readout reads the OS
        QPushButton plain, own;
        own.setCursor(Qt::SizeVerCursor);
        plain.ensurePolished();
        own.ensurePolished();
        ProcessUsage u = processUsage();
        CHECK(plain.cursor().shape() == Qt::PointingHandCursor && own.cursor().shape() == Qt::SizeVerCursor && u.residentBytes > 1e6 &&
                u.cpuSeconds > 0,
            "buttons show the hand cursor, and the process's CPU time and memory are read");
    }
    {   // the Parametric EQ's curve: dragging a band's handle moves that band and switches it on
        Id t1 = app.project().mixerTracks[1].id;
        Id slot = app.addEffect(t1, "native.parametriceq");
        app.engine().rebuild(app.project());
        IProcessor* eq = app.processor(slot);
        CHECK(eq && eqHandleDragSelfTest(eq), "dragging a band handle on the Parametric EQ curve moves its freq and gain and turns it on");
        app.removeEffect(t1, slot);
        app.engine().rebuild(app.project());
    }
    {   // the delay and modulation editors are played on their display: a delay's first echo is dragged later and louder (time
        // and mix), a tremolo's wave wider and taller (rate and depth)
        const Id t1 = app.project().mixerTracks[1].id;
        auto mouse = [](QWidget* w, QEvent::Type type, QPointF at, Qt::MouseButtons held) {
            QMouseEvent e(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &e);
        };
        auto drag = [&](QWidget* w, QPointF from, QPointF to) {
            mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton);
            mouse(w, QEvent::MouseMove, to, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton);
        };
        const Id dly = app.addEffect(t1, "native.delay"), trem = app.addEffect(t1, "native.tremolo");
        app.engine().rebuild(app.project());
        IProcessor *d = app.processor(dly), *tr = app.processor(trem);
        bool delayOk = false, tremOk = false;
        if (d && tr) {
            d->params().set(Delay::pSync, 0);   // free time: a sideways drag sets milliseconds
            std::unique_ptr<QWidget> ed(d->createEditor(nullptr));
            ed->resize(ed->sizeHint());
            ed->show();
            QWidget* view = ed->findChild<QWidget*>("timeDisplay");
            const double y = view->height() * 0.3;   // in the left lane, above the centre line
            auto echoFrom = [&](int from) {   // hover along the lane until the pointer is over an echo
                for (int x = from; x < view->width(); ++x) {
                    mouse(view, QEvent::MouseMove, QPointF(x, y), Qt::NoButton);
                    if (view->cursor().shape() == Qt::SizeAllCursor) return x + 10.0;
                }
                return -1.0;
            };
            const float time = d->params().get(Delay::pTime), mix = d->params().get(Delay::pMix), fb = d->params().get(Delay::pFeedback);
            const double x1 = echoFrom(30);
            if (x1 > 0) drag(view, QPointF(x1, y), QPointF(x1 + 40, y - 30));
            const double x2 = x1 > 0 ? echoFrom((int)x1 + 52) : -1;   // the second echo: its height is the feedback
            if (x2 > 0) drag(view, QPointF(x2, y), QPointF(x2, y - 20));
            delayOk = x2 > 0 && d->params().get(Delay::pTime) > time + 20 && d->params().get(Delay::pMix) > mix + 0.05f &&
                d->params().get(Delay::pFeedback) > fb + 0.05f;

            std::unique_ptr<QWidget> ed2(tr->createEditor(nullptr));
            ed2->resize(ed2->sizeHint());
            ed2->show();
            QWidget* lfo = ed2->findChild<QWidget*>("timeDisplay");
            const float rate = tr->params().get(Tremolo::pRate), depth = tr->params().get(Tremolo::pDepth);
            drag(lfo, QPointF(lfo->width() * 0.2, lfo->height() * 0.35), QPointF(lfo->width() * 0.3, lfo->height() * 0.2));
            tremOk = tr->params().get(Tremolo::pRate) < rate * 0.9f && tr->params().get(Tremolo::pDepth) > depth + 0.1f;
        }
        app.removeEffect(t1, trem);
        app.removeEffect(t1, dly);
        app.engine().rebuild(app.project());
        CHECK(delayOk, "dragging a delay's first echo later and higher sets time and mix; raising the second echo raises the feedback");
        CHECK(tremOk, "dragging a tremolo's wave wider and taller sets a slower rate and more depth");
    }
    {   // the Saturation editor's curve is a control: dragging it up raises Drive and leaves Bias where it was
        const Id track = app.project().mixerTracks[1].id, slot = app.addEffect(track, "native.saturation");
        app.engine().rebuild(app.project());
        bool ok = false;
        if (IProcessor* sat = app.processor(slot)) {
            const float drive = sat->params().get(Saturation::pDrive), bias = sat->params().get(Saturation::pBias);
            std::unique_ptr<QWidget> ed(sat->createEditor(nullptr));
            ed->resize(ed->sizeHint());
            ed->show();
            QCoreApplication::processEvents();
            if (QWidget* d = ed->findChild<QWidget*>("display")) {
                auto send = [d](QEvent::Type type, QPointF at, Qt::MouseButtons held) {
                    QMouseEvent e(type, at, d->mapToGlobal(at), Qt::LeftButton, held, Qt::NoModifier);
                    QCoreApplication::sendEvent(d, &e);
                };
                const QPointF at(d->height() / 2.0, d->height() / 2.0);   // the middle of the curve panel
                send(QEvent::MouseButtonPress, at, Qt::LeftButton);
                send(QEvent::MouseMove, at - QPointF(0, 60), Qt::LeftButton);
                send(QEvent::MouseButtonRelease, at - QPointF(0, 60), Qt::NoButton);
                ok = sat->params().get(Saturation::pDrive) > drive + 0.1f && sat->params().get(Saturation::pBias) == bias;
            }
        }
        CHECK(ok, "dragging up on the Saturation editor's curve raises Drive and leaves Bias alone");
        app.removeEffect(track, slot);
        app.engine().rebuild(app.project());
    }
    {   // the Synth editor's displays edit by mouse: an envelope point, the filter curve and a click on an oscillator's shape
        Id ch = app.addChannel(ChannelType::Synth, "native.synth", "EditorDrag");
        app.engine().rebuild(app.project());
        IProcessor* proc = app.processor(ch);
        std::unique_ptr<QWidget> ed(proc ? proc->createEditor(nullptr) : nullptr);
        auto mouse = [](QWidget* w, QPointF from, QPointF to) {   // press, move, release
            QMouseEvent press(QEvent::MouseButtonPress, from, w->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent move(QEvent::MouseMove, to, w->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, to, w->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &press);
            if (from != to) QCoreApplication::sendEvent(w, &move);
            QCoreApplication::sendEvent(w, &release);
        };
        bool env = false, filter = false, shape = false;
        if (ed) {
            ed->show();
            QCoreApplication::processEvents();
            auto& ps = proc->params();
            auto* amp = ed->findChild<QWidget*>("ampEnv");
            auto* curve = ed->findChild<QWidget*>("filter");
            auto* osc = ed->findChild<QWidget*>("OSC 1");
            const float release = ps.get(Synth::pAR), cutoff = ps.get(Synth::pCutoff);
            const int wave = ps.getInt(Synth::pOsc1Wave);
            const size_t steps = app.model().undoStack().count();
            if (amp) {   // the bottom-right corner is nearest the release point: dragging left shortens it, one undo puts it back
                const QPointF at(amp->width() - 12, amp->height() - 10);
                mouse(amp, at, at - QPointF(40, 0));
                const bool shorter = ps.get(Synth::pAR) < release - 0.01f && app.model().undoStack().count() == steps + 1;
                app.model().undoStack().undo();
                env = shorter && std::fabs(ps.get(Synth::pAR) - release) < 1e-4f;
            }
            if (curve) {
                const QPointF at(curve->width() / 2.0, curve->height() / 2.0);
                mouse(curve, at, at + QPointF(30, 0));
                filter = ps.get(Synth::pCutoff) > cutoff * 1.2f && std::fabs(ps.get(Synth::pReso) - 0.2f) < 1e-3f;
            }
            if (osc) {
                const QPointF at(osc->width() / 2.0, osc->height() / 2.0);
                mouse(osc, at, at);
                shape = ps.getInt(Synth::pOsc1Wave) == wave + 1;
            }
        }
        CHECK(env && filter && shape,
            QString("Synth editor: dragging the envelope sets the release in one undo step (%1), the filter curve the cutoff (%2), a "
                    "click the next shape (%3)")
                .arg(env)
                .arg(filter)
                .arg(shape)
                .toUtf8()
                .constData());
        ed.reset();
        app.deleteChannel(ch);
    }
    {   // the dynamics displays drag like knobs: every handle changes its parameters and undo puts them back; dragged away and
        // back it lands on the values it started from, so each drag is the inverse of where its handle is drawn
        Id t1 = app.project().mixerTracks[1].id;
        auto values = [](IProcessor* p) {
            std::vector<float> v;
            for (int i = 0; i < p->params().count(); ++i) v.push_back(p->params().desc(i).toNormalized(p->params().getAt(i)));
            return v;
        };
        auto near = [](const std::vector<float>& a, const std::vector<float>& b) {
            for (size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i] - b[i]) > 0.02f) return false;
            return true;
        };
        auto drag = [](QWidget* view, std::initializer_list<QPoint> path) {
            const QPoint from = *path.begin(), to = *(path.end() - 1);
            QMouseEvent press(QEvent::MouseButtonPress, from, view->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &press);
            for (const QPoint& p : path) {
                QMouseEvent move(QEvent::MouseMove, p, view->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(view, &move);
            }
            QMouseEvent rel(QEvent::MouseButtonRelease, to, view->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &rel);
        };
        QStringList stuck;
        int handles = 0;
        for (const char* key :
            {"native.compressor", "native.limiter", "native.gate", "native.expander", "native.clipper", "native.transient"}) {
            Id slot = app.addEffect(t1, key);
            app.engine().rebuild(app.project());
            IProcessor* fx = app.processor(slot);
            std::unique_ptr<QWidget> ed(fx ? fx->createEditor(nullptr) : nullptr);
            if (!ed) stuck << key;
            else {
                ed->resize(ed->sizeHint());
                ed->layout()->activate();
            }
            QPoint at;
            for (int i = 0; QWidget* view = ed ? dynamicsHandle(ed.get(), i, at) : nullptr; ++i, ++handles) {
                const QPoint away = at + QPoint(12, -18);
                const auto before = values(fx);
                const size_t undo0 = app.model().undoStack().count();
                drag(view, {at, away});
                const bool changed = values(fx) != before, undone = app.model().undoStack().count() > undo0;
                while (app.model().undoStack().count() > undo0) app.model().undoStack().undo();
                const bool restored = near(values(fx), before);
                drag(view, {at, away, at});
                const bool back = near(values(fx), before);
                while (app.model().undoStack().count() > undo0) app.model().undoStack().undo();
                if (!changed || !undone || !restored || !back) stuck << QString("%1 handle %2").arg(key).arg(i);
            }
            ed.reset();
            app.removeEffect(t1, slot);
        }
        app.engine().rebuild(app.project());
        CHECK(stuck.isEmpty() && handles == 11,
            QString("every dynamics display handle drags, undoes and inverts its position (%1 handles; stuck: %2)")
                .arg(handles)
                .arg(stuck.join(", "))
                .toUtf8()
                .constData());
    }
    {   // projects saved straight into Documents/Vybrant/Projects get a folder of their own; anywhere else the chosen place is kept
        const QString root = App::projectsDir();
        CHECK(QDir(root).exists() && App::projectFolderPath(root + "/Song.mtp") == root + "/Song/Song.mtp" &&
                App::projectFolderPath(root + "/Song/Song v2.mtp") == root + "/Song/Song v2.mtp" &&
                App::projectFolderPath(tmp.path() + "/Song.mtp") == tmp.path() + "/Song.mtp",
            "a project saved into the Projects folder gets its own folder; one saved elsewhere stays where it was put");
    }
#ifndef _WIN32
    {   // an AppImage installs itself: a copy in ~/Applications, a launcher pointing at it, the icon and the .mtp type; and goes again
        const QByteArray home = qgetenv("HOME"), data = qgetenv("XDG_DATA_HOME");
        qputenv("HOME", tmp.path().toUtf8() + "/home");
        qputenv("XDG_DATA_HOME", tmp.path().toUtf8() + "/home/.local/share");
        const QString fake = tmp.path() + "/Vybrant-v9-x86_64.AppImage", share = tmp.path() + "/home/.local/share";
        QFile f(fake);
        const bool made = f.open(QIODevice::WriteOnly) && f.write("not really an AppImage") > 0;
        f.close();
        QFile desk(share + "/applications/vybrant.desktop");
        const bool ok = made && appimage::install(fake) && appimage::installed() && desk.open(QIODevice::ReadOnly) &&
            desk.readAll().contains("Exec=\"" + (tmp.path() + "/home/Applications/Vybrant.AppImage").toUtf8() + "\" %f") &&
            QFileInfo(appimage::installedPath()).isExecutable() && QFile::exists(share + "/icons/hicolor/512x512/apps/vybrant.png") &&
            QFile::exists(share + "/mime/packages/vybrant.xml");
        appimage::uninstall();
        CHECK(ok && !appimage::installed() && !QFile::exists(appimage::installedPath()) && !QFile::exists(desk.fileName()),
            "an AppImage installs itself into the applications menu (copy, launcher, icon, .mtp type) and removes itself again");
        qputenv("HOME", home);
        data.isNull() ? qunsetenv("XDG_DATA_HOME") : qputenv("XDG_DATA_HOME", data);
    }
#endif
    {   // a license made by the server (api/src/license.ts, signed with a test key) checks out here, and only unchanged
        const QByteArray pub = QByteArray::fromHex("c7d5ddbdb4d3285224433778bb38133fccef47eecfb7f2c018e0b61bf4c60018");
        const QString key = "eyJ2IjoxLCJzdWIiOiJ1c2VyX3NlbGZ0ZXN0IiwibmFtZSI6IlNlbGYgVGVzdCIsImVtYWlsIjoic2VsZnRlc3RAZXhhbXBsZS5jb20ifQ."
                            "T4FwK67mTrwshPYRytoiujOfzzOFPOPJ8TOajzpIC1KLrMgKtE7KQ8GC_kFSWRzM2VbJhXjZlsohEguJpv98Ag";
        const license::Info i = license::verify(key, pub);
        QString forged = key;
        forged[10] = forged[10] == 'A' ? 'B' : 'A';
        CHECK(i.valid && i.id == "user_selftest" && i.name == "Self Test" && i.email == "selftest@example.com" &&
                license::verify("  " + key.left(60) + "\n" + key.mid(60) + " ", pub).valid && !license::verify(forged, pub).valid &&
                !license::verify(key + "x", pub).valid && !license::verify("garbage", pub).valid,
            "a server-made license verifies, pasted with line breaks too; a changed one, another key's or garbage does not");
    }
    {   // the processor picker sorts plugins by what they do, from VST3 / CLAP / LV2 categories and the name
        auto cat = [](const char* category, const char* sub, const char* name) {
            PluginInfo p;
            p.category = category;
            p.subCategories = sub;
            p.name = name;
            return ProcessorPicker::categoryOf(p);
        };
        CHECK(cat("Effect", "Fx|Dynamics", "Squash") == "Dynamics" &&
                cat("Effect", "audio-effect|equalizer|stereo", "X") == "EQ / Filter" &&
                cat("Effect", "Reverb", "LSP Room Builder") == "Time" && cat("Effect", "audio-effect", "Step Sequencer") == "Other" &&
                cat("Effect", "Fx", "Tape Saturator") == "Distortion" &&
                cat("Instrument", "instrument|synthesizer", "Surge XT") == "Synths" &&
                cat("Instrument", "Instrument|Drum", "Kit") == "Drums" &&
                cat("Instrument", "Instrument Plugin", "LSP Multi-Sampler") == "Samplers" &&
                cat("Instrument", "Instrument", "SRX PIANO 1") == "Keys",
            "the processor picker puts plugins in the right category");
    }
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

}   // namespace mt

#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
namespace mt {
// `vybrant --preview-bench <dir>`: measures click-to-play latency of the browser preview for every file in a folder
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
// `vybrant --uitest <png>`: Ctrl+drag a selection rectangle in the playlist and screenshot the result
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

// `vybrant --embed-test` (on X11): the CLAP test plugin's editor embeds into our window, in-process and from its own
// process. The window bypasses the window manager and sits off-screen, so nothing shows.
#include <QVBoxLayout>
#include <QJsonDocument>
#include "dsp/DspUtils.h"
#ifdef _WIN32   // no X11: the plugin test's editor pixel check is skipped there
#include <windows.h>
#else
#include "plugins/Lv2Plugin.h"
#include <X11/Xlib.h>   // last: its macros clash with Qt
#include <X11/Xutil.h>
#endif
namespace mt {
#ifndef _WIN32
static bool findWindow(Display* d, Window w, const char* name, XWindowAttributes* at) {
    char* n = nullptr;
    bool hit = XFetchName(d, w, &n) && n && !std::strcmp(n, name);
    if (n) XFree(n);
    if (hit) return XGetWindowAttributes(d, w, at);
    Window root, parent, *kids = nullptr;
    unsigned nk = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &nk)) return false;
    bool found = false;
    for (unsigned i = 0; i < nk && !found; ++i) found = findWindow(d, kids[i], name, at);
    if (kids) XFree(kids);
    return found;
}
// `vybrant --plugin-test <name> [--no-editor] [--no-bridge]`: every installed plugin whose name contains <name>, in-process
// and in its own process: it loads, makes sound (instruments: a note; effects: a sine in), reports latency and extra outputs,
// its state round-trips into a new instance, and (on X11) its editor draws inside our window. The window sits off-screen.
static double editorVariance(Display* d, Window w, int* depth) {   // how much the pixels differ: 0 = an empty window
    XWindowAttributes at{};
    if (!XGetWindowAttributes(d, w, &at) || at.width < 8 || at.height < 8) return 0;
    XImage* img = XGetImage(d, w, 0, 0, at.width, at.height, AllPlanes, ZPixmap);
    if (!img) return 0;
    double sum = 0, sq = 0;
    int n = 0;
    for (int y = 0; y < at.height; y += 3)
        for (int x = 0; x < at.width; x += 3) {
            unsigned long px = XGetPixel(img, x, y);
            double v = ((px >> 16) & 255) + ((px >> 8) & 255) + (px & 255);
            sum += v;
            sq += v * v;
            ++n;
        }
    XDestroyImage(img);
    Window root, parent, *kids = nullptr;
    unsigned nk = 0;
    *depth = 0;
    if (XQueryTree(d, w, &root, &parent, &kids, &nk) && nk) *depth = 1;
    if (kids) XFree(kids);
    return n ? sq / n - (sum / n) * (sum / n) : 0;
}
#endif
int runPluginTest(App& app, const QString& filter, bool editors, bool bridge) {
    std::printf("Vybrant plugin test: \"%s\"\n", qPrintable(filter));
    std::vector<PluginInfo> found;
    for (auto& f : PluginScanner::candidateFiles(PluginDatabase::defaultDirectories())) {
        QProcess scan;
        scan.start(QCoreApplication::applicationFilePath(), {"--scan-plugin", f});
        scan.waitForFinished(120000);
        for (auto& line : scan.readAllStandardOutput().split('\n')) {
            QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.isEmpty()) continue;
            PluginInfo p;
            p.format = o["format"].toString();
            p.path = f;
            p.uid = o["uid"].toString();
            p.name = o["name"].toString();
            p.category = o["category"].toString();
            found.push_back(p);
        }
    }
#ifndef _WIN32
    for (auto& p : Lv2Plugin::scan()) found.push_back(p);
#endif
    std::erase_if(found, [&](const PluginInfo& p) { return p.failed || !p.name.contains(filter, Qt::CaseInsensitive); });
    std::printf("  %zu plugin(s)\n", found.size());
#ifdef _WIN32
    const bool probe = editors && QGuiApplication::platformName() == "windows";
#else
    bool x11 = editors && QGuiApplication::platformName() == "xcb";
    Display* dpy = x11 ? XOpenDisplay(nullptr) : nullptr;
    const bool probe = dpy;
#endif
    PluginHost host(app.pluginDb());
    const int sr = 48000, B = 512;
    int bad = 0;
    for (auto& info : found)
        for (bool bridged : {false, true}) {
            if (bridged && !bridge) continue;
            QString row = QString("%1 [%2%3]").arg(info.name, info.format, bridged ? ", own process" : "");
            QElapsedTimer t;
            t.start();
            PluginInstanceData d = PluginHost::makeInstanceData(info, app.project().newId());
            d.bridged = bridged;
            QString err;
            auto proc = host.createInstance(d, &err);
            if (!proc) {
                std::printf("  FAIL %s: does not load (%s)\n", qPrintable(row), qPrintable(err));
                ++bad;
                continue;
            }
            proc->prepare(sr, B);
            qint64 loadMs = t.elapsed();
            bool inst = proc->kind() == ProcessorKind::Instrument;
            // a second of audio: a note held for 0.7 s (instruments) or a 440 Hz sine (effects)
            auto play = [&](IProcessor& p, float* peakOut) {
                std::vector<float> L(B), R(B);
                MidiEventList midi;
                ProcessContext ctx;
                ctx.sampleRate = sr;
                ctx.blockSize = B;
                ctx.playing = true;
                bool finite = true;
                float peak = 0;
                double phase = 0;
                for (int b = 0; b < sr / B; ++b) {
                    midi.clear();
                    if (inst && (b == 0 || b == sr * 7 / 10 / B)) {
                        MidiEvent e;
                        e.type = b == 0 ? MidiEvent::NoteOn : MidiEvent::NoteOff;
                        e.data1 = 60;
                        e.data2 = 100;
                        e.velocity = 0.8f;
                        midi.add(e);
                    }
                    for (int i = 0; i < B; ++i, phase += 2 * dsp::kPi * 440 / sr) L[i] = R[i] = inst ? 0.f : 0.3f * (float)std::sin(phase);
                    ctx.positionSamples = (int64_t)b * B;
                    ctx.positionBeats = ctx.positionSamples / (double)sr * 2;
                    p.process(L.data(), R.data(), B, midi, ctx);
                    QCoreApplication::processEvents();   // plugins finish state loads and report changes from the main thread
                    for (int i = 0; i < B; ++i) {
                        finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
                        peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
                    }
                }
                *peakOut = peak;
                return finite;
            };
            float peak = 0;
            bool finite = play(*proc, &peak);
            // state: change every continuous parameter a little, save, load into a fresh instance, compare
            ParameterSet& ps = proc->params();
            for (int i = 0; i < ps.count(); ++i)
                if (ps.desc(i).steps == 0 && ps.desc(i).automatable) ps.setNormalized(ps.desc(i).id, 0.37f);
            float tmp;
            play(*proc, &tmp);   // let the plugin take the new values
            QJsonObject st = proc->saveState();
            auto settle = [] {   // real time for what plugins finish on the main thread after a state load
                QElapsedTimer w;
                w.start();
                while (w.elapsed() < 500) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            };
            proc->restoreState(st);   // the original re-reads what the plugin holds (a type switch can reset other parameters)
            play(*proc, &tmp);
            settle();
            play(*proc, &tmp);   // a plugin in its own process reports its values with the next block
            auto copy = host.createInstance(d, &err);
            int mismatched = -1;
            if (copy) {
                copy->prepare(sr, B);
                copy->restoreState(st);
                play(*copy, &tmp);
                settle();
                play(*copy, &tmp);
                if (qEnvironmentVariableIsSet("MT_PLUGIN_TEST_WAIT"))
                    for (int k = 0; k < 4; ++k) {
                        play(*copy, &tmp);
                        int m = 0;
                        for (int i = 0; i < ps.count() && i < copy->params().count(); ++i)
                            m += std::fabs(ps.getAt(i) - copy->params().getAt(i)) > 1e-3f;
                        std::printf("       after %d more s: %d differ\n", k + 1, m);
                        if (k == 3) {
                            copy->restoreState(copy->saveState());
                            play(*copy, &tmp);
                            m = 0;
                            for (int i = 0; i < ps.count() && i < copy->params().count(); ++i)
                                m += std::fabs(ps.getAt(i) - copy->params().getAt(i)) > 1e-3f;
                            std::printf("       after the copy re-reads its own state: %d differ\n", m);
                        }
                    }
                mismatched = 0;
                for (int i = 0; i < ps.count() && i < copy->params().count(); ++i)
                    if (std::fabs(ps.getAt(i) - copy->params().getAt(i)) >
                        1e-3f * std::max(1.f, std::fabs(ps.desc(i).max - ps.desc(i).min))) {
                        if (++mismatched <= 4 && qEnvironmentVariableIsSet("MT_PLUGIN_TEST_DEBUG"))
                            std::printf("       %s: %g -> %g (range %g..%g, def %g)\n", ps.desc(i).name.c_str(), ps.getAt(i),
                                copy->params().getAt(i), ps.desc(i).min, ps.desc(i).max, ps.desc(i).def);
                    }
            }
            QString editor = "-";
            if (probe && proc->hasCustomEditor()) {
                QWidget top(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);
                top.move(-6000, -6000);
                auto* lay = new QVBoxLayout(&top);
                lay->setContentsMargins(0, 0, 0, 0);
                QWidget* ed = proc->createEditor(&top);
                double var = 0;
                int depth = 0;
                if (ed) {
                    lay->addWidget(ed);
                    top.show();
                    QElapsedTimer et;
                    et.start();
                    while (et.elapsed() < 6000 && var < 50) {   // plugins draw a moment after they attach
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
#ifdef _WIN32   // attached: the editor made its window inside ours (its pixels aren't read here)
                        var = GetWindow((HWND)ed->winId(), GW_CHILD) ? 50 : 0;
#else
                        XSync(dpy, 0);
                        if (et.elapsed() > 800) var = editorVariance(dpy, (Window)top.winId(), &depth);
#endif
                    }
                }
                double dpr = top.devicePixelRatioF();
                editor = !ed    ? "no editor"
                    : var >= 50 ? QString("ok %1x%2").arg(std::lround(top.width() * dpr)).arg(std::lround(top.height() * dpr))
                                : "EMPTY";
            }
            bool ok = finite && mismatched == 0 && editor != "EMPTY" && editor != "no editor";
            bool silent = inst && peak <= 1e-4f;   // a sampler with nothing loaded is silent by design: a warning, not a failure
            bad += !ok;
            std::printf("  %s %s: load %lld ms, peak %.3f%s, latency %d, extra outputs %d, params %d, state %s, editor %s\n",
                !ok ? "FAIL"
                    : silent ? "warn"
                             : "ok  ",
                qPrintable(row), (long long)loadMs, peak,
                !finite      ? " NaN/inf!"
                    : silent ? " (silent)"
                             : "",
                proc->latencySamples(), proc->auxOutputCount(), ps.count(),
                mismatched < 0        ? "copy failed"
                    : mismatched == 0 ? "round-trips"
                                      : qPrintable(QString("%1 differ").arg(mismatched)),
                qPrintable(editor));
        }
#ifndef _WIN32
    if (dpy) XCloseDisplay(dpy);
#endif
    std::printf(bad ? "\n%d FAILED\n" : "\nAll plugin checks passed\n", bad);
    return bad ? 1 : 0;
}
int runEmbedTest(App& app) {
    std::printf("Vybrant editor embedding test\n");
#ifdef _WIN32
    const char* native = "windows";
#else
    const char* native = "xcb";
#endif
    if (QGuiApplication::platformName() != native) {
        std::printf("  skip (needs the %s platform, it is %s)\n", native, qPrintable(QGuiApplication::platformName()));
        return 0;
    }
    QString root = QCoreApplication::applicationDirPath() + "/..", cxx = QStandardPaths::findExecutable("c++");
    QTemporaryDir dir;
    QString so = dir.path() + "/MT Test Gain.clap";
    if (cxx.isEmpty() ||
        QProcess::execute(
            cxx, {"-std=c++17", "-shared", "-fPIC", "-I" + root + "/src", root + "/tests/clap_gain.cpp", "-o", so, kGuiLib}) != 0) {
        std::printf("  FAIL compile the CLAP test plugin\n");
        return 1;
    }
    PluginInfo info;
    info.format = "clap";
    info.uid = "org.vybrant.test-gain";
    info.name = "MT Test Gain";
    info.path = so;
    PluginHost host(app.pluginDb());
#ifndef _WIN32
    Display* dpy = XOpenDisplay(nullptr);
#endif
    for (bool bridged : {false, true}) {
#ifdef _WIN32
        if (bridged) continue;   // no own-process plugins on Windows yet
#endif
        PluginInstanceData d = PluginHost::makeInstanceData(info, app.project().newId());
        d.bridged = bridged;
        auto proc = host.createInstance(d);
        if (proc) proc->prepare(44100, 512);
        QWidget top(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);
        top.move(-4000, -4000);
        auto* lay = new QVBoxLayout(&top);
        lay->setContentsMargins(0, 0, 0, 0);
        QWidget* ed = proc ? proc->createEditor(&top) : nullptr;
        if (ed) lay->addWidget(ed);
        top.show();
        int w = 0, h = 0;   // the editor window's size once it shows
        QElapsedTimer t;
        t.start();
        while (!w && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
#ifdef _WIN32
            HWND win = ed ? FindWindowExW((HWND)ed->winId(), nullptr, nullptr, L"mt-test-gain-editor") : nullptr;
            RECT r{};
            if (win && IsWindowVisible(win) && GetClientRect(win, &r)) w = r.right, h = r.bottom;
#else
            XWindowAttributes at{};
            XSync(dpy, 0);
            if (findWindow(dpy, (Window)top.winId(), "mt-test-gain-editor", &at) && at.map_state == IsViewable) w = at.width, h = at.height;
#endif
        }
        double dpr = top.devicePixelRatioF();   // plugin sizes are physical pixels
        CHECK(ed && w == 320 && h == 200 && std::lround(top.width() * dpr) >= 320 && std::lround(top.height() * dpr) >= 200,
            bridged ? "the editor of a plugin in its own process shows inside our window, at its size"
                    : "a CLAP editor shows inside our window, at its size");
    }
#ifndef _WIN32
    XCloseDisplay(dpy);
#endif
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
// `vybrant --editor-shot <processor key> <out.png>` (e.g. native.compressor): a native processor's editor after a few seconds
// of test signal, so its meters, curves and scopes show something. Effects get drum hits over a tone sweeping 80 Hz - 8 kHz,
// instruments a repeated chord. For working on editors without opening the app; QT_QPA_PLATFORM=offscreen is enough.
int runEditorShot(App& app, const QString& key, const QString& out) {
    auto proc = NativeRegistry::instance().create(key);
    if (!proc) {
        std::printf("no native processor %s\n", qPrintable(key));
        return 1;
    }
    const int sr = 44100, B = 512;
    proc->prepare(sr, B);
    std::unique_ptr<QWidget> ed(proc->createEditor(nullptr));
    if (!ed) ed.reset(new GenericParamPanel(app, 0, proc.get(), nullptr, 6));   // no editor of its own: the generic knobs
    ed->resize(ed->sizeHint().expandedTo(QSize(320, 200)));
    ed->show();
    const bool inst = proc->kind() == ProcessorKind::Instrument;
    std::vector<float> L(B), R(B);
    MidiEventList midi;
    ProcessContext ctx;
    ctx.sampleRate = sr;
    ctx.blockSize = B;
    ctx.playing = true;
    double phase = 0;
    for (int64_t n = 0, blk = 0; n < 3 * sr; n += B, ++blk) {
        midi.clear();
        for (int i = 0; i < B; ++i) {
            const double s = (n + i) / double(sr), beat = std::fmod(s, 0.5);
            phase += 2 * M_PI * 80 * std::pow(100.0, std::fmod(s, 3.0) / 3.0) / sr;
            const float kick = beat < 0.15 ? (float)(std::exp(-beat * 25) * std::sin(2 * M_PI * 55 * s)) : 0.f;
            L[i] = R[i] = inst ? 0.f : 0.25f * (float)std::sin(phase) + 0.8f * kick;
            if (inst && (n + i) % (sr / 2) == 0)
                for (int note : {60, 64, 67}) {
                    MidiEvent on;
                    on.data1 = (uint8_t)note, on.data2 = 100, on.velocity = 0.8f, on.sampleOffset = i;
                    midi.add(on);
                }
            if (inst && (n + i) % (sr / 2) == sr / 4)
                for (int note : {60, 64, 67}) {
                    MidiEvent off;
                    off.type = MidiEvent::NoteOff, off.data1 = (uint8_t)note, off.sampleOffset = i;
                    midi.add(off);
                }
        }
        ctx.positionSamples = n;
        proc->process(L.data(), R.data(), B, midi, ctx);
        if (blk % 2 == 0) {   // in about real time, so the editor's refresh timers see what the DSP just measured
            QThread::msleep(20);
            QCoreApplication::processEvents();
        }
    }
    QCoreApplication::processEvents();
    const bool ok = ed->grab().save(out);
    std::printf("%s: %s (%dx%d)\n", qPrintable(key), ok ? qPrintable(out) : "could not save", ed->width(), ed->height());
    return ok ? 0 : 1;
}
}   // namespace mt
