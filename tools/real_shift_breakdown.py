"""Breaks down the real-audio shift test (ctest 'real'): which tracks fail, and does the plugin know it?

   python tools/real_shift_breakdown.py <real_shifts.csv> <real_baseline.csv>

A shifted copy of a song must read exactly `shift` cents higher than the original. A track "fails" when a shifted copy is more than 0.5 cents off. What matters for the
user is whether a failing track is FLAGGED (not locked / low confidence / wide interval) or reads confidently wrong.
"""
import csv
import sys
from collections import defaultdict


def main():
    shifts = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
    base = {r["file"]: r for r in csv.DictReader(open(sys.argv[2], encoding="utf-8"))}
    per = defaultdict(list)
    for r in shifts:
        per[r["file"]].append(r)
    tracks = len(per)
    cases = len(shifts)
    bad_cases = [r for r in shifts if abs(float(r["error"])) > 0.5]
    print(f"{tracks} tracks, {cases} shifted cases, {cases - len(bad_cases)} within 0.5 cents ({100.0 * (cases - len(bad_cases)) / cases:.1f} %)")
    clean = [t for t, rs in per.items() if all(abs(float(r["error"])) <= 0.5 for r in rs)]
    print(f"{len(clean)} tracks have every shifted copy within 0.5 cents; {tracks - len(clean)} have at least one that is not")
    print()
    print(f"{'track':64s} {'bad':>3s} {'worst':>6s} {'base conf':>9s} {'base CI':>7s} {'locked':>6s} {'secondary':>12s}")
    rows = []
    for t, rs in per.items():
        errs = [abs(float(r["error"])) for r in rs]
        nbad = sum(e > 0.5 for e in errs)
        if nbad == 0:
            continue
        b = base.get(t, {})
        sec = f"{float(b.get('secondary_cents', 0)):+.1f} ({100 * float(b.get('secondary_share', 0)):.0f}%)" if b and float(b.get("secondary_share", 0)) > 0 else "-"
        rows.append((float(b.get("confidence", 0)), t, nbad, max(errs), b.get("confidence", "?"), b.get("ci", "?"), b.get("locked", "?"), sec))
    for conf, t, nbad, worst, c, ci, lk, sec in sorted(rows, reverse=True):
        print(f"{t[:64]:64s} {nbad:3d} {worst:6.2f} {float(c):9.2f} {float(ci):7.2f} {lk:>6s} {sec:>12s}")
    # how many failing cases come from tracks the plugin itself would not trust
    def flagged(t):
        b = base.get(t, {})
        return float(b.get("confidence", 0)) < 0.5 or float(b.get("ci", 9)) > 1.0
    flagged_cases = sum(1 for r in bad_cases if flagged(r["file"]))
    print()
    print(f"failing cases: {len(bad_cases)}; from tracks that read with confidence < 0.5 or an interval > 1 cent (flagged by the plugin itself): {flagged_cases}; "
          f"from tracks that read confidently: {len(bad_cases) - flagged_cases}")
    worst_conf = max((abs(float(r["error"])) for r in bad_cases if not flagged(r["file"])), default=0.0)
    print(f"largest error among the confident ones: {worst_conf:.2f} cents")


if __name__ == "__main__":
    main()
