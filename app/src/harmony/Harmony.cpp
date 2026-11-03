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

std::vector<Segment> segments(const std::vector<Note>& notes, Tick tolerance) {
    std::vector<const Note*> sorted;
    for (auto& n : notes)
        if (!n.muted) sorted.push_back(&n);
    std::sort(sorted.begin(), sorted.end(), [](const Note* a, const Note* b) { return a->start < b->start; });
    std::vector<Segment> out;
    for (size_t i = 0; i < sorted.size();) {
        Segment s;
        s.start = sorted[i]->start;
        for (; i < sorted.size() && sorted[i]->start - s.start <= tolerance; ++i) {
            s.pitches.push_back(sorted[i]->pitch);
            s.end = std::max(s.end, sorted[i]->start + sorted[i]->duration);
        }
        std::sort(s.pitches.begin(), s.pitches.end());
        s.pitches.erase(std::unique(s.pitches.begin(), s.pitches.end()), s.pitches.end());
        s.chord = identify(s.pitches);
        if (s.chord.root >= 0) out.push_back(std::move(s));
    }
    return out;
}

Key detectKey(const std::vector<std::pair<int, double>>& weighted, const Chord& first) {
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
    Key best;
    double bestR = -2;
    for (int t = 0; t < 12; ++t)
        for (bool m : {false, true}) {
            double r = corr(m ? minorP : major, t);
            if (r > bestR) bestR = r, best = Key{t, m};
        }
    // relative keys share their notes: the first chord decides which of the two the progression is in
    if (first.root >= 0) {
        bool firstMinor = qualities()[first.quality].minor;
        if (!best.minor && firstMinor && first.root == pc(best.tonic + 9)) best = Key{first.root, true};
        else if (best.minor && !firstMinor && first.root == pc(best.tonic + 3)) best = Key{first.root, false};
    }
    return best;
}

// Spelling: keys written with flats get flat names
static bool flatKey(const Key& k) {
    static const std::set<int> majorFlats = {5, 10, 3, 8, 1, 6}, minorFlats = {2, 7, 0, 5, 10, 3};
    return (k.minor ? minorFlats : majorFlats).count(k.tonic) > 0;
}
static QString pcName(int p, const Key& k, bool flat = false) {
    static const char* sharps[] = {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
    static const char* flats[] = {"C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"};
    return (flat || flatKey(k) ? flats : sharps)[pc(p)];
}
// a root its numeral calls lowered (♭II, ♭VII, ...) is spelled with a flat in any key: B♭, not A♯, in A minor
static bool loweredDegree(int interval, const Key& k) {
    const int i = pc(interval);
    return k.minor ? i == 1 : i == 1 || i == 3 || i == 8 || i == 10;
}
QString keyName(const Key& k) { return pcName(k.tonic, k) + (k.minor ? " minor" : " major"); }
QString chordName(const Chord& c, const Key& k) {
    if (c.root < 0) return {};
    QString s = pcName(c.root, k, loweredDegree(c.root - k.tonic, k)) + qualities()[c.quality].suffix;
    if (c.bass >= 0) s += "/" + pcName(c.bass, k);
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
            {{5, Maj, .9, "the pop move, as in I–V–vi–IV"}, {2, Min, .6, "falls a fifth toward ii–V"},
                {7, Maj, .55, "climbs back toward home"}, {4, Min, .35, "steps down"}}},
        {11, {{0, Maj, .9, "resolves the tension home"}, {4, Min, .4, "falls to iii"}}},
    };
    static const std::vector<std::pair<int, std::vector<Move>>> minor = {
        {0,
            {{8, Maj, .85, "lifts to the major side"}, {5, Min, .8, "a darker subdominant"},
                {10, Maj, .75, "steps down a tone: the natural minor sound"}, {7, Maj, .6, "a major V pulls straight back home"},
                {3, Maj, .5, "the relative major"}}},
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
        static const std::vector<Move> v = {{0, Min, .8, "a soft return home"}, {8, Maj, .6, "steps up to VI"}};
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
        {{0, 5, 7}, "I\u2013IV\u2013V, the rock and blues basis"}};
    static const std::vector<Loop> inMinor = {{{0, 8, 3, 10}, "i\u2013VI\u2013III\u2013VII, the epic pop loop"},
        {{0, 10, 8, 7}, "i\u2013VII\u2013VI\u2013V, the Andalusian cadence"}, {{0, 8, 10}, "i\u2013VI\u2013VII, the cinematic rise"},
        {{0, 5, 7}, "i\u2013iv\u2013V, the minor cadence"}};
    return k.minor ? inMinor : inMajor;
}
// Colour from outside the key: borrowed from the parallel key, or chromatic chords with a pull of their own
const std::vector<Move>& spicy(const Key& k) {
    static const std::vector<Move> inMajor = {{5, Min, 0, "borrowed from minor: bittersweet"},
        {10, Maj, 0, "\u266dVII: a Mixolydian, rock and film colour"}, {8, Maj, 0, "\u266dVI: a borrowed, cinematic lift"},
        {2, Maj, 0, "V of V: leans hard into V"}, {4, Maj, 0, "V of vi: pulls into the relative minor"}};
    static const std::vector<Move> inMinor = {{1, Maj, 0, "\u266dII, the Neapolitan: dark and cinematic"},
        {5, Maj, 0, "a major IV: Dorian brightness"}, {7, Dom7, 0, "V7: the strongest pull home"},
        {0, Maj, 0, "a major I: ends bright (Picardy third)"}};
    return k.minor ? inMinor : inMajor;
}
Chord chordAt(const Key& k, int interval, int quality) { return Chord{pc(k.tonic + interval), quality, -1}; }
}   // namespace

std::vector<Suggestion> next(const std::vector<Chord>& before, const Key& k) {
    if (before.empty()) return {};
    const Chord& last = before.back();
    const int at = pc(last.root - k.tonic);
    std::vector<std::pair<double, Suggestion>> ranked;
    auto add = [&](const Chord& c, double w, const QString& reason, bool spice) {
        if (c == last) return;
        for (auto& [rw, s] : ranked)
            if (s.chord == c) {   // the same chord found twice: keep it once, at its better rank and with that reason
                if (w > rw) rw = w, s.reason = reason;
                return;
            }
        ranked.push_back({w, Suggestion{c, reason, spice}});
    };
    // a well-known loop the progression is in the middle of: its next chord comes first
    std::vector<int> played;
    for (auto& c : before) played.push_back(pc(c.root - k.tonic));
    for (auto& loop : loops(k)) {
        const int n = (int)loop.intervals.size();
        for (int len = std::min<int>(n - 1, (int)played.size()); len >= 2; --len)
            for (int start = 0; start < n; ++start) {
                bool match = true;
                for (int i = 0; i < len && match; ++i) match = loop.intervals[(start + i) % n] == played[played.size() - len + i];
                if (!match) continue;
                int iv = loop.intervals[(start + len) % n];
                int q = diatonicQuality(k, iv);
                if (k.minor && iv == 7) q = Maj;   // these loops resolve with a major V
                add(chordAt(k, iv, q < 0 ? Maj : q), 1.0 + len * 0.1, QString("continues %1").arg(loop.name), false);
            }
    }
    for (auto& m : movesFrom(k, at, qualities()[last.quality].minor)) add(chordAt(k, m.interval, m.quality), m.weight, m.reason, false);
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Suggestion> out;
    for (auto& [w, s] : ranked)
        if (out.size() < 4) out.push_back(s);
    for (auto& m : spicy(k)) {
        Chord c = chordAt(k, m.interval, m.quality);
        if (c == last || std::any_of(out.begin(), out.end(), [&](const Suggestion& s) { return s.chord == c; })) continue;
        if (std::count_if(out.begin(), out.end(), [](const Suggestion& s) { return s.spicy; }) < 3)
            out.push_back(Suggestion{c, m.reason, true});
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
    const int seventh = diatonicQuality(k, at) == Dim ? HalfDim7
        : !minor                                      ? (k.minor ? (at == 7 || at == 10 ? Dom7 : Maj7) : (at == 7 ? Dom7 : Maj7))
                                                      : Min7;
    add(Chord{c.root, seventh, -1}, "adds the seventh: richer, smoother");
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
