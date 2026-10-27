// Pieces of track analysis shared by analyzeTrack (track_features.cpp) and the section / tempo-automation analysis (sections.cpp).
// Offline code; allocates freely.
#pragma once
#include <centsible/progress.h>
#include <centsible/track.h>
#include "key_detector.h"
#include <cstddef>

namespace centsible::detail
{
// Key of a stretch of audio, measured against the tuning (a constant, or `opt.tuningCurve` when the pitch moves over the stretch).
KeyEstimate estimateKey (const float* x, std::size_t n, double sr, double tuningCents, const keydetail::KeyFeatureOptions& opt = {});

// Whole-stretch tempo (octave-agnostic, folded into 80 - 160 BPM, the other octave as altBpm).
TempoEstimate estimateTempo (const float* x, std::size_t n, double sr);

// analyzeTrack with progress (fractions from..to of a larger job) and cancellation; `cancelled` is set when the callback returned false.
TrackAnalysis analyzeTrackProgress (const float* mono, std::size_t n, double sr, const ProgressFn& progress, double from, double to);
} // namespace centsible::detail
