#pragma once
#include "fft.h"
#include <algorithm>
#include <vector>

inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}

// Output sample n reads the input at position n * ratio, so pitch (and speed) change by `ratio`.
inline std::vector<float> resample (const std::vector<float>& in, double ratio)
{
    constexpr int half = 32, phases = 2048;
    constexpr double beta = 9.0;
    const double cutoff = std::min (1.0, 1.0 / ratio) * 0.97; // anti-alias when reading faster
    const int taps = 2 * half;
    std::vector<float> table ((size_t) (phases + 1) * taps);
    for (int p = 0; p <= phases; ++p)
        for (int t = 0; t < taps; ++t)
        {
            const double x = (t - half + 1) - (double) p / phases; // distance from the fractional position
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - (x / half) * (x / half)))) / besselI0 (beta);
            const double s = std::fabs (x) < 1e-12 ? 1.0 : std::sin (kPi * x * cutoff) / (kPi * x * cutoff);
            table[(size_t) p * taps + t] = (float) (cutoff * s * w);
        }

    const size_t outLen = (size_t) ((double) (in.size() - 2 * half) / ratio);
    std::vector<float> out (outLen);
    for (size_t n = 0; n < outLen; ++n)
    {
        const double pos = (double) n * ratio + half;
        const long i0 = (long) pos;
        const double frac = pos - (double) i0;
        const double ph = frac * phases;
        const int p = (int) ph;
        const float a = (float) (ph - p);
        const float* k0 = &table[(size_t) p * taps];
        const float* k1 = k0 + taps;
        double acc = 0.0;
        for (int t = 0; t < taps; ++t)
            acc += (double) in[(size_t) (i0 - half + 1 + t)] * ((double) k0[t] * (1.0 - a) + (double) k1[t] * a);
        out[n] = (float) acc;
    }
    return out;
}

