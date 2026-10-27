"""Does a second major-vs-minor opinion from another profile (Albrecht-Shanahan numbers as remembered, or Shaath / EDMA) help the tonic stage?
Same end-to-end leave-one-out as tools/key_stage2.py (family from Temperley on sustain-gated peaks, tonic by a softmax over the family's seven notes), with extra
features for the I-vs-vi decision: score(major f) - score(minor f+9) under each extra profile. Prints exact / family / MIREX per feature set.
   python tools/key_stage2_as.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import key_eval as K
import key_stage2 as S
import key_profiles_bench  # noqa: F401  (registers the 'albrecht' and 'edma' profiles)

MAJ, DEG_MODE = S.MAJ, S.DEG_MODE


def prep_extra(songs, extra_profiles, cols='peaks_g'):
    for s in songs:
        ch = K.chroma_of(s, cols, 0.5, norm='l2', power=0.5)
        s.mm_extra = {}
        for name in extra_profiles:
            sc = K.score_pearson(ch, K.profile_matrix(K.PROFILES[name]))
            s.mm_extra[name] = np.array([sc[f] - sc[12 + (f + 9) % 12] for f in range(12)])


def deg_feats(s, fam, sources, extra):
    X = np.zeros((7, len(sources) + 1 + len(extra)))
    for di, d in enumerate(MAJ):
        pc = (fam + d) % 12
        for j, c in enumerate(sources):
            X[di, j] = s.src[c][pc]
        sign = 1 if DEG_MODE[di] == 0 else -1
        if di in (0, 5):
            X[di, len(sources)] = s.mm[fam] * sign
            for k, name in enumerate(extra):
                X[di, len(sources) + 1 + k] = s.mm_extra[name][fam] * sign
    return X


def run(songs, sources, extra, lam):
    nf = len(sources) + 1 + len(extra)
    tr = []
    for s in songs:
        t, m = s.primary
        fam = K.family(t, m, s.notes)
        d = (t - fam) % 12
        tr.append((deg_feats(s, fam, sources, extra), MAJ.index(d)) if d in MAJ else None)
    ex = famok = mir = 0
    for i, s in enumerate(songs):
        pairs = [p for j, p in enumerate(tr) if j != i and p is not None]
        w = S.fit([p[0] for p in pairs], [p[1] for p in pairs], nf, lam)
        fam = s.fam_pred
        sc = deg_feats(s, fam, sources, extra) @ w[:nf] + w[nf:]
        di = int(np.argmax(sc))
        det = ((fam + MAJ[di]) % 12, DEG_MODE[di])
        ex += det == s.primary
        famok += fam in s.fams
        mir += K.mirex(det, s.primary)
    n = len(songs)
    return ex / n, famok / n, mir / n


if __name__ == '__main__':
    songs = [s for s in K.load_songs('C:/cb/keycache') if not s.edge]
    allsrc = ['canc_bass', 'sub', 'bassall', 'canc_g', 'peaks_g', 'mid', 'canc']
    S.prep(songs, allsrc, dict())
    prep_extra(songs, ['albrecht', 'edma', 'shaath', 'ks'])
    sources = ['canc_bass', 'canc_g']          # what the shipped detector uses (bass notes + notes)
    for extra in [[], ['albrecht'], ['edma'], ['shaath'], ['ks'], ['albrecht', 'ks']]:
        for lam in [0.1, 1.0]:
            print('%-26s lam %-4s exact %.3f family %.3f mirex %.3f' % ('+'.join(extra) or 'temperley only', lam, *run(songs, sources, extra, lam)))
