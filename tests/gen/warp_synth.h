#pragma once
// Synthetic songs whose tempo and pitch MOVE over time, for the Beatswitch tests.
//   * Plan::bpm (t)   - tempo at output time t; the beat grid (chords change on bars, drums on beats) is its integral.
//   * Plan::cents (t) - detune of every pitched note at output time t, in cents against 12-TET A440.
// A varispeed song (tape / turntable / "pitch + tempo" knob) is the plan with cents (t) = base + 1200 log2 (bpm (t) / bpmReference);
// a time-stretched one (pitch preserved) has a constant cents (t) while bpm (t) moves; a retuned one moves cents (t) with a constant bpm.
#include "synth.h"
#include <functional>

namespace synth
{
struct Plan
{
    std::function<double (double)> bpm;
    std::function<double (double)> cents;
};

inline Plan constantPlan (double bpm, double cents)
{
    return { [=] (double) { return bpm; }, [=] (double) { return cents; } };
}

// Tempo ramps linearly from bpm0 (until tA) to bpm1 (from tB). Pitch follows (varispeed) or stays at `base` (time-stretch).
inline Plan rampPlan (double bpm0, double bpm1, double tA, double tB, bool varispeed, double base, double bpmRef)
{
    auto bpm = [=] (double t) { return t <= tA ? bpm0 : t >= tB ? bpm1 : bpm0 + (bpm1 - bpm0) * (t - tA) / (tB - tA); };
    return { bpm, [=] (double t) { return varispeed ? base + 1200.0 * std::log2 (bpm (t) / bpmRef) : base; } };
}

struct ChordSongSpec
{
    int tonic = 0;           // pitch class, 0 = C
    bool minor = false;
    double seconds = 60.0;
    unsigned seed = 1;
    bool drums = true;
    double drumGain = 0.3;
    int beatsPerBar = 4;
};

// Output time of every beat (quarter note), from the integral of the tempo.
inline std::vector<double> beatGrid (const Plan& plan, double seconds)
{
    std::vector<double> beats;
    const double dt = 0.0005;
    double b = 0.0, nextBeat = 0.0;
    beats.push_back (0.0);
    nextBeat = 1.0;
    for (double t = 0.0; t < seconds + 2.0; t += dt)
    {
        const double step = plan.bpm (t + 0.5 * dt) / 60.0 * dt;
        while (b + step >= nextBeat)
        {
            beats.push_back (t + dt * (nextBeat - b) / step);
            nextBeat += 1.0;
        }
        b += step;
    }
    return beats;
}

// One pitched note whose pitch follows plan.cents (t): all 16 partials are exact multiples, amplitude 1/h.
inline void addPlannedNote (Signal& x, double sr, double t0, double t1, double semisFromA4, const Plan& plan, double gain, int maxPartials)
{
    const size_t i0 = (size_t) std::max (0.0, t0 * sr), i1 = std::min (x.size(), (size_t) (t1 * sr));
    double phase = 0.0;
    const double f0 = noteHz (semisFromA4);
    for (size_t i = i0; i < i1; ++i)
    {
        const double t = (double) i / sr, rem = t1 - t, up = t - t0;
        const double env = std::min ({ 1.0, up / 0.02, rem / 0.02 });
        phase += 2.0 * kPi * f0 * centsToRatio (plan.cents (t)) / sr;
        const double zc = std::cos (phase), zs = std::sin (phase);
        double pc = 1.0, ps = 0.0, sum = 0.0;
        for (int h = 1; h <= maxPartials; ++h)
        {
            const double nc = pc * zc - ps * zs, ns = ps * zc + pc * zs;
            pc = nc; ps = ns;
            sum += ps / h;
        }
        x[i] += (float) (gain * env * sum);
    }
}

// Four-chord loop (I V vi IV, or i VI III VII) on triads with a bass root; drums on every beat of the plan's beat grid.
inline Signal renderChordSong (const ChordSongSpec& spec, const Plan& plan, double sr)
{
    struct Chord { int degree; bool minor; };
    const Chord major[4] = { { 0, false }, { 7, false }, { 9, true }, { 5, false } };
    const Chord minorK[4] = { { 0, true }, { 8, false }, { 3, false }, { 10, false } };
    Signal x ((size_t) (spec.seconds * sr), 0.0f);
    const auto beats = beatGrid (plan, spec.seconds);
    for (size_t bar = 0; (bar + 1) * (size_t) spec.beatsPerBar < beats.size(); ++bar)
    {
        const double t0 = beats[bar * (size_t) spec.beatsPerBar], t1 = beats[(bar + 1) * (size_t) spec.beatsPerBar];
        if (t0 >= spec.seconds) break;
        const auto& c = (spec.minor ? minorK : major)[bar % 4];
        const int rootSemis = (spec.tonic + c.degree) % 12 - 21;
        for (int iv : { 0, c.minor ? 3 : 4, 7 }) addPlannedNote (x, sr, t0, t1, rootSemis + iv, plan, 0.07, 16);
        addPlannedNote (x, sr, t0, t1, rootSemis - 12, plan, 0.12, 12);
    }
    if (spec.drums)
    {
        std::mt19937 rng (spec.seed);
        Signal d (x.size(), 0.0f);
        for (size_t b = 0; b + 1 < beats.size() && beats[b] < spec.seconds; ++b)
        {
            const double bt = beats[b], half = 0.5 * (beats[b] + beats[b + 1]);
            if (b % 2 == 0) addKick (d, sr, bt, 0.8);
            else            addSnare (d, sr, bt, 0.45, rng);
            addHat (d, sr, bt, 0.12, rng);
            addHat (d, sr, half, 0.08, rng);
        }
        mixInto (x, d, (float) spec.drumGain);
    }
    return x;
}
} // namespace synth
