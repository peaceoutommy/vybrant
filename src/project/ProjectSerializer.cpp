#include "project/ProjectSerializer.h"
#include "core/Log.h"
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>

namespace mt {

static QJsonValue colorJ(const QColor& c) { return c.name(QColor::HexArgb); }
static QColor colorF(const QJsonValue& v, QColor def) { return v.isString() ? QColor(v.toString()) : def; }
static QString relPath(const QString& abs, const QString& dir) {
    if (dir.isEmpty() || abs.isEmpty()) return abs;
    QString r = QDir(dir).relativeFilePath(abs);
    return r.startsWith("..") ? abs : r;
}
static QString absPath(const QString& rel, const QString& dir) {
    if (rel.isEmpty() || QFileInfo(rel).isAbsolute() || dir.isEmpty()) return rel;
    return QDir(dir).absoluteFilePath(rel);
}

static QJsonObject targetJ(const ParamTarget& t) {
    return {{"kind", (int)t.kind}, {"owner", (double)t.ownerId}, {"slot", (double)t.slotId}, {"param", (double)t.paramId}};
}
static ParamTarget targetF(const QJsonObject& o) {
    ParamTarget t;
    t.kind = (ParamTarget::Kind)o["kind"].toInt();
    t.ownerId = (Id)o["owner"].toDouble();
    t.slotId = (Id)o["slot"].toDouble();
    t.paramId = (uint32_t)o["param"].toDouble();
    return t;
}

QJsonObject ProjectSerializer::toJson(const Project& p) {
    const QString dir = p.settings.projectDir;
    QJsonObject root;
    root["formatVersion"] = kFormatVersion;
    root["application"] = "MusicThingy";
    root["project"] = QJsonObject{{"title", p.meta.title}, {"author", p.meta.author}, {"comments", p.meta.comments},
        {"created", p.meta.created}, {"modified", p.meta.modified}, {"genre", p.meta.genre}, {"sampleRate", p.settings.sampleRate},
        {"bitDepth", p.settings.bitDepth}, {"nextId", (double)p.nextId}, {"selectedPattern", (double)p.selectedPatternId},
        {"selectedChannel", (double)p.selectedChannelId}};
    const auto& t = p.transport;
    root["transport"] = QJsonObject{{"bpm", t.bpm}, {"tsNum", t.timeSig.numerator}, {"tsDen", t.timeSig.denominator},
        {"metronome", t.metronome}, {"countIn", t.countIn}, {"loop", t.loopEnabled}, {"loopStart", (double)t.loopStart},
        {"loopEnd", (double)t.loopEnd}, {"songMode", t.songMode}, {"snap", t.snap}, {"snapDivision", t.snapDivision},
        {"recordMode", t.recordMode}, {"countInBars", t.countInBars}, {"position", (double)t.position}, {"punch", t.punchEnabled},
        {"punchIn", (double)t.punchIn}, {"punchOut", (double)t.punchOut}, {"loopRecord", t.loopRecord}};
    QJsonArray pats;
    for (auto& pat : p.patterns) {
        QJsonArray chans;
        for (auto& c : pat.channels) {
            QJsonArray notes, steps;
            for (auto& n : c.notes)
                notes.append(QJsonObject{{"id", (double)n.id}, {"p", n.pitch}, {"s", (double)n.start}, {"d", (double)n.duration},
                    {"v", n.velocity}, {"rv", n.releaseVelocity}, {"pan", n.pan}, {"fp", n.finePitch}, {"ch", n.channel}, {"m", n.muted}});
            for (auto& s : c.steps)
                steps.append(QJsonObject{
                    {"e", s.enabled}, {"v", s.velocity}, {"pan", s.pan}, {"pi", s.pitch}, {"pr", s.probability}, {"o", s.offset}});
            chans.append(QJsonObject{{"channelId", (double)c.id}, {"notes", notes}, {"steps", steps}});
        }
        QJsonArray lanes;
        for (Id l : pat.automationLaneIds) lanes.append((double)l);
        pats.append(QJsonObject{{"id", (double)pat.id}, {"name", pat.name}, {"color", colorJ(pat.color)}, {"length", (double)pat.length},
            {"stepsPerBeat", pat.stepsPerBeat}, {"stepCount", pat.stepCount}, {"swing", pat.swing}, {"channels", chans},
            {"automation", lanes}});
    }
    root["patterns"] = pats;
    QJsonArray chans;
    for (auto& c : p.channels)
        chans.append(QJsonObject{{"id", (double)c.id}, {"name", c.name}, {"color", colorJ(c.color)}, {"type", (int)c.type},
            {"instrument", c.instrumentKey}, {"plugin", (double)c.pluginInstanceId}, {"sample", relPath(c.samplePath, dir)},
            {"volume", c.volume}, {"pan", c.pan}, {"mute", c.mute}, {"solo", c.solo}, {"enabled", c.enabled}, {"cutItself", c.cutItself},
            {"mixerTrack", (double)c.mixerTrackId}, {"rootNote", c.rootNote}, {"midiOutPort", c.midiOutPort},
            {"midiOutChannel", c.midiOutChannel}, {"state", c.instrumentState}});
    root["channels"] = chans;
    QJsonArray tracks, clips;
    for (auto& t : p.playlistTracks)
        tracks.append(QJsonObject{{"id", (double)t.id}, {"name", t.name}, {"color", colorJ(t.color)}, {"height", t.height},
            {"mute", t.mute}, {"solo", t.solo}, {"locked", t.locked}, {"armed", t.armed}, {"recordInput", t.recordInput},
            {"mixerTrack", (double)t.mixerTrackId}});
    for (auto& c : p.clips)
        clips.append(QJsonObject{{"id", (double)c.id}, {"type", (int)c.type}, {"track", (double)c.trackId}, {"start", (double)c.start},
            {"length", (double)c.length}, {"source", (double)c.sourceId}, {"muted", c.muted}, {"gain", c.gain},
            {"fadeIn", (double)c.fadeIn}, {"fadeOut", (double)c.fadeOut}, {"fadeInShape", c.fadeInShape}, {"fadeOutShape", c.fadeOutShape},
            {"stretch", c.timeStretch}, {"pitch", c.pitch}, {"offset", (double)c.sourceOffset}, {"reverse", c.reverse},
            {"loop", c.loopSource}, {"name", c.name}});
    root["playlist"] = QJsonObject{{"tracks", tracks}, {"clips", clips}};
    QJsonArray mixer;
    for (auto& t : p.mixerTracks) {
        QJsonArray fx, sends;
        for (auto& e : t.effects)
            fx.append(QJsonObject{{"id", (double)e.id}, {"key", e.processorKey}, {"plugin", (double)e.pluginInstanceId},
                {"bypass", e.bypass}, {"mix", e.mix}, {"state", e.state}, {"name", e.name}});
        for (auto& s : t.sends)
            sends.append(QJsonObject{{"target", (double)s.targetTrackId}, {"level", s.level}, {"pre", s.preFader}, {"enabled", s.enabled},
                {"sidechain", s.sidechain}});
        mixer.append(QJsonObject{{"id", (double)t.id}, {"name", t.name}, {"color", colorJ(t.color)}, {"volume", t.volume}, {"pan", t.pan},
            {"mute", t.mute}, {"solo", t.solo}, {"armed", t.armed}, {"input", t.inputChannel}, {"output", (double)t.outputTrackId},
            {"effects", fx}, {"sends", sends}, {"master", t.isMaster}});
    }
    root["mixer"] = mixer;
    QJsonArray plugs;
    for (auto& pl : p.plugins) {
        QJsonObject cache;
        for (auto& [k, v] : pl.paramCache) cache[QString::number(k)] = v;
        plugs.append(QJsonObject{{"id", (double)pl.id}, {"format", pl.format}, {"identifier", pl.identifier}, {"name", pl.name},
            {"manufacturer", pl.manufacturer}, {"type", pl.type}, {"path", pl.path}, {"state", QString::fromLatin1(pl.state.toBase64())},
            {"controllerState", QString::fromLatin1(pl.controllerState.toBase64())}, {"latency", pl.latency}, {"bypass", pl.bypass},
            {"params", cache}});
    }
    root["plugins"] = plugs;
    QJsonArray autom;
    for (auto& l : p.automationLanes) {
        QJsonArray pts;
        for (auto& pt : l.points)
            pts.append(QJsonObject{{"t", (double)pt.time}, {"v", pt.value}, {"c", (int)pt.curve}, {"tn", pt.tension}});
        autom.append(QJsonObject{{"id", (double)l.id}, {"name", l.name}, {"color", colorJ(l.color)}, {"target", targetJ(l.target)},
            {"points", pts}, {"length", (double)l.length}, {"pattern", (double)l.patternId}});
    }
    root["automation"] = autom;
    QJsonArray audio;
    for (auto& a : p.audioSources) {
        QJsonArray sl;
        for (auto s : a.slices) sl.append((double)s);
        audio.append(QJsonObject{{"id", (double)a.id}, {"path", relPath(a.filePath, dir)}, {"name", a.name}, {"sampleRate", a.sampleRate},
            {"channels", a.channels}, {"frames", (double)a.frames}, {"reverse", a.reverse}, {"normalize", a.normalize}, {"loop", a.loop},
            {"trimStart", (double)a.trimStart}, {"trimEnd", (double)a.trimEnd}, {"gain", a.gain}, {"slices", sl}});
    }
    root["audioReferences"] = audio;
    QJsonArray markers;
    for (auto& m : p.markers) markers.append(QJsonObject{{"id", (double)m.id}, {"name", m.name}, {"position", (double)m.position}});
    root["markers"] = markers;
    QJsonArray maps;
    for (auto& m : p.midiMappings)
        maps.append(QJsonObject{{"cc", m.cc}, {"channel", m.midiChannel}, {"target", targetJ(m.target)}, {"name", m.name}});
    root["midiMappings"] = maps;
    return root;
}

QJsonObject ProjectSerializer::migrate(QJsonObject root, int fromVersion) {
    // Future versions add cases here; each step upgrades one version.
    while (fromVersion < kFormatVersion) {
        switch (fromVersion) {
        default: break;
        }
        ++fromVersion;
    }
    root["formatVersion"] = kFormatVersion;
    return root;
}

bool ProjectSerializer::fromJson(const QJsonObject& rootIn, Project& p, QString* error) {
    int ver = rootIn["formatVersion"].toInt(0);
    if (ver <= 0) {
        if (error) *error = "Not a MusicThingy project (missing formatVersion)";
        return false;
    }
    if (ver > kFormatVersion) {
        if (error) *error = QString("Project was saved by a newer version (format %1)").arg(ver);
        return false;
    }
    QJsonObject root = ver < kFormatVersion ? migrate(rootIn, ver) : rootIn;
    const QString dir = p.settings.projectDir;
    p = Project();
    p.settings.projectDir = dir;
    auto pr = root["project"].toObject();
    p.meta.title = pr["title"].toString("Untitled");
    p.meta.author = pr["author"].toString();
    p.meta.comments = pr["comments"].toString();
    p.meta.created = pr["created"].toString();
    p.meta.modified = pr["modified"].toString();
    p.meta.genre = pr["genre"].toString();
    p.settings.sampleRate = pr["sampleRate"].toDouble(44100);
    p.settings.bitDepth = pr["bitDepth"].toInt(24);
    p.nextId = (Id)pr["nextId"].toDouble(1);
    p.selectedPatternId = (Id)pr["selectedPattern"].toDouble();
    p.selectedChannelId = (Id)pr["selectedChannel"].toDouble();
    auto t = root["transport"].toObject();
    auto& tr = p.transport;
    tr.bpm = t["bpm"].toDouble(140);
    tr.timeSig.numerator = t["tsNum"].toInt(4);
    tr.timeSig.denominator = t["tsDen"].toInt(4);
    tr.metronome = t["metronome"].toBool();
    tr.countIn = t["countIn"].toBool();
    tr.loopEnabled = t["loop"].toBool();
    tr.loopStart = (Tick)t["loopStart"].toDouble();
    tr.loopEnd = (Tick)t["loopEnd"].toDouble(PPQ * 16);
    tr.songMode = t["songMode"].toBool();
    tr.snap = t["snap"].toBool(true);
    tr.snapDivision = t["snapDivision"].toInt(4);
    tr.recordMode = t["recordMode"].toInt();
    tr.countInBars = t["countInBars"].toInt(1);
    tr.position = (Tick)t["position"].toDouble();
    tr.punchEnabled = t["punch"].toBool();
    tr.punchIn = (Tick)t["punchIn"].toDouble();
    tr.punchOut = (Tick)t["punchOut"].toDouble(PPQ * 16);
    tr.loopRecord = t["loopRecord"].toBool();
    for (auto pv : root["patterns"].toArray()) {
        auto o = pv.toObject();
        Pattern pat;
        pat.id = (Id)o["id"].toDouble();
        pat.name = o["name"].toString();
        pat.color = colorF(o["color"], pat.color);
        pat.length = (Tick)o["length"].toDouble();
        pat.stepsPerBeat = o["stepsPerBeat"].toInt(4);
        pat.stepCount = o["stepCount"].toInt(16);
        pat.swing = (float)o["swing"].toDouble();
        for (auto cv : o["channels"].toArray()) {
            auto co = cv.toObject();
            PatternChannelData c;
            c.id = (Id)co["channelId"].toDouble();
            for (auto nv : co["notes"].toArray()) {
                auto n = nv.toObject();
                Note note;
                note.id = (Id)n["id"].toDouble();
                note.pitch = n["p"].toInt();
                note.start = (Tick)n["s"].toDouble();
                note.duration = (Tick)n["d"].toDouble();
                note.velocity = (float)n["v"].toDouble(0.8);
                note.releaseVelocity = (float)n["rv"].toDouble(0.5);
                note.pan = (float)n["pan"].toDouble();
                note.finePitch = (float)n["fp"].toDouble();
                note.channel = n["ch"].toInt();
                note.muted = n["m"].toBool();
                c.notes.push_back(note);
            }
            for (auto sv : co["steps"].toArray()) {
                auto s = sv.toObject();
                Step st;
                st.enabled = s["e"].toBool();
                st.velocity = (float)s["v"].toDouble(0.8);
                st.pan = (float)s["pan"].toDouble();
                st.pitch = (float)s["pi"].toDouble();
                st.probability = (float)s["pr"].toDouble(1);
                st.offset = (float)s["o"].toDouble();
                c.steps.push_back(st);
            }
            if ((int)c.steps.size() < pat.stepCount) c.steps.resize(pat.stepCount);
            pat.channels.push_back(c);
        }
        for (auto lv : o["automation"].toArray()) pat.automationLaneIds.push_back((Id)lv.toDouble());
        p.patterns.push_back(pat);
    }
    for (auto cv : root["channels"].toArray()) {
        auto o = cv.toObject();
        Channel c;
        c.id = (Id)o["id"].toDouble();
        c.name = o["name"].toString();
        c.color = colorF(o["color"], c.color);
        c.type = (ChannelType)o["type"].toInt();
        c.instrumentKey = o["instrument"].toString();
        c.pluginInstanceId = (Id)o["plugin"].toDouble();
        c.samplePath = absPath(o["sample"].toString(), dir);
        c.volume = (float)o["volume"].toDouble(0.8);
        c.pan = (float)o["pan"].toDouble();
        c.mute = o["mute"].toBool();
        c.solo = o["solo"].toBool();
        c.enabled = o["enabled"].toBool(true);
        c.cutItself = o["cutItself"].toBool(false);
        c.mixerTrackId = (Id)o["mixerTrack"].toDouble();
        c.rootNote = o["rootNote"].toInt(60);
        c.midiOutPort = o["midiOutPort"].toInt(-1);
        c.midiOutChannel = o["midiOutChannel"].toInt();
        c.instrumentState = o["state"].toObject();
        p.channels.push_back(c);
    }
    auto pl = root["playlist"].toObject();
    for (auto tv : pl["tracks"].toArray()) {
        auto o = tv.toObject();
        PlaylistTrack t;
        t.id = (Id)o["id"].toDouble();
        t.name = o["name"].toString();
        t.color = colorF(o["color"], t.color);
        t.height = o["height"].toInt(60);
        t.mute = o["mute"].toBool();
        t.solo = o["solo"].toBool();
        t.locked = o["locked"].toBool();
        t.armed = o["armed"].toBool();
        t.recordInput = o["recordInput"].toInt(-1);
        t.mixerTrackId = (Id)o["mixerTrack"].toDouble();
        p.playlistTracks.push_back(t);
    }
    for (auto cv : pl["clips"].toArray()) {
        auto o = cv.toObject();
        Clip c;
        c.id = (Id)o["id"].toDouble();
        c.type = (ClipType)o["type"].toInt();
        c.trackId = (Id)o["track"].toDouble();
        c.start = (Tick)o["start"].toDouble();
        c.length = (Tick)o["length"].toDouble();
        c.sourceId = (Id)o["source"].toDouble();
        c.muted = o["muted"].toBool();
        c.gain = (float)o["gain"].toDouble(1);
        c.fadeIn = (Tick)o["fadeIn"].toDouble();
        c.fadeOut = (Tick)o["fadeOut"].toDouble();
        c.fadeInShape = (float)o["fadeInShape"].toDouble();
        c.fadeOutShape = (float)o["fadeOutShape"].toDouble();
        c.timeStretch = o["stretch"].toDouble(1);
        c.pitch = (float)o["pitch"].toDouble();
        c.sourceOffset = (Tick)o["offset"].toDouble();
        c.reverse = o["reverse"].toBool();
        c.loopSource = o["loop"].toBool(true);
        c.name = o["name"].toString();
        p.clips.push_back(c);
    }
    for (auto mv : root["mixer"].toArray()) {
        auto o = mv.toObject();
        MixerTrack t;
        t.id = (Id)o["id"].toDouble();
        t.name = o["name"].toString();
        t.color = colorF(o["color"], t.color);
        t.volume = (float)o["volume"].toDouble(0.8);
        t.pan = (float)o["pan"].toDouble();
        t.mute = o["mute"].toBool();
        t.solo = o["solo"].toBool();
        t.armed = o["armed"].toBool();
        t.inputChannel = o["input"].toInt(-1);
        t.outputTrackId = (Id)o["output"].toDouble();
        t.isMaster = o["master"].toBool();
        for (auto ev : o["effects"].toArray()) {
            auto e = ev.toObject();
            EffectSlot s;
            s.id = (Id)e["id"].toDouble();
            s.processorKey = e["key"].toString();
            s.pluginInstanceId = (Id)e["plugin"].toDouble();
            s.bypass = e["bypass"].toBool();
            s.mix = (float)e["mix"].toDouble(1);
            s.state = e["state"].toObject();
            s.name = e["name"].toString();
            t.effects.push_back(s);
        }
        for (auto sv : o["sends"].toArray()) {
            auto s = sv.toObject();
            Send sd;
            sd.targetTrackId = (Id)s["target"].toDouble();
            sd.level = (float)s["level"].toDouble();
            sd.preFader = s["pre"].toBool();
            sd.enabled = s["enabled"].toBool(true);
            sd.sidechain = s["sidechain"].toBool();
            t.sends.push_back(sd);
        }
        p.mixerTracks.push_back(t);
    }
    for (auto pv : root["plugins"].toArray()) {
        auto o = pv.toObject();
        PluginInstanceData pl;
        pl.id = (Id)o["id"].toDouble();
        pl.format = o["format"].toString();
        pl.identifier = o["identifier"].toString();
        pl.name = o["name"].toString();
        pl.manufacturer = o["manufacturer"].toString();
        pl.type = o["type"].toString();
        pl.path = o["path"].toString();
        pl.state = QByteArray::fromBase64(o["state"].toString().toLatin1());
        pl.controllerState = QByteArray::fromBase64(o["controllerState"].toString().toLatin1());
        pl.latency = o["latency"].toInt();
        pl.bypass = o["bypass"].toBool();
        auto cache = o["params"].toObject();
        for (auto it = cache.begin(); it != cache.end(); ++it) pl.paramCache[it.key().toUInt()] = it.value().toDouble();
        p.plugins.push_back(pl);
    }
    for (auto av : root["automation"].toArray()) {
        auto o = av.toObject();
        AutomationLane l;
        l.id = (Id)o["id"].toDouble();
        l.name = o["name"].toString();
        l.color = colorF(o["color"], l.color);
        l.target = targetF(o["target"].toObject());
        l.length = (Tick)o["length"].toDouble(PPQ * 4);
        l.patternId = (Id)o["pattern"].toDouble();
        for (auto pv : o["points"].toArray()) {
            auto pt = pv.toObject();
            AutomationPoint ap;
            ap.time = (Tick)pt["t"].toDouble();
            ap.value = (float)pt["v"].toDouble();
            ap.curve = (CurveType)pt["c"].toInt();
            ap.tension = (float)pt["tn"].toDouble();
            l.points.push_back(ap);
        }
        p.automationLanes.push_back(l);
    }
    for (auto av : root["audioReferences"].toArray()) {
        auto o = av.toObject();
        AudioSource a;
        a.id = (Id)o["id"].toDouble();
        a.filePath = absPath(o["path"].toString(), dir);
        a.name = o["name"].toString();
        a.sampleRate = o["sampleRate"].toDouble();
        a.channels = o["channels"].toInt();
        a.frames = (int64_t)o["frames"].toDouble();
        a.reverse = o["reverse"].toBool();
        a.normalize = o["normalize"].toBool();
        a.loop = o["loop"].toBool();
        a.trimStart = (int64_t)o["trimStart"].toDouble();
        a.trimEnd = (int64_t)o["trimEnd"].toDouble();
        a.gain = (float)o["gain"].toDouble(1);
        for (auto sv : o["slices"].toArray()) a.slices.push_back((int64_t)sv.toDouble());
        a.missing = !a.filePath.isEmpty() && !QFileInfo::exists(a.filePath);
        p.audioSources.push_back(a);
    }
    for (auto mv : root["markers"].toArray()) {
        auto o = mv.toObject();
        Marker m;
        m.id = (Id)o["id"].toDouble();
        m.name = o["name"].toString();
        m.position = (Tick)o["position"].toDouble();
        p.markers.push_back(m);
    }
    for (auto mv : root["midiMappings"].toArray()) {
        auto o = mv.toObject();
        MidiMapping m;
        m.cc = o["cc"].toInt(-1);
        m.midiChannel = o["channel"].toInt(-1);
        m.target = targetF(o["target"].toObject());
        m.name = o["name"].toString();
        p.midiMappings.push_back(m);
    }
    if (p.mixerTracks.empty()) {
        MixerTrack m;
        m.id = p.newId();
        m.name = "Master";
        m.isMaster = true;
        p.mixerTracks.push_back(m);
    }
    // ensure nextId is beyond every id present
    Id maxId = 0;
    auto bump = [&](Id i) { maxId = std::max(maxId, i); };
    for (auto& x : p.patterns) {
        bump(x.id);
        for (auto& c : x.channels)
            for (auto& n : c.notes) bump(n.id);
    }
    for (auto& x : p.channels) bump(x.id);
    for (auto& x : p.playlistTracks) bump(x.id);
    for (auto& x : p.clips) bump(x.id);
    for (auto& x : p.mixerTracks) {
        bump(x.id);
        for (auto& e : x.effects) bump(e.id);
    }
    for (auto& x : p.plugins) bump(x.id);
    for (auto& x : p.automationLanes) bump(x.id);
    for (auto& x : p.audioSources) bump(x.id);
    for (auto& x : p.markers) bump(x.id);
    if (p.nextId <= maxId) p.nextId = maxId + 1;
    return true;
}

bool ProjectSerializer::save(const Project& p, const QString& path, QString* error) {
    Project copy = p;
    copy.settings.projectDir = QFileInfo(path).absolutePath();
    QJsonDocument doc(toJson(copy));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(doc.toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool ProjectSerializer::load(const QString& path, Project& out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        if (error) *error = pe.errorString();
        return false;
    }
    out.settings.projectDir = QFileInfo(path).absolutePath();
    return fromJson(doc.object(), out, error);
}

}   // namespace mt
