#include "harmony/ChordLibrary.h"
#include "harmony/Harmony.h"
#include "project/MidiFile.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace mt::harmony {

int writeChordLibrary(const QString& dir) {
    QFile f(":/progressions.json");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    int written = 0;
    for (const auto& gv : QJsonDocument::fromJson(f.readAll()).object()["genres"].toArray()) {
        const auto g = gv.toObject();
        const QString folder = dir + "/" + g["name"].toString();
        QDir().mkpath(folder);
        int i = 0;
        for (const auto& pv : g["progressions"].toArray()) {
            std::vector<Chord> cs;   // "0 7 9:m 5": each root in semitones above the first chord's, then its suffix
            std::vector<std::pair<int, double>> pitches;
            for (const auto& t : pv.toObject()["c"].toString().split(' ')) {
                Chord c{t.section(':', 0, 0).toInt(), Maj, -1};
                for (int q = 0; q < (int)qualities().size(); ++q)
                    if (t.section(':', 1) == qualities()[q].suffix) c.quality = q;
                cs.push_back(c);
                for (int iv : qualities()[c.quality].intervals) pitches.push_back({c.root + iv, 1.0});
            }
            Key k = detectKey(pitches, cs);
            const int shift = (k.minor ? 9 : 0) - k.tonic;   // into C major or A minor
            k.tonic = k.minor ? 9 : 0;
            MidiFileTrack track;
            track.name = "Chords";
            QStringList numerals, names;
            std::vector<int> voicing;
            for (size_t j = 0; j < cs.size(); ++j) {
                cs[j].root = ((cs[j].root + shift) % 12 + 12) % 12;
                voicing = voice(cs[j], voicing);
                for (int p : voicing) {
                    Note n;
                    n.pitch = p, n.start = (Tick)j * 4 * PPQ, n.duration = 4 * PPQ, n.velocity = 0.8f;
                    track.notes.push_back(n);
                }
                numerals << numeral(cs[j], k);
                names << chordName(cs[j], k);
            }
            MidiFileData d;
            d.bpm = g["bpm"].toDouble(120);
            d.tracks.push_back(track);
            const QString name = QString("%1 %2 (%3).mid").arg(++i, 2, 10, QChar('0')).arg(numerals.join("–"), names.join(' '));
            written += MidiFile::save(folder + "/" + name, d);
        }
    }
    return written;
}

QString chordLibrary() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Chord progressions";
    QFile list(":/progressions.json");
    const QByteArray stamp = list.open(QIODevice::ReadOnly) ? QByteArray::number(qHash(list.readAll())) : QByteArray();
    QFile mark(dir + "/.version");
    if (mark.open(QIODevice::ReadOnly) && mark.readAll() == stamp) return dir;
    mark.close();
    // ponytail: a new list overwrites files of the same name; progressions dropped from it stay until the folder is deleted
    if (writeChordLibrary(dir) && mark.open(QIODevice::WriteOnly)) mark.write(stamp);
    return dir;
}

}   // namespace mt::harmony
