#pragma once
// Synthetic music for the accuracy tests: rich harmonic tones, stretched (inharmonic) partials, vocals with vibrato
// and weak/missing fundamentals, bass below the analysis range, drums, reverb, noise, and whole "fake songs".
// Everything is built from 12-TET notes (A4 = 440 Hz) and then shifted by a known detune in cents.
#include "fft.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace synth
{
using Signal = std::vector<float>;

inline double noteHz (double semisFromA4) { return 440.0 * std::pow (2.0, semisFromA4 / 12.0); }
inline double centsToRatio (double c) { return std::pow (2.0, c / 1200.0); }
inline double wrap50 (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }

enum class Timbre
{
    Saw,                // 1/h, up to 24 partials: the rich case
    Odd,                // clarinet-like, odd partials only
    Soft,               // 1/h^2
    Voice,              // formants at ~700 Hz and ~2.4 kHz, weak fundamental
    VoiceNoFundamental, // same, fundamental absent (high-passed vocal)
    PartialsOnly        // fundamental missing, partials 2.. strong (small speaker / bass heard through a high-pass)
};

inline std::vector<double> amplitudes (Timbre t, double f0, double sr, int maxPartials = 24)
{
    std::vector<double> a;
    for (int h = 1; h <= maxPartials; ++h)
    {
        const double fh = f0 * h;
        if (fh > 0.45 * sr || fh > 12000.0) break;
        double v = 0.0;
        switch (t)
        {
            case Timbre::Saw:  v = 1.0 / h; break;
            case Timbre::Odd:  v = (h % 2) ? 1.0 / h : 0.0; break;
            case Timbre::Soft: v = 1.0 / (h * h); break;
            case Timbre::Voice:
            case Timbre::VoiceNoFundamental:
            {
                auto g = [&] (double c, double w) { const double x = (fh - c) / w; return std::exp (-0.5 * x * x); };
                v = (1.0 / std::sqrt ((double) h)) * (0.15 + 1.6 * g (700.0, 250.0) + 1.0 * g (2400.0, 600.0));
                if (h == 1) v *= (t == Timbre::VoiceNoFundamental ? 0.0 : 0.25);
                break;
            }
            case Timbre::PartialsOnly: v = (h == 1) ? 0.0 : 1.0 / h; break;
        }
        a.push_back (v);
    }
    return a;
}

// Steady note. Partial h sits at h*f0*sqrt((1+B h^2)/(1+B)): the fundamental is exactly f0, upper partials are
// stretched when B > 0 (piano, bass strings). Amplitudes `amps[h-1]`, linear fades at both ends.
inline void addStaticNote (Signal& x, double sr, double t0, double t1, double f0, const std::vector<double>& amps,
                           double B, double gain, double fade = 0.02)
{
    const size_t i0 = (size_t) std::max (0.0, t0 * sr), i1 = std::min (x.size(), (size_t) (t1 * sr));
    if (i1 <= i0) return;
    const size_t n = i1 - i0;
    std::vector<float> env (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr, rem = (double) (n - i) / sr;
        env[i] = (float) std::min ({ 1.0, t / fade, rem / fade });
    }
    for (size_t h = 1; h <= amps.size(); ++h)
    {
        if (amps[h - 1] == 0.0) continue;
        const double fh = (double) h * f0 * std::sqrt ((1.0 + B * (double) (h * h)) / (1.0 + B));
        if (fh >= 0.49 * sr) break;
        const double w = 2.0 * kPi * fh / sr;
        const double cw = std::cos (w), sw = std::sin (w);
        double c = 1.0, s = 0.0; // phase 0 at note start
        const float amp = (float) (gain * amps[h - 1]);
        for (size_t i = 0; i < n; ++i)
        {
            x[i0 + i] += amp * env[i] * (float) s;
            const double nc = c * cw - s * sw, ns = s * cw + c * sw;
            c = nc; s = ns;
            if ((i & 1023) == 1023) { const double r = 1.0 / std::hypot (c, s); c *= r; s *= r; }
        }
    }
}

// Note with vibrato (and an optional pitch scoop at the start, as singers do). All partials are exact multiples.
inline void addVibratoNote (Signal& x, double sr, double t0, double t1, double f0, const std::vector<double>& amps,
                            double gain, double vibDepthCents, double vibHz, double scoopCents, double vibPhase)
{
    const size_t i0 = (size_t) std::max (0.0, t0 * sr), i1 = std::min (x.size(), (size_t) (t1 * sr));
    double phase = 0.0;
    for (size_t i = i0; i < i1; ++i)
    {
        const double t = (double) (i - i0) / sr, rem = (double) (i1 - i) / sr;
        const double env = std::min ({ 1.0, t / 0.03, rem / 0.03 });
        const double cents = vibDepthCents * std::sin (2.0 * kPi * vibHz * t + vibPhase) * std::min (1.0, t / 0.25)
                             + scoopCents * std::exp (-t / 0.06);
        phase += 2.0 * kPi * f0 * std::pow (2.0, cents / 1200.0) / sr;
        const double zc = std::cos (phase), zs = std::sin (phase);
        double pc = 1.0, ps = 0.0, sum = 0.0;
        for (size_t h = 1; h <= amps.size(); ++h)
        {
            const double nc = pc * zc - ps * zs, ns = ps * zc + pc * zs;
            pc = nc; ps = ns;
            sum += amps[h - 1] * ps;
        }
        x[i] += (float) (gain * env * sum);
    }
}

inline void addKick (Signal& x, double sr, double t0, double gain)
{
    double ph = 0.0;
    for (size_t i = (size_t) (t0 * sr), j = 0; i < x.size() && j < (size_t) (0.25 * sr); ++i, ++j)
    {
        const double t = (double) j / sr;
        ph += 2.0 * kPi * (48.0 + 120.0 * std::exp (-t / 0.03)) / sr;
        x[i] += (float) (gain * std::exp (-t / 0.09) * std::sin (ph));
    }
}

inline void addSnare (Signal& x, double sr, double t0, double gain, std::mt19937& rng)
{
    std::normal_distribution<float> g (0.0f, 1.0f);
    for (size_t i = (size_t) (t0 * sr), j = 0; i < x.size() && j < (size_t) (0.2 * sr); ++i, ++j)
    {
        const double t = (double) j / sr;
        x[i] += (float) (gain * std::exp (-t / 0.05) * (0.7 * g (rng) + 0.5 * std::sin (2.0 * kPi * 190.0 * t)));
    }
}

inline void addHat (Signal& x, double sr, double t0, double gain, std::mt19937& rng)
{
    std::normal_distribution<float> g (0.0f, 1.0f);
    float prev = 0.0f;
    for (size_t i = (size_t) (t0 * sr), j = 0; i < x.size() && j < (size_t) (0.05 * sr); ++i, ++j)
    {
        const float n = g (rng), hp = n - prev; // crude high-pass
        prev = n;
        x[i] += (float) (gain * std::exp (-(double) j / (0.012 * sr))) * hp;
    }
}

// Schroeder reverb (6 combs + 2 allpasses). Returns the wet signal only.
inline Signal reverbWet (const Signal& in, double sr)
{
    const double scale = sr / 44100.0;
    const int combs[] = { 1557, 1617, 1491, 1422, 1277, 1356 };
    Signal out (in.size(), 0.0f);
    for (int d0 : combs)
    {
        const size_t d = (size_t) (d0 * scale);
        std::vector<float> buf (d, 0.0f);
        size_t p = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const float y = buf[p];
            buf[p] = in[i] + 0.84f * y;
            out[i] += y * (1.0f / 6.0f);
            p = (p + 1) % d;
        }
    }
    for (int d0 : { 225, 556 })
    {
        const size_t d = (size_t) (d0 * scale);
        std::vector<float> buf (d, 0.0f);
        size_t p = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float b = buf[p], y = -0.5f * out[i] + b;
            buf[p] = out[i] + 0.5f * y;
            out[i] = y;
            p = (p + 1) % d;
        }
    }
    return out;
}

inline double rms (const Signal& x)
{
    double p = 0.0;
    for (float v : x) p += (double) v * v;
    return std::sqrt (p / (double) std::max<size_t> (1, x.size()));
}

inline void addNoise (Signal& x, double snrDb, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> g (0.0f, 1.0f);
    const float s = (float) (rms (x) * std::pow (10.0, -snrDb / 20.0));
    for (auto& v : x) v += s * g (rng);
}

// ---------------------------------------------------------------------------------------------------------
// Whole signals. `detune` is applied to every note.

inline Signal sawMelody (double sr, double seconds, double detune)
{
    const int melody[] = { 0, 3, 7, 10, 5, 2, -2, -5, 12, 7, 3, 0 };
    Signal x ((size_t) (seconds * sr), 0.0f);
    const double k = centsToRatio (detune);
    for (int n = 0; n * 0.8 < seconds; ++n)
    {
        const double f0 = noteHz (melody[n % 12]) * k;
        addStaticNote (x, sr, n * 0.8, (n + 1) * 0.8, f0, amplitudes (Timbre::Saw, f0, sr), 0.0, 0.25);
    }
    return x;
}

inline Signal sawChords (double sr, double seconds, double detune)
{
    const int chords[4][3] = { { -12, -8, -5 }, { -14, -10, -7 }, { -16, -12, -9 }, { -12, -9, -5 } };
    Signal x ((size_t) (seconds * sr), 0.0f);
    const double k = centsToRatio (detune);
    for (int n = 0; n * 1.5 < seconds; ++n)
        for (int semis : chords[n % 4])
        {
            const double f0 = noteHz (semis) * k;
            addStaticNote (x, sr, n * 1.5, (n + 1) * 1.5, f0, amplitudes (Timbre::Saw, f0, sr, 16), 0.0, 0.07);
        }
    return x;
}

// One timbre, one note at a time. B > 0 stretches the upper partials; the fundamental stays on the grid.
inline Signal singleVoiceMelody (double sr, double seconds, double detune, Timbre t, double B, int octaveShift = 0)
{
    const int melody[] = { 0, 3, 7, 10, 5, 2, -2, -5, 7, 3, 0, -4 };
    Signal x ((size_t) (seconds * sr), 0.0f);
    const double k = centsToRatio (detune);
    for (int n = 0; n * 0.7 < seconds; ++n)
    {
        const double f0 = noteHz (melody[n % 12] + 12 * octaveShift) * k;
        addStaticNote (x, sr, n * 0.7, (n + 1) * 0.7, f0, amplitudes (t, f0, sr), B, 0.22);
    }
    return x;
}

// Bass line whose fundamentals (41-62 Hz) lie at/below the analysed range, with stretched partials.
inline Signal stretchedBass (double sr, double seconds, double detune, double B)
{
    const int line[] = { -41, -41, -36, -34, -41, -39, -36, -34 };
    Signal x ((size_t) (seconds * sr), 0.0f);
    const double k = centsToRatio (detune);
    for (int n = 0; n * 0.6 < seconds; ++n)
    {
        const double f0 = noteHz (line[n % 8]) * k;
        addStaticNote (x, sr, n * 0.6, (n + 1) * 0.6, f0, amplitudes (Timbre::Saw, f0, sr, 16), B, 0.3);
    }
    return x;
}

// Singer: vibrato, scoops, and optional natural scatter (random per-note intonation error in cents).
inline Signal vibratoVocal (double sr, double seconds, double detune, double scatterCents, unsigned seed)
{
    const int melody[] = { 0, 2, 3, 7, 5, 3, 2, 0, -2, 0, 3, 5, 7, 10, 7, 5 };
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    std::uniform_real_distribution<double> u (0.0, 2.0 * kPi);
    Signal x ((size_t) (seconds * sr), 0.0f);
    double t = 0.0;
    for (int n = 0; t < seconds; ++n)
    {
        const double len = 0.5 + 0.1 * (n % 4);
        const double f0 = noteHz (melody[n % 16] - 12 + 12 * ((n / 16) % 2)) * centsToRatio (detune + scatterCents * g (rng));
        addVibratoNote (x, sr, t, t + len, f0, amplitudes (Timbre::Voice, f0, sr), 0.2, 25.0, 5.5, -40.0, u (rng));
        t += len;
    }
    return x;
}

inline Signal drumLoop (double sr, double seconds, unsigned seed, double beat = 0.6)
{
    std::mt19937 rng (seed);
    Signal x ((size_t) (seconds * sr), 0.0f);
    for (int b = 0; b * beat < seconds; ++b)
    {
        if (b % 2 == 0) addKick (x, sr, b * beat, 0.8);
        else            addSnare (x, sr, b * beat, 0.45, rng);
        addHat (x, sr, b * beat, 0.12, rng);
        addHat (x, sr, b * beat + beat / 2, 0.08, rng);
    }
    return x;
}

inline void mixInto (Signal& dst, const Signal& src, float gain)
{
    for (size_t i = 0; i < std::min (dst.size(), src.size()); ++i) dst[i] += gain * src[i];
}

struct SongOptions
{
    bool chords = true, bass = true, drums = true, melody = true;
    double reverb = 0.25;       // wet amount for chords + melody
    double noiseSnrDb = 0.0;    // 0 = none
    double bassB = 8e-4;
    double scatterCents = 0.0;  // human intonation error on the melody
};

inline Signal fakeSong (double sr, double seconds, double detune, const SongOptions& o = {}, unsigned seed = 1)
{
    Signal x ((size_t) (seconds * sr), 0.0f);
    Signal bed ((size_t) (seconds * sr), 0.0f);
    if (o.chords) mixInto (bed, sawChords (sr, seconds, detune), 1.0f);
    if (o.melody) mixInto (bed, vibratoVocal (sr, seconds, detune, o.scatterCents, seed), 0.8f);
    mixInto (x, bed, 1.0f);
    if (o.reverb > 0.0) mixInto (x, reverbWet (bed, sr), (float) o.reverb * 2.0f);
    if (o.bass) mixInto (x, stretchedBass (sr, seconds, detune, o.bassB), 0.7f);
    if (o.drums) mixInto (x, drumLoop (sr, seconds, seed + 7), 0.35f);
    if (o.noiseSnrDb != 0.0) addNoise (x, o.noiseSnrDb, seed + 11);
    return x;
}

// Rap / pop vocal: short notes with gaps, a random intonation error per note (mean zero, `scatterCents` rms), random vibrato, no scoop bias.
inline Signal scatterVocal (double sr, double seconds, double detune, double scatterCents, unsigned seed, float gain)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    std::uniform_real_distribution<double> u01 (0.0, 1.0);
    const int scale[] = { 0, 2, 3, 5, 7, 8, 10, 12 };
    Signal x ((size_t) (seconds * sr), 0.0f);
    double t = 0.2;
    while (t < seconds)
    {
        const double len = 0.12 + 0.5 * u01 (rng);
        if (u01 (rng) < 0.65)
        {
            const double f0 = noteHz (scale[(int) (u01 (rng) * 8) % 8] - 12 - 12 * (u01 (rng) < 0.3)) * centsToRatio (detune + scatterCents * g (rng));
            addVibratoNote (x, sr, t, t + len, f0, amplitudes (Timbre::Voice, f0, sr), gain, 25.0 * u01 (rng), 5.0 + 1.5 * u01 (rng), 0.0, 6.28 * u01 (rng));
        }
        t += len + 0.05 * u01 (rng);
    }
    return x;
}

// 808: a sine with a short upward-sliding start (the slide is rejected by steadiness, the sustain is exactly on the grid).
inline Signal bass808 (double sr, double seconds, double detune, float gain, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u01 (0.0, 1.0);
    const int line[] = { -37, -37, -34, -32, -37, -35, -34, -30 };
    Signal x ((size_t) (seconds * sr), 0.0f);
    for (int n = 0; n * 0.6 < seconds; ++n)
        addVibratoNote (x, sr, n * 0.6, (n + 1) * 0.6 - 0.05, noteHz (line[n % 8]) * centsToRatio (detune), { 1.0, 0.15 }, gain, 0.0, 5.0, 150.0 * (0.5 + u01 (rng)), 0.0);
    return x;
}

// A dense modern mix with a known tuning: loud scattered vocals, a quiet exact pad (optional), an 808, drums, noise and reverb. This is the kind
// of material on which real tracks only resolve to about +-1 cent, so it is a benchmark, not a pass/fail test.
inline Signal denseSong (double sr, double seconds, double detune, unsigned seed, double scatterCents, float vocalGain, float padGain)
{
    Signal bed ((size_t) (seconds * sr), 0.0f);
    mixInto (bed, scatterVocal (sr, seconds, detune, scatterCents, seed, 1.0f), vocalGain);
    if (padGain > 0.0f)
    {
        const int chords[4][3] = { { -12, -9, -5 }, { -14, -10, -7 }, { -16, -12, -9 }, { -12, -9, -5 } };
        const double k = centsToRatio (detune);
        for (int n = 0; n * 3.0 < seconds; ++n)
            for (int semis : chords[n % 4])
            {
                const double f0 = noteHz (semis) * k;
                addStaticNote (bed, sr, n * 3.0, (n + 1) * 3.0, f0, amplitudes (Timbre::Saw, f0, sr, 10), 0.0, 0.1 * padGain, 0.2);
            }
    }
    Signal x (bed);
    mixInto (x, reverbWet (bed, sr), 0.6f);
    mixInto (x, bass808 (sr, seconds, detune, 0.4f, seed + 3), 1.0f);
    mixInto (x, drumLoop (sr, seconds, seed + 7), 0.35f);
    addNoise (x, 14.0, seed + 11);
    return x;
}

// A song at tuning d1 with a second source (a sample / a section / a different instrument) at tuning d2 that is
// present for a fraction `share2` of the time, in separate stretches spread across the whole song.
inline Signal twoTunings (double sr, double seconds, double d1, double d2, double share2, unsigned seed = 3)
{
    Signal x = fakeSong (sr, seconds, d1, SongOptions {}, seed);
    const double period = 20.0, len2 = period * share2;
    const int melody[] = { 0, 3, 5, 7, 10, 7, 5, 3 };
    for (int p = 0; p * period < seconds; ++p)
        for (int n = 0; (n + 1) * 0.8 <= len2; ++n)
        {
            const double f0 = noteHz (melody[n % 8] + 12) * centsToRatio (d2);
            const double t0 = p * period + 8.0 + n * 0.8;
            addStaticNote (x, sr, t0, t0 + 0.8, f0, amplitudes (Timbre::Saw, f0, sr, 12), 0.0, 0.12);
        }
    return x;
}

// Concatenate two signals (song A then song B), as when the user plays a different track.
inline Signal concat (const Signal& a, const Signal& b)
{
    Signal x (a);
    x.insert (x.end(), b.begin(), b.end());
    return x;
}
} // namespace synth
