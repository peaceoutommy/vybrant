# Builds resources/progressions.json: python3 resources/progressions.py chordonomicon_v2.csv resources/progressions.json
# (the CSV: huggingface.co/datasets/ailsntua/Chordonomicon, CC BY-NC 4.0).
# The chords song sections open with, per genre, from Chordonomicon: transposed to start on 0, counted once per song,
# rotations of one loop folded together. Writes only these patterns and their counts: no songs.
import csv, sys, re, json, collections
csv.field_size_limit(10**9)
LETTER = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
QUAL = {'': '', 'min': 'm', '7': '7', 'min7': 'm7', 'no3d': '5', 'maj7': 'maj7', 'add9': 'add9', 'sus4': 'sus4', 'sus2': 'sus2',
        'add13': '6', '9': '9', '7sus4': 'sus4', 'dim': 'dim', 'min9': 'm9', 'minadd13': 'm6', 'dim7': 'dim7', 'maj9': 'maj9',
        'aug': 'aug', '13': '7', 'min11': 'm7', '11': 'sus4', 'add11': '', 'minadd11': 'm', '7sus2': 'sus2', 'minadd9': 'm(add9)',
        'minmaj7': 'm', '7b9': '7', 'maj7sus2': 'sus2', 'augmaj7': 'aug', 'maj13': 'maj7', 'min13': 'm7', 'maj7sus4': 'sus4',
        'maj11': 'maj7', 'min7b5': 'm7♭5', 'hdim7': 'm7♭5'}

def chord(t):
    t = t.split('/')[0]
    if not t or t[0] not in LETTER: return None
    root, rest = LETTER[t[0]], t[1:]
    if rest.startswith('s') and not rest.startswith('sus'): root, rest = root + 1, rest[1:]
    elif rest.startswith('b'): root, rest = root - 1, rest[1:]
    q = QUAL.get(rest)
    return None if q is None else (root % 12, q)

GROUPS = [  # name, tempo, test on (main genre, spotify tags)
    ('Pop', 110, lambda m, g: m == 'pop'),
    ('Rock', 120, lambda m, g: m in ('rock', 'pop rock')),
    ('Indie & Alternative', 115, lambda m, g: m == 'alternative' or any('indie' in t for t in g)),
    ('Punk & Emo', 165, lambda m, g: m == 'punk' or any('emo' in t for t in g)),
    ('Metal', 130, lambda m, g: m == 'metal'),
    ('Country', 105, lambda m, g: m == 'country'),
    ('Folk & Singer-songwriter', 95, lambda m, g: any('folk' in t or 'singer-songwriter' in t for t in g)),
    ('Hip-hop & Rap', 90, lambda m, g: m == 'rap' or any('hip hop' in t or 'rap' == t for t in g)),
    ('Trap', 140, lambda m, g: any('trap' in t for t in g)),
    ('R&B & Soul', 85, lambda m, g: m == 'soul' or any('r&b' in t or 'soul' in t for t in g)),
    ('Gospel & Worship', 75, lambda m, g: any('gospel' in t or 'worship' in t or t == 'ccm' for t in g)),
    ('Jazz', 120, lambda m, g: m == 'jazz' or any('jazz' in t for t in g)),
    ('Blues', 100, lambda m, g: any('blues' in t for t in g)),
    ('Funk & Disco', 115, lambda m, g: any('funk' in t or 'disco' in t for t in g)),
    ('Reggae & Ska', 90, lambda m, g: m == 'reggae' or any('ska' in t or 'reggae' in t or 'dancehall' in t for t in g)),
    ('Latin', 100, lambda m, g: any('latin' in t or 'reggaeton' in t or 'bossa' in t or 'samba' in t for t in g)),
    ('EDM & House', 124, lambda m, g: m == 'electronic' or any(w in t for t in g for w in ('edm', 'house', 'techno', 'trance', 'dubstep', 'electro'))),
    ('Lo-fi & Chill', 80, lambda m, g: any('lo-fi' in t or 'chill' in t for t in g)),
    ('K-pop & J-pop', 120, lambda m, g: any(w in t for t in g for w in ('k-pop', 'j-pop', 'anime', 'j-rock'))),
    ('Cinematic', 90, lambda m, g: any(w in t for t in g for w in ('soundtrack', 'orchestral', 'video game', 'score'))),
]

def rel(seq):  # transposed to start on 0
    r0 = seq[0][0]
    return tuple(((r - r0) % 12, q) for r, q in seq)

counts = {name: collections.Counter() for name, _, _ in GROUPS}
songs = collections.Counter()
everywhere, allSongs = collections.Counter(), 0
with open(sys.argv[1], newline='') as f:
    for row in csv.DictReader(f):
        tags = re.findall(r"'([^']*)'", row['genres'])
        groups = [name for name, _, test in GROUPS if test(row['main_genre'], tags)]
        if not groups: continue
        found = set()
        for sec in re.split(r'<[^>]*>', row['chords']):
            cs = []
            for t in sec.split():
                c = chord(t)
                if c is None: cs = None; break
                if not cs or cs[-1] != c: cs.append(c)
            if not cs: continue
            if len(cs) >= 4 and len({c for c in cs[:4]}) >= 2: found.add(rel(cs[:4]))
            eight = cs[:8]
            periodic = any(all(eight[i] == eight[i % k] for i in range(len(eight))) for k in (2, 3, 4))   # a shorter loop played twice
            if len(cs) >= 8 and not periodic and len(set(eight)) >= 3: found.add(rel(eight))
        for g in groups:
            songs[g] += 1
            counts[g].update(found)
        everywhere.update(found)
        allSongs += 1

def canon(p):  # one loop, whichever chord it starts on
    return min(rel(p[i:] + p[:i]) for i in range(len(p)))

def text(p): return ' '.join(f'{r}:{q}' if q else str(r) for r, q in p)

out = {'source': 'Most common section-opening progressions per genre, counted in Chordonomicon (Kantarelis et al. 2024, '
                  'arXiv:2410.22046, CC BY-NC 4.0). Intervals in semitones from the first chord.', 'genres': []}
for name, bpm, _ in GROUPS:
    # common in the genre, and more common there than elsewhere: n x lift^0.7 (lift capped), at least 0.15% of its songs
    def score(item):
        p, n = item
        lift = (n / songs[name]) / (everywhere[p] / allSongs)
        return n * min(lift, 5) ** 0.7
    pool = [(p, n) for p, n in counts[name].items() if n >= max(5, songs[name] * 0.0015)]
    pool.sort(key=score, reverse=True)
    picked = {4: [], 8: []}
    seen, vamps = set(), 0
    for p, n in pool:
        k = len(p)
        if len(picked[k]) >= (24 if k == 4 else 8) or canon(p) in seen: continue
        vamp = len(set(p)) == 2
        if vamp and vamps >= 2: continue
        vamps += vamp
        seen.add(canon(p))
        picked[k].append((p, n))
    out['genres'].append({'name': name, 'bpm': bpm, 'songs': songs[name],
                          'progressions': [{'c': text(p), 'n': n} for p, n in picked[4] + picked[8]]})
json.dump(out, open(sys.argv[2], 'w'), ensure_ascii=False, indent=1)
for g in out['genres']:
    print(f"{g['name']} ({g['songs']} songs):", ' | '.join(f"{p['c']} ({p['n']})" for p in g['progressions'][:6]))
