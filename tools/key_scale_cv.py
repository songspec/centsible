"""Does a LEARNED scale stage beat Temperley's profiles now that there are ~140 labelled songs?

   python tools/key_scale_cv.py <features csv from key_dump>

The scale (the major key whose seven notes the song uses; relative major / minor share it) is scored from the song's chroma by 12 shift-invariant weights per chroma
type: score(s) = sum_i wp[i] pg[s+i] + wc[i] cg[s+i] + wb[i] cb[s+i], softmax over the 12 scales, fitted with an L2 penalty. 10-fold cross-validation (repeated with
different splits) against the Temperley baseline of the shipped detector. Songs are scored by "the detected scale is one of the song's accepted scales".
"""
import sys

import numpy as np
from scipy.optimize import minimize

sys.path.insert(0, "tools")
import key_eval as K
import key_train as T


def norm(c):
    s = c.sum()
    return c / s if s > 0 else c


def rot(c, s):
    return np.roll(c, -s)          # element i = c[(s + i) % 12]


def design(f, kinds):
    """12 scales x (12 * len(kinds)) design matrix"""
    cols = []
    for k in kinds:
        c = norm(f[k])
        cols.append(np.array([rot(c, s) for s in range(12)]))
    return np.concatenate(cols, axis=1)


def fit(X, y, lam):
    d = X[0].shape[1]

    def obj(w):
        loss = 0.0
        g = np.zeros_like(w)
        for Xi, yi in zip(X, y):
            s = Xi @ w
            s -= s.max()
            p = np.exp(s)
            p /= p.sum()
            loss -= np.log(p[yi] + 1e-12)
            d_ = p.copy()
            d_[yi] -= 1.0
            g += Xi.T @ d_
        return loss + lam * (w ** 2).sum(), g + 2 * lam * w

    return minimize(obj, np.zeros(d), jac=True, method="L-BFGS-B").x


def main():
    feats = T.load_features(sys.argv[1])
    songs = [s for s in K.load_songs("", with_features=False) if not s.edge and s.file in feats]
    rows = []
    for s in songs:
        t, m = s.primary
        truefam = K.family(t, m, s.notes)
        rows.append((s, feats[s.file], truefam))
    n = len(rows)
    print(f"{n} songs")
    # baseline: the shipped stage 1 (Temperley correlation of the gated peak chroma)
    base = sum(T.stage1(f["pg"])[0] in s.fams for s, f, _ in rows) / n
    print(f"Temperley (shipped): same scale {100 * base:.1f} %")
    for kinds in (["pg"], ["pg", "cg"], ["pg", "cb"], ["pg", "cg", "cb"]):
        for lam in (0.3, 3.0, 30.0):
            hits = []
            for rep in range(3):
                rng = np.random.RandomState(rep)
                order = rng.permutation(n)
                folds = np.array_split(order, 10)
                ok = 0
                for k in range(10):
                    test = set(folds[k].tolist())
                    tr = [i for i in range(n) if i not in test]
                    X = [design(rows[i][1], kinds) for i in tr]
                    y = [rows[i][2] for i in tr]
                    w = fit(X, y, lam)
                    for i in test:
                        s, f, _ = rows[i]
                        pred = int(np.argmax(design(f, kinds) @ w))
                        ok += pred in s.fams
                hits.append(ok / n)
            print(f"learned {'+'.join(kinds):10s} lam {lam:5.1f}: same scale {100 * np.mean(hits):.1f} %  (splits: {', '.join(f'{100 * h:.1f}' for h in hits)})")


if __name__ == "__main__":
    main()
