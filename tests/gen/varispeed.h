#pragma once
// Time-varying resampling: a real song played at a speed that changes over time (a varispeed ramp), with the pitch following the speed.
// rate (t) is the playback speed at OUTPUT time t (1 = unchanged, 1.1 = 10 % faster and 165 cents higher); the input is read at the integral of that.
// Kaiser-windowed sinc interpolation, band-limited for the highest rate.
#include "resample.h"
#include <functional>

inline std::vector<float> resampleVarying (const std::vector<float>& in, double sr, const std::function<double (double)>& rate, double maxRate)
{
    constexpr int half = 32, phases = 1024;
    constexpr double beta = 9.0;
    const double cutoff = std::min (1.0, 1.0 / maxRate) * 0.97;
    const int taps = 2 * half;
    std::vector<float> table ((size_t) (phases + 1) * taps);
    for (int p = 0; p <= phases; ++p)
        for (int t = 0; t < taps; ++t)
        {
            const double x = (t - half + 1) - (double) p / phases;
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - (x / half) * (x / half)))) / besselI0 (beta);
            const double s = std::fabs (x) < 1e-12 ? 1.0 : std::sin (kPi * x * cutoff) / (kPi * x * cutoff);
            table[(size_t) p * taps + t] = (float) (cutoff * s * w);
        }
    std::vector<float> out;
    out.reserve (in.size());
    double pos = half;
    for (size_t n = 0; pos < (double) in.size() - half - 2; ++n)
    {
        const long i0 = (long) pos;
        const double ph = (pos - (double) i0) * phases;
        const int p = (int) ph;
        const float a = (float) (ph - p);
        const float* k0 = &table[(size_t) p * taps];
        const float* k1 = k0 + taps;
        double acc = 0.0;
        for (int t = 0; t < taps; ++t)
            acc += (double) in[(size_t) (i0 - half + 1 + t)] * ((double) k0[t] * (1.0 - a) + (double) k1[t] * a);
        out.push_back ((float) acc);
        pos += rate ((double) n / sr);
    }
    return out;
}
