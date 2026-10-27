"""End-to-end two-stage key detector: (1) diatonic family from Temperley on sustain-gated peaks, (2) tonic degree by a small softmax over the family's 7 notes. LOO-evaluated."""
import sys, itertools
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, 'tools')
import key_eval as K
MAJ = [0, 2, 4, 5, 7, 9, 11]
DEG_MODE = [0, 1, 1, 0, 0, 1, 1]       # I ii iii IV V vi vii  ->  major / minor family of the mode
def norm01(v):
    m = v.max(); return v / m if m > 0 else v
class Cache: pass
def prep(songs, sources, fam_cfg):
    P = K.profile_matrix(K.PROFILES[fam_cfg.get('profile', 'temperley')])
    for s in songs:
        ch = K.chroma_of(s, fam_cfg.get('cols', 'peaks_g'), fam_cfg.get('weight', 0.5), norm='l2', power=fam_cfg.get('power', 0.5))
        sc = K.score_pearson(ch, P)
        k = int(np.argmax(sc)); s.fam_pred = k if k < 12 else (k - 12 + 3) % 12
        s.fam_sc = sc
        s.src = {c: norm01(K.chroma_of(s, c, 0.5, norm='l2')) for c in sources}
        # temperley major-vs-minor evidence inside each family: score(major f) - score(minor f+9)
        s.mm = np.array([sc[f] - sc[12 + (f + 9) % 12] for f in range(12)])
def deg_feats(s, fam, sources):
    X = np.zeros((7, len(sources) + 1))
    for di, d in enumerate(MAJ):
        pc = (fam + d) % 12
        for j, c in enumerate(sources): X[di, j] = s.src[c][pc]
        X[di, len(sources)] = s.mm[fam] * (1 if DEG_MODE[di] == 0 else -1) if di in (0, 5) else 0.0   # only the I-vs-vi decision uses it
    return X
def fit(Xs, ys, nf, lam):
    w0 = np.zeros(nf + 7)
    def f(w):
        loss = 0; g = np.zeros_like(w)
        for X, y in zip(Xs, ys):
            s = X @ w[:nf] + w[nf:]; s -= s.max(); p = np.exp(s); p /= p.sum()
            loss -= np.log(p[y] + 1e-12); d = p.copy(); d[y] -= 1
            g[:nf] += X.T @ d; g[nf:] += d
        return loss + lam * (w ** 2).sum(), g + 2 * lam * w
    return minimize(f, w0, jac=True, method='L-BFGS-B').x
def run(songs, sources, lam, verbose=False):
    nf = len(sources) + 1
    # training examples use the TRUE (modal-aware) family and degree
    tr = []
    for s in songs:
        t, m = s.primary; fam = K.family(t, m, s.notes); d = (t - fam) % 12
        tr.append((deg_feats(s, fam, sources), MAJ.index(d)) if d in MAJ else None)
    ex = famok = mir = 0
    for i, s in enumerate(songs):
        pairs = [p for j, p in enumerate(tr) if j != i and p is not None]
        w = fit([p[0] for p in pairs], [p[1] for p in pairs], nf, lam)
        fam = s.fam_pred
        sc = deg_feats(s, fam, sources) @ w[:nf] + w[nf:]
        di = int(np.argmax(sc)); det = ((fam + MAJ[di]) % 12, DEG_MODE[di])
        ex += det == s.primary; famok += fam in s.fams; mir += K.mirex(det, s.primary)
        if verbose: print('%-34s label %s %d  det %s %d %s' % (s.file[:34], K.NAMES[s.primary[0]], s.primary[1], K.NAMES[det[0]], det[1], 'OK' if det == s.primary else ''))
    n = len(songs); return ex / n, famok / n, mir / n
if __name__ == '__main__':
    songs = [s for s in K.load_songs('C:/cb/keycache') if not s.edge]
    allsrc = ['canc_bass', 'sub', 'bassall', 'canc_g', 'peaks_g', 'mid', 'canc']
    prep(songs, allsrc, dict())
    for sources in [['canc_bass'], ['canc_bass', 'canc_g'], ['bassall'], ['bassall', 'canc_g'], ['canc_bass', 'sub', 'canc_g'], ['canc_bass', 'bassall', 'canc_g', 'peaks_g'], allsrc]:
        for lam in [0.1, 1.0]:
            print('%-45s lam %-4s end-to-end LOO: exact %.3f family %.3f mirex %.3f' % ('+'.join(sources), lam, *run(songs, sources, lam)))
