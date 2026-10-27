"""Score the CLI's key column against the tracker labels (testdata/key_labels.csv) and compare with the old detector's output.
   python tools/key_report.py <new cli csv> [<old cli csv>] [out.md]"""
import sys, csv, re
sys.path.insert(0, 'tools')
import key_eval as K
def detected(row):
    detail = row.get('key_detail') or row['key']
    t, m = K.parse_key(row['key'])
    mode = re.search(r'\((\w+)\)', detail)
    fam = K.family(t, m, '%s %s' % (K.NAMES[t], mode.group(1)) if mode else '')
    return (t, m), fam, detail
def score(rows, songs, label):
    ex = anyk = fam = 0; mir = 0.0; n = 0; en = ef = 0; tier = {'A': [0, 0], 'B': [0, 0]}; lines = []
    for s in songs:
        r = rows.get(s.file)
        if not r or not r['key']: continue
        det, f, detail = detected(r)
        ok_fam = f in s.fams
        if s.edge:
            ok_fam = ok_fam or ((f + 1) % 12 in s.fams) or ((f - 1) % 12 in s.fams); en += 1; ef += ok_fam
            lines.append((s, det, detail, 'edge', ok_fam)); continue
        n += 1; ex += det == s.primary; anyk += det in s.keys; fam += ok_fam; mir += K.mirex(det, s.primary)
        tier[s.tier][0] += 1; tier[s.tier][1] += det == s.primary
        lines.append((s, det, detail, '', ok_fam))
    out = dict(label=label, n=n, exact=ex / n, anyk=anyk / n, fam=fam / n, mirex=mir / n, en=en, ef=ef / max(1, en), tierA=tier['A'], tierB=tier['B'], lines=lines)
    return out
def load(path):
    return {r['file']: r for r in csv.DictReader(open(path, encoding='utf-8'))}
if __name__ == '__main__':
    songs = K.load_songs('', with_features=False)
    new = score(load(sys.argv[1]), songs, 'new detector')
    old = score(load(sys.argv[2]), songs, 'old detector') if len(sys.argv) > 2 else None
    md = ['# Key detector vs the QO & SPECTRE tracker', '', '%d labelled songs (not within ~8 cents of a semitone edge), %d more pitched by ~half a semitone ("edge", scored with +-1 semitone tolerance).' % (new['n'], new['en']), '',
          '| detector | exact key | any listed key | same scale (relative / modal) | MIREX score | edge songs, scale +-1 st | exact on verified labels (A) | exact on unverified (B) |', '|---|---|---|---|---|---|---|---|']
    for o in [x for x in (old, new) if x]:
        md.append('| %s | %.1f %% | %.1f %% | %.1f %% | %.3f | %.0f %% | %d / %d | %d / %d |' % (o['label'], 100 * o['exact'], 100 * o['anyk'], 100 * o['fam'], o['mirex'], 100 * o['ef'], o['tierA'][1], o['tierA'][0], o['tierB'][1], o['tierB'][0]))
    md += ['', '## Per song (new detector)', '', '| song | tracker key | detected | |', '|---|---|---|---|']
    for s, det, detail, edge, okf in new['lines']:
        md.append('| %s | %s%s | %s | %s |' % (s.file[:44], K.NAMES[s.primary[0]] + (' minor' if s.primary[1] else ' major'), ' (' + s.notes.split('.')[0][:22] + ')' if re.search('phrygian|dorian|locrian', s.notes, re.I) else '', detail,
                                                 'exact' if det == s.primary else ('edge, scale ok' if edge and okf else 'same scale' if okf else '')))
    text = '\n'.join(md)
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if out: open(out, 'w', encoding='utf-8').write(text + '\n')
    print('\n'.join(md[:8]))
