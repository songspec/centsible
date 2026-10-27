import sys, itertools
import numpy as np
sys.path.insert(0, 'tools')
import key_eval as K
MAJOR_SCALE = [0, 2, 4, 5, 7, 9, 11]
DEG_MODE = {0: 0, 5: 1, 2: 1, 1: 1, 6: 1, 3: 0, 4: 0}   # scale degree index -> 0 major / 1 minor  (I, vi=aeolian, iii=phrygian, ii=dorian, vii=locrian, IV=lydian, V=mixolydian)
# degree index in scale order: 0=I 1=ii 2=iii 3=IV 4=V 5=vi 6=vii
def fam_scores(ch, mode='sum', outpen=0.0):
    ch = ch / (ch.sum() + 1e-12)
    out = np.zeros(12)
    for f in range(12):
        ins = sum(ch[(f + d) % 12] for d in MAJOR_SCALE)
        out[f] = ins - outpen * (1 - ins)
    return out
def detect2(song, cfg):
    ch = K.chroma_of(song, cfg.get('cols', 'peaks'), cfg.get('weight', 0.5), norm=cfg.get('norm', 'l2'), power=cfg.get('power', 1.0))
    fs = fam_scores(ch, outpen=cfg.get('outpen', 0.0))
    if cfg.get('fam_prof'):                      # blend the diatonic fit with a profile score of the best key of each family
        P = K.profile_matrix(K.PROFILES[cfg['fam_prof']]); sc = K.score_pearson(ch, P)
        fs = fs + cfg.get('fam_w', 0.5) * np.maximum(sc[:12], sc[12:] if False else np.roll(sc[12:], 3))   # relative minor of family f is f+9; row (f+9)%12 -> shift so index = family
    f = int(np.argmax(fs))
    b = K.chroma_of(song, cfg.get('tonic_cols', 'bass'), cfg.get('weight', 0.5), norm='l2')
    b = b / (b.sum() + 1e-12)
    prior = cfg.get('prior', {0: 1.0, 5: 1.0, 2: 0.6, 1: 0.5, 6: 0.3, 3: 0.4, 4: 0.5})
    w = []
    for di, d in enumerate(MAJOR_SCALE):
        pc = (f + d) % 12
        w.append((b[pc] + cfg.get('mid_w', 0.0) * ch[pc] / (ch.sum() + 1e-12)) * prior[di])
    di = int(np.argmax(w))
    tonic = (f + MAJOR_SCALE[di]) % 12
    return (tonic, DEG_MODE[di]), fs
def evaluate2(songs, cfg):
    n = ex = fam = 0; cnt_fam_ok_tonic = 0
    for s in songs:
        if s.edge: continue
        det, fs = detect2(s, cfg)
        n += 1; ex += det == s.primary
        famdet = det[0] if det[1] == 0 else (det[0] + 3) % 12
        # family = the stage-1 decision itself
        fam += int(np.argmax(fs)) in s.fams
    return ex / n, fam / n
if __name__ == '__main__':
    songs = K.load_songs('C:/cb/keycache')
    for cols in ['peaks', 'peaks_g', 'sal', 'sal_g', 'mid']:
        for outpen in [0.0, 1.0]:
            print(cols.ljust(8), 'outpen', outpen, 'two-stage exact %.3f  family(stage 1) %.3f' % evaluate2(songs, dict(cols=cols, outpen=outpen)))
