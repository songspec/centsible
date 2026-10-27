#pragma once
#include <array>
#include <cmath>

namespace centsible
{
struct VizPeak
{
    float cents = 0.0f;   // folded offset from the 12-TET grid, [-50, 50)
    float weight = 0.0f;  // 0..1
};

// Result of the analysis. cents is relative to 12-TET A440, wrapped to [-50, +50).
struct Reading
{
    static constexpr int historyPoints = 120;
    static constexpr int vizPoints = 32;

    // --- the answer
    float cents = 0.0f;           // global tuning offset of the dominant tuning
    float refHz = 440.0f;         // equivalent reference pitch, 440 * 2^(cents/1200)
    float confidence = 0.0f;      // 0..1: concentrated evidence, a clear winner, several notes agreeing
    float spreadCents = 0.0f;     // how far the estimate moved over the recent past
    float liveCents = 0.0f;       // latest single-frame estimate (noisy by design)
    float liveConfidence = 0.0f;  // 0..1, for the live estimate
    float seconds = 0.0f;         // seconds of usable audio heard since the song (re)started
    bool valid = false;           // at least one usable frame seen
    bool settled = false;         // enough audio, a clear winner, backed by several notes, not moving, not contested
    float dominance = 0.0f;       // support of the winning tuning relative to the runner-up (>= 1)
    int notes = 0;                // distinct notes backing the winning tuning

    // --- a second tuning, only when it is statistically solid
    bool hasSecondary = false;
    float secondaryCents = 0.0f;
    float secondaryShare = 0.0f;  // 0..1 of the evidence (winner + secondary)
    int secondaryNotes = 0;

    // --- candidate runner-up regardless of significance (diagnostics) and song-change state
    bool hasAlt = false;
    float altCents = 0.0f;
    bool contested = false;       // the newest audio disagrees with what was accumulated: possibly a different song
    int songChanges = 0;          // how many times a new song was detected

    // --- for visuals (filled by the analysis thread)
    float centroidHz = 0.0f;      // spectral centroid of the last analysed frame
    unsigned frameId = 0;         // increments with every analysed frame that produced observations
    int vizCount = 0;
    std::array<VizPeak, vizPoints> viz {};
    float rms = 0.0f;             // recent signal level (filled by AnalysisThread)
    unsigned onsetCount = 0;      // increments on each detected onset (filled by AnalysisThread)

    float ciCents = 0.0f;         // 95% confidence half-width of `cents` (0 until the estimator provides it)
    bool locked = false;          // the confidence interval is small enough that the readout may stop moving
    bool competing = false;       // two tunings carry about the same evidence (the other one is the secondary): not locked, ciCents covers both

    // --- tuning drift (tape/vinyl wow, sampled sections, a retuned outro): the later half of the song against the earlier half
    bool hasDrift = false;
    float driftCents = 0.0f;      // later half minus earlier half; only meaningful if hasDrift (needs >= 40 s and a significant difference)

    int historyCount = 0;         // number of valid entries in history (oldest first)
    std::array<float, historyPoints> history {}; // running estimate, one point per second of usable audio
};

// Wraps cents into [-50, +50).
inline double wrapCents (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }
} // namespace centsible
