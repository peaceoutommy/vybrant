// Standard MIDI File reading (type 0/1) and writing (type 1), converted to and from the project's ticks (PPQ).
#pragma once
#include "project/Project.h"

namespace mt {

struct MidiFileTrack {
    QString name;
    std::vector<Note> notes;   // ids unset, MIDI channel 0
    std::vector<ControlPoint> controls;   // sorted by tick
};
struct MidiFileData {
    double bpm = 120;   // the first tempo event
    TimeSignature timeSig;   // the first time signature event
    bool hasTempo = false;
    std::vector<MidiFileTrack> tracks;   // reading: each track (type 0: each MIDI channel) with notes or controllers
};

class MidiFile {
public:
    static bool isMidiFile(const QString& path);
    static bool read(const QByteArray& bytes, MidiFileData& out, QString* error = nullptr);
    static QByteArray write(const MidiFileData& d);   // track i plays on MIDI channel i % 16
    static bool load(const QString& path, MidiFileData& out, QString* error = nullptr);
    static bool save(const QString& path, const MidiFileData& d, QString* error = nullptr);
};

}   // namespace mt
