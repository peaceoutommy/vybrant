// Built-in self test (`musicthingy --selftest`): exercises the project model, serializer, undo, DSP and engine.
#include "ui/App.h"
#include "project/ProjectSerializer.h"
#include "dsp/NativeRegistry.h"
#include "dsp/effects/Effects.h"
#include "dsp/instruments/Instruments.h"
#include "audio/RenderEngine.h"
#include "project/MidiFile.h"
#include "ui/PianoRollPanel.h"
#include "ui/widgets/Controls.h"
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
            CHECK(tracks && takes[0]->muted && takes[1]->muted && !takes[2]->muted, "takes stack on their own tracks, only the newest audible");
        }
        model.undoStack().undo();
        CHECK(pr.clips.size() == clips0 && pr.playlistTracks.size() == tracks0 && model.undoStack().count() == undo0,
            "one undo removes the whole pass");
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
        auto clip = std::find_if(pr.clips.begin(), pr.clips.end(), [&](const Clip& c) { return c.type == ClipType::Pattern && c.sourceId == pat; });
        CHECK(notes == 1 && clip != pr.clips.end() && clip->start == bar * 2, "song-mode notes get a pattern clip at the bar the pass started in");
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
