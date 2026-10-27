"""Learn a key scoring function (shared weights per mode over tonic-rotated chroma) with leave-one-out CV, shrunk toward the Krumhansl profile."""
import sys
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, 'tools')
import key_eval as K

def rot(v, t):                       # chroma relative to tonic t
    return np.roll(v, -t)

def feats(song, sources, weight=0.5):
    out = []
    for cols in sources:
        c = K.chroma_of(song, cols, weight)
        c = c / (c.sum() + 1e-12)
        out.append(c * 12.0 - 1.0)   # centred: uniform chroma -> 0
    return out

def design(song, sources):
    """tensor [24 keys, nsrc*12]"""
    fs = feats(song, sources)
    X = np.zeros((24, 12 * len(fs)))
    for k in range(24):
        t = k % 12
        X[k] = np.concatenate([rot(f, t) for f in fs])
    return X

def unpack(w, ns):
    return w[:12 * ns].reshape(ns, 12), w[12 * ns:24 * ns].reshape(ns, 12), w[24 * ns], w[24 * ns + 1]

def scores(X, w, ns):
    wmaj, wmin, bmaj, bmin = unpack(w, ns)
    Wm = np.concatenate([wmaj.reshape(-1)])  # [ns*12]
    Wn = np.concatenate([wmin.reshape(-1)])
    s = np.empty(24)
    s[:12] = X[:12] @ Wm + bmaj
    s[12:] = X[12:] @ Wn + bmin
    return s

def fit(Xs, ys, ns, w0, lam):
    def f(w):
        loss = 0.0; g = np.zeros_like(w)
        Wm = w[:12 * ns]; Wn = w[12 * ns:24 * ns]
        for X, y in zip(Xs, ys):
            s = np.empty(24); s[:12] = X[:12] @ Wm + w[24 * ns]; s[12:] = X[12:] @ Wn + w[24 * ns + 1]
            s -= s.max(); p = np.exp(s); p /= p.sum()
            loss -= np.log(p[y] + 1e-12)
            d = p.copy(); d[y] -= 1.0
            g[:12 * ns] += X[:12].T @ d[:12]; g[12 * ns:24 * ns] += X[12:].T @ d[12:]
            g[24 * ns] += d[:12].sum(); g[24 * ns + 1] += d[12:].sum()
        loss += lam * np.sum((w - w0) ** 2); g += 2 * lam * (w - w0)
        return loss, g
    return minimize(f, w0, jac=True, method='L-BFGS-B', options=dict(maxiter=200)).x

def ks_w0(ns, gamma=3.0, source_weights=None):
    sw = source_weights or [1.0] + [0.0] * (ns - 1)
    z = lambda p: (np.array(p) - np.mean(p)) / np.std(p)
    wmaj = np.concatenate([gamma * sw[i] * z(K.KS['major']) / 12 for i in range(ns)])
    wmin = np.concatenate([gamma * sw[i] * z(K.KS['minor']) / 12 for i in range(ns)])
    return np.concatenate([wmaj, wmin, [0.0, 0.0]])

def loo(songs, sources, lam, gamma=3.0, w0=None):
    ns = len(sources)
    songs = [s for s in songs if not s.edge]
    Xs = [design(s, sources) for s in songs]
    ys = [s.primary[0] + 12 * s.primary[1] for s in songs]
    w0 = ks_w0(ns, gamma) if w0 is None else w0
    ex = fam = 0; mx = 0.0
    for i in range(len(songs)):
        tr = [j for j in range(len(songs)) if j != i]
        w = fit([Xs[j] for j in tr], [ys[j] for j in tr], ns, w0, lam)
        k = int(np.argmax(scores(Xs[i], w, ns)))
        det = (k % 12, 0 if k < 12 else 1)
        ex += det == songs[i].primary
        fd = det[0] if det[1] == 0 else (det[0] + 3) % 12
        fam += fd in songs[i].fams
        mx += K.mirex(det, songs[i].primary)
    n = len(songs)
    return ex / n, fam / n, mx / n

if __name__ == '__main__':
    songs = K.load_songs('C:/cb/keycache')
    for sources in [['canc_g'], ['canc_g', 'canc_bass'], ['canc_g', 'bassall'], ['canc_g', 'canc_bass', 'sub']]:
        for lam in [0.3, 3.0, 30.0]:
            print('%-30s lam %-5s LOO exact %.3f family %.3f mirex %.3f' % ('+'.join(sources), lam, *loo(songs, sources, lam)))
