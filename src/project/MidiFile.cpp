#include "project/MidiFile.h"
#include <QFile>
#include <QFileInfo>

namespace mt {

bool MidiFile::isMidiFile(const QString& path) {
    QString s = QFileInfo(path).suffix().toLower();
    return s == "mid" || s == "midi";
}

namespace {
struct ByteReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint8_t byte() {
        if (p < end) return *p++;
        ok = false;
        return 0;
    }
    uint32_t be(int n) {
        uint32_t v = 0;
        while (n--) v = v << 8 | byte();
        return v;
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = byte();
            v = v << 7 | (b & 0x7f);
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return v;
    }
    void skip(uint32_t n) { p += std::min<size_t>(n, end - p); }
};
}   // namespace

bool MidiFile::read(const QByteArray& bytes, MidiFileData& out, QString* error) {
    auto fail = [&](const QString& e) {
        if (error) *error = e;
        return false;
    };
    ByteReader r{(const uint8_t*)bytes.constData(), (const uint8_t*)bytes.constData() + bytes.size()};
    if (r.be(4) != 0x4d546864) return fail("Not a MIDI file");   // "MThd"
    uint32_t headerLen = r.be(4);
    int format = (int)r.be(2), trackCount = (int)r.be(2), division = (int)r.be(2);
    if (!r.ok || headerLen < 6) return fail("The MIDI file header is truncated");
    if (division == 0 || (division & 0x8000)) return fail("MIDI files timed in SMPTE frames are not supported");
    r.skip(headerLen - 6);
    out = MidiFileData();
    bool hasTimeSig = false;
    auto toTicks = [&](int64_t t) { return (Tick)((t * PPQ + division / 2) / division); };
    for (int ti = 0; ti < trackCount && r.ok && r.p < r.end;) {
        uint32_t id = r.be(4), len = r.be(4);
        ByteReader t{r.p, r.p + std::min<size_t>(len, r.end - r.p)};   // lenient about a last chunk that claims too much
        r.skip(len);
        if (id != 0x4d54726b) continue;   // not "MTrk": unknown chunks are skipped
        ++ti;
        // type 0 holds every channel in one track: each MIDI channel becomes its own part; otherwise one part per track
        std::vector<MidiFileTrack> parts(format == 0 ? 16 : 1);
        int open[16][128];   // sounding note -> index in its part
        std::fill(&open[0][0], &open[0][0] + 16 * 128, -1);
        QString name;
        int64_t now = 0;
        uint8_t status = 0;
        auto endNote = [&](int ch, int pitch, Tick at, float releaseVelocity) {
            int& i = open[ch][pitch];
            if (i < 0) return;
            Note& n = parts[format == 0 ? ch : 0].notes[i];
            n.duration = std::max<Tick>(at - n.start, 1);
            n.releaseVelocity = releaseVelocity;
            i = -1;
        };
        while (t.ok && t.p < t.end) {
            now += t.vlq();
            uint8_t b = t.byte();
            if (b == 0xff) {   // meta event
                uint8_t type = t.byte();
                uint32_t n = t.vlq();
                if (!t.ok || n > (uint32_t)(t.end - t.p)) break;
                const uint8_t* d = t.p;
                t.skip(n);
                if (type == 0x2f) break;
                if (type == 0x03 && name.isEmpty()) name = QString::fromUtf8((const char*)d, (int)n).trimmed();
                else if (type == 0x51 && n == 3 && !out.hasTempo && (d[0] | d[1] | d[2])) {
                    out.bpm = 60e6 / (d[0] << 16 | d[1] << 8 | d[2]);
                    out.hasTempo = true;
                } else if (type == 0x58 && n >= 2 && !hasTimeSig) {
                    out.timeSig = {std::max(1, (int)d[0]), 1 << std::min(6, (int)d[1])};
                    hasTimeSig = true;
                }
                continue;
            }
            if (b == 0xf0 || b == 0xf7) {   // sysex
                t.skip(t.vlq());
                continue;
            }
            uint8_t d1;
            if (b & 0x80) {
                status = b;
                d1 = t.byte();
            } else d1 = b;   // running status: the previous status byte repeats
            if (status < 0x80 || status >= 0xf0) break;   // data without a status, or a stray system message: corrupt track
            int type = status & 0xf0, ch = status & 0x0f;
            uint8_t d2 = (type == 0xc0 || type == 0xd0) ? 0 : t.byte();
            d1 &= 0x7f;
            d2 &= 0x7f;
            Tick tick = toTicks(now);
            MidiFileTrack& part = parts[format == 0 ? ch : 0];
            if (type == 0x90 && d2 > 0) {
                endNote(ch, d1, tick, Note{}.releaseVelocity);   // retriggered while sounding: the old note ends here
                Note n;
                n.pitch = d1;
                n.start = tick;
                n.velocity = d2 / 127.f;
                open[ch][d1] = (int)part.notes.size();
                part.notes.push_back(n);
            } else if (type == 0x80 || type == 0x90) endNote(ch, d1, tick, type == 0x80 ? d2 / 127.f : Note{}.releaseVelocity);
            else if (type == 0xb0) setControl(part.controls, tick, d1, d2);
            else if (type == 0xd0) setControl(part.controls, tick, kCtrlPressure, d1);
            else if (type == 0xe0) setControl(part.controls, tick, kCtrlPitchBend, d1 | d2 << 7);
        }
        for (int ch = 0; ch < 16; ++ch)
            for (int p = 0; p < 128; ++p) endNote(ch, p, toTicks(now), Note{}.releaseVelocity);   // still sounding at the end
        for (size_t i = 0; i < parts.size(); ++i) {
            MidiFileTrack& part = parts[i];
            if (part.notes.empty() && part.controls.empty()) continue;
            part.name = format == 0 ? QString("Channel %1").arg(i + 1) : !name.isEmpty() ? name : QString("Track %1").arg(ti);
            out.tracks.push_back(std::move(part));
        }
    }
    return true;
}

QByteArray MidiFile::write(const MidiFileData& d) {
    auto be = [](QByteArray& b, uint32_t v, int n) {
        while (n--) b.append(char(v >> (n * 8) & 0xff));
    };
    auto vlq = [](QByteArray& b, uint32_t v) {
        char buf[5];
        int i = 4;
        buf[i] = char(v & 0x7f);
        while (v >>= 7) buf[--i] = char(0x80 | (v & 0x7f));
        b.append(buf + i, 5 - i);
    };
    auto chunk = [&](QByteArray& file, const QByteArray& body) {
        file += "MTrk";
        be(file, (uint32_t)body.size(), 4);
        file += body;
    };
    const QByteArray endOfTrack("\x00\xff\x2f\x00", 4);
    QByteArray file("MThd");
    be(file, 6, 4);
    be(file, 1, 2);   // type 1: a conductor track (tempo, time signature), then one track per part
    be(file, (uint32_t)d.tracks.size() + 1, 2);
    be(file, PPQ, 2);
    QByteArray conductor("\x00\xff\x51\x03", 4);
    be(conductor, (uint32_t)std::lround(60e6 / std::max(1.0, d.bpm)), 3);
    int denPow = 0;
    while ((1 << denPow) < d.timeSig.denominator && denPow < 6) ++denPow;
    conductor += QByteArray("\x00\xff\x58\x04", 4);
    conductor += char(d.timeSig.numerator);
    conductor += char(denPow);
    conductor += char(24);   // MIDI clocks per metronome click
    conductor += char(8);   // 32nd notes per quarter
    conductor += endOfTrack;
    chunk(file, conductor);
    struct Ev {
        Tick t;
        int rank;   // at one tick: note-offs, then controllers (so a bend applies to the note), then note-ons
        uint8_t status, d1, d2;
    };
    for (size_t i = 0; i < d.tracks.size(); ++i) {
        const MidiFileTrack& tr = d.tracks[i];
        uint8_t ch = (uint8_t)(i % 16);
        std::vector<Ev> ev;
        for (auto& n : tr.notes) {
            uint8_t p = (uint8_t)clamp(n.pitch, 0, 127);
            ev.push_back({n.start, 2, uint8_t(0x90 | ch), p, (uint8_t)clamp((int)std::lround(n.velocity * 127), 1, 127)});
            ev.push_back({n.start + std::max<Tick>(n.duration, 1), 0, uint8_t(0x80 | ch), p,
                (uint8_t)clamp((int)std::lround(n.releaseVelocity * 127), 0, 127)});
        }
        for (auto& c : tr.controls) {
            if (c.controller == kCtrlPitchBend) {
                int v = clamp(c.value, 0, 16383);
                ev.push_back({c.tick, 1, uint8_t(0xe0 | ch), uint8_t(v & 0x7f), uint8_t(v >> 7)});
            } else if (c.controller == kCtrlPressure) ev.push_back({c.tick, 1, uint8_t(0xd0 | ch), (uint8_t)clamp(c.value, 0, 127), 0});
            else ev.push_back({c.tick, 1, uint8_t(0xb0 | ch), (uint8_t)clamp(c.controller, 0, 127), (uint8_t)clamp(c.value, 0, 127)});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t || (a.t == b.t && a.rank < b.rank); });
        QByteArray body("\x00\xff\x03", 3);
        QByteArray name = tr.name.toUtf8();
        vlq(body, (uint32_t)name.size());
        body += name;
        Tick last = 0;
        uint8_t running = 0;
        for (auto& e : ev) {
            Tick at = std::max(e.t, last);
            vlq(body, (uint32_t)(at - last));
            last = at;
            if (e.status != running) body += char(e.status);   // running status
            running = e.status;
            body += char(e.d1);
            if ((e.status & 0xf0) != 0xd0) body += char(e.d2);
        }
        body += endOfTrack;
        chunk(file, body);
    }
    return file;
}

bool MidiFile::load(const QString& path, MidiFileData& out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    return read(f.readAll(), out, error);
}

bool MidiFile::save(const QString& path, const MidiFileData& d, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(write(d)) < 0) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

}   // namespace mt
