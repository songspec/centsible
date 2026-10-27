"""Benchmark of key profiles on the tracker-labelled songs (single-stage: chroma correlated with 24 rotated profiles, as in tools/key_eval.py).
Profiles: Krumhansl-Kessler, Temperley (the one the shipped detector uses for the scale), Shaath, Albrecht-Shanahan, EDMA.
The Shaath, Albrecht-Shanahan and EDMA numbers below were typed from memory, NOT checked against the papers or a library: treat their scores as
"profiles with these numbers", not as a verdict on the published ones.
   python tools/key_profiles_bench.py [C:\\cb\\keycache]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import key_eval as K

K.PROFILES['albrecht'] = dict(major=[0.238, 0.006, 0.111, 0.006, 0.137, 0.094, 0.016, 0.214, 0.009, 0.080, 0.008, 0.081],
                              minor=[0.220, 0.006, 0.104, 0.123, 0.019, 0.103, 0.012, 0.214, 0.062, 0.022, 0.061, 0.052])
K.PROFILES['edma'] = dict(major=[0.16519551, 0.04749251, 0.08293076, 0.06687112, 0.09994645, 0.09274123, 0.05294487, 0.13159476, 0.05218986, 0.07443388, 0.07319706, 0.07046184],
                          minor=[0.17235348, 0.04275438, 0.07651455, 0.12090164, 0.05379381, 0.08793306, 0.06088048, 0.13633263, 0.09044627, 0.05161118, 0.08002483, 0.05645284])

if __name__ == '__main__':
    cache = sys.argv[1] if len(sys.argv) > 1 else r'C:\cb\keycache'
    songs = K.load_songs(cache)
    print(len(songs), 'songs with features and labels')
    for cols in ['peaks_g', 'sal_g', 'canc_g']:
        for prof in ['ks', 'temperley', 'shaath', 'albrecht', 'edma']:
            print('%-8s %-10s' % (cols, prof), K.fmt(K.evaluate(songs, dict(profile=prof, cols=cols))))
