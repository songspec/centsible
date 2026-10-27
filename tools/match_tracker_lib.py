import csv, os, re, collections, unicodedata
def compact(s):
    s = unicodedata.normalize('NFKD', s).lower()
    s = re.sub(r'\(.*?\)|\[.*?\]', ' ', s)
    s = s.replace('&', 'and')
    s = re.sub(r'\b(feat|ft|featuring|with|prod)\b.*$', ' ', s)
    return re.sub(r'[^a-z0-9]', '', s)
def parts(fname):
    stem = os.path.splitext(fname)[0]
    stem = re.sub(r'^\s*\d+(?:[-\.]\d+)?\s*[-\.]?\s*', '', stem)
    ps = [p.strip() for p in re.split(r'\s+-\s+', stem) if p.strip()]
    ps = [re.sub(r'^\d+\s*[-\.]?\s*', '', p) for p in ps]
    return stem, [p for p in ps if p]
def load_rows():
    return list(csv.DictReader(open('testdata/tracker_songs.csv', encoding='utf-8')))
def index(rows):
    d = collections.defaultdict(list)
    for r in rows:
        t = compact(r['song'].split(' | ')[0])
        if t: d[t].append(r)
    return d
