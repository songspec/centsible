"""Joint (scale, tonic degree) softmax: candidates = 12 scales x 7 degrees. Features: scale fit, bass/tonic evidence at the candidate tonic, degree priors."""
import sys
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, 'tools')
import key_eval as K, key_stage2 as S
MAJ = S.MAJ; DEG_MODE = S.DEG_MODE
def cand_feats(s, sources):
    X = np.zeros((84, len(sources) + 2))
    sc = s.fam_sc
    famfit = np.maximum(sc[:12], np.roll(sc[12:], 3))               # best of major(f) / relative minor(f+9) under Temperley
    famfit = (famfit - famfit.mean()) / (famfit.std() + 1e-9)
    for f in range(12):
        for di, d in enumerate(MAJ):
            r = f * 7 + di; pc = (f + d) % 12
            X[r, 0] = famfit[f]
            for j, c in enumerate(sources): X[r, 1 + j] = s.src[c][pc]
            X[r, 1 + len(sources)] = s.mm[f] * (1 if DEG_MODE[di] == 0 else -1) if di in (0, 5) else 0.0
    return X
def idx(fam, d): return fam * 7 + MAJ.index(d)
def fit(Xs, ys, nf, lam):
    def f(w):
        loss = 0; g = np.zeros_like(w)
        for X, y in zip(Xs, ys):
            s = X @ w[:nf] + w[nf:][np.arange(84) % 7]; s -= s.max(); p = np.exp(s); p /= p.sum()
            loss -= np.log(p[y] + 1e-12); d = p.copy(); d[y] -= 1
            g[:nf] += X.T @ d
            for di in range(7): g[nf + di] += d[di::7].sum()
        return loss + lam * (w ** 2).sum(), g + 2 * lam * w
    w0 = np.zeros(nf + 7); w0[0] = 1.0
    return minimize(f, w0, jac=True, method='L-BFGS-B').x
def run(songs, sources, lam, verbose=False):
    nf = len(sources) + 2
    Xs = [cand_feats(s, sources) for s in songs]
    ys = []
    for s in songs:
        t, m = s.primary; fam = K.family(t, m, s.notes); d = (t - fam) % 12
        ys.append(idx(fam, d) if d in MAJ else None)
    ex = famok = mir = 0
    for i, s in enumerate(songs):
        tr = [j for j in range(len(songs)) if j != i and ys[j] is not None]
        w = fit([Xs[j] for j in tr], [ys[j] for j in tr], nf, lam)
        sc = Xs[i] @ w[:nf] + w[nf:][np.arange(84) % 7]
        k = int(np.argmax(sc)); fam, di = divmod(k, 7); det = ((fam + MAJ[di]) % 12, DEG_MODE[di])
        ex += det == s.primary; famok += fam in s.fams; mir += K.mirex(det, s.primary)
    n = len(songs); return ex / n, famok / n, mir / n
if __name__ == '__main__':
    songs = [s for s in K.load_songs('C:/cb/keycache') if not s.edge]
    allsrc = ['canc_bass', 'sub', 'bassall', 'canc_g', 'peaks_g', 'mid', 'canc']
    S.prep(songs, allsrc, dict())
    for sources in [['canc_bass'], ['canc_bass', 'canc_g'], ['bassall', 'canc_g'], ['canc_bass', 'sub', 'canc_g'], ['canc_bass', 'bassall', 'canc_g', 'peaks_g']]:
        for lam in [0.3, 3.0]:
            print('%-40s lam %-4s joint LOO: exact %.3f family %.3f mirex %.3f' % ('+'.join(sources), lam, *run(songs, sources, lam)))
