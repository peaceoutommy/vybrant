#include "harmony/Harmony.h"
#include <algorithm>
#include <array>
#include <climits>
#include <numeric>
#include <cmath>
#include <set>

namespace mt::harmony {

const std::vector<Quality>& qualities() {
    static const std::vector<Quality> q = {
        {"", {0, 4, 7}, false},
        {"m", {0, 3, 7}, true},
        {"dim", {0, 3, 6}, true},
        {"aug", {0, 4, 8}, false},
        {"sus2", {0, 2, 7}, false},
        {"sus4", {0, 5, 7}, false},
        {"7", {0, 4, 7, 10}, false},
        {"maj7", {0, 4, 7, 11}, false},
        {"m7", {0, 3, 7, 10}, true},
        {"m7♭5", {0, 3, 6, 10}, true},
        {"dim7", {0, 3, 6, 9}, true},
        {"add9", {0, 2, 4, 7}, false},
        {"m(add9)", {0, 2, 3, 7}, true},
        {"maj9", {0, 2, 4, 7, 11}, false},
        {"m9", {0, 2, 3, 7, 10}, true},
        {"9", {0, 2, 4, 7, 10}, false},
        {"6", {0, 4, 7, 9}, false},
        {"m6", {0, 3, 7, 9}, true},
        {"5", {0, 7}, false},
    };
    return q;
}

static int pc(int p) { return ((p % 12) + 12) % 12; }
static int mask(int root, const std::vector<int>& intervals) {
    int m = 0;
    for (int i : intervals) m |= 1 << pc(root + i);
    return m;
}

Chord identify(const std::vector<int>& pitches) {
    if (pitches.empty()) return {};
    int have = 0;
    for (int p : pitches) have |= 1 << pc(p);
    if (__builtin_popcount(have) < 2) return {};
    const int bass = pc(*std::min_element(pitches.begin(), pitches.end()));
    // an exact match wins (and among equal ones, the one rooted on the bass: C6 over Am7 when C is lowest); else the
    // largest chord inside the notes, the rest being extra colour
    Chord best;
    int bestScore = -1;
    for (int exact = 1; exact >= 0 && best.root < 0; --exact)
        for (int q = 0; q < (int)qualities().size(); ++q)
            for (int r = 0; r < 12; ++r) {
                int m = mask(r, qualities()[q].intervals);
                if (exact ? m != have : (m & have) != m) continue;
                int score = (int)qualities()[q].intervals.size() * 4 + (r == bass ? 2 : 0) - (q == Power ? 8 : 0);
                if (score > bestScore) {
                    bestScore = score;
                    best = Chord{r, q, -1};
                }
            }
    if (best.root >= 0 && best.root != bass) best.bass = bass;
    return best;
}

static bool spellsChord(int pcs) {   // these pitch classes are exactly one of the chords
    for (auto& q : qualities())
        for (int r = 0; r < 12; ++r)
            if (mask(r, q.intervals) == pcs) return true;
    return false;
}

std::vector<Segment> segments(const std::vector<Note>& notes, Tick beat, Tick bar) {
    std::vector<const Note*> sorted;
    for (auto& n : notes)
        if (!n.muted) sorted.push_back(&n);
    std::sort(sorted.begin(), sorted.end(), [](const Note* a, const Note* b) { return a->start < b->start; });
    // 1. events: notes that start together, or a little apart while all of them still sound (a strum)
    struct Event {
        Tick start, end, firstEnd;
        int pcs = 0;
        std::vector<const Note*> notes;
    };
    std::vector<Event> events;
    for (const Note* n : sorted) {
        const Tick end = n->start + n->duration;
        if (!events.empty()) {
            Event& e = events.back();
            Tick d = n->start - e.start;
            if (d <= PPQ / 8 || (d <= beat / 2 && n->start < e.firstEnd)) {
                e.notes.push_back(n), e.pcs |= 1 << pc(n->pitch);
                e.end = std::max(e.end, end), e.firstEnd = std::min(e.firstEnd, end);
                continue;
            }
        }
        events.push_back(Event{n->start, end, end, 1 << pc(n->pitch), {n}});
    }
    // 2. chords: consecutive events joined while they still spell one chord
    std::vector<Segment> out;
    std::vector<const Note*> cur;
    int curPcs = 0;
    Tick curStart = 0, curEnd = 0;
    bool stated = false;   // the current chord began as a block or a strum, not one note at a time
    auto finish = [&] {
        if (cur.empty()) return;
        Segment s;
        s.start = curStart, s.end = curEnd;
        for (const Note* n : cur) {
            s.pitches.push_back(n->pitch);
            s.notes.push_back(n->id);
            s.block = s.block && n->start - curStart <= PPQ / 8;
        }
        std::sort(s.pitches.begin(), s.pitches.end());
        s.pitches.erase(std::unique(s.pitches.begin(), s.pitches.end()), s.pitches.end());
        s.chord = identify(s.pitches);
        if (s.chord.root >= 0 && (__builtin_popcount(curPcs) >= 3 || s.block))
            out.push_back(std::move(s));   // a lone fifth only as a block
        cur.clear();
    };
    int line = -1;   // the previous event's pitch, when it was a single note
    for (const Event& e : events) {
        // a single note a step from the single note before it is a melody or a bass line moving, not a chord unfolding
        const int p = e.notes.size() == 1 ? e.notes[0]->pitch : -1;
        const bool step = p >= 0 && line >= 0 && p != line && std::abs(p - line) <= 2;
        line = p;
        bool join = false;
        const bool eventFull = __builtin_popcount(e.pcs) >= 3 && spellsChord(e.pcs);
        if (!cur.empty() && e.start - curEnd <= beat) {
            const int both = curPcs | e.pcs;
            const bool curFull = __builtin_popcount(curPcs) >= 3 && spellsChord(curPcs);
            const bool sameBar = e.start / bar == curStart / bar;
            // under a block or strummed chord still ringing in its bar, other notes are a line over it, not part of a chord
            if (stated && !eventFull && sameBar && e.start < curEnd) continue;
            if (curFull && eventFull) join = e.pcs == curPcs;   // two full chords: only the same one again
            else if (curFull) {   // a full chord, then more notes: the same chord going on, or the same root extended in the bar
                std::vector<int> ps;
                for (const Note* n : cur) ps.push_back(n->pitch);
                for (const Note* n : e.notes) ps.push_back(n->pitch);
                join = (both == curPcs && (sameBar || e.pcs == curPcs)) ||
                    (!step && sameBar && spellsChord(both) &&
                        identify(ps).root == identify(std::vector<int>(ps.begin(), ps.begin() + cur.size())).root);
            } else join = !step && e.start - curStart < bar && (__builtin_popcount(both) <= 2 || spellsChord(both));   // still collecting
        }
        if (!join) {
            finish();
            curPcs = 0, curStart = e.start, curEnd = e.end, stated = eventFull;
        }
        cur.insert(cur.end(), e.notes.begin(), e.notes.end());
        curPcs |= e.pcs, curEnd = std::max(curEnd, e.end);
    }
    finish();
    return out;
}

std::vector<Note> follow(const std::vector<Note>& model, const std::vector<int>& chord, Tick start) {
    std::vector<Note> out;
    if (model.empty() || chord.empty()) return out;
    Tick first = model[0].start, end = 0;
    float vel = 0;
    for (auto& n : model) first = std::min(first, n.start), end = std::max(end, n.start + n.duration), vel += n.velocity;
    if (std::all_of(model.begin(), model.end(), [&](const Note& n) { return n.start - first <= PPQ / 8; })) {   // a block chord
        for (int p : chord) {
            Note n = model[0];
            n.id = 0, n.pitch = p, n.start = start, n.duration = end - first, n.velocity = vel / model.size();
            out.push_back(n);
        }
        return out;
    }
    std::vector<int> ranks;   // the model's pitches, low to high: rank i plays the new chord's i-th note
    for (auto& n : model) ranks.push_back(n.pitch);
    std::sort(ranks.begin(), ranks.end());
    ranks.erase(std::unique(ranks.begin(), ranks.end()), ranks.end());
    std::vector<int> v = chord;
    for (size_t i = 0; v.size() < ranks.size() && v[i] + 12 <= 127; ++i) v.push_back(v[i] + 12);   // fewer notes: double them up an octave
    for (auto& m : model) {
        size_t r = std::lower_bound(ranks.begin(), ranks.end(), m.pitch) - ranks.begin();
        Note n = m;   // more notes than the model has: keep the low ones and the top one, drop from the middle
        n.id = 0;
        n.pitch = v[std::min(r + 1 == ranks.size() ? v.size() - 1 : r, v.size() - 1)];
        n.start = start + (m.start - first);
        out.push_back(n);
    }
    return out;
}

// Spelling: keys written with flats get flat names
static bool flatKey(const Key& k) {
    static const std::set<int> majorFlats = {5, 10, 3, 8, 1}, minorFlats = {2, 7, 0, 5, 10};
    return (k.minor ? minorFlats : majorFlats).count(k.tonic) > 0;
}
static QString pcName(int p, const Key& k) {
    static const char* sharps[] = {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
    static const char* flats[] = {"C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"};
    return (flatKey(k) ? flats : sharps)[pc(p)];
}
// a note named by its scale degree's letter, as the key writes it: B as IV of F♯ major, B♭ as ♭II of A minor, A♯ as iii
// of F♯; a double sharp or flat gives way to the plain name
static QString spell(int p, const Key& k) {
    static const int letterPc[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int degreeLetter[12] = {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};   // I ♭II II ♭III III IV ♯IV V ♭VI VI ♭VII VII
    static const QString letters = "CDEFGAB";
    const int l = (letters.indexOf(pcName(k.tonic, k)[0]) + degreeLetter[pc(p - k.tonic)]) % 7;
    const int acc = (pc(p - letterPc[l]) + 6) % 12 - 6;
    if (std::abs(acc) > 1) return pcName(p, k);
    return letters[l] + QString(acc > 0 ? "♯" : acc < 0 ? "♭" : "");
}
QString keyName(const Key& k) { return pcName(k.tonic, k) + (k.minor ? " minor" : " major"); }
QString chordName(const Chord& c, const Key& k) {
    if (c.root < 0) return {};
    QString s = spell(c.root, k) + qualities()[c.quality].suffix;
    if (c.bass >= 0) s += "/" + spell(c.bass, k);
    return s;
}
QString numeral(const Chord& c, const Key& k) {
    if (c.root < 0) return {};
    static const char* inMajor[] = {"I", "♭II", "II", "♭III", "III", "IV", "♯IV", "V", "♭VI", "VI", "♭VII", "VII"};
    static const char* inMinor[] = {"I", "♭II", "II", "III", "♯III", "IV", "♯IV", "V", "VI", "♯VI", "VII", "♯VII"};
    QString n = (k.minor ? inMinor : inMajor)[pc(c.root - k.tonic)];
    if (qualities()[c.quality].minor) n = n.toLower();
    switch (c.quality) {
    case Dim: n += "°"; break;
    case Dim7: n += "°7"; break;
    case HalfDim7: n += "ø7"; break;
    case Aug: n += "+"; break;
    case Dom7:
    case Min7: n += "7"; break;
    case Maj7: n += "maj7"; break;
    case Dom9:
    case Min9: n += "9"; break;
    case Maj9: n += "maj9"; break;
    case Six:
    case MinSix: n += "6"; break;
    case Sus2: n += "sus2"; break;
    case Sus4: n += "sus4"; break;
    case Add9:
    case MinAdd9: n += "add9"; break;
    case Power: n += "5"; break;
    }
    return n;
}

// ---- what comes next: a hand-written table of common moves per scale degree, and well-known loops

namespace {
struct Move {
    int interval;   // root, in semitones above the tonic
    int quality;
    double weight;
    const char* reason;
};
// The key's own triads, by the semitone their root sits above the tonic
int diatonicQuality(const Key& k, int interval) {
    static const std::array<int, 12> major = {Maj, -1, Min, -1, Min, Maj, -1, Maj, -1, Min, -1, Dim};
    static const std::array<int, 12> minor = {Min, -1, Dim, Maj, -1, Min, -1, Min, Maj, -1, Maj, -1};
    return (k.minor ? minor : major)[pc(interval)];
}
const std::vector<Move>& movesFrom(const Key& k, int interval, bool minorChord) {
    static const std::vector<std::pair<int, std::vector<Move>>> major = {
        {0,
            {{5, Maj, .9, "lifts away from home"}, {7, Maj, .85, "builds toward coming home"},
                {9, Min, .8, "the relative minor: same notes, darker"}, {2, Min, .55, "starts a ii–V"}, {4, Min, .3, "a gentle step up"}}},
        {2,
            {{7, Maj, .95, "ii–V: the strongest setup for home"}, {5, Maj, .5, "steps down, same role, softer"},
                {11, Dim, .3, "tense, leans into I"}}},
        {4,
            {{9, Min, .8, "falls a fifth to the relative minor"}, {5, Maj, .6, "steps up to the subdominant"},
                {2, Min, .35, "steps down toward ii–V"}}},
        {5,
            {{7, Maj, .9, "tension that wants to resolve"}, {0, Maj, .8, "plagal: the soft 'amen' ending"},
                {2, Min, .5, "same role, softer"}, {9, Min, .5, "sinks into the relative minor"}}},
        {7,
            {{0, Maj, .95, "resolves home"}, {9, Min, .75, "deceptive: dodges the expected resolution"},
                {5, Maj, .55, "rock-style step back"}}},
        {9,
            {{5, Maj, .9, "the pop move, as in I–V–vi–IV"}, {2, Min, .6, "falls a fifth toward ii–V"}, {0, Maj, .58, "back up home"},
                {7, Maj, .55, "climbs back toward home"}, {4, Min, .35, "steps down"}}},
        {11, {{0, Maj, .9, "resolves the tension home"}, {4, Min, .4, "falls to iii"}}},
    };
    static const std::vector<std::pair<int, std::vector<Move>>> minor = {
        {0,
            {{8, Maj, .85, "lifts to the major side"}, {5, Min, .8, "a darker subdominant"},
                {10, Maj, .75, "steps down a tone: the natural minor sound"}, {7, Maj, .6, "a major V pulls straight back home"},
                {3, Maj, .72, "the relative major"}}},
        {2, {{7, Maj, .9, "ii°–V–i: the minor cadence"}, {5, Min, .4, "softer, same role"}}},
        {3, {{8, Maj, .8, "falls a fifth"}, {10, Maj, .7, "climbs a tone"}, {5, Min, .5, "steps up to iv"}}},
        {5,
            {{7, Maj, .85, "a major V: strong pull back home"}, {0, Min, .75, "minor plagal: a quiet ending"},
                {10, Maj, .6, "on to the major side"}, {8, Maj, .4, "settles a step down"}}},
        {7, {{0, Min, .9, "resolves home"}, {8, Maj, .65, "deceptive: VI instead of i"}}},
        {8,
            {{10, Maj, .85, "climbs toward the tonic, VI–VII–i"}, {3, Maj, .65, "brightens to the relative major"},
                {5, Min, .55, "turns darker"}, {0, Min, .5, "settles home"}}},
        {10,
            {{3, Maj, .8, "lands on the relative major"}, {0, Min, .75, "VII–i: the natural minor way home"},
                {8, Maj, .5, "steps back down"}}},
    };
    static const std::vector<Move> fallbackMajor = {{0, Maj, .8, "back home"}, {5, Maj, .6, "to the subdominant"},
        {7, Maj, .6, "to the dominant"}, {9, Min, .5, "to the relative minor"}};
    static const std::vector<Move> fallbackMinor = {
        {0, Min, .8, "back home"}, {5, Min, .6, "to the subdominant"}, {8, Maj, .6, "to the major side"}, {10, Maj, .5, "a step down"}};
    // a minor v and a major V in minor move differently; elsewhere the root alone decides
    if (k.minor && interval == 7 && minorChord) {
        static const std::vector<Move> v = {
            {0, Min, .8, "a soft return home"}, {8, Maj, .6, "steps up to VI"}, {10, Maj, .5, "climbs to VII"}};
        return v;
    }
    for (auto& [i, moves] : k.minor ? minor : major)
        if (i == interval) return moves;
    return k.minor ? fallbackMinor : fallbackMajor;
}
struct Loop {
    std::vector<int> intervals;
    const char* name;
};
const std::vector<Loop>& loops(const Key& k) {
    static const std::vector<Loop> inMajor = {{{0, 7, 9, 5}, "I\u2013V\u2013vi\u2013IV, the pop progression"},
        {{0, 9, 5, 7}, "I\u2013vi\u2013IV\u2013V, the 50s progression"}, {{2, 7, 0}, "ii\u2013V\u2013I, the jazz cadence"},
        {{0, 5, 7}, "I\u2013IV\u2013V, the rock and blues basis"},
        {{0, 5, 0, 7, 5, 0, 7}, "the 12-bar blues"}};   // its repeated bars merge into one chord each
    static const std::vector<Loop> inMinor = {{{0, 8, 3, 10}, "i\u2013VI\u2013III\u2013VII, the epic pop loop"},
        {{0, 10, 8, 7}, "i\u2013VII\u2013VI\u2013V, the Andalusian cadence"}, {{0, 8, 10}, "i\u2013VI\u2013VII, the cinematic rise"},
        {{0, 5, 7}, "i\u2013iv\u2013V, the minor cadence"}};
    return k.minor ? inMinor : inMajor;
}
// Colour from outside the key: borrowed from the parallel key, or chromatic chords with a pull of their own, each first in
// line after the degrees it usually follows (IV–iv, V–♭VI, iv–♭II...)
struct Spice {
    int interval, quality;
    std::vector<int> after;
    const char* reason;
};
const std::vector<Spice>& spicy(const Key& k) {
    static const std::vector<Spice> inMajor = {{5, Min, {0, 5}, "borrowed from minor: bittersweet"},
        {10, Maj, {0, 5, 9}, "\u266dVII: a Mixolydian, rock and film colour"}, {8, Maj, {0, 5, 7}, "\u266dVI: a borrowed, cinematic lift"},
        {2, Maj, {0, 9, 5}, "V of V: leans hard into V"}, {4, Maj, {0, 5, 2}, "V of vi: pulls into the relative minor"},
        {9, Maj, {0, 4, 5}, "V of ii: leans into ii"}, {3, Maj, {0, 8, 10}, "\u266dIII: borrowed, a bold rock lift"}};
    static const std::vector<Spice> inMinor = {{1, Maj, {0, 5, 8}, "\u266dII, the Neapolitan: dark and cinematic"},
        {5, Maj, {0, 10}, "a major IV: Dorian brightness"}, {7, Dom7, {0, 5, 8, 2, 1}, "V7: the strongest pull home"},
        {0, Maj, {7}, "a major I: ends bright (Picardy third)"}, {2, Maj, {0, 8}, "a major II: lifts toward V"}};
    return k.minor ? inMinor : inMajor;
}
Chord chordAt(const Key& k, int interval, int quality) { return Chord{pc(k.tonic + interval), quality, -1}; }
}   // namespace

// the key's home chord, plain or coloured
static bool isTonic(const Chord& c, const Key& k) {
    if (c.root != k.tonic || c.quality == Dim || c.quality == Dim7 || c.quality == HalfDim7 || c.quality == Aug) return false;
    return c.quality == Power || c.quality == Sus2 || c.quality == Sus4 || qualities()[c.quality].minor == k.minor;
}

Key detectKey(const std::vector<std::pair<int, double>>& weighted, const std::vector<Chord>& chords) {
    std::vector<Chord> cs;
    for (auto& c : chords)
        if (c.root >= 0) cs.push_back(c);
    // a lone chord (or one chord over and over) is its own key; a diminished one leads into the major key a semitone up
    if (!cs.empty() && std::all_of(cs.begin(), cs.end(), [&](const Chord& c) { return c == cs[0]; })) {
        const Chord& c = cs[0];
        if (c.quality == Dim || c.quality == Dim7 || c.quality == HalfDim7) return Key{pc(c.root + 1), false};
        return Key{c.root, qualities()[c.quality].minor && c.quality != Aug};
    }
    // Krumhansl-Kessler key profiles: how strongly each scale degree belongs to a major or minor key
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minorP[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    double h[12] = {};
    for (auto& [p, w] : weighted) h[pc(p)] += w;
    auto corr = [&](const double* prof, int t) {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) mx += h[pc(i + t)], my += prof[i];
        mx /= 12, my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) {
            double x = h[pc(i + t)] - mx, y = prof[i] - my;
            sxy += x * y, sxx += x * x, syy += y * y;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0.0;
    };
    // how much of the progression sits in the key's scale (minor with its leading tone), chord by chord: the triad decides,
    // a 7th or a 9th on top doesn't take a chord out of the key (the blues' I7 and IV7)
    static const std::vector<int> majorScale = {0, 2, 4, 5, 7, 9, 11}, minorScale = {0, 2, 3, 5, 7, 8, 10, 11};
    auto inScale = [&](const Key& k, int p) {
        const auto& sc = k.minor ? minorScale : majorScale;
        return std::find(sc.begin(), sc.end(), pc(p - k.tonic)) != sc.end();
    };
    auto chordFit = [&](const Key& k) {
        int in = 0;   // a chord in the key counts whole, a common outsider half
        for (auto& c : cs) {
            bool ok = true;
            for (int i : qualities()[c.quality].intervals)
                if (i != 9 && i != 10 && i != 11 && (i != 2 || c.quality == Sus2)) ok = ok && inScale(k, c.root + i);
            const bool common = std::any_of(spicy(k).begin(), spicy(k).end(), [&](const Spice& m) {   // borrowed, V of...
                return pc(k.tonic + m.interval) == c.root && qualities()[m.quality].minor == qualities()[c.quality].minor;
            });
            in += ok ? 2 : common ? 1 : 0;
        }
        return cs.empty() ? 0.0 : in / (2.0 * cs.size());
    };
    // keys sharing their notes (C major, A minor, G major...) fit alike: then the chords decide, the way the ear hears home:
    // starting there, resolving there from V, ending there. Weighed against 50 real songs: 45 read as their sources say.
    auto home = [&](const Key& k) {
        double s = isTonic(cs.front(), k) * 3 + isTonic(cs.back(), k);
        for (size_t j = 0; j + 1 < cs.size(); ++j)
            if (cs[j].root == pc(k.tonic + 7) && !qualities()[cs[j].quality].minor && isTonic(cs[j + 1], k)) {
                s += 3;
                break;
            }
        return s;
    };
    Key best;
    double bestScore = -1e9;
    for (int i = 0; i < 24; ++i) {
        const Key k{i / 2, i % 2 == 1};
        const double r = corr(k.minor ? minorP : major, k.tonic);
        const double score = (cs.empty() ? 0 : home(k) + 6 * chordFit(k)) + 2 * r;   // the notes' profile settles ties
        if (score > bestScore) bestScore = score, best = k;
    }
    return best;
}

// the seventh chord a triad on this degree grows into: V7 (and any major chord in the blues) is dominant
static int seventhOf(const Key& k, int at, int quality, bool bluesy) {
    if (quality == Dim) return HalfDim7;
    if (qualities()[quality].minor) return Min7;
    return bluesy || at == 7 || (k.minor && at == 10) ? Dom7 : Maj7;
}
// the same root with a note more or less (E after E7): not a move
static bool sameRoot(const Chord& a, const Chord& b) {
    if (a.root != b.root) return false;
    const int x = mask(a.root, qualities()[a.quality].intervals), y = mask(b.root, qualities()[b.quality].intervals);
    return (x & y) == x || (x & y) == y;
}

std::vector<Suggestion> next(const std::vector<Chord>& before, const Key& k) {
    if (before.empty()) return {};
    const Chord& last = before.back();
    const int at = pc(last.root - k.tonic);
    // a progression of seventh chords gets seventh chords; dominant sevenths on the tonic and elsewhere: the blues, all dominant
    const auto seventh = [](const Chord& c) {
        return (c.quality >= Dom7 && c.quality <= Dim7) || (c.quality >= Maj9 && c.quality <= Dom9);
    };
    const bool sevenths = 2 * std::count_if(before.begin(), before.end(), seventh) >= (long)before.size();
    std::set<int> dominants;
    for (auto& c : before)
        if (c.quality == Dom7) dominants.insert(c.root);
    const bool bluesy = dominants.count(k.tonic) && dominants.size() >= 2;
    auto colour = [&](Chord c) {
        if (sevenths && (c.quality == Maj || c.quality == Min || c.quality == Dim))
            c.quality = seventhOf(k, pc(c.root - k.tonic), c.quality, bluesy);
        return c;
    };
    std::vector<std::pair<double, Suggestion>> ranked;
    auto add = [&](Chord c, double w, const QString& reason, bool spice) {
        c = colour(c);
        if (sameRoot(c, last)) return;
        for (auto& [rw, s] : ranked)
            if (s.chord == c) {   // the same chord found twice: keep it once, at its better rank and with that reason
                if (w > rw) rw = w, s.reason = reason;
                return;
            }
        ranked.push_back({w, Suggestion{c, reason, spice}});
    };
    // a well-known loop the progression is in the middle of: its next chord comes first, unless only two chords match
    std::vector<int> played;
    for (auto& c : before) played.push_back(pc(c.root - k.tonic));
    for (auto& loop : loops(k)) {
        const int n = (int)loop.intervals.size();
        for (int len = std::min<int>(n - 1, (int)played.size()); len >= (n > 4 ? 3 : 2); --len)
            for (int start = 0; start < n; ++start) {
                bool match = true;
                for (int i = 0; i < len && match; ++i) match = loop.intervals[(start + i) % n] == played[played.size() - len + i];
                if (!match) continue;
                int iv = loop.intervals[(start + len) % n];
                int q = diatonicQuality(k, iv);
                if (k.minor && iv == 7) q = Maj;   // these loops resolve with a major V
                add(chordAt(k, iv, q < 0 ? Maj : q), len >= 3 ? 1.0 + len * 0.1 : 0.9, QString("continues %1").arg(loop.name), false);
            }
    }
    if (last.quality == Dom7 || last.quality == Dom9) {   // a dominant seventh pulls a fifth down, wherever it stands
        const int to = pc(at + 5), q = diatonicQuality(k, to);
        add(chordAt(k, to, q < 0 ? Maj : q == Dim ? Min : q), 1.05, "resolves a fifth down, where a 7th chord pulls", false);
    }
    for (auto& m : movesFrom(k, at, qualities()[last.quality].minor)) add(chordAt(k, m.interval, m.quality), m.weight, m.reason, false);
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Suggestion> out;
    for (auto& [w, s] : ranked)
        if (out.size() < 4) out.push_back(s);
    std::vector<const Spice*> spices;   // the ones idiomatic after this chord first
    for (auto& m : spicy(k)) spices.push_back(&m);
    std::stable_partition(
        spices.begin(), spices.end(), [&](const Spice* m) { return std::count(m->after.begin(), m->after.end(), at) > 0; });
    for (const Spice* m : spices) {
        Chord c = colour(chordAt(k, m->interval, m->quality));
        if (sameRoot(c, last) || std::any_of(out.begin(), out.end(), [&](const Suggestion& s) { return s.chord == c; })) continue;
        if (std::count_if(out.begin(), out.end(), [](const Suggestion& s) { return s.spicy; }) < 3)
            out.push_back(Suggestion{c, m->reason, true});
    }
    return out;
}

std::vector<Suggestion> replacements(const Chord& c, const Key& k) {
    std::vector<Suggestion> out;
    auto add = [&](const Chord& r, const QString& reason, bool spice = false) {
        if (r.root < 0 || r == c || std::any_of(out.begin(), out.end(), [&](const Suggestion& s) { return s.chord == r; })) return;
        out.push_back(Suggestion{r, reason, spice});
    };
    const int at = pc(c.root - k.tonic);
    const bool minor = qualities()[c.quality].minor;
    // the key's chords a third above and below share two notes with this one: the same role, another colour
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11}, minorScale[7] = {0, 2, 3, 5, 7, 8, 10};
    const int* scale = k.minor ? minorScale : major;
    for (int d = 0; d < 7; ++d)
        if (scale[d] == at)
            for (int step : {2, 5}) {   // a third up, a third down
                int iv = scale[(d + step) % 7];
                add(chordAt(k, iv, diatonicQuality(k, iv)),
                    step == 2 ? "a third up: shares two notes, a lighter colour" : "a third down: shares two notes, a deeper colour");
            }
    // the same root, coloured
    add(Chord{c.root, seventhOf(k, at, c.quality == Dim ? Dim : minor ? Min : Maj, false), -1}, "adds the seventh: richer, smoother");
    add(Chord{c.root, minor ? MinAdd9 : Add9, -1}, "adds the ninth: open and modern");
    add(Chord{c.root, Sus2, -1}, "sus2: neither major nor minor, airy");
    add(Chord{c.root, Sus4, -1}, "sus4: suspended, wants to resolve");
    add(Chord{c.root, minor ? Maj : Min, -1}, minor ? "the major version: brighter, borrowed" : "the minor version: darker, borrowed",
        true);
    return out;
}

std::vector<int> voice(const Chord& c, const std::vector<int>& from) {
    if (c.root < 0) return {};
    const auto& iv = qualities()[c.quality].intervals;
    std::vector<int> tones;
    for (int i : iv) tones.push_back(pc(c.root + i));
    const int center = from.empty() ? 60 : (int)std::lround(std::accumulate(from.begin(), from.end(), 0.0) / from.size());
    const bool doubleRoot = from.size() > tones.size();   // a fuller chord before: keep its size by doubling the root on top
    std::vector<int> best;
    long bestCost = LONG_MAX;
    for (size_t inv = 0; inv < tones.size(); ++inv)
        for (int start = center - 18; start <= center + 6; ++start) {
            if (pc(start) != tones[inv]) continue;
            std::vector<int> v = {start};
            for (size_t i = 1; i < tones.size(); ++i) {   // close position, upward from the lowest note
                int p = v.back() + 1;
                while (pc(p) != tones[(inv + i) % tones.size()]) ++p;
                v.push_back(p);
            }
            if (doubleRoot) {
                int p = v.back() + 1;
                while (pc(p) != tones[0]) ++p;
                v.push_back(p);
            }
            if (v.front() < 0 || v.back() > 127) continue;
            long cost = 0;   // every note to the nearest note of the other chord, both ways, plus a little for drifting in register
            for (int a : v) {
                int m = INT_MAX;
                for (int b : from) m = std::min(m, std::abs(a - b));
                cost += from.empty() ? 0 : m;
            }
            for (int b : from) {
                int m = INT_MAX;
                for (int a : v) m = std::min(m, std::abs(a - b));
                cost += m;
            }
            cost = cost * 4 + std::abs((v.front() + v.back()) / 2 - center) + (from.empty() && inv ? 100 : 0);
            if (cost < bestCost) bestCost = cost, best = v;
        }
    return best;
}

}   // namespace mt::harmony
