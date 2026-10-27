"""Is the template-averaged tuning (mode_scan's "marg" columns) more stable across exact shifts than what fitSingle picks?

   python tools/mode_marg.py <mode_sweep output folder>

Per song: spread over the shifts (relative to the shift; 0 = perfectly shift-equivariant) of the production pick (fitS) and of the averaged pick (marg), and whether the
gap between the best and the second mode of the averaged profile (effective nats) is small enough to flag the song as "two tunings compete".
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mode_rules import spread


def load(folder):
    songs = {}
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith(".txt"):
            continue
        rows, cur = [], None
        for line in open(os.path.join(folder, fn), encoding="utf-8", errors="replace"):
            t = line.split()
            if len(t) >= 13 and t[9] in ("f2", "--"):
                try:
                    cur = dict(shift=float(t[0]), prod=float(t[1]), fitS=float(t[2]), rich1=float(t[3]))
                except ValueError:
                    cur = None
            elif len(t) == 4 and t[0] == "marg" and cur is not None:
                cur.update(m1=float(t[1]), m2=float(t[2]), gap=float(t[3]))
                rows.append(cur)
                cur = None
        if len(rows) >= 2:
            songs[fn[:-4]] = rows
    return songs


def main():
    songs = load(sys.argv[1])
    print(f"{len(songs)} songs with at least two shifts")
    unstable_fit = [n for n, r in songs.items() if spread([x["fitS"] for x in r]) > 1.0]
    unstable_marg = [n for n, r in songs.items() if spread([x["m1"] for x in r]) > 1.0]
    print(f"unstable (spread > 1 cent): fitSingle {len(unstable_fit)} songs, template-averaged {len(unstable_marg)} songs")
    print("both:", len(set(unstable_fit) & set(unstable_marg)), " only fitSingle:", len(set(unstable_fit) - set(unstable_marg)), " only averaged:", len(set(unstable_marg) - set(unstable_fit)))
    for g in (0.5, 1.0, 1.5, 2.0, 3.0):
        flagged = {n for n, r in songs.items() if min(x["gap"] for x in r) < g}
        print(f"flag when the gap is below {g:.1f} effective nats: {len(flagged)} songs flagged; of the averaged-unstable ones {len(flagged & set(unstable_marg))} of {len(unstable_marg)}, "
              f"of the fitSingle-unstable ones {len(flagged & set(unstable_fit))} of {len(unstable_fit)}")
    print()
    print(f"{'song':56s} {'fitS':>6s} {'marg':>6s} {'min gap':>8s}")
    for n, r in songs.items():
        sf, sm = spread([x["fitS"] for x in r]), spread([x["m1"] for x in r])
        if sf > 1.0 or sm > 1.0:
            print(f"{n[:56]:56s} {sf:6.2f} {sm:6.2f} {min(x['gap'] for x in r):8.2f}")


if __name__ == "__main__":
    main()
