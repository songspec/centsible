#pragma once
#include <centsible/progress.h>
#include <centsible/track.h>
#include <string>
#include <vector>

// "Beatswitch": what changes inside one track?
//   * analyzeSections: cut the audio at timestamps (a beat switch, a drop, a sampled part) and measure detune, key and tempo of every section
//     separately, with the differences between neighbours;
//   * analyzeTempoAutomation: detune over time against tempo over time. Decides whether a moving tempo is a VARISPEED (pitch follows tempo: the
//     detune changes by 1200 * log2 (bpm / bpm_ref) cents) or a TIME-STRETCH (tempo moves, pitch does not), gives the reference tempo at which a
//     varispeed track sits exactly on A440, and suggests boundaries where the detune jumps or the tempo steps.
// Everything here only REPORTS. Offline code: it allocates freely, runs for seconds on a long track and must never be called from an audio thread;
// pass a ProgressFn to show progress and to cancel.
namespace centsible
{
// ---------------------------------------------------------------------------------------------------------------------------------------
// tempo map and timestamps

struct TempoPoint
{
    double time = 0.0;    // seconds
    double bpm = 120.0;
};

// Tempo as a function of time: manual points the user types, or the host's tempo (collected while the plugin listens). Before the first point
// and after the last the tempo is held constant. `linear`: the tempo ramps between points; otherwise it steps at each point.
class TempoMap
{
public:
    TempoMap() = default;
    static TempoMap constant (double bpm);

    bool linear = true;

    void add (double timeSeconds, double bpm);    // keeps the points sorted; a point at an existing time replaces it
    void clear() { pts.clear(); }
    bool empty() const { return pts.empty(); }
    bool valid() const { return ! pts.empty(); }
    const std::vector<TempoPoint>& points() const { return pts; }

    double bpmAt (double timeSeconds) const;
    double beatsAt (double timeSeconds) const;    // quarter notes since time 0: the integral of bpm / 60
    double timeOfBeat (double beats) const;       // the inverse of beatsAt

private:
    std::vector<TempoPoint> pts;
};

// How to read "17|1" (bar | beat): bars and beats count from 1 as in a DAW; bar 1 beat 1 is at `firstBarSeconds`.
struct TimeContext
{
    const TempoMap* tempoMap = nullptr;   // used when set (host / manual tempo)
    double bpm = 0.0;                     // else a constant tempo (0: bar|beat cannot be read)
    int beatsPerBar = 4;                  // beats in a bar as the DAW counts them (the numerator of the time signature)
    double quartersPerBeat = 1.0;         // how long one of those beats is in quarter notes (1 in 4/4, 0.5 in 6/8); the tempo is always per quarter note
    double firstBarSeconds = 0.0;
};

struct ParsedTime
{
    bool ok = false;
    double seconds = 0.0;
    std::string error;                    // a sentence for the user when !ok
};

// Accepted: "83.5" or "83.5s" (seconds), "1:23.5" and "1:02:03.25" (m:ss.ms, h:mm:ss.ms), "17|1" and "17|2.5" (bar|beat, needs a tempo), "bar 17".
ParsedTime parseTimestamp (const std::string& text, const TimeContext& ctx = {});

struct ParsedTimes
{
    bool ok = true;
    std::vector<double> seconds;          // sorted, without duplicates
    std::string error;                    // the first problem, naming the offending text
};
ParsedTimes parseTimestampList (const std::string& text, const TimeContext& ctx = {});   // separated by commas, semicolons, new lines or spaces

// Tempo as typed: "120" (one constant tempo) or "0=120, 1:20=140" (time=bpm points; the tempo ramps between them, or steps with linear = false).
struct ParsedTempo
{
    bool ok = false;
    TempoMap map;
    std::string error;                    // a sentence for the user when !ok
};
ParsedTempo parseTempoText (const std::string& text, bool linear = true);

std::string formatTimestamp (double seconds);                                 // "1:23.456"
std::string formatBarBeat (double seconds, const TimeContext& ctx);           // "17|3.5", empty without a tempo
std::string sectionLabel (int index);                                         // 0 -> "A", 25 -> "Z", 26 -> "AA"

// ---------------------------------------------------------------------------------------------------------------------------------------
// sections

struct SectionOptions
{
    double trimSeconds = 0.75;    // dropped on each side of an inner boundary (the transition, a riser, a reverb tail of the previous part)
    double minSeconds = 8.0;      // a section shorter than this after trimming is reported as too short instead of guessed at
};

struct SectionResult
{
    int index = 0;
    std::string label;            // "A", "B", ...
    double start = 0.0, end = 0.0;            // the section as asked for
    double usedStart = 0.0, usedEnd = 0.0;    // what was analysed (trimmed at inner boundaries)
    bool ok = false;
    std::string note;             // why a section could not be analysed
    TrackAnalysis analysis;       // tuning (cents +- ci, locked, drift, secondary), key (probabilities, runner-up, relative ambiguity), tempo
};

// B compared with A (sections[i] -> sections[i + 1], or any pair through compareSections).
struct SectionDelta
{
    int from = 0, to = 0;
    bool tuningKnown = false;
    double cents = 0.0;           // detune of `to` minus detune of `from`, wrapped to [-50, 50)
    double ci = 0.0;              // 95 % half-width of that difference
    bool significant = false;     // |cents| exceeds ci
    bool keysKnown = false;
    int semitones = 0;            // how far the scale moves, [-6, 5] (relative major / minor and modes of one scale count as the same scale)
    bool sameScale = false;
    bool sameKey = false;         // same tonic and mode as well
    double totalCents = 0.0;      // semitones * 100 + cents: the full pitch shift between the two sections
    bool tempoKnown = false;
    double tempoRatio = 1.0;      // bpm of `to` over bpm of `from`
    double tempoCents = 0.0;      // 1200 * log2 (tempoRatio): what a varispeed of that tempo change would add to the pitch
};

struct SectionsAnalysis
{
    std::vector<SectionResult> sections;
    std::vector<SectionDelta> deltas;     // sections[i] -> sections[i + 1]
    double seconds = 0.0;
    bool cancelled = false;
};

// boundaries: times in seconds inside the audio (unsorted, duplicates and out-of-range values are dropped). N boundaries give N + 1 sections.
SectionsAnalysis analyzeSections (const float* mono, std::size_t numSamples, double sampleRate, const std::vector<double>& boundaries,
                                  const SectionOptions& options = {}, const ProgressFn& progress = {});
SectionDelta compareSections (const SectionResult& from, const SectionResult& to);

// ---------------------------------------------------------------------------------------------------------------------------------------
// tempo automation

enum class TempoBehaviour
{
    Unknown,       // not enough evidence
    Constant,      // tempo and pitch both steady
    Varispeed,     // tempo moves and the pitch moves with it (tape, turntable, a "speed" knob): detune = offset + 1200 log2 (bpm / bpm_ref)
    TimeStretch,   // tempo moves, the pitch does not (a stretch with pitch preserved, or a tempo change in the music itself: audio alone cannot tell them apart)
    PitchMoves,    // the pitch moves while the tempo stays: a retune, a pitch bend, a different sample
    Mixed          // both move but not in the way of either model: look at the regions
};
const char* behaviourName (TempoBehaviour b);

struct AutomationOptions
{
    double windowSeconds = 3.0;          // detune windows (2 - 4 s), overlapped
    double hopSeconds = 1.0;
    double tempoWindowSeconds = 6.0;     // tempo needs a few beats per window
    const TempoMap* tempoMap = nullptr;  // manual points or the host's tempo map: used INSTEAD of measuring the tempo from the audio
    double referenceBpm = 0.0;           // the tempo the source ran at, if known (then the detune at that tempo is reported)
    std::vector<double> boundaries;      // regions to report; empty: the suggested boundaries
    bool regionKeys = true;              // key per region, measured against the tuning curve
};

struct CurvePoint
{
    double time = 0.0;           // window centre (s)
    double window = 0.0;         // seconds of audio behind the detune (3 s, widened where the evidence is thin)
    bool detuneValid = false;
    double cents = 0.0;          // detune of the window, wrapped to [-50, 50)
    double ci = 0.0;             // 95 % half-width
    double unwrapped = 0.0;      // the same on a continuous scale (a varispeed ramp of 267 cents shows as 267, not as 2.7 turns of the circle)
    double mass = 0.0;           // how much evidence the window holds
    double sharpness = 0.0;      // how peaked that evidence is on the 100-cent circle (about 0 for noise, 0.25 - 2.5 for real music)
    bool tempoValid = false;
    double bpm = 0.0;
    double tempoCents = 0.0;     // 1200 * log2 (bpm / referenceTempo), folded to one octave: what a varispeed would add to the detune
};

struct SuggestedBoundary
{
    double time = 0.0;           // seconds
    double strength = 0.0;       // larger = clearer (a log-likelihood-like score)
    bool detuneJump = false, tempoStep = false;
    bool passageStart = false, passageEnd = false;   // one of the two ends of a short part (< 25 s) at another detune that then returns: not a switch to a new beat
    double jumpCents = 0.0;      // detune jump (after removing a varispeed trend)
    double significance = 0.0;   // detune jump over the combined 95 % interval of the two plateaus (steady tuning only; 0 otherwise): above 2 counts
    double bpmBefore = 0.0, bpmAfter = 0.0;
    std::string reason;
};

struct Region
{
    double start = 0.0, end = 0.0;
    bool ok = false;             // enough windows
    int windows = 0;
    double cents = 0.0, ci = 0.0;            // mean detune of the region (circular), and its scatter / interval
    double centsStart = 0.0, centsEnd = 0.0; // unwrapped detune at both ends when the pitch moves inside the region
    bool pitchMoves = false;
    double bpm = 0.0, bpmMin = 0.0, bpmMax = 0.0;
    bool tempoMoves = false;
    TempoBehaviour behaviour = TempoBehaviour::Unknown;
    KeyEstimate key;             // measured against the tuning curve (valid only if options.regionKeys)
};

struct TempoAutomationResult
{
    bool valid = false;
    bool cancelled = false;
    double seconds = 0.0;
    bool tempoFromMap = false;            // the tempo came from AutomationOptions::tempoMap, not from the audio

    TempoBehaviour behaviour = TempoBehaviour::Unknown;
    double slope = 0.0;                   // fitted d(detune) / d(tempo in cents): 1 = varispeed, 0 = time-stretch
    double spreadVarispeed = 0.0;         // circular sd (cents) of the detune once a varispeed trend is removed ...
    double spreadFixed = 0.0;             // ... and of the raw detune (what a time-stretch would give)
    double tempoNoise = 0.0;              // jitter of the tempo curve (cents): moves smaller than a few of these are not trusted

    double medianBpm = 0.0, minBpm = 0.0, maxBpm = 0.0;
    double centsStart = 0.0, centsEnd = 0.0;            // unwrapped detune at the first and last window
    double bpmStart = 0.0, bpmEnd = 0.0;
    double referenceBpm = 0.0;            // varispeed: the tempo at which the track sits on A440 (0 cents), the one closest to the starting tempo
    std::vector<double> zeroBpms;         // ... and the same source tempo one semitone up / down inside the observed tempo range
    double detuneAtReference = 0.0;       // when AutomationOptions::referenceBpm was given: the detune the track would have at that tempo

    std::vector<CurvePoint> curve;
    std::vector<SuggestedBoundary> boundaries;
    std::vector<Region> regions;
    std::string summary;                  // a few sentences, ready to show
};

TempoAutomationResult analyzeTempoAutomation (const float* mono, std::size_t numSamples, double sampleRate, const AutomationOptions& options = {},
                                              const ProgressFn& progress = {});
} // namespace centsible
