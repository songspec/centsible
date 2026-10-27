import sys, itertools
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, 'tools')
import key_eval as K, key_train as T
MAJ = T.MAJ
def feats_for(f, scale, sc, names):
    bass, notes, pg = T.norm01(f['cb']), T.norm01(f['cg']), T.norm01(f['pg'])
    mm = sc[scale] - sc[12 + (scale + 9) % 12]
    X = np.zeros((7, len(names)))
    for di, d in enumerate(MAJ):
        pc = (scale + d) % 12
        tri = [(scale + MAJ[(di + k) % 7]) % 12 for k in (0, 2, 4)]
        v = dict(bass=bass[pc], notes=notes[pc], pg=pg[pc], mm=(mm if di == 0 else -mm if di == 5 else 0.0),
                 tri_notes=np.mean([notes[p] for p in tri]), tri_bass=np.mean([bass[p] for p in tri]), tri_pg=np.mean([pg[p] for p in tri]),
                 fifth_bass=bass[tri[2]], fifth_notes=notes[tri[2]], third_notes=notes[tri[1]], third_pg=pg[tri[1]])
        for j, nme in enumerate(names): X[di, j] = v[nme]
    return X
def fit(Xs, ys, nf, lam):
    def f(w):
        loss = 0; g = np.zeros_like(w)
        for X, y in zip(Xs, ys):
            s = X @ w[:nf] + w[nf:]; s -= s.max(); p = np.exp(s); p /= p.sum()
            loss -= np.log(p[y] + 1e-12); d = p.copy(); d[y] -= 1
            g[:nf] += X.T @ d; g[nf:] += d
        return loss + lam * (w ** 2).sum(), g + 2 * lam * w
    return minimize(f, np.zeros(nf + 7), jac=True, method='L-BFGS-B').x
songs = K.load_songs('', with_features=False); feats = T.load_features(sys.argv[1] if len(sys.argv) > 1 else 'results/keylab/feat_shift0.csv')
rows = [r for r in T.prepare(songs, feats) if not r['song'].edge]
def loo(names, lam=1.0):
    nf = len(names); ex = 0
    Xt = [feats_for(r['f'], K.family(*r['song'].primary, r['song'].notes), r['sc'], names) if r['y'] is not None else None for r in rows]
    ex = fam = 0; mir = 0
    for i, r in enumerate(rows):
        idx = [j for j in range(len(rows)) if j != i and r['y'] is not None and rows[j]['y'] is not None]
        w = fit([Xt[j] for j in idx], [rows[j]['y'] for j in idx], nf, lam)
        X = feats_for(r['f'], r['scale'], r['sc'], names)
        di = int(np.argmax(X @ w[:nf] + w[nf:])); det = ((r['scale'] + MAJ[di]) % 12, T.DEG_MINOR[di])
        ex += det == r['song'].primary; mir += K.mirex(det, r['song'].primary)
    return ex / len(rows), mir / len(rows)
base = ['bass', 'notes', 'mm']
print('base %-40s LOO exact %.3f mirex %.3f' % (base, *loo(base)))
for extra in ['pg', 'tri_notes', 'tri_bass', 'tri_pg', 'fifth_bass', 'fifth_notes', 'third_notes', 'third_pg']:
    print('+ %-10s %-30s LOO exact %.3f mirex %.3f' % (extra, '', *loo(base + [extra])))
for combo in [['bass', 'tri_notes', 'mm'], ['bass', 'notes', 'tri_notes', 'mm'], ['bass', 'notes', 'tri_pg', 'mm'], ['bass', 'notes', 'tri_bass', 'tri_notes', 'mm']]:
    print('%-50s LOO exact %.3f mirex %.3f' % (combo, *loo(combo)))
