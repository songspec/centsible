# Beatswitch: what changes inside one track?

Centsible reports how far a track is pitched from 12-TET A440. A track can change halfway: a **beat switch** (new instrumental, new key, new tempo, a sample
that was pitched differently), or a **varispeed** (turntable / tape / "speed" knob: tempo and pitch move together) or a **time-stretch** (tempo moves, pitch
does not). Beatswitch answers three questions about one track. It only reports; it never changes audio.

* v1.2.0 has it in three places: the **plugin** (the `beatswitch` button, section 3), the library (`centsible/sections.h`) and the command line (sections 1 and 2).
  All three run the same code.

## 1. Sections: detune, key and tempo on either side of a timestamp

```bat
centsible_cli --sections "0:45, 1:30.5" track.flac               :: A | B | C
centsible_cli --sections "17|1, 33|1" --bpm 128 track.flac       :: bar|beat times (1-based) need a tempo
centsible_cli --sections "17|1, 33|1" --tempo-points "0=120, 1:00=128" track.flac
centsible_cli --sections auto track.flac                          :: cut where the detune jumps or the tempo steps (the suggested boundaries of section 2)
```

Times: `83.5`, `83.5s`, `1:23.5`, `1:02:03.25` (h:mm:ss.ms), `17|1`, `17|2.5`, `bar 17` (bar | beat, counted from 1 like a DAW; `--first-bar` says where bar 1 starts,
`--beats-per-bar` defaults to 4). A tempo map (several `--tempo-points`) is integrated, so bars are right through a ramp.

Every section is analysed on its own with the same analysis as a whole track. 0.75 s is dropped on each side of an inner boundary (the transition, a riser, the
reverb tail of the part before), and a section with less than 8 s left is reported as too short instead of guessed at. For each section: detune with its
95 % interval (and LOCKED / drift / second tuning as usual), key, tempo. For each neighbouring pair: the detune difference **with its interval and whether it is
significant**, how far the scale moves (relative keys and modes of one scale count as the same scale), the total pitch shift in cents, and the tempo ratio.

Key output (everywhere a key is shown): the key with its **Camelot code** (`A minor (8A)`), its probability, the **runner-up** (`runner-up F# minor (11A), 18 %`) and,
when the relative major / minor pair cannot be told apart, the pair with the split (`A minor / C major, 60 / 40`). The chroma is measured against the tuning (a song
pitched +37 cents is read in the right key), with a tuning *curve* when the pitch moves.

## 2. Tempo automation: is a moving tempo a varispeed or a time-stretch?

```bat
centsible_cli --automation track.flac
centsible_cli --automation --tempo-points "0=120, 0:20=120, 1:20=140" --reference-bpm 120 --curve-csv curve.csv track.flac
```

* **Detune over time**: overlapping windows (3 s; widened in 0.5 s steps up to 16 s where the evidence is thin, as in a sparse part), every window with a
  bootstrap interval. All windows share one template (cluster width, partial roll-off, string stiffness) learned from the track itself in three passes, so a
  varispeed ramp does not smear the model. A window during a fast glide is fitted with a template widened by the glide.
* **Tempo over time**: a windowed tempogram + Viterbi path (6 s windows), scored like the whole-track tempo (octave-agnostic), with a gentle pull towards the tempo
  around each window so that it does not flip between metrical levels (5:4, 4:3 of the same pulse); or the tempo points you give (`--tempo-points`; in a plugin:
  the host's tempo map). For a varispeed the whole-track estimate also fixes the level when the tempogram counted the whole track at a related one.
* **Verdict**. With `x = 1200 log2 (bpm / bpm_ref)` the tempo in cents, a **varispeed** has `detune = a + x` and a **time-stretch** has `detune = a`.
  Both are fitted as circular regressions (the detune lives on a 100-cent circle, so a 267-cent ramp wraps 2.7 times) and the tighter one wins; the free slope is
  reported (1 = varispeed, 0 = time-stretch). The tempo must move by at least 2.6 % (45 cents) to count.
  * *Varispeed*: "the tempo runs 120.0 -> 140.0 BPM and the pitch follows it (+10 -> +277 cents). The track sits exactly on A440 at 119.3 BPM (also at 126.4, 133.9,
    141.9: the same source shifted by whole semitones)". The reference tempo is the tempo at which the detune is 0 (it is only defined modulo a semitone, so the
    one closest to the starting tempo is named).
  * *Tempo moves, pitch stays*: the pitch does not follow the tempo. Audio alone cannot tell a time-stretch from a tempo change in the music itself.
  * *Pitch moves*: steady tempo, the detune steps or drifts. *Mixed*: both move but not in step like either model: look at the regions.
  * *Constant*: nothing moves.
* **Suggested boundaries**: only discontinuities (a ramp or a kink is not a boundary): detune steps and tempo steps. Each one is checked against evidence that is
  not the windows it was found in: the pooled evidence of the plateau on each side (each side fitted with a template of its own) must differ by more than twice its combined 95 % interval and 8 cents;
  the whole-stretch tempo on each side must differ by 2.6 %; a part must last 10 s (8 s at the ends of the track). A **passage** (a part shorter than 25 s at
  another detune that then comes back: a vocal-only verse, a breakdown) is marked as such and does not count as a beat switch. Detune steps are placed to a frame
  (about 0.1 s); tempo steps snap to the nearest clear change of the evidence's pattern (or to a beat). Candidates come from the detune curve, the tempo curve and
  from a template-free measure of how much the pattern of the evidence changes, because a part of a track that the track's template fits badly can read half a
  semitone off and hide a step from the curve.
* **Regions** between the boundaries (or between the ones you give with `--boundaries`): detune with interval, tempo, behaviour, key (measured against the tuning curve).

## 3. In the plugin: the `beatswitch` button

The button at the right end of the bottom row opens a panel over the plate (the sand rests while it is open); press the button again to go back. Everything on the
panel only reports: the audio path is read-only and bit-identical, also while recording (checked by the tests).

* **Where the audio comes from** (two buttons):
  * *dropped file*: drop **one** audio file on the plugin (it is decoded once and kept while the plugin window exists). Two files stay the match advice of before.
  * *live audio*: press **record**, play the track from the start in your DAW, press **stop**. The plugin keeps a mono copy of what it hears on its analysis
    thread (never on the audio thread; at most 11 minutes at 48 kHz, then it says "full") together with what the host says about its **tempo, time signature and
    position**. `clear` forgets the recording. Seeking or looping while recording breaks the link between the host's bars and the recording: record one pass.
* **mark switch** (live audio): while recording, press it at the moment the beat switches: the time of the recording so far is added to the cut field. If the host's position jumps while recording (a seek or a loop) the panel warns that bar | beat times no longer fit.
* **sections** mode: type the cut times in the field (`0:45, 1:30.5`, `83.5`, `1:02:03`, `17|1`, `bar 17`; commas or spaces between them) and press **analyse**
  (or Enter). With one cut the two halves are shown side by side. In general the result is the list of parts A, B, C... each with its detune and interval, key (Camelot, probability, runner-up or relative-pair split), tempo and
  drift, and between neighbours the difference: detune with its interval, **significant** or **within the error**, how far the scale moves, the tempo ratio (and what a
  varispeed of that tempo change would add to the pitch), and the shift that makes the second part match the first. The window repeats the bar | beat of every start
  when a tempo is known.
  * **bar | beat** needs a tempo: the one typed in the second field (`128`, or points `0=120, 1:20=128`; the word `step` makes the points steps instead of ramps), or, with
    live audio, **the host's tempo map** as it was recorded (with the host's time signature and where its bar 1 is). Without either, the field says what is missing.
* **tempo** mode: no times needed. The verdict (varispeed / time-stretched / the pitch moves / steady), for a varispeed `120.0 -> 140.0 BPM = +10 -> +277 cents` and the
  tempo where the track sits exactly on A440, the sentence of the library, the **curve plot** (detune above, tempo below, one time axis, the suggested boundaries as
  ticks, short passages shaded), and the **regions** between the boundaries with detune, tempo (a range and "time-stretched" when the tempo moves and the pitch does
  not), key and what started each region. Boundaries typed in the first field replace the suggested ones; points typed in the second replace the measured tempo; with live
  audio **use host tempo** takes the host's recorded tempo map instead of measuring.
* **use as cuts** (tempo mode, when switches were suggested) turns them into the cuts of sections mode and runs it; **copy results** puts the results on the clipboard as text.
* A progress bar with the stage and a **cancel** button while it works (about 10 s for the sections of a 3.5 minute song, 5 s for its curves); the results scroll with
  the mouse wheel. Bad input says what is wrong (`'2:75': seconds must be below 60`, a time outside the audio, a bar | beat without a tempo).
* Not verified: every host. The panel needs keyboard input for the times; a host that keeps the keyboard for itself will not pass the keys on (the Standalone application
  always does). In a time signature other than x/4 the beats of `bar | beat` are the host's (6/8: eighth notes).

## 4. What it cannot do (read this)

* A real mix gives a detune window only a few cents of precision (a 3 s window of a dense mix holds a tenth of the evidence of a clean synthetic song). Windows
  scatter by about 5 cents; only steps larger than about 3x that scatter and lasting several seconds are reported.
* If tempo and pitch both step at the same instant by amounts that happen to differ by whole semitones, the step looks like a varispeed step. A ramp removes the ambiguity.
* The tempo comes from the audio's rhythm. Without a steady pulse (ambient, free time, a cappella) there is no tempo curve, and the verdict rests on the detune alone.
* Tempo steps by a simple ratio (5:4, 4:3...) can be the same pulse counted differently; those are only believed when the old tempo is really gone from the new part.
* A detune that differs between the vocal and the instrumental of a song is reported as it is: it is evidence, not necessarily a beat switch.

## 5. Tests

* `centsible_beatswitch_tests` (ctest `beatswitch`): synthetic songs with known detune / key / tempo (timestamps, tempo map, sections, varispeed ramp, time-stretch,
  constant, pitch step, tempo step, beat switch, manual tempo points, a reference tempo, 48 / 96 kHz, silence / noise / empty / too short, progress and cancel), the
  tempo text parser, 3/4 and 6/8 bar | beat.
* `centsible_beatswitch_plugin_test` (ctest `beatswitch_plugin`): the real processor, editor and panel on synthetic songs with a stand-in host: live recording with a host
  tempo and a cut at `16|1`; a host tempo ramp with a varispeed song and "use host tempo"; a dropped WAV file with a typed time, a typed tempo, bad input and cancel;
  bit-identical pass-through while recording.
* `centsible_ui_snapshot ... --beatswitch sections|tempo --analyse` renders the open panel (the pictures in `results/ui_beatswitch`).
* `tools/beatswitch_real_eval.py` (not in ctest; needs the real songs, about 35 minutes) drives `beatswitch_eval`: untouched songs (false alarms), real songs
  played through a known varispeed ramp (`--varispeed r0 r1 tA tB`), real songs spliced together (`--splice fileB tA tB`, the second song converted to the first
  one's sample rate). Latest: 24 / 24 varispeed songs, 12 / 12 splices, 68 of 104 untouched songs with no suggested boundary. See `REPORT.md` section 7.
