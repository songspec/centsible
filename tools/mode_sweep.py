"""Runs tests/mode_scan over a folder of songs in parallel and summarises how stable the reading is across exact pitch shifts.

   python tools/mode_sweep.py <mode_scan.exe> <folder> <out folder> [shifts=0,3,12,37,88] [maxSeconds=180] [workers=6]

Per song it prints, over the shifts: the spread of what the production estimator reports (relative to the shift, so a stable song has a spread of 0), the spread of
the best tuning under the richest template family, the smallest log-likelihood gap between the two best tunings (effective nats, raw / 40) and the shares of the two.
"""
import concurrent.futures as cf
import os
import re
import subprocess
import sys


def wrap(x):
    return x - 100.0 * ((x + 50.0) // 100.0)


def spread(values):
    """circular range of a list of cents values (the smallest arc that holds them all)"""
    if len(values) < 2:
        return 0.0
    v = sorted(wrap(x) + 50.0 for x in values)
    gaps = [v[i + 1] - v[i] for i in range(len(v) - 1)] + [v[0] + 100.0 - v[-1]]
    return 100.0 - max(gaps)


def run(args):
    exe, path, out, shifts, maxsec = args
    name = os.path.basename(path)
    target = os.path.join(out, name + ".txt")
    if os.path.exists(target) and os.path.getsize(target) > 0:
        return name
    p = subprocess.run([exe, path, str(maxsec), shifts], capture_output=True, text=True, encoding="utf-8", errors="replace")
    with open(target, "w", encoding="utf-8") as f:
        f.write(p.stdout)
        if p.returncode != 0:
            f.write(f"\nEXIT {p.returncode}\n")
    return name


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    exe, folder, out = sys.argv[1], sys.argv[2], sys.argv[3]
    shifts = sys.argv[4] if len(sys.argv) > 4 else "0,3,12,37,88"
    maxsec = sys.argv[5] if len(sys.argv) > 5 else "180"
    workers = int(sys.argv[6]) if len(sys.argv) > 6 else 6
    os.makedirs(out, exist_ok=True)
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith((".flac", ".mp3", ".wav")))
    with cf.ThreadPoolExecutor(max_workers=workers) as ex:
        for done in ex.map(run, [(exe, os.path.join(folder, f), out, shifts, maxsec) for f in files]):
            print("done", done, flush=True)
    summarise(out)


def summarise(out):
    row = re.compile(r"^\s*([+-]?\d+)\s+([+-]?\d+\.\d+)\s+([+-]?\d+\.\d+)\s+([+-]?\d+\.\d+)\s+([+-]?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(\d+\.\d+)\s+(\d+\.\d+)")
    print()
    print(f"{'song':60s} {'prod':>6s} {'rich':>6s} {'min gap/40':>10s} {'share2':>7s}")
    for fn in sorted(os.listdir(out)):
        if not fn.endswith(".txt"):
            continue
        rows = []
        for line in open(os.path.join(out, fn), encoding="utf-8", errors="replace"):
            m = row.match(line)
            if m:
                rows.append([float(g) for g in m.groups()])
        if not rows:
            continue
        prod = spread([r[1] for r in rows])
        rich = spread([r[3] for r in rows])
        gap = min(r[6] for r in rows)
        s2 = max(r[8] for r in rows)
        flag = "  <== unstable" if prod > 1.0 else ""
        print(f"{fn[:60]:60s} {prod:6.2f} {rich:6.2f} {gap:10.2f} {s2:7.2f}{flag}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--summary":
        summarise(sys.argv[2])
    else:
        main()
