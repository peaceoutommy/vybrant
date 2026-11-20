// Chord progressions to start from: the most common in each genre (counted in Chordonomicon, resources/progressions.json),
// written as MIDI files, one chord a bar in C major or A minor, into a folder the browser lists
#pragma once
#include <QString>

namespace mt::harmony {

int writeChordLibrary(const QString& dir);   // every progression as dir/<genre>/<nn> <numerals> (<chords>).mid; how many
QString chordLibrary();   // its folder in the app's data, written on first use and again when the app ships a new list

}   // namespace mt::harmony
