"""Score the CLI's BPM against the tracker (tempos in the tracker are folded into 80-160): python tools/bpm_report.py <cli csv>"""
import sys, csv, os, re, collections
sys.path.insert(0, 'tools')
import match_tracker_lib as M
rows = M.load_rows(); by = M.index(rows)
MANUAL = {"06 - Can't Tell Me Nothing.flac": ('Kanye West', "Can't Tell Me Nothing", 'Graduation'), "06 Can't Tell Me Nothing.m4a": ('Kanye West', "Can't Tell Me Nothing", 'Graduation'), '01 - POP OUT.flac': ('Playboi Carti', 'POP OUT', 'MUSIC'),
          '06. x2.flac': ('Lil Uzi Vert', 'x2', 'Pink Tape'), '10. Glass.flac': ('kuru', 'Glass', ''), '3. Gracious.flac': ('kuru', 'Gracious', ''), 'SHOW ME RIGHT! pitched up 30 cents .flac': ('Natanya', 'SHOW ME RIGHT!', '')}
def fold(x):
    while x < 80: x *= 2
    while x >= 160: x /= 2
    return x
labels = {}
for f in sorted(os.listdir('testdata/real')):
    if 'detune test' in f: continue
    stem, ps = M.parts(f)
    if f in MANUAL:
        a, s, al = MANUAL[f]; c = [r for r in rows if a.lower() in r['artist'].lower() and s.lower() in r['song'].lower() and al.lower() in r['album'].lower()][:1]
    else:
        c = []
        for p in ps: c += by.get(M.compact(p), [])
        low = M.compact(stem); c = [x for x in c if M.compact(x['artist']) and M.compact(x['artist']) in low] or c
        seen = {}; u = []
        for x in c:
            k = (x['artist'], x['song'], x['album'])
            if k not in seen: seen[k] = 1; u.append(x)
        c = u if len(u) == 1 else []
    if len(c) != 1: continue
    b = [float(v) for v in re.findall(r'\d+(?:\.\d+)?', c[0]['bpm'])]
    if b: labels[f] = (b, c[0]['validity'].lower().startswith(('sure', 'bpm sure')))
det = {r['file']: r for r in csv.DictReader(open(sys.argv[1], encoding='utf-8'))}
n = ok1 = ok2 = okany = ok_alt = 0; tiers = collections.Counter(); bad = []
for f, (b, sure) in labels.items():
    r = det.get(f)
    if not r or not r.get('bpm') or float(r['bpm']) == 0: continue
    mine = fold(float(r['bpm'])); alt = fold(float(r['bpm_alt'])) if float(r['bpm_alt']) > 0 else None
    best = min(abs(mine - fold(x)) / fold(x) for x in b)
    n += 1; ok1 += best <= 0.01; ok2 += best <= 0.02
    if best > 0.02 and alt is not None and min(abs(alt - fold(x)) / fold(x) for x in b) <= 0.02: ok_alt += 1
    if best > 0.02: bad.append((f[:42], round(mine, 1), [round(fold(x), 1) for x in b], round(float(r['bpm']), 1)))
print('BPM vs tracker, folded into 80-160: %d songs; within 1%%: %d (%.0f%%), within 2%%: %d (%.0f%%); misses rescued by the reported half/double alternative: %d' % (n, ok1, 100 * ok1 / n, ok2, 100 * ok2 / n, ok_alt))
for x in bad: print('   miss:', x)
