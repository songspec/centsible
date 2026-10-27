"""Evaluates the "which of two competing tunings is the answer" rule on the output of tools/mode_sweep.py (tests/mode_scan).

   python tools/mode_rules.py <sweep output folder> [gapMin=1.0] [shareMin=0.6] [competeShare=0.35]

Per song and shift, mode_scan gives what fitSingle picks (fitS) and the richest-template view: the best tuning (rich1), the second best at least 5 cents away (rich2),
the advantage of the first over the second in effective nats (gap) and the shares of the evidence of the two (share1, share2).
The rule (what spectral_likelihood.h does when knobs.arbitrate is on):
  * the single fit sits on rich2 (not on rich1) and either gap >= gapMin or rich1 carries at least shareMin of the pair: the answer is rich1;
  * the other tuning carrying at least competeShare of the pair means the two COMPETE: not locked, the interval covers both.
A rule is judged by the SPREAD over the shifts of the tuning it reports (relative to the shift; 0 = perfectly shift-equivariant): songs above 1 cent are "unstable".
"""
import os
import sys


def wrap(x):
    return x - 100.0 * ((x + 50.0) // 100.0)


def spread(values):
    if len(values) < 2:
        return 0.0
    v = sorted(wrap(x) + 50.0 for x in values)
    gaps = [v[i + 1] - v[i] for i in range(len(v) - 1)] + [v[0] + 100.0 - v[-1]]
    return 100.0 - max(gaps)


def load(folder):
    songs = {}
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith(".txt"):
            continue
        rows = []
        for line in open(os.path.join(folder, fn), encoding="utf-8", errors="replace"):
            t = line.split()
            if len(t) < 9:
                continue
            try:
                rows.append(dict(shift=float(t[0]), prod=float(t[1]), fitS=float(t[2]), rich1=float(t[3]), rich2=float(t[4]), gap=float(t[6]), sh1=float(t[7]), sh2=float(t[8])))
            except ValueError:
                pass
        if rows:
            songs[fn[:-4]] = rows
    return songs


def rule(r, gap_min, share_min, compete):
    """(tuning the rule reports, relative to the shift; does the other tuning compete?)"""
    at1 = abs(wrap(r["fitS"] - r["rich1"])) < 3.0
    at2 = abs(wrap(r["fitS"] - r["rich2"])) < 3.0
    pick = r["fitS"]
    other_share = 0.0
    if at1:
        other_share = r["sh2"]
    elif at2:
        if r["gap"] >= gap_min or r["sh1"] >= share_min:
            pick, other_share = r["rich1"], r["sh2"]
        else:
            other_share = r["sh1"]
    return pick, other_share >= compete


def main():
    folder = sys.argv[1]
    gap_min = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    share_min = float(sys.argv[3]) if len(sys.argv) > 3 else 0.6
    compete = float(sys.argv[4]) if len(sys.argv) > 4 else 0.35
    songs = load(folder)
    print(f"{len(songs)} songs; override when gap >= {gap_min} or the better tuning has >= {share_min} of the pair; compete when the other has >= {compete}")
    print(f"{'song':58s} {'fitS':>6s} {'rich':>6s} {'rule':>6s} {'compete':>8s} {'changed':>8s}")
    unstable_fit = unstable_rule = unstable_rich = songs_changed = songs_compete = 0
    for name, rows in songs.items():
        sp_fit = spread([r["fitS"] for r in rows])
        sp_rich = spread([r["rich1"] for r in rows])
        picks = [rule(r, gap_min, share_min, compete) for r in rows]
        sp_rule = spread([p[0] for p in picks])
        competing = sum(1 for p in picks if p[1])
        changed = sum(1 for r, p in zip(rows, picks) if abs(wrap(p[0] - r["fitS"])) > 0.5)
        unstable_fit += sp_fit > 1.0
        unstable_rule += sp_rule > 1.0
        unstable_rich += sp_rich > 1.0
        songs_changed += changed > 0
        songs_compete += competing > 0
        if sp_fit > 1.0 or sp_rule > 1.0 or competing or changed:
            print(f"{name[:58]:58s} {sp_fit:6.2f} {sp_rich:6.2f} {sp_rule:6.2f} {competing:3d}/{len(rows):<3d}  {changed:3d}/{len(rows):<3d}")
    print()
    print(f"unstable (spread > 1 cent over the shifts): fitSingle {unstable_fit} songs, rich model alone {unstable_rich}, the rule {unstable_rule}")
    print(f"songs where the rule changes a reading: {songs_changed}; songs with at least one 'compete' reading: {songs_compete}")


if __name__ == "__main__":
    main()
