// Chord assistant: what the notes in the piano roll are (chords, key), which chords could come next or stand in for one,
// and how to voice a chord so it moves the least from the one before. Plain music theory and a hand-written table of
// common moves: no model, instant, offline.
#pragma once
#include "project/Project.h"
#include <QString>
#include <vector>

namespace mt::harmony {

struct Quality {
    const char* suffix;   // in a chord name: "m", "maj7", ...
    std::vector<int> intervals;   // semitones above the root
    bool minor;   // a minor third over the root: a lower-case numeral
};
const std::vector<Quality>& qualities();
enum : int { Maj = 0, Min, Dim, Aug, Sus2, Sus4, Dom7, Maj7, Min7, HalfDim7, Dim7, Add9, MinAdd9, Maj9, Min9, Dom9, Six, MinSix, Power };

struct Key {
    int tonic = 0;   // pitch class
    bool minor = false;
};
struct Chord {
    int root = -1;   // pitch class; -1: not a chord
    int quality = Maj;
    int bass = -1;   // pitch class of the lowest note, when it isn't the root (an inversion)
    bool operator==(const Chord& o) const { return root == o.root && quality == o.quality; }
};
struct Segment {   // one chord in the piano roll: notes played together, strummed or arpeggiated
    Tick start = 0, end = 0;
    std::vector<int> pitches;   // its distinct pitches, low to high
    std::vector<Id> notes;   // the notes that play it
    bool block = true;   // all its notes start together (else a strum or an arpeggio: a new chord follows its rhythm)
    Chord chord;
};
struct Suggestion {
    Chord chord;
    QString reason;
    bool spicy = false;   // outside the key: borrowed or chromatic
};

Chord identify(const std::vector<int>& pitches);
// Chords in a channel's notes. Notes that start together, or a little apart while they ring together (a strum), form an
// event; consecutive events join while they still spell one chord (an arpeggio, a bass note under a chord, Am growing to
// Am7). Two full chords stay apart unless they are the same one repeated; a complete chord stops collecting at a bar line;
// a beat of silence ends one. Notes moving by step (a melody or a bass line) don't unfold a chord, and notes that never
// spell one are skipped.
std::vector<Segment> segments(const std::vector<Note>& notes, Tick beat, Tick bar);
// 'chord' (MIDI pitches, low to high) played the way 'model' plays its chord, from 'start': a block chord gives the whole
// voicing at once, a strum or an arpeggio keeps each note's timing, length and velocity and gives it the new chord's note
// of the same rank (low to high)
std::vector<Note> follow(const std::vector<Note>& model, const std::vector<int>& chord, Tick start);
// The key of these chords (and pitches, weighted by how long they sound): the one whose scale holds the chords (borrowed
// and secondary-dominant ones half) and whose home chord the progression starts on, resolves to from V, or ends on. A lone
// chord is its own key.
Key detectKey(const std::vector<std::pair<int, double>>& weighted, const std::vector<Chord>& chords);
QString keyName(const Key& k);
QString chordName(const Chord& c, const Key& k);
QString numeral(const Chord& c, const Key& k);
std::vector<Suggestion> next(const std::vector<Chord>& before, const Key& k);   // what could follow the last of these
std::vector<Suggestion> replacements(const Chord& c, const Key& k);   // what could stand in for this one
std::vector<int> voice(const Chord& c, const std::vector<int>& from);   // MIDI pitches, moving the least from 'from'

}   // namespace mt::harmony
