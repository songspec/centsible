// Whole-track features: tuning (shared Analyzer), key (tuning-corrected chroma + Krumhansl-Schmuckler), tempo (flux autocorrelation + pulse trains).
// Offline code: it allocates freely and is never called from the audio thread.
#include <centsible/track.h>
#include "fft.h"
#include "track_internal.h"
#include "key_detector.h"
#include "tempo_detector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace centsible
{
namespace
{
int ceilPow2Order (double x)
{
    int o = 4;
    while ((double) (1 << o) < x) ++o;
    return o;
}

} // namespace

namespace detail
{
// ---- key ---------------------------------------------------------------------------------------------------------------------
// Two-stage detector (core/src/key_detector.h). Stage-2 weights fitted with tools/key_train.py on 53 songs with tracker-verified keys (REPORT.md).
KeyEstimate estimateKey (const float* x, std::size_t n, double sr, double tuningCents, const keydetail::KeyFeatureOptions& base)
{
    KeyEstimate key;
    auto opt = base;
    opt.hpss = true;                   // harmonic/percussive mask: drums do not pollute the chroma (benchmarked: at least as good as the sustain gate alone)
    const auto feats = keydetail::computeKeyFeatures (x, n, sr, tuningCents, opt);
    keydetail::KeyModel model;
    model.wBass = 0.8435; model.wNotes = 1.4350; model.wMajorMinor = 0.1705;
    model.degreeBias = { 1.3333, -0.8154, 0.2550, -1.3267, -0.4802, 1.6067, -0.5726 };
    const auto d = keydetail::decideKey (feats, model);
    if (! d.valid) return key;
    key.valid = true;
    key.root = d.tonic;
    key.minor = d.minor;
    key.scaleRoot = d.scale;
    key.degree = d.degree;
    for (int i = 0; i < 7; ++i) key.modeProbability[(size_t) i] = (float) d.modeProb[(size_t) i];
    key.scaleConfidence = (float) d.scaleConfidence;
    key.confidence = (float) (d.scaleConfidence * d.tonicConfidence);
    key.betweenKeys = ! base.tuningCurve && std::fabs (tuningCents) >= 42.0;     // with a moving tuning the question is per frame, not for the stretch
    const int self = d.tonic + 12 * (d.minor ? 1 : 0);
    key.probability = (float) d.keyProb[(size_t) self];
    int best2 = -1;
    for (int i = 0; i < 24; ++i) if (i != self && (best2 < 0 || d.keyProb[(size_t) i] > d.keyProb[(size_t) best2])) best2 = i;
    key.runnerUpRoot = best2 % 12; key.runnerUpMinor = best2 >= 12; key.runnerUpProbability = (float) d.keyProb[(size_t) best2];
    const int rel = relativeRoot (key) + 12 * (relativeIsMinor (key) ? 1 : 0);
    key.relativeProbability = (float) d.keyProb[(size_t) rel];
    const double pair = (double) key.probability + (double) key.relativeProbability;
    key.relativeAmbiguous = pair > 0.0 && std::min ((double) key.probability, (double) key.relativeProbability) / pair >= 0.30;
    return key;
}

// ---- tempo -------------------------------------------------------------------------------------------------------------------
// The main tempo is reported in 80 - 160 BPM (the DJ / producer convention, also the one of the tracker the detector was checked against): a song is
// scored OCTAVE-AGNOSTICALLY (pulse trains at b, 2b and b/2 all support b) inside that range, so half / double time can no longer pull it to the wrong
// metrical level. The other octave (b/2 or 2b) is reported as `altBpm` when its own pulse train is nearly as strong.
TempoEstimate estimateTempo (const float* x, std::size_t n, double sr)
{
    TempoEstimate tempo;
    const auto curve = tempodetail::computeTempoCurve (x, n, sr, 0);
    if (! curve.valid) return tempo;
    auto score = [&] (double bpm) { return curve.pulse (bpm * 0.5) + curve.pulse (bpm) + curve.pulse (bpm * 2.0); };
    double bestBpm = 0.0, bestScore = -1.0, sum = 0.0;
    int count = 0;
    for (double bpm = 80.0; bpm < 160.0; bpm += 0.25)
    {
        const double sc = score (bpm);
        sum += sc; ++count;
        if (sc > bestScore) { bestScore = sc; bestBpm = bpm; }
    }
    for (double bpm = bestBpm - 0.6; bpm <= bestBpm + 0.6; bpm += 0.02)
    {
        if (bpm < 80.0 || bpm >= 160.0) continue;
        const double sc = score (bpm);
        if (sc > bestScore) { bestScore = sc; bestBpm = bpm; }
    }
    // the other octave, if its own pulse train is nearly as good as the main one's
    {
        const double main = curve.pulse (bestBpm);
        double altBpm = 0.0, altScore = 0.0;
        for (double mult : { 2.0, 0.5 })
        {
            const double cand = bestBpm * mult;
            if (cand < 55.0 || cand > 320.0) continue;
            double sc = 0.0;
            for (double d = -0.6; d <= 0.6; d += 0.02) sc = std::max (sc, curve.pulse (cand + d * mult));
            if (sc > altScore) { altScore = sc; altBpm = cand; }
        }
        tempo.altBpm = altScore >= 0.7 * main ? (float) altBpm : 0.0f;
    }
    const double mean = sum / std::max (1, count);
    tempo.valid = bestScore > 0.0 && bestBpm > 0.0;
    tempo.bpm = (float) bestBpm;
    tempo.confidence = (float) std::clamp ((bestScore / std::max (1e-9, mean) - 1.0) / 3.0, 0.0, 1.0);
    return tempo;
}
} // namespace detail

std::string keyName (int root, bool minor)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string (names[((root % 12) + 12) % 12]) + (minor ? " minor" : " major");
}

const char* modeName (int degree)
{
    static const char* modes[7] = { "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian" };
    return degree >= 0 && degree < 7 ? modes[degree] : "";
}

std::string modeSummary (const KeyEstimate& k, int count)
{
    std::array<int, 7> order {};
    for (int i = 0; i < 7; ++i) order[(size_t) i] = i;
    std::sort (order.begin(), order.end(), [&] (int a, int b) { return k.modeProbability[(size_t) a] > k.modeProbability[(size_t) b]; });
    std::string s;
    for (int i = 0; i < std::min (count, 7); ++i)
    {
        char buf[48];
        std::snprintf (buf, sizeof buf, "%s%s %d %%", i ? ", " : "", modeName (order[(size_t) i]), (int) std::lround (100.0 * (double) k.modeProbability[(size_t) order[(size_t) i]]));
        s += buf;
    }
    return s;
}

std::string keyDetail (const KeyEstimate& k)
{
    std::string s = keyName (k.root, k.minor);
    if (k.degree >= 0 && k.degree < 7) s += std::string (" (") + modeName (k.degree) + ")";
    return s;
}

int relativeRoot (const KeyEstimate& k)
{
    // a minor mode shares its notes with the major scale (A minor / C major; C Phrygian / A-flat major); a major mode with the minor on the scale's sixth degree
    return k.minor ? k.scaleRoot : (k.scaleRoot + 9) % 12;
}

bool relativeIsMinor (const KeyEstimate& k) { return ! k.minor; }

std::string relativeKeyName (const KeyEstimate& k)
{
    return keyName (relativeRoot (k), relativeIsMinor (k));
}

std::string camelot (int root, bool minor)
{
    static const int major[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };     // C, C#, D, D#, E, F, F#, G, G#, A, A#, B
    static const int minorN[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };    // C minor = 5A ... A minor = 8A
    const int r = ((root % 12) + 12) % 12;
    return std::to_string (minor ? minorN[r] : major[r]) + (minor ? "A" : "B");
}

std::string keyLabel (const KeyEstimate& k) { return keyDetail (k) + " (" + camelot (k.root, k.minor) + ")"; }

std::string keyAmbiguityText (const KeyEstimate& k)
{
    const double a = k.probability, b = k.relativeProbability, pair = std::max (1e-9, a + b);
    char buf[160];
    std::snprintf (buf, sizeof buf, "%s / %s, %d / %d", keyName (k.root, k.minor).c_str(), relativeKeyName (k).c_str(), (int) std::lround (100.0 * a / pair), (int) std::lround (100.0 * b / pair));
    return buf;
}

std::string runnerUpText (const KeyEstimate& k)
{
    char buf[160];
    std::snprintf (buf, sizeof buf, "runner-up %s (%s), %d %%", keyName (k.runnerUpRoot, k.runnerUpMinor).c_str(), camelot (k.runnerUpRoot, k.runnerUpMinor).c_str(), (int) std::lround (100.0 * k.runnerUpProbability));
    return buf;
}

namespace detail
{
TrackAnalysis analyzeTrackProgress (const float* mono, std::size_t n, double sr, const ProgressFn& progress, double from, double to)
{
    TrackAnalysis t;
    t.seconds = (double) n / sr;
    auto report = [&] (double f, const char* stage) { return ! progress || progress (from + (to - from) * f, stage); };
    Analyzer analyzer;
    analyzer.prepare (sr);
    for (std::size_t pos = 0; pos < n; pos += 4096)
    {
        analyzer.push (mono + pos, (int) std::min<std::size_t> (4096, n - pos));
        if ((pos / 4096) % 32 == 0 && ! report (0.6 * (double) pos / (double) std::max<std::size_t> (1, n), "tuning")) { t.cancelled = true; return t; }
    }
    t.tuning = analyzer.reading();
    if (! report (0.6, "key")) { t.cancelled = true; return t; }
    t.key = estimateKey (mono, n, sr, t.tuning.valid ? (double) t.tuning.cents : 0.0, {});
    if (! report (0.85, "tempo")) { t.cancelled = true; return t; }
    t.tempo = estimateTempo (mono, n, sr);
    report (1.0, "tempo");
    return t;
}
} // namespace detail

TrackAnalysis analyzeTrack (const float* mono, std::size_t n, double sr)
{
    return detail::analyzeTrackProgress (mono, n, sr, {}, 0.0, 1.0);
}

MatchAdvice matchAdvice (const TrackAnalysis& target, const TrackAnalysis& source)
{
    MatchAdvice m;
    m.fineCents = wrapCents ((double) target.tuning.cents - (double) source.tuning.cents);
    m.keysKnown = target.key.valid && source.key.valid;
    if (m.keysKnown)
    {
        const int a = target.key.scaleRoot, b = source.key.scaleRoot;
        int semis = ((a - b) % 12 + 12) % 12;
        if (semis > 5) semis -= 12;
        m.semitones = semis;
        m.sameKeyFamily = semis == 0;
    }
    m.totalCents = m.semitones * 100.0 + m.fineCents;
    return m;
}
} // namespace centsible
