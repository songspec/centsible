"""Key-detector research evaluator.

Loads testdata/key_labels.csv (ground truth from the QO & SPECTRE tracker), the tuning of every song (results/keylab/baseline_cli.csv), and cached frame
features written by tests/key_frames.cpp, then scores any aggregation x profile combination.

Scores (all on songs whose tuning is not within ~8 cents of a semitone boundary, "edge" songs are reported separately):
  exact    detected (tonic, mode) == the tracker's primary key
  any      ... equals any key the tracker lists for the song
  family   same diatonic key family (the relative major/minor, and the tracker's modes: a "C Phrygian" song is the notes of A-flat major)
  mirex    MIREX weighted score: exact 1, perfect fifth 0.5, relative 0.3, parallel 0.2
"""
import csv, os, re, sys, math, itertools, json
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOTE = {'C': 0, 'C#': 1, 'Db': 1, 'D': 2, 'D#': 3, 'Eb': 3, 'E': 4, 'F': 5, 'F#': 6, 'Gb': 6, 'G': 7, 'G#': 8, 'Ab': 8, 'A': 9, 'A#': 10, 'Bb': 10, 'B': 11}
NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
MODE_OFFSET = {'phrygian': 8, 'dorian': 10, 'locrian': 1, 'lydian': 7, 'mixolydian': 5}   # pitch class of the major key whose notes the mode uses, relative to the tonic

COLS = dict(peaks=slice(0, 12), bass=slice(12, 24), mid=slice(24, 36), sal=slice(36, 48), peaks_g=slice(48, 60), sal_g=slice(60, 72),
            sub=slice(75, 87), bassall=slice(87, 99), canc=slice(99, 111), canc_bass=slice(111, 123), canc_g=slice(123, 135))

KS = dict(major=[6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88], minor=[6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
TEMPERLEY = dict(major=[5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0], minor=[5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0])
SHAATH = dict(major=[6.6, 2.0, 3.5, 2.25, 4.6, 4.0, 2.5, 5.2, 2.4, 3.7, 2.3, 3.4], minor=[6.5, 2.7, 3.5, 5.4, 2.6, 3.5, 2.5, 5.2, 4.0, 2.7, 4.3, 3.2])
PROFILES = dict(ks=KS, temperley=TEMPERLEY, shaath=SHAATH)


def parse_key(s):
    t, m = s.split()
    return NOTE[t], 0 if m == 'major' else 1       # (tonic pc, mode 0 = major / 1 = minor)


def family(tonic, mode, notes=''):
    """pitch class of the major scale that holds the song's notes."""
    fam = tonic if mode == 0 else (tonic + 3) % 12
    m = re.search(r'\b([A-G][#b]?)\s+(phrygian|dorian|locrian|lydian|mixolydian)', notes, re.I)
    if m and NOTE.get(m.group(1)) == tonic:
        fam = (tonic + MODE_OFFSET[m.group(2).lower()]) % 12 if m.group(2).lower() in ('phrygian', 'dorian', 'locrian') else (tonic - (5 if m.group(2).lower() == 'lydian' else 7)) % 12
    return fam


def mirex(det, ref):
    (dt, dm), (rt, rm) = det, ref
    if det == ref: return 1.0
    if dm == rm and (dt - rt) % 12 in (7, 5): return 0.5
    if dm != rm and ((dm == 0 and rm == 1 and dt == (rt + 3) % 12) or (dm == 1 and rm == 0 and dt == (rt - 3) % 12)): return 0.3
    if dt == rt and dm != rm: return 0.2
    return 0.0


class Song:
    pass


def load_songs(cache_dir, with_features=True):
    tuning = {}
    for r in csv.DictReader(open(os.path.join(ROOT, 'results/keylab/baseline_cli.csv'), encoding='utf-8')):
        tuning[r['file']] = float(r['cents']) if r['valid'] == '1' else 0.0
    songs = []
    for r in csv.DictReader(open(os.path.join(ROOT, 'testdata/key_labels.csv'), encoding='utf-8')):
        s = Song()
        s.file = r['file']; s.tier = r['tier']; s.notes = r['notes']; s.artist = r['artist']; s.song = r['song']
        s.keys = [parse_key(k) for k in r['keys'].split(';')]
        s.primary = s.keys[0]
        s.fams = {family(t, m, s.notes if (t, m) == s.primary else '') for t, m in s.keys} | {family(t, m) for t, m in s.keys}   # plain relative-key family and the modal one
        s.bpms = [float(b) for b in r['bpms'].split(';') if b]
        s.cents = tuning.get(s.file, 0.0)
        s.edge = abs(s.cents) > 42.0          # half a semitone off the grid: the key is genuinely ambiguous by a semitone
        if with_features:
            p = os.path.join(cache_dir, s.file + '.f32')
            if not os.path.exists(p): continue
            a = np.fromfile(p, dtype=np.float32)
            s.F = a.reshape(-1, 135)
        songs.append(s)
    return songs


def profile_matrix(prof):
    """24 x 12: row k = key k (0-11 major, 12-23 minor) profile rotated to its tonic."""
    P = np.zeros((24, 12))
    for t in range(12):
        for pc in range(12):
            P[t, pc] = prof['major'][(pc - t) % 12]
            P[12 + t, pc] = prof['minor'][(pc - t) % 12]
    return P


def chroma_of(song, cols='peaks', weight=0.5, gate_silence=0.02, norm='l2', power=1.0, lo=None, hi=None):
    F = song.F
    c = sum(F[:, COLS[n]] for n in cols.split('+')) if '+' in cols else F[:, COLS[cols]]
    c = c.astype(np.float64)
    lvl = F[:, 72].astype(np.float64)
    keep = lvl > gate_silence * lvl.max()
    if lo is not None or hi is not None:
        t = F[:, 74]; keep &= (t >= (lo if lo is not None else -1)) & (t < (hi if hi is not None else 1e9))
    c = c[keep]; lvl = lvl[keep]
    if len(lvl) == 0: return np.zeros(12)
    if norm == 'l2':
        n = np.linalg.norm(c, axis=1, keepdims=True); n[n == 0] = 1; c = c / n
    elif norm == 'sum':
        n = c.sum(axis=1, keepdims=True); n[n == 0] = 1; c = c / n
    w = (lvl / lvl.max()) ** weight if weight else np.ones(len(lvl))
    c = c ** power if power != 1.0 else c
    return (c * w[:, None]).sum(axis=0)


def score_pearson(chroma, P):
    a = chroma - chroma.mean()
    Pm = P - P.mean(axis=1, keepdims=True)
    return (Pm @ a) / (np.linalg.norm(Pm, axis=1) * np.linalg.norm(a) + 1e-12)


def detect(song, cfg):
    prof = cfg['profile'] if isinstance(cfg['profile'], dict) else PROFILES[cfg['profile']]
    P = profile_matrix(prof)
    if 'segments' in cfg and cfg['segments']:
        T = song.F[:, 74].max(); seg = cfg['segments']
        votes = np.zeros(24)
        for s0 in np.arange(0, T, seg):
            ch = chroma_of(song, cfg['cols'], cfg.get('weight', 0.5), lo=s0, hi=s0 + seg, norm=cfg.get('norm', 'l2'), power=cfg.get('power', 1.0))
            if ch.sum() <= 0: continue
            sc = score_pearson(ch, P)
            votes += np.exp(cfg.get('beta', 8.0) * sc) / np.exp(cfg.get('beta', 8.0) * sc).sum()
        sc = votes
    else:
        ch = chroma_of(song, cfg['cols'], cfg.get('weight', 0.5), norm=cfg.get('norm', 'l2'), power=cfg.get('power', 1.0))
        if cfg.get('bass'):
            ch = ch / (np.linalg.norm(ch) + 1e-12) + cfg['bass'] * (lambda b: b / (np.linalg.norm(b) + 1e-12))(chroma_of(song, 'bass', cfg.get('weight', 0.5), norm=cfg.get('norm', 'l2')))
        sc = score_pearson(ch, P)
    k = int(np.argmax(sc))
    return (k % 12, 0 if k < 12 else 1), sc


def evaluate(songs, cfg, verbose=False):
    res = dict(n=0, exact=0, anyk=0, fam=0, mirex=0.0)
    edge = dict(n=0, fam=0)
    rows = []
    for s in songs:
        det, sc = detect(s, cfg)
        fam_det = det[0] if det[1] == 0 else (det[0] + 3) % 12
        ok_fam = fam_det in s.fams
        if s.edge:
            # the key is ambiguous by a semitone: accept the neighbour on either side
            ok_fam = ok_fam or ((fam_det + 1) % 12 in s.fams) or ((fam_det - 1) % 12 in s.fams)
            edge['n'] += 1; edge['fam'] += ok_fam
        else:
            res['n'] += 1
            res['exact'] += det == s.primary
            res['anyk'] += det in s.keys
            res['fam'] += ok_fam
            res['mirex'] += max(mirex(det, k) for k in s.keys[:1])
        rows.append((s, det, sc))
    n = max(1, res['n'])
    out = dict(n=res['n'], exact=res['exact'] / n, anyk=res['anyk'] / n, fam=res['fam'] / n, mirex=res['mirex'] / n, edge_n=edge['n'], edge_fam=edge['fam'] / max(1, edge['n']))
    if verbose:
        for s, det, sc in rows:
            print('%-40s label %-9s det %-9s %s %s' % (s.file[:40], '%s %s' % (NAMES[s.primary[0]], 'min' if s.primary[1] else 'maj'), '%s %s' % (NAMES[det[0]], 'min' if det[1] else 'maj'),
                                                         'EDGE' if s.edge else '    ', 'OK' if det == s.primary else ''))
    return out


def fmt(o):
    return 'exact %.3f  any %.3f  family %.3f  mirex %.3f  (n=%d)  edge family %.2f (n=%d)' % (o['exact'], o['anyk'], o['fam'], o['mirex'], o['n'], o['edge_fam'], o['edge_n'])


if __name__ == '__main__':
    cache = sys.argv[1] if len(sys.argv) > 1 else r'C:\cb\keycache'
    songs = load_songs(cache)
    print(len(songs), 'songs with features and labels')
    for prof in PROFILES:
        for cols in ['peaks', 'peaks_g', 'sal', 'sal_g']:
            print('%-10s %-8s' % (prof, cols), fmt(evaluate(songs, dict(profile=prof, cols=cols))))
