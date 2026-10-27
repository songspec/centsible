## Centsible 1.3.0

**Download `Centsible-Setup-1.3.0.exe`** (Windows, VST3 + optional Standalone app and command line tool). The source code is attached automatically by GitHub below.

### New: the beatswitch button
- Press **beatswitch** (bottom right): *When does the beat switch?*, *First half BPM*, *Second half BPM*, and a `[ ] add a third part` check box for a second switch.
- Result: the first half and the second half **side by side**, each with detune, key (with mode) and tempo, then what changed between them.
- **mark switch**: record your DAW's playback and press it at the moment the beat switches.
- **tempo curve**: detune and tempo over the whole song, varispeed or time-stretch, the suggested switches (**use as switches**), **copy results**.
- A warning if the DAW position jumps (seek / loop) or the recording lost audio.

### New: all seven diatonic modes
Every key is named with its mode (`C major (Ionian)`, `D minor (Dorian)`, `F major (Lydian)` ...) and the probability of each mode of the song's scale is reported.

### Honest limits
- Tuning on a catalog of 369 real songs: 88 % of exact-shift readings within 0.5 ¢; a few songs read a wrong tuning 13 to 15 ¢ away (see REPORT.md section 8).
- Key on songs the detector had never seen: 45 % exact, 72 % right scale.
- The installer is not code-signed yet: Windows shows "unknown publisher" the first time.
- Tested with a stand-in host, not yet in every DAW.

Full changelog: [CHANGELOG.md](https://github.com/songspec/centsible/blob/main/CHANGELOG.md)
