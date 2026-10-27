"""Build testdata/key_labels.csv from the tracker match. Tier A = validity Sure / Key Sure (key verified), tier B = other (key listed but unverified)."""
import csv, os, re, sys, collections
sys.path.insert(0, 'tools')
import match_tracker_lib as M
rows = M.load_rows()
by_title = M.index(rows)
# hand-resolved ambiguous / near matches: file -> (artist, song) substrings of the tracker row to take
MANUAL = {
    "06 - Can't Tell Me Nothing.flac": ('Kanye West', "Can't Tell Me Nothing", 'Graduation'),
    "06 Can't Tell Me Nothing.m4a": ('Kanye West', "Can't Tell Me Nothing", 'Graduation'),
    '01 - POP OUT.flac': ('Playboi Carti', 'POP OUT', 'MUSIC'),
    '06. x2.flac': ('Lil Uzi Vert', 'x2', 'Pink Tape'),
    '10. Glass.flac': ('kuru', 'Glass', ''),
    '3. Gracious.flac': ('kuru', 'Gracious', ''),
    'SHOW ME RIGHT! pitched up 30 cents .flac': ('Natanya', 'SHOW ME RIGHT!', ''),
    'Justin Timberlake - FutureSex+LoveSounds - 09 - Summer Love + Set the Mood (prelude).flac': ('Justin Timberlake', 'Set the Mood Prelude', ''),
}
SKIP_PITCHED = ('pitched up 50', 'pitched down -50', 'PITCHED DOWN -50', 'detune test')   # half-semitone offsets: the key is genuinely between two semitones
KEY = re.compile(r'\b([A-G])\s*([#b♯♭]?)\s*(major|minor)\b', re.I)
def parse_keys(s):
    out = []
    for m in KEY.finditer(s):
        out.append((m.group(1).upper() + {'♯': '#', '♭': 'b'}.get(m.group(2), m.group(2)), m.group(3).lower()))
    return out
def bpms(s):
    return [float(x) for x in re.findall(r'\d+(?:\.\d+)?', s)]
labels = []
for f in sorted(os.listdir('testdata/real')):
    if any(k in f for k in SKIP_PITCHED): continue
    stem, ps = M.parts(f)
    cands = []
    if f in MANUAL:
        a, s, al = MANUAL[f]
        cands = [r for r in rows if a.lower() in r['artist'].lower() and s.lower() in r['song'].lower() and al.lower() in r['album'].lower()]
    else:
        for p in ps: cands += by_title.get(M.compact(p), [])
        low = M.compact(stem)
        cands = [c for c in cands if M.compact(c['artist']) and M.compact(c['artist']) in low] or cands
    seen = {}; uniq = []
    for c in cands:
        k = (c['artist'], c['song'], c['album'])
        if k not in seen: seen[k] = 1; uniq.append(c)
    if f in MANUAL: uniq = uniq[:1]
    if len(uniq) != 1: continue
    c = uniq[0]
    keys = parse_keys(c['key'])
    if not keys: continue
    val = c['validity'].lower()
    tier = 'A' if val.startswith('sure') or val.startswith('key sure') else 'B'
    labels.append(dict(file=f, artist=c['artist'], song=c['song'].split(' | ')[0], keys=';'.join(f'{t} {m}' for t, m in keys), bpms=';'.join(str(b) for b in bpms(c['bpm'])),
                       validity=c['validity'].split(' | ')[0], tier=tier, key_raw=c['key'], notes=c['notes']))
with open('testdata/key_labels.csv', 'w', newline='', encoding='utf-8') as fh:
    w = csv.DictWriter(fh, fieldnames=list(labels[0].keys())); w.writeheader(); w.writerows(labels)
print(len(labels), 'labelled files;', collections.Counter(l['tier'] for l in labels), ' with bpm:', sum(bool(l['bpms']) for l in labels))
print(collections.Counter(l['keys'].split(';')[0].split()[1] for l in labels))
for l in labels: print(l['tier'], l['file'][:42].ljust(42), l['keys'].ljust(28), l['bpms'].ljust(12), l['validity'][:8], '|', l['notes'][:50])
