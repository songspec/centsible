"""Label-free checks: transposing a song by k semitones must transpose the detected key by k; shifting the tuning by +-37 cents must not change it.
   Uses the product feature extractor via key_dump (tuning shifted along, exactly as the plugin would measure it) and the same decision rule as C++."""
import sys, subprocess, os
import numpy as np
sys.path.insert(0, 'tools')
import key_eval as K, key_train as T
W = np.array([0.8318, 1.1263, 0.3298, 0.9165, -0.4731, 0.1954, -0.8159, -0.8435, 1.7313, -0.7106])
def detect(f):
    scale, sc, fam = T.stage1(f['pg'])
    X = T.deg_x(f, scale, sc)
    di = int(np.argmax(X @ W[:3] + W[3:])); return ((scale + T.MAJ[di]) % 12, T.DEG_MINOR[di])
def dump(shift):
    out = 'results/keylab/feat_shift%d.csv' % round(shift)
    if not os.path.exists(out):
        subprocess.run([r'C:\cb2\tests\Release\key_dump.exe', out, 'results/keylab/baseline_cli.csv', str(shift), 'testdata/real'], check=True)
    return T.load_features(out)
base = dump(0)
songs = [s for s in K.load_songs('', with_features=False) if s.file in base]
all_files = list(base.keys())
print('songs', len(all_files))
for shift in [100, 300, 500, 700, -200, 37, -37, 20]:
    f = dump(shift); k = shift / 100.0
    ok = n = 0
    for name in all_files:
        if name not in f: continue
        b = detect(base[name]); d = detect(f[name])
        want = ((b[0] + round(k)) % 12, b[1]) if abs(shift) >= 100 else b      # whole semitones transpose, cent shifts keep the key
        n += 1; ok += d == want
    print('shift %+5d cents: key follows in %d / %d songs (%.1f %%)' % (shift, ok, n, 100 * ok / n))
