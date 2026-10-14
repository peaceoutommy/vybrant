// Built-in self test (`musicthingy --selftest`): exercises the project model, serializer, undo, DSP and engine.
#include "ui/App.h"
#include "project/ProjectSerializer.h"
#include "dsp/NativeRegistry.h"
#include "dsp/effects/Effects.h"
#include "dsp/instruments/Instruments.h"
#include "audio/RenderEngine.h"
#include "ui/widgets/Controls.h"
#include "ui/PlaylistPanel.h"
#include <QMouseEvent>
#include <QMenu>
#include <QInputDialog>
#include <QApplication>
#include <QTimer>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>

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
        Id track = app.firstPlaylistTrack();
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
        bool vol = recorded(ParamTarget::MixerVolume, t1), pan = recorded(ParamTarget::MixerPan, t1), snd = recorded(ParamTarget::MixerSend, t1);
        CHECK(vol && pan && snd, QString("automation recording catches mixer volume, pan and send moves (%1 %2 %3)")
                                     .arg(vol).arg(pan).arg(snd).toUtf8().constData());
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
        app.edit<MixerTrack>(t1, "Rename", [](MixerTrack& x) { x.name = "Strings bus"; x.customName = true; });
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
