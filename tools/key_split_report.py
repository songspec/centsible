"""Key detector scores on the songs it was designed on versus songs it has never seen.

   python tools/key_split_report.py <cli csv> [old labels csv = testdata/key_labels_106.csv]

The first labelled catalog (106 songs, 62 labelled) is what the v1.1 detector was designed and fitted on; every song labelled since is a true held-out test.
Scores as in tools/key_report.py: exact key, same scale (relative major / minor / the tracker's mode), MIREX score; songs within ~8 cents of a semitone edge are left out.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import key_eval as K
import key_report as R


def main():
    rows = R.load(sys.argv[1])
    old = {r["file"] for r in csv.DictReader(open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(K.ROOT, "testdata/key_labels_106.csv"), encoding="utf-8"))}
    songs = K.load_songs("", with_features=False)
    for label, subset in (("designed on (old catalog)", [s for s in songs if s.file in old]), ("held out (new songs)", [s for s in songs if s.file not in old]), ("all", songs)):
        for tier in ("A", "B", "A+B"):
            sel = [s for s in subset if tier == "A+B" or s.tier == tier]
            o = R.score(rows, sel, label)
            if not o["n"]:
                continue
            print(f"{label:28s} tier {tier:3s}: {o['n']:3d} songs  exact {100 * o['exact']:5.1f} %  same scale {100 * o['fam']:5.1f} %  MIREX {o['mirex']:.3f}")


if __name__ == "__main__":
    main()
