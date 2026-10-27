"""Match audio files in testdata/real to rows of testdata/tracker_songs.csv; write testdata/key_labels.csv (+ ambiguity report)."""
import csv, os, re, sys, collections, unicodedata
def compact(s):
    s = unicodedata.normalize('NFKD', s).lower()
    s = re.sub(r'\(.*?\)|\[.*?\]', ' ', s)          # (feat ...), [FLAC ...]
    s = s.replace('&', 'and')
    s = re.sub(r'\b(feat|ft|featuring|with|prod)\b.*$', ' ', s)
    return re.sub(r'[^a-z0-9]', '', s)
def parts(fname):
    stem = os.path.splitext(fname)[0]
    stem = re.sub(r'^\s*\d+(?:[-\.]\d+)?\s*[-\.]?\s*', '', stem)   # leading track number
    ps = [p.strip() for p in re.split(r'\s+-\s+', stem) if p.strip()]
    ps = [re.sub(r'^\d+\s*[-\.]?\s*', '', p) for p in ps]
    return stem, [p for p in ps if p]
rows = list(csv.DictReader(open('testdata/tracker_songs.csv', encoding='utf-8')))
by_title = collections.defaultdict(list)
for r in rows:
    t = compact(r['song'].split(' | ')[0])
    if t: by_title[t].append(r)
out = []
for f in sorted(os.listdir('testdata/real')):
    stem, ps = parts(f)
    cands = []
    for i, p in enumerate(ps):
        cands += by_title.get(compact(p), [])
    # artist words in the filename narrow down
    low = compact(stem)
    narrowed = [c for c in cands if compact(c['artist']) and compact(c['artist']) in low] or cands
    seen = {}; uniq = []
    for c in narrowed:
        k = (c['artist'], c['song'], c['album'])
        if k not in seen: seen[k] = 1; uniq.append(c)
    out.append((f, uniq))
n0 = sum(1 for f, u in out if not u); n1 = sum(1 for f, u in out if len(u) == 1); nm = sum(1 for f, u in out if len(u) > 1)
print('files', len(out), 'unmatched', n0, 'unique', n1, 'ambiguous', nm)
for f, u in out:
    if len(u) == 1:
        c = u[0]; print('OK  ', f[:45].ljust(45), '->', c['artist'][:18], '|', c['song'][:28], '|', c['key'], '|', c['bpm'], '|', c['validity'][:12])
print('--- ambiguous')
for f, u in out:
    if len(u) > 1:
        print(f[:60]); [print('      ', c['artist'][:22], '|', c['album'][:22], '|', c['song'][:30], '|', c['key'], '|', c['bpm'], '|', c['validity'][:10]) for c in u[:6]]
print('--- unmatched')
for f, u in out:
    if not u: print('  ', f)
