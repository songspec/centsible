# Centsible: developer guide

Build, test and design notes (moved out of the README). The project overview is in [../README.md](../README.md).

## Build on Windows

Needs Visual Studio 2022 (Desktop development with C++), CMake ≥ 3.22 and Git (JUCE is downloaded on first configure). Use a **short build directory**:
JUCE's paths otherwise exceed MAX_PATH.

```bat
cmake -S . -B C:\cb -G "Visual Studio 17 2022" -A x64
cmake --build C:\cb --config Release -j
```

Outputs

| what | where |
|---|---|
| VST3 | `C:\cb\plugin\Centsible_artefacts\Release\VST3\Centsible.vst3` |
| Standalone | `C:\cb\plugin\Centsible_artefacts\Release\Standalone\Centsible.exe` |
| command line analyzer | `C:\cb\cli\centsible_cli_artefacts\Release\centsible_cli.exe` |

Install the VST3: close the DAW, copy the `Centsible.vst3` folder to `C:\Program Files\Common Files\VST3\` (or run `install-vst3.ps1`, which asks for admin rights), rescan plugins.

### Tests

```bat
ctest --test-dir C:\cb -C Release --output-on-failure            :: passthrough (bit-identical) + synthetic suite
ctest --test-dir C:\cb -C Release -L real --output-on-failure    :: real audio from testdata\real (slow)
```

* `centsible_tests` – synthetic suite: ±200 ¢ sweep in 0.5 ¢ steps, instrument/condition zoo, synthetic songs, genuine/false second tunings, song switches, the +88 regression.
  Writes `synthetic_results.csv` / `synthetic_summary.md` (every case with expected, detected, error, CI, pass/fail) to `C:\cb\test-results`.
* `centsible_real_tests <audio dir> <results dir> [maxSeconds] [filter] [shifts…]` – every file is resampled by exact known shifts (Kaiser sinc) and the detected *difference* must equal the shift.
* `tools\compare_estimators.py <audio dir> <results dir>` – the same audio through Centsible, `librosa.estimate_tuning` and an essentia-style reference.
* `centsible_robustness_test` – the real processor under host-style abuse: every sample rate / block size / layout, repeated prepare, reset mid-stream, zero-length blocks, NaN / Inf / clipping input. Output must stay bit-identical.
* `centsible_features_tests` – key, tempo and match advice on synthetic songs of known key and tempo (each pitched by a known detune).
* `centsible_beatswitch_tests` – Beatswitch on synthetic songs of known detune, key and tempo: timestamp parsing, the tempo map, sections (two songs joined at a boundary), tempo automation (varispeed ramp, time-stretch, constant, pitch step, tempo step, a beat switch where everything changes, manual tempo points), key probabilities / Camelot, progress and cancel. `centsible_beatswitch_tests quick` skips the long automation cases.
* `beatswitch_eval [--varispeed r0 r1 tA tB | --splice fileB tA tB] <files>` – Beatswitch on real songs: untouched (how often does it suggest a boundary that is not there?), after a real varispeed ramp (`tests/gen/varispeed.h`), or spliced from two songs. `curve_probe` prints the windowed detune / tempo curves of the synthetic songs.
* `centsible_shift <in> <out.wav> <cents>` – writes an audio file resampled by an exact shift (used by the comparison script).
* Key / tempo research (not in ctest): `key_frames`, `key_dump`, `tempo_dump` dump features; `tools/key_eval.py`, `key_train.py`, `key_invariance.py`, `key_report.py`, `bpm_report.py` evaluate and train against `testdata/key_labels.csv` (built by `tools/build_key_labels.py` from the tracker export `testdata/tracker_songs.csv`).
* Developer tools, not in ctest: `probe_synth` (reading vs truth on a few controlled signals), `hist_dump <wav>` (the evidence histogram of one file and the fitted model).
* `centsible_ui_snapshot <out.png> <cents | audio file> <seconds>` – renders the real editor offscreen (real processor, real analysis thread).

### Design notes

* The analysis runs on a thread the host or plugin creates, with a stack of unknown size, so the core keeps its working set small (`static_assert` on the size of a fit) and `centsible_tests` is linked with a 256 KB stack reserve: a stack-hungry change crashes the test, not a DAW.
* One algorithm, one implementation (`core/src/spectral_likelihood.h`); the plugin, the CLI and every test call the same `centsible::Analyzer` / `analyzeTrack`.

### Command line

```bat
centsible_cli track.flac other.mp3                 :: one block per file
centsible_cli --csv results.csv folder\*.flac      :: batch, one row per file (cents, ci, A4 Hz, confidence, locked, second tuning, key, bpm)
centsible_cli --match a.flac b.flac                :: how to shift b to sit on a
```

#### Beatswitch (what changes inside one track?)

```bat
centsible_cli --sections "0:45, 1:30.5" track.flac               :: A | B | C: detune, key and tempo of every section on its own, and the differences
centsible_cli --sections "17|1, 33|1" --bpm 128 track.flac       :: the same with bar|beat times (1-based, needs a tempo)
centsible_cli --automation track.flac                            :: detune over time against tempo over time: varispeed or time-stretch?
centsible_cli --automation --tempo-points "0=120, 0:20=120, 1:20=140" track.flac   :: tempo from points you give (add --tempo-step for steps)
centsible_cli --automation --reference-bpm 120 --boundaries "0:50" --curve-csv curve.csv track.flac
```

* Sections drop 0.75 s on each side of an inner boundary (the transition) and refuse sections shorter than 8 s instead of guessing.
* Automation measures the detune in overlapping 3 s windows (each with a bootstrap interval) and the tempo with a windowed tempogram + Viterbi track (or takes your tempo points). With `x = 1200 log2 (bpm / bpm_ref)`, a varispeed has detune = a + x and a time-stretch has detune = a; both are fitted as circular regressions and the tighter one wins. For a varispeed it reports the tempo at which the track sits on A440 (0 cents).
* Suggested boundaries are discontinuities only (a ramp or a kink is not a boundary): detune steps (checked on the pooled evidence of the plateaus either side) and tempo steps.
* Library entry points: `centsible/sections.h` (`parseTimestamp`, `TempoMap`, `analyzeSections`, `analyzeTempoAutomation`, both with progress + cancel).
