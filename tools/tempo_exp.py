import sys, csv, itertools
import numpy as np
sys.path.insert(0, 'tools')
import bpm_labels as B          # builds B.labels: file -> ([bpms], sure)
grid = np.arange(60.0, 200.5, 0.5)
curves = {}
for r in csv.DictReader(open('results/keylab/tempo_curves.csv', encoding='utf-8')):
    v = np.array([float(r[k]) for k in r if k != 'file']); curves[r['file']] = v.reshape(3, -1)
files = [f for f in B.labels if f in curves]
fold = B.fold
def err(bpm, bs): return min(abs(fold(bpm) - fold(b)) / fold(b) for b in bs)
def pick(score):
    return grid[int(np.argmax(score))]
def acc(fn, tol=0.01):
    ok = 0; res = []
    for f in files:
        bpm = fn(curves[f]); e = err(bpm, B.labels[f][0]); ok += e <= tol; res.append(e)
    return ok / len(files), res
def prior(c, w): return np.exp(-0.5 * (np.log2(grid / c) / w) ** 2)
print('songs with tracker BPM:', len(files))
print('current (band 0, prior 118/0.9): %.3f' % acc(lambda C: pick(C[0] * prior(118, 0.9)))[0])
for c, w in itertools.product([100, 118, 130], [0.5, 0.9, 1.5]):
    print('  band 0 prior %3d / %.1f: %.3f' % (c, w, acc(lambda C: pick(C[0] * prior(c, w)))[0]))
for band in (1, 2):
    print('band %d alone, prior 118/0.9: %.3f' % (band, acc(lambda C: pick(C[band] * prior(118, 0.9)))[0]))
print('band0 + band1: %.3f' % acc(lambda C: pick((C[0] + C[1]) * prior(118, 0.9)))[0])
print('band1 * band2 (geometric): %.3f' % acc(lambda C: pick(np.sqrt(np.maximum(C[1], 1e-9) * np.maximum(C[2], 1e-9)) * prior(118, 0.9)))[0])
# metrical-level fold: octave-agnostic score, then the 80-160 range
def octave_sum(c):
    out = np.zeros(len(grid))
    for i, b in enumerate(grid):
        s = 0
        for m in (0.5, 1, 2):
            j = int(round((b * m - 60) / 0.5))
            if 0 <= j < len(grid): s += c[j]
        out[i] = s
    return out
sel = (grid >= 80) & (grid < 160)
def folded_pick(c, prior_w=None):
    sc = octave_sum(c) * np.where(sel, 1.0, 0.0)
    if prior_w: sc = sc * prior(118, prior_w)
    return grid[int(np.argmax(sc))]
for band in (0, 1, 2):
    print('octave-folded 80-160, band %d: %.3f' % (band, acc(lambda C: folded_pick(C[band]))[0]))
print('octave-folded band0+band1: %.3f' % acc(lambda C: folded_pick(C[0] + C[1]))[0])
