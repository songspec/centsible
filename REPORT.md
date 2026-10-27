# Centsible — final report (2026-10-02)

Centsible reads how many cents a song is pitched away from 12-TET A = 440 Hz (±50 ¢, wrapped). This rebuild is C++20 + JUCE 8.0.4: one pure-DSP core (`core/`) shared by the plugin, the command line tool and every test, VST3 + Standalone, bit-identical audio passthrough.

## Verdict against the acceptance criteria

| criterion | target | result | met |
|---|---|---|---|
| synthetic, max error | ≤ 0.1 ¢ | **0.174 ¢**; 1745 of 1755 measured cases are within 0.1 ¢ (99.4 %); the 1602-case sweep −200…+200 ¢ has max 0.038 ¢, mean 0.016 ¢; everything except the stretched-string bass scenario is ≤ 0.082 ¢ | **no** (10 cases, one scenario) |
| real audio, within ±0.5 ¢ | ≥ 99 % | **98.5 %** (128 / 130 shifted-track cases) | **no** (misses are 0.53 and 0.54 ¢) |
| real audio, max error | ≤ 1.0 ¢ | **0.54 ¢** on the first 3 minutes (mean 0.135 ¢, median 0.106 ¢); **1.04 ¢** when whole songs are analysed (one case, on a track the plugin flags at ±1.3 ¢) | yes for 3 min, **no** for whole songs |
| false second tunings | 0 | **0** in 1811 synthetic cases; 6 / 6 genuine second tunings found | yes |
| beat librosa and essentia on mean and max error | both | see the comparison table below | yes |
| audio passthrough | bit-identical | `ctest` passthrough passes | yes |

I am not calling this "done": two of the numeric criteria are narrowly missed and I would rather show you exactly where than tune against the test set until the numbers turn green.

## 1. Root causes and fixes

| symptom | root cause | fix | evidence |
|---|---|---|---|
| usually off by 1–2 ¢ | Upper partials were treated as notes (3rd +1.96, 5th −13.69, 7th −31.17, 9th +3.91, 11th −48.68 ¢ …, stretched further by piano/bass stiffness); hard steadiness gates threw away most real-music evidence, so the answer rested on a few dozen frames. | Partial-aware likelihood on the cents circle (partial offsets modelled, stiffness fitted, cluster width fitted), soft weights instead of gates, evidence from the whole song, exact polish of the winner. | sweep mean 0.016 ¢; real-audio mean 0.135 ¢; baseline was 77 % within 0.5 ¢ and max 30 ¢ |
| wrong or false "two tunings" | Ghost clusters at truth + a partial offset (missing fundamentals, chords); the previous song's evidence lingering as a "second tuning"; one genuine 808 ~15 ¢ off the rest. | The template explains partials, so there are no ghosts; a second tuning needs a likelihood-ratio win, ≥ 12 % of the evidence and ≥ 5 ¢ separation; song changes discard old evidence. | 0 false second tunings in 1811 cases, 6 / 6 genuine ones found |
| +88 ¢ track read "not pitched" | **Not** a wrap bug (the wrap maths is right: +88 → −12). The previous song's evidence was carried over for minutes and the new song's own 808 cluster won the vote. | Song-change detection (likelihood ratio on the newest chunks, plus a silence hint) restarts the evidence. | regression test, 6 / 6 song-switch tests |
| finicky: the answer depended on how long the song had played | Vote counts between clusters of similar strength flip as audio accumulates (hero never dies: +28 → −2 beyond 240 s). | Maximum-likelihood fit with a bootstrap interval and a lock with hysteresis (lock at ±0.3 ¢, release at ±0.5 ¢). | hero never dies reads +28.5 (30 s), +28.2 (90–150 s), +28.6 (240–600 s) |

Details and the raw measurements are in `DIAGNOSIS.md` and `results/`.

## 2. Test results

### Synthetic suite (`centsible_tests`, 1811 cases) — `results/final/synthetic_results.csv`

| section | cases | passed | max abs error (¢) | mean abs error (¢) |
|---|---|---|---|---|
| 1 sweep −200…+200 ¢ in 0.5 ¢ steps | 1602 | 1602 | 0.038 | 0.016 |
| 2 zoo (piano, treble piano, clarinet, voices, partials-only, bass, vibrato, noise, drums, reverb) | 96 | 86 | 0.174 | 0.034 |
| 3 synthetic songs (chords + bass + drums, noise, reverb, stiff bass) | 40 | 40 | 0.082 | 0.033 |
| 4 two tunings (found when present, never when absent) | 6 | 6 | — | — |
| 5 song switches (with and without a gap, similar tunings) | 6 | 6 | 0.056 | 0.037 |
| 6 the +88 ¢ regression | 1 | 1 | 0.031 | 0.031 |
| 7 sample rates 22.05 – 192 kHz | 18 | 18 | 0.032 | 0.024 |
| 8 dense mixes (benchmark: pass = error covered by 1.5x the reported interval) | 36 | 36 | see 3b | see 3b |
| 9 drift (3 drifting songs reported, 3 steady songs never flagged) | 6 | 6 | — | — |

The 10 failures are all "bass 41–62 Hz with stretched partials" (B = 1e-3: 7 cases at 0.12–0.17 ¢; B = 3e-4: 3 cases at 0.145–0.154 ¢): fundamentals lie below the tonal band, the upper partials alone are ambiguous on a 100-cent circle, so the answer comes from the sparse low-register fundamentals. Vibrato-only a cappella audio is reported as low confidence (8 / 8) rather than guessed.

### Real audio (`centsible_real_tests`, 13 tracks × 10 shifts −49 … +112 ¢, exact Kaiser-sinc resampling) — `results/final/real_summary.md`

The test compares the *difference* between the reading of the shifted and the unshifted file with the exact shift. Where two readings of the same audio can only differ by their own uncertainty, 129 / 130 cases fall inside the plugin's combined 95 % interval, and in the 40 cases where the plugin claims ±0.5 ¢ or better the worst real error is 0.45 ¢.

| track | unshifted | ± (95 %) | worst error over 10 shifts |
|---|---|---|---|
| 0103 - The hero never dies | +28.57 | 0.18 | 0.33 |
| 0109 - Like you always do | +45.01 | 0.66 | 0.19 |
| 10. Death by MP3 | −49.86 | 0.37 | 0.20 |
| DEATH BY MP3 PITCHED DOWN −50 | −1.24 | 0.30 | 0.19 |
| LIKE YOU ALWAYS DO PITCHED DOWN −50 | −6.21 | 0.61 | 0.25 |
| SHOW ME RIGHT! (+30) | +34.82 | 0.41 | 0.40 |
| detune test +88 | −13.69 | 0.64 | 0.31 |
| detune test −25 | −26.96 | 1.10 | 0.38 |
| detune test control | +0.40 | 0.72 | 0.18 |
| on ur time (+30) | +32.38 | 0.65 | 0.45 |
| premeditated murder (+40) | +33.45 | 0.87 | 0.48 |
| professional vengeance (+50) | +32.20 | 1.01 | **0.53** |
| you dont know my name (+30) | +27.59 | 0.87 | **0.54** |

**Whole songs.** The same suite with up to 900 s per track (every track is 2:24 – 4:28, so: the whole song) gives 96.9 % within 0.5 ¢, mean 0.156 ¢, median 0.122 ¢ and a **maximum of 1.04 ¢**, again only on the two noisy tracks ("professional vengeance": one case at 1.04 ¢ with the plugin reporting ±1.3 ¢; "you dont know my name": 0.52 ¢). 128 / 130 cases are inside the combined 95 % interval and in the 43 cases where the plugin claims ±0.5 ¢ or better the worst error is 0.33 ¢. So on those two tracks more audio does not tighten the answer: the evidence is a broad, flat likelihood plateau (fitted width at the top of its range, ~70 % of the evidence is background), and the plugin says so with a ±1.0–1.3 ¢ interval and no lock (`results/full/`). The "max ≤ 1.0 ¢" criterion therefore holds for the first 3 minutes (0.54 ¢) and misses by 0.04 ¢ on whole songs.

Ground-truth caveat: your pitching tool adds ~1.4–2.3 ¢ beyond its labels (e.g. "detune test −25" measures −26.96 against a control of +0.40, "+88" measures −13.69), and the "+30 / +40 / +50" songs were pitched from originals whose own tuning is unknown, so these absolute values cannot be checked against their labels. That is why the suite tests consistency under exact shifts.

### Head to head on identical audio — `results/compare180/compare_summary.md`

Same 130 shifted-track cases (13 tracks × 10 exact shifts, first 180 s of each), every estimator reading the identical float WAV; error = detected difference minus the exact shift.

| estimator | cases | mean abs error (cents) | median | 99th pct | max | within 0.5 | within 1.0 |
|---|---|---|---|---|---|---|---|
| centsible | 130 | 0.135 | 0.110 | 0.515 | 0.540 | 98.5% | 100.0% |
| librosa | 130 | 2.100 | 2.000 | 6.000 | 9.000 | 19.2% | 44.6% |
| librosa_fine | 130 | 2.227 | 1.800 | 7.210 | 7.500 | 14.6% | 30.0% |
| essentia_style | 130 | 2.603 | 1.029 | 18.225 | 19.097 | 30.8% | 49.2% |

Centsible's mean error is about 15x smaller than librosa's and 19x smaller than the essentia-style reference, and its worst case (0.54 ¢) is 14x better than librosa's worst (7.5–9.0 ¢) and 35x better than the reference's worst (19.1 ¢).

`librosa` = `piptrack` + `pitch_tuning` (exactly `estimate_tuning`) at its default 1-cent resolution; `librosa_fine` at 0.1-cent resolution. `essentia_style` is **my re-implementation of the documented essentia TuningFrequency approach** (essentia has no Windows wheel and WSL is not installed), not the essentia binary.

## 3. Plugin, UI and extras

* Readout `−12.0 ¢ ± 0.2`, `A4 = 436.9 Hz`, `(= +88.0 ¢ from one semitone down)`, `if −12.0 doesn't work, try +88.0 instead`, `shift +12.0 ¢ to fix` (click "copy"), LOCKED ring/state with hysteresis, history sparkline, hold / reset, music-reactive plate with toggle, eased marker with the interval as a band on the −50…+50 strip, resizable at a locked aspect ratio, vector drawing (HiDPI), lock-free threading (audio thread only copies into a FIFO).
* Drag one audio file onto the plugin: tuning, key and tempo, analysed faster than real time ("The hero never dies", 3:21 long, took 12.6 s in the plugin's analysis thread). Drag two: match advice (second file onto the first, tuning only or tuning + key).
* `centsible_cli`: batch folders / wildcards in parallel, `--csv`, `--match a b`; the 13 test tracks (≈ 43 minutes of audio in total) in 31 s on this machine.
* Screenshots rendered by the real editor, real processor and real analysis thread (offscreen): `screenshots/final/`.

### Something I found and fixed on the way

While dumping an evidence histogram I hit a stack overflow: the fitting code kept several 83 kB objects on the stack (each fit carried a 24 × 400 density table), so its peak use was close to the 1 MB Windows default thread stack. It never failed in my tests, but a host that creates the analysis thread with a smaller stack would have crashed. The table is gone (the signal density does not depend on the background share, one 400-entry table is enough), a fit is now ~10 kB (`static_assert`), the plugin's two background threads ask for 2 MB explicitly, and `centsible_tests` is linked with a 256 KB stack so any regression crashes the test. Results are numerically identical before and after (same 1801 / 1811, same 0.174 ¢ maximum).

## 3b. Second round: five accuracy ideas, what each one did

| idea | outcome |
|---|---|
| 1. Better test set | Partly done. I cannot make real songs with a verified true tuning, so I built a **dense-mix benchmark with known truth** (scattered vocals, 808, optional exact pad, drums, noise, reverb; 36 cases, `bench_dense`, section 8 of `centsible_tests`) and a **baseline-free consistency metric** for the real tracks (each shifted version against the track's own mean, because one unlucky unshifted reading otherwise fails all ten of its cases). With the quiet exact pad the plugin is within ~0.2 ¢; with only vocals + 808 it is ~0.5–1 ¢ off with a ±1.0–1.6 ¢ interval, which matches the two noisy real tracks. Real files with known pitch changes from you would still be the best test set. |
| 2. Narrow + broad cluster widths | **Kept, small gain.** Synthetic dense mixes: median error 0.158 → 0.113 ¢; real audio unchanged (98.5 % / max 0.54 ¢). My first version was worse on real audio (86 %): the adoption margin had a sign error and was also in the wrong units (log-likelihood "mass" counts correlated frames); fixed and re-measured. |
| 3. Joint low + tonal fit for stretched strings | **Tried, reverted.** It fixed one stiffness (B = 3e-4: error 0.14 → 0.006 ¢) but made the other worse (B = 1e-3: 0.13–0.17 → 0.7 ¢), so the 10 stiff-bass cases remain at 0.12–0.17 ¢. |
| 4. Tuning drift | **Done.** The later half of the song is tested against the earlier half; a significant difference (≥ 1.5 ¢ and 3x the scatter) is reported as "drifts +4.0 ¢ over the song" while the reading stays the average. To make that possible a slow drift is no longer mistaken for a new song (song changes must now be abrupt). Synthetic: 3 / 3 drifting songs reported within 0.2 ¢ of the true drift, 0 false drift reports in all steady cases; song-switch tests unchanged (6 / 6). None of the 13 real tracks is flagged. |
| 5. Denser analysis frames | **Tried, reverted.** 87.5 % frame overlap improved the typical error slightly (median 0.106 → 0.090 ¢) but not the worst case (largest deviation from a track's own mean 0.47 → 0.76 ¢), so it is not worth twice the CPU. |

## 4. Build

See `README.md` (Windows, VS 2022, CMake, short build directory `C:\cb`, install script `install-vst3.ps1`). Built artefacts of this session are in `dist/` (`Centsible.vst3`, `Centsible-Standalone.exe`, `centsible_cli.exe`).

## 5. Known limitations

* Two numeric criteria are not met (above), a third misses by 0.04 ¢ on whole songs. The remaining real-audio misses are two tracks where the plugin itself says ±1.0–1.3 ¢ (dense vocal + 808 mixes): analysing the whole song does not tighten them (it is slightly worse).
* Stretched-string bass below the tonal band: 0.12–0.17 ¢ (synthetic only; no real track exercised this).
* Key detection (Krumhansl–Schmuckler) confuses relative major/minor when there is no leading tone; tempo reports the half/double alternative when it is nearly as plausible.
* The plugin was not loaded into a DAW in this session; "no allocation in the audio path" holds by construction but was not verified with an allocation tracer.
* A song whose tuning drifts or that mixes several tunings is reported by its dominant tuning (a second one only when statistically solid).

## 6. Key and tempo detection (v1.1): measured against your tracker

**Ground truth.** The QO & SPECTRE tracker export (`testdata/tracker_songs.csv`, 57,857 songs) was matched to your 106 files by title and artist: 62 files have a tracker key (37 with validity *Sure* / *Key Sure*, 25 unverified), 83 have a tracker BPM. Songs the tracker says are pitched by about half a semitone (their key is genuinely between two semitones) are scored separately with a ±1-semitone tolerance. The tracker's "key" only says minor or major *mode*; modes such as Phrygian are explained in its notes, so I also score "same scale" (the relative major/minor pair and the modal reading share the same seven notes).

| detector | exact key | same scale | MIREX score | exact on verified labels | BPM within 1 % |
|---|---|---|---|---|---|
| v1.0 | 32.1 % | 52.8 % | 0.525 | 10 / 32 | 80 % (66 / 83) |
| **v1.1** | **62.3 %** | **83.0 %** | **0.728** | **19 / 32** | **98 % (81 / 83)** |

(53 labelled songs away from the half-semitone edge; per-song table in `results/keylab/key_report.md`.)

**What was wrong.** Of the songs the old detector got wrong, most reported the *fifth* of the real key. Trap bass notes (808s at 35-70 Hz) put their strongest chroma on their third harmonic, which is a fifth above the root, and the old chroma also threw away everything below 80 Hz, where the 808's actual root lives. A second error class was structural: 8 of the 62 labelled songs are modal (Phrygian etc., typical of trap), where the tonic is the bass note, not what a major/minor profile suggests.

**What it does now.** (1) Scale: sustain-gated spectral-peak chroma against Temperley's profiles (82-85 % right scale across every setting I tried; the old front-end plus Krumhansl-Schmuckler: 41-53 %). (2) Tonic: a small softmax over the scale's seven degrees using the bass notes found by iterative harmonic cancellation (strongest note, subtract its overtones, repeat) and the notes of the whole mix, plus a prior over degrees. Ten fitted numbers in total. Tempo: pulse-train scoring that is octave-agnostic inside 80-160 BPM, because the tracker (and DJ practice) list tempos there; the other octave is reported as the alternative.

**How much to trust it.** Stage 2's ten weights were fitted on the same 53 songs I report on; with leave-one-out cross-validation (each song scored by a model that never saw it) the accuracy is the same, 62.3 % exact, and training accuracy equals it, so it is not memorising. But the scale stage and the overall design were chosen while looking at these songs, so the numbers are somewhat optimistic until measured on songs I haven't touched. Honest context: "always the minor key of the detected scale" scores 53 % exact on this catalog (it is 57 % minor degree), so the bass evidence adds about 10 points over the prior; the biggest gain is the scale stage (53 % to 83 %). The detector inherits a minor-key lean from your catalog (74 % of the labelled songs are minor), which would hurt on major-heavy pop.

**Label-free checks (no ground truth needed).** Transposing every song by whole semitones: the detected scale follows in 91-95 % of songs, the exact key in 84-89 %, and the songs whose key flips have lower confidence (0.37 vs 0.50). Shifting the tuning by ±20 / ±37 ¢ without crossing a semitone boundary: the key is unchanged in 95-99 %. Synthetic songs of known key: all 7 minor-key songs exact, major-key songs come back as their relative minor half of the time (the I-V-vi-IV loop is genuinely ambiguous between the two). Confidence is informative: the top third of songs by confidence are 78 % exact, the bottom third 53 %.

**Ideas that did not help** (tried and dropped): a learned scale profile (equal to Temperley at best), blending note-evidence into the scale stage, a joint scale-and-tonic model (worse, 70 % scale), triad-energy features and segment voting (changes of one or two songs, noise at this sample size). The remaining errors are mostly scales one sharp or flat away (they share six of seven notes) and majors called minor.

**To go further:** more labelled songs, ideally a few hundred with audio, would allow a real held-out test and a better-fitted stage 2; the tracker has thousands of verified keys, the missing piece is the matching audio. Reproduce: `tools/build_key_labels.py`, `key_dump`, `tools/key_train.py`, `tools/key_report.py`.

## 7. Beatswitch and the richer key readout (v1.2)

**What was added.** (1) `analyzeSections`: cut a track at timestamps (`1:23.5`, `83.5`, or bar|beat with a tempo or a tempo map) and measure detune, key and tempo of every section on its own, with the differences between neighbours. (2) `analyzeTempoAutomation`: detune over time against tempo over time, the verdict (varispeed / tempo moves, pitch stays / pitch moves / constant / mixed), the tempo at which a varispeed track sits on A440, suggested boundaries, regions with their own detune, tempo and key. (3) The key readout: probabilities, runner-up, the relative major / minor split, Camelot codes; the chroma is measured against the tuning (a tuning *curve* inside a varispeed). Details: `BEATSWITCH.md`. The plugin panel for it is a proposal (`BEATSWITCH_UI_PROPOSAL.md`); only the key line of the file readout changed.

**How it was checked: against exact truth, never against itself.**

| check | what is known exactly | result |
|---|---|---|
| synthetic songs (`centsible_beatswitch_tests`, 104 checks) | detune, key, tempo, ramps, steps, tempo maps, 48 / 96 kHz, silence / noise / empty | all pass |
| 24 real songs played through a speed ramp (+8 % or -7 %, the pitch follows; `beatswitch_eval --varispeed`) | the ramp: +133 or -126 cents | 24 / 24 read as varispeed (fitted slope 0.96 - 1.04); the tempo at which the track sits on A440 is within 3 cents of the truth (modulo a semitone) on every song (median 1 cent); the total detune change is off by 2.1 cents (median) and 9.3 at most |
| 12 pairs of real songs spliced (different tuning, key, tempo, sample rate; `--splice`) | where the splice is, what each song reads alone | 12 / 12: boundary within 1.2 s, each part's detune within 0.6 cents of the same song analysed alone |
| 104 untouched songs (`beatswitch_eval`) | nothing (no ground truth): false alarms | no boundary suggested in 68, one in 11, more in 25; mean 0.9 per song (the first version: 5.1) |

For the untouched songs a suggested boundary is a place where the pooled evidence of the 10 s - 40 s on either side fits different tunings (each side with its own template, difference more than twice the combined 95 % interval and 8 cents), not proof of a beat switch: a vocal-only verse or a breakdown shows up too (those are marked as *passages* and ignored by the verdict). Consistency check without ground truth: the same song and its pitched-down copy (`10. Death by MP3` and `DEATH BY MP3 PITCHED DOWN -50`) get the same boundary, 2:21.7 and 2:22.2, with the same size, -18 cents.

**What went wrong on the way, and what fixed it** (so that the numbers above are not the first thing that came out):
* First version: 5 suggested boundaries per untouched song and a false varispeed. Causes: windows without a tempo made a varispeed trend look like a step; a straight line fitted to a kink looked like a jump (now only discontinuities count, checked on lines fitted just either side); thin evidence in real mixes (windows widen until they hold enough); short stretches that come back (passages).
* The tempogram sat on a related metrical level (4:5, 4:3 of the true tempo) for whole parts of some songs: it now scores like the whole-track tempo (octave-agnostic), is pulled gently towards the tempo around it, and windows where the song's own tempo is still as strong are not trusted.
* Counting the turns of the 100-cent circle in a 267-cent ramp slipped by a whole turn on noisy stretches: now a dynamic programme over the whole track (continuity first, the tempo as a weak guide).
* Splices read 5 of 12 at first, with levels exactly 146 or 47 cents off. The analysis was right: my test tool joined 44.1 kHz and 48 kHz songs without converting the rate, so the second song really was 1.088 times too fast (+146 cents, +8.8 % tempo). Fixed in the tool (`resample`), the analysis unchanged.
* A part of a track the shared template fits badly can read half a semitone off: every stretch that matters (a plateau, a region) is now fitted with a template of its own, with the same low-register cross-check the streaming estimator uses.

**Key readout benchmark** (`tools/key_profiles_bench.py`, `tools/key_stage2_as.py`; 53 labelled songs, single-stage correlation of the sustain-gated chroma with 24 rotated profiles, exact key / same scale): Krumhansl-Kessler 32.1 / 45.3 %, Temperley 26.4 / **83.0** %, Albrecht-Shanahan 54.7 / 73.6 %, EDMA 34.0 / 50.9 %, Shaath 37.7 / 54.7 %. The Shaath, Albrecht-Shanahan and EDMA numbers were typed from memory and not checked against the papers or a library, so these rows say how profiles *with these numbers* behave. The shipped two-stage detector (scale from Temperley, tonic from the bass) is 62.3 / 83.0 %. Using a second profile's major-versus-minor opinion inside the tonic stage (leave-one-out): 62.3 % -> 62.3 - 64.2 %, one song of 53, which is noise: not adopted. Beat-synchronous chroma (groups of 1, 2, 4 beats) changed nothing measurable and stays optional and off. A harmonic / percussive mask before the chroma is on (it lifts the same-scale rate from 83.0 to 84.9 % and the MIREX score from 0.728 to 0.734, exact key unchanged at 62.3 %, tempo unchanged at 98 % within 1 %; re-measured with the final build: `results/keylab/v12_report.md`). Per-section keys use the same detector on the section.

**The plugin panel.** The `beatswitch` button opens the same analyses inside the plugin (`BEATSWITCH.md` section 3, pictures in `results/ui_beatswitch/`). Source: the one file dropped on the plugin, or what the plugin heard while *record* was on, with the host's tempo, time signature and bar position (read with `getPlayHead()` on the audio thread into a wait-free FIFO; the audio itself is copied on the analysis thread; at most 11 minutes at 48 kHz). The analysis runs on its own thread with a progress bar and cancel. Checked end to end by `centsible_beatswitch_plugin_test` (70 checks): a live recording through the real processor with a stand-in host (two synthetic songs at +20 and -15 cents in A minor and E-flat major, cut at `16|1` = 30 s: both detunes within 0.02 cents, the delta -35.0, the scale +3 semitones, bar | beat shown as 16|1; also typed as `0:30` and with several cuts), a host tempo ramp 100 -> 120 BPM with a varispeed song (verdict varispeed, tempo 100 -> 120 read from the host's map, the detune rise 316 cents within 6, the tempo at 0 cents among the reported ones, the same verdict measured from the audio alone), a dropped WAV file (typed time, typed tempo `90` puts `16|1` at 40 s, bar | beat without a tempo refused with a message, bad times and tempos refused naming the text, cancel within 1 s, tempo mode finds the switch at 29.9 s with regions +20.0 / -15.0), and a recording that lost audio (the analysis thread starved: it says "gaps" and the results carry the warning). Pass-through stays bit-identical in every one of those runs, also while recording and when the recorder is overloaded.

**Limits.** A real mix gives a window of 3 - 16 s only a few cents of precision; steps smaller than about 8 cents are not suggested. If tempo and pitch both step at the same instant by amounts that differ by a whole number of semitones the step looks like a varispeed step; a ramp removes the doubt. Tempo comes from the rhythm: without a steady pulse there is no tempo curve and the verdict rests on the detune. "Tempo moves, pitch stays" cannot tell a time-stretch from a tempo change in the music itself. Tempo steps by a simple ratio (5:4, 4:3) are only believed when the old tempo is really gone from the new part. Reproduce: `python tools/beatswitch_real_eval.py` (about 35 minutes), `ctest -R beatswitch`.

## 8. A bigger catalog (369 songs): what held up and what did not (v1.2)

The catalog grew from 106 to 369 songs (mostly dense modern rap / pop mixes and mp3 rips). Everything below was measured on the shipped estimator; tables are in `results/final_v12/`.

**Tuning, exact-shift consistency** (`centsible_real_tests`, 367 decodable songs x 10 shifts = 3670 cases, first 180 s): **88.4 % within 0.5 cents** (the old 13-track claim was 98.5 %), 253 of 367 songs perfect. The error is inside the plugin's own combined 95 % interval in 95.6 % of cases, so the intervals are honest on the whole, but 25 cases (about 0.7 %) were wrong by more than 0.5 cents while claiming +-0.5 or better, and the worst are **harmonic aliases**: the same notes explained by another partial pattern, 13 - 15 cents away (also 43 cents on one song). Examples: "10 vamp city", "19. flamethrower", "Pizza Steve", "Luv Is Rage 2 - For Real". 296 of the 427 failing cases come from songs the plugin itself reads with low confidence or a wide interval. The key and tempo of the same songs are unaffected.
**What I tried and did not adopt:** restricting the fit's roll-off set to two values (1.2 and 2) gave 89.0 % (+0.6 points) but more confident-wrong songs (34 vs 25), so the shipped fit is unchanged; a "tuning that carries more evidence wins" rule moved two stable songs by 13 - 15 cents; averaging the likelihood over all templates was less stable than the single fit (37 vs 25 unstable songs of 356). The offline tools for the next attempt are in `tests/` (`hist_cache`, `fit_variants`, `mode_scan`, `mode_probe`, `tools/mode_*.py`, `tools/real_shift_*.py`): the fit can now be re-run on cached evidence in minutes. The honest state: dense mixes with two groups of notes a partial-offset apart (13.7 cents = the fifth partial) can read either tuning, the plugin shows a wide interval for about half of those songs and a tight one for the rest.

**Key detector against the tracker, on songs it never saw.** 165 catalog songs now have a tracker key (95 verified). On the 87 non-edge songs added since v1.1: **44.8 % exact key, 72.4 % same scale** (MIREX 0.586), against 62.3 % / 84.9 % on the 53 songs it was designed on, so the v1.1 numbers were optimistic. Errors given the right scale: 22 of 140 relative major / minor swaps, 19 other tonic degrees; 31 of 140 wrong scales, mostly a fifth away. A learned scale stage, richer tonic features and a joint model did not beat Temperley's profiles by more than noise (cross-validated 78.6 % vs 77.9 % scale). I refitted the ten tonic weights on all 140 songs (cross-validated 48 - 49 % exact; they ship in v1.2). The probability the plugin shows is calibrated: songs with probability 0.57 - 0.84 are 74 % exact, 0.21 - 0.36 are 31 %.
