"""Train / evaluate the stage-2 tonic model on features written by the PRODUCT extractor (tests/key_dump.cpp).
   python tools/key_train.py [features.csv]   -> LOO accuracy, final weights as C++ constants"""
import sys, csv, collections
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, 'tools')
import key_eval as K
MAJ = [0, 2, 4, 5, 7, 9, 11]
DEG_MINOR = [0, 1, 1, 0, 0, 1, 1]
def load_features(path):
    d = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        v = lambda g: np.array([float(r['%s%d' % (g, i)]) for i in range(12)])
        d[r['file']] = dict(pg=v('pg'), cb=v('cb'), cg=v('cg'), frames=int(r['frames']))
    return d
def stage1(pg):
    P = K.profile_matrix(K.TEMPERLEY if hasattr(K, 'TEMPERLEY') else K.PROFILES['temperley'])
    sc = K.score_pearson(pg, P)
    fam = np.maximum(sc[:12], np.array([sc[12 + (s + 9) % 12] for s in range(12)]))
    return int(np.argmax(fam)), sc, fam
def norm01(c):
    m = c.max(); return c / m if m > 0 else c
def deg_x(f, scale, sc):
    bass, notes = norm01(f['cb']), norm01(f['cg'])
    mm = sc[scale] - sc[12 + (scale + 9) % 12]
    X = np.zeros((7, 3))
    for di, d in enumerate(MAJ):
        pc = (scale + d) % 12
        X[di, 0] = bass[pc]; X[di, 1] = notes[pc]
        X[di, 2] = mm if di == 0 else (-mm if di == 5 else 0.0)
    return X
def fit(Xs, ys, lam):
    nf = 3
    def f(w):
        loss = 0; g = np.zeros_like(w)
        for X, y in zip(Xs, ys):
            s = X @ w[:nf] + w[nf:]; s -= s.max(); p = np.exp(s); p /= p.sum()
            loss -= np.log(p[y] + 1e-12); d = p.copy(); d[y] -= 1
            g[:nf] += X.T @ d; g[nf:] += d
        return loss + lam * (w ** 2).sum(), g + 2 * lam * w
    return minimize(f, np.zeros(nf + 7), jac=True, method='L-BFGS-B').x
def prepare(songs, feats):
    rows = []
    for s in songs:
        f = feats.get(s.file)
        if f is None: continue
        scale, sc, fam = stage1(f['pg'])
        t, m = s.primary; truefam = K.family(t, m, s.notes); d = (t - truefam) % 12
        rows.append(dict(song=s, f=f, scale=scale, sc=sc, X=deg_x(f, scale, sc), Xtrue=deg_x(f, truefam, sc), y=MAJ.index(d) if d in MAJ else None))
    return rows
def decide(r, w):
    s = r['X'] @ w[:3] + w[3:]
    di = int(np.argmax(s)); return ((r['scale'] + MAJ[di]) % 12, DEG_MINOR[di]), di
def evaluate(rows, w_of, verbose=False):
    ex = fam = mir = 0; n = 0
    for i, r in enumerate(rows):
        det, _ = decide(r, w_of(i)); s = r['song']
        n += 1; ex += det == s.primary; fam += r['scale'] in s.fams; mir += K.mirex(det, s.primary)
        if verbose: print('%-36s label %s %d  det %s %d  %s' % (s.file[:36], K.NAMES[s.primary[0]], s.primary[1], K.NAMES[det[0]], det[1], 'OK' if det == s.primary else ''))
    return ex / n, fam / n, mir / n, n
if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'results/keylab/feat_shift0.csv'
    songs = K.load_songs('', with_features=False)
    feats = load_features(path)
    rows_all = prepare(songs, feats)
    rows = [r for r in rows_all if not r['song'].edge]
    print('songs: %d non-edge, %d edge' % (len(rows), len(rows_all) - len(rows)))
    tr = [r for r in rows if r['y'] is not None]
    for lam in [0.03, 0.1, 0.3, 1.0, 3.0]:
        def w_of(i, lam=lam):
            others = [r for j, r in enumerate(rows) if j != i and r['y'] is not None]
            return fit([r['Xtrue'] for r in others], [r['y'] for r in others], lam)
        e = evaluate(rows, w_of)
        print('lam %-5s LOO exact %.3f family %.3f mirex %.3f (n=%d)' % ((lam,) + e))
    # prior-only reference: always the vi (relative minor) of the predicted scale
    w_prior = np.zeros(10); w_prior[3 + 5] = 1.0
    print('reference "always vi of predicted scale": exact %.3f' % evaluate(rows, lambda i: w_prior)[0])
    lam = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    wf = fit([r['Xtrue'] for r in tr], [r['y'] for r in tr], lam)
    print('\nfinal weights (lam %g, all %d songs): wBass %.4f wNotes %.4f wMajorMinor %.4f' % (lam, len(tr), wf[0], wf[1], wf[2]))
    print('degreeBias { ' + ', '.join('%.4f' % b for b in wf[3:]) + ' }')
    print('train-set (optimistic) :', evaluate(rows, lambda i: wf))
