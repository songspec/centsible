#pragma once
#include <centsible/analyzer.h>
#include <array>
#include <cstddef>
#include <string>

// Whole-track analysis for offline use (CLI batch, drag-and-drop in the plugin, match mode): tuning from the shared Analyzer, plus key and tempo.
// Key: Krumhansl-Schmuckler profiles on a chroma that is corrected by the detected tuning (so a song pitched +37 cents is still read in the right key).
// Tempo: spectral-flux onset envelope, autocorrelation, octave-agnostic pulse-train scoring inside 80-160 BPM (half / double time reported as the alternative).
// Known limits: relative major/minor are easily confused by the key method; half/double tempo is a judgement call for some genres.
namespace centsible
{
struct KeyEstimate
{
    bool valid = false;
    int root = 0;              // tonic, 0 = C ... 11 = B
    bool minor = false;        // major or minor mode ("minor" also covers Dorian / Phrygian / Locrian, "major" Lydian / Mixolydian; see `degree`)
    float confidence = 0.0f;   // 0..1: how much to trust (root, minor): scale confidence x tonic confidence
    int scaleRoot = 0;         // root of the MAJOR scale that holds the song's notes: the relative major/minor pair shares it
    float scaleConfidence = 0.0f; // 0..1: how sure the scale is (the tonic choice within it is the weaker part)
    int degree = 0;            // which degree of that scale is the tonic: 0 I, 1 ii (Dorian), 2 iii (Phrygian), 3 IV (Lydian), 4 V (Mixolydian), 5 vi (natural minor), 6 vii
    bool betweenKeys = false;  // the song is tuned about half a semitone off the grid, so the key is ambiguous by a semitone (the neighbouring key is as likely)

    // probabilities (all 24 keys sum to 1)
    float probability = 0.0f;           // of the reported key
    int runnerUpRoot = 0;               // the second most likely key
    bool runnerUpMinor = false;
    float runnerUpProbability = 0.0f;
    float relativeProbability = 0.0f;   // of the relative key: the one that shares all seven notes with the reported key
    std::array<float, 7> modeProbability {};   // the seven modes of the scale the song's notes belong to, by the degree of the tonic: Ionian, Dorian, Phrygian, Lydian, Mixolydian, Aeolian, Locrian (sum 1)
    bool relativeAmbiguous = false;     // the relative key is at least 30 % of the pair: "C major / A minor" cannot be told apart
};

struct TempoEstimate
{
    bool valid = false;
    float bpm = 0.0f;         // reported in 80 - 160 (DJ / producer convention); the other octave is altBpm
    float confidence = 0.0f;  // 0..1, how much the best pulse train stands out
    float altBpm = 0.0f;      // the half- or double-tempo reading when it is nearly as plausible (0 = none): which octave is 'the' tempo is a judgement call
};

struct TrackAnalysis
{
    Reading tuning;
    KeyEstimate key;
    TempoEstimate tempo;
    double seconds = 0.0;
    bool cancelled = false;   // only set by the progress-reporting analyses (sections): the caller stopped the job
};

TrackAnalysis analyzeTrack (const float* mono, std::size_t numSamples, double sampleRate);

std::string keyName (int root, bool minor);   // "F# minor"
const char* modeName (int degree);            // 0 Ionian, 1 Dorian, 2 Phrygian, 3 Lydian, 4 Mixolydian, 5 Aeolian, 6 Locrian
std::string modeSummary (const KeyEstimate& k, int count = 3);   // "Ionian 38 %, Aeolian 33 %, Dorian 12 %": the most likely modes of the scale
std::string keyDetail (const KeyEstimate& k);   // "C major (Ionian)", "A minor (Aeolian)", "C minor (Phrygian)": every one of the seven diatonic modes is named
std::string relativeKeyName (const KeyEstimate& k);   // "C major" for A minor: the key that shares all seven notes
int relativeRoot (const KeyEstimate& k);
bool relativeIsMinor (const KeyEstimate& k);
std::string camelot (int root, bool minor);   // "8A" for A minor, "8B" for C major (the standard Camelot wheel)
std::string keyLabel (const KeyEstimate& k);  // "A minor (8A)", "C minor (Phrygian) (5A)"
std::string keyAmbiguityText (const KeyEstimate& k);   // "A minor / C major, 55 / 45": the reported key against its relative key, as shares of the pair
std::string runnerUpText (const KeyEstimate& k);        // "runner-up F# minor (2A), 18 %"

// Advice for making track B sit on track A.
struct MatchAdvice
{
    double fineCents = 0.0;     // [-50, 50): moves B's tuning grid onto A's
    int semitones = 0;          // [-6, 5]: moves B's scale onto A's (relative major/minor and modes of the same scale count as the same); 0 if either key is unknown
    double totalCents = 0.0;    // semitones * 100 + fineCents
    bool keysKnown = false;
    bool sameKeyFamily = false; // both tracks already in the same key (or relative keys) after the shift
};
MatchAdvice matchAdvice (const TrackAnalysis& target, const TrackAnalysis& source);
} // namespace centsible
