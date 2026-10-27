"""Does the plugin's own interval hold up on the real-audio shift test?

   python tools/real_shift_claims.py <real_shifts.csv> <real_baseline.csv>

A shifted copy of a song must read exactly `shift` cents higher than the original. The plugin reports an interval (95 % half-width) with every reading. For each case the
two readings (original and shifted) have a combined interval sqrt (ci0^2 + ci1^2); an honest interval contains the error about 95 % of the time. Cases where the plugin
claimed a tight interval (combined <= 0.5 cents) and was wrong by more than 0.5 cents are the dangerous ones: a confident wrong answer.
"""
import csv
import math
import sys


def main():
    shifts = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
    base = {r["file"]: r for r in csv.DictReader(open(sys.argv[2], encoding="utf-8"))}
    n = len(shifts)
    inside = 0
    tight = []
    wrong_tight = []
    wrong_loose = []
    for r in shifts:
        e = abs(float(r["error"]))
        ci1 = float(r["ci"])
        ci0 = float(base[r["file"]]["ci"]) if r["file"] in base else 0.0
        comb = math.hypot(ci0, ci1)
        inside += e <= comb
        if comb <= 0.5:
            tight.append(r)
            if e > 0.5:
                wrong_tight.append((e, comb, r["file"], r["shift_cents"]))
        elif e > 0.5:
            wrong_loose.append((e, comb, r["file"], r["shift_cents"]))
    print(f"{n} cases; the error is inside the combined 95 % interval in {inside} ({100.0 * inside / n:.1f} %)")
    print(f"the plugin claimed a tight interval (combined <= 0.5 cents) in {len(tight)} cases; wrong by more than 0.5 cents in {len(wrong_tight)} of them")
    for e, comb, f, s in sorted(wrong_tight, reverse=True)[:20]:
        print(f"    CONFIDENT AND WRONG: {f[:60]:60s} shift {s:>5s}  error {e:6.2f}  claimed +-{comb:.2f}")
    print(f"wrong by more than 0.5 cents with a wider claim: {len(wrong_loose)}")
    over = [(e, comb) for e, comb, *_ in wrong_loose if e > comb]
    print(f"    of which outside their own interval: {len(over)}")


if __name__ == "__main__":
    main()
