#!/usr/bin/env python
"""Head-to-head accuracy comparison on identical audio: Centsible vs librosa.estimate_tuning vs an essentia-style reference.

usage: python tools/compare_estimators.py <audio dir> <results dir> [--seconds 120] [--shifts -49 -37 ...] [--filter text] [--jobs N]

For every (file, shift) the audio is resampled by the exact shift (Kaiser-sinc, centsible_shift) into a float WAV that ALL estimators read.
Each estimator is scored on the DIFFERENCE between its reading of the shifted file and of the unshifted file, compared with the exact shift
(wrapped to +-50 cents). That is the same consistency test as centsible_real_tests; it does not need ground truth for the unshifted reading.

* librosa: librosa.piptrack + librosa.pitch_tuning, i.e. exactly what librosa.estimate_tuning does, at its default resolution (0.01 bin = 1 cent)
  and at resolution 0.001 (0.1 cent), the fairer setting for a 0.1-cent comparison. Result is in fractions of a semitone -> x100 cents.
* essentia-style: NOT the essentia binary (no Windows wheel; no WSL). A reimplementation of the documented essentia TuningFrequency approach:
  Hann STFT (4096/2048), up to 100 spectral peaks per frame with parabolic interpolation (100-5000 Hz), magnitude-weighted 1-cent histogram of the
  deviation from the equal-tempered grid around A=440, circular parabolic refinement of the histogram maximum.
"""
import argparse, concurrent.futures as cf, csv, json, math, os, re, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("CENTSIBLE_BIN", r"C:\cb")
SHIFT_EXE = os.path.join(BIN, "tests", "Release", "centsible_shift.exe")
CLI_EXE = os.path.join(BIN, "cli", "centsible_cli_artefacts", "Release", "centsible_cli.exe")


def wrap(c):
    return (c + 50.0) % 100.0 - 50.0


def librosa_readings(y, sr):
    import librosa
    pitch, mag = librosa.piptrack(y=y, sr=sr)
    mask = pitch > 0
    thr = np.median(mag[mask]) if mask.any() else 0.0
    sel = pitch[(mag >= thr) & mask]
    if sel.size == 0:
        return float("nan"), float("nan")
    d = librosa.pitch_tuning(sel, resolution=0.01, bins_per_octave=12) * 100.0
    f = librosa.pitch_tuning(sel, resolution=0.001, bins_per_octave=12) * 100.0
    return d, f


def essentia_style(y, sr, frame=4096, hop=2048, max_peaks=100):
    win = np.hanning(frame)
    n = (len(y) - frame) // hop
    hist = np.zeros(100)
    freqs = np.arange(frame // 2 + 1) * sr / frame
    lo, hi = int(100 * frame / sr), int(5000 * frame / sr)
    for i in range(max(n, 0)):
        seg = y[i * hop:i * hop + frame] * win
        m = np.abs(np.fft.rfft(seg))
        k = np.arange(max(lo, 1), min(hi, len(m) - 1))
        pk = k[(m[k] > m[k - 1]) & (m[k] >= m[k + 1]) & (m[k] > 1e-4)]
        if pk.size == 0:
            continue
        pk = pk[np.argsort(m[pk])[::-1][:max_peaks]]
        a, b, c = np.log(m[pk - 1] + 1e-12), np.log(m[pk] + 1e-12), np.log(m[pk + 1] + 1e-12)
        p = 0.5 * (a - c) / (a - 2 * b + c)
        f = (pk + p) * sr / frame
        cents = wrap(1200.0 * np.log2(f / 440.0))
        np.add.at(hist, np.floor(cents + 50.0).astype(int) % 100, m[pk])
    if hist.sum() == 0:
        return float("nan")
    s = int(np.argmax(hist))
    l0, l1, l2 = hist[(s - 1) % 100], hist[s], hist[(s + 1) % 100]
    den = l0 - 2 * l1 + l2
    off = 0.5 * (l0 - l2) / den if den < 0 else 0.0
    return wrap(-50.0 + s + 0.5 + off)


def run_case(args):
    path, shift, seconds = args
    import soundfile as sf
    with tempfile.TemporaryDirectory(dir=os.environ.get("TEMP")) as td:
        wav = os.path.join(td, "x.wav")
        subprocess.run([SHIFT_EXE, path, wav, str(shift), str(seconds)], check=True, capture_output=True)
        csv_path = os.path.join(td, "r.csv")
        subprocess.run([CLI_EXE, "--csv", csv_path, wav], check=True, capture_output=True, text=True)
        with open(csv_path) as fh:
            row = list(csv.DictReader(fh))[0]
        valid = int(row["valid"])
        cents, ci = (float(row["cents"]), float(row["ci_cents"])) if valid else (float("nan"), 0.0)
        y, sr = sf.read(wav, dtype="float32")
        if y.ndim > 1:
            y = y.mean(axis=1)
        ld, lf = librosa_readings(y, sr)
        es = essentia_style(y, sr)
    return dict(file=os.path.basename(path), shift=shift, centsible=cents, ci=ci, valid=valid, librosa=ld, librosa_fine=lf, essentia_style=es)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio"); ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--shifts", type=float, nargs="*", default=[0, -49, -37, -12, -3, 3, 12, 37, 50, 88, 112])
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files = sorted(os.path.join(a.audio, f) for f in os.listdir(a.audio) if f.lower().endswith((".mp3", ".flac", ".wav")) and a.filter in f)
    shifts = [0.0] + [s for s in a.shifts if s != 0]
    jobs = [(f, s, a.seconds) for f in files for s in shifts]
    print(f"{len(files)} files x {len(shifts)} shifts = {len(jobs)} cases on {a.jobs} workers", flush=True)
    res = []
    with cf.ProcessPoolExecutor(a.jobs) as ex:
        for i, r in enumerate(ex.map(run_case, jobs)):
            res.append(r)
            if (i + 1) % 20 == 0:
                print(f"  {i + 1}/{len(jobs)}", flush=True)
    base = {r["file"]: r for r in res if r["shift"] == 0}
    names = ["centsible", "librosa", "librosa_fine", "essentia_style"]
    rows, errs = [], {n: [] for n in names}
    for r in res:
        if r["shift"] == 0:
            continue
        row = dict(file=r["file"], shift=r["shift"])
        for n in names:
            e = wrap(wrap(r[n] - base[r["file"]][n]) - wrap(r["shift"]))
            row[n + "_err"] = e
            if not math.isnan(e):
                errs[n].append(abs(e))
        rows.append(row)
    with open(os.path.join(a.out, "compare_cases.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    with open(os.path.join(a.out, "compare_baseline.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file"] + names + ["centsible_ci"])
        for f, r in base.items():
            w.writerow([f] + [f"{r[n]:+.2f}" for n in names] + [f"{r['ci']:.2f}"])
    lines = ["| estimator | cases | mean abs error (cents) | median | 99th pct | max | within 0.5 | within 1.0 |", "|---|---|---|---|---|---|---|---|"]
    for n in names:
        e = np.array(errs[n])
        lines.append(f"| {n} | {len(e)} | {e.mean():.3f} | {np.median(e):.3f} | {np.percentile(e, 99):.3f} | {e.max():.3f} | {100 * np.mean(e <= 0.5):.1f}% | {100 * np.mean(e <= 1.0):.1f}% |")
    table = "\n".join(lines)
    with open(os.path.join(a.out, "compare_summary.md"), "w") as fh:
        fh.write(f"# Estimator comparison on identical audio ({len(files)} files, {len(shifts) - 1} shifts, first {a.seconds:.0f} s of each)\n\n{table}\n")
    print(table)


if __name__ == "__main__":
    main()
