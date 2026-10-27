#pragma once
// Independent measurement of the pitch shift between two recordings of the same audio:
// cross-correlates long-term log-frequency spectra (500 Hz - 8 kHz). No peak picking.
#include "fft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Mean-removed log power spectrum on a 0.5-cent log-frequency axis.
inline std::vector<double> logSpectrum (const std::vector<float>& x, double sr)
{
    constexpr int order = 16, N = 1 << order;
    Fft fft (order);
    std::vector<double> power (N / 2 + 1, 0.0);
    std::vector<std::complex<float>> buf (N);
    int frames = 0;
    for (size_t pos = 0; pos + N <= x.size(); pos += N / 2, ++frames)
    {
        for (int i = 0; i < N; ++i)
            buf[(size_t) i] = { x[pos + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N)), 0.0f };
        fft.forward (buf.data());
        for (int k = 0; k <= N / 2; ++k) power[(size_t) k] += std::norm (buf[(size_t) k]);
    }
    const double f0 = 500.0, stepC = 0.5;
    const int J = (int) (4800.0 / stepC);
    std::vector<double> L ((size_t) J);
    for (int j = 0; j < J; ++j)
    {
        const double f = f0 * std::pow (2.0, j * stepC / 1200.0);
        const double pos = f / (sr / N);
        const int k = (int) pos;
        const double a = pos - k;
        L[(size_t) j] = std::log (power[(size_t) k] * (1 - a) + power[(size_t) k + 1] * a + 1e-9);
    }
    // remove the broad envelope (100-cent moving average) so only partial structure remains
    std::vector<double> out ((size_t) J);
    const int w = 100;
    double run = 0; int cnt = 0;
    std::vector<double> csum ((size_t) J + 1, 0.0);
    for (int j = 0; j < J; ++j) csum[(size_t) j + 1] = csum[(size_t) j] + L[(size_t) j];
    for (int j = 0; j < J; ++j)
    {
        const int a = std::max (0, j - w), b = std::min (J, j + w + 1);
        out[(size_t) j] = L[(size_t) j] - (csum[(size_t) b] - csum[(size_t) a]) / (b - a);
    }
    (void) run; (void) cnt;
    return out;
}

// Shift (cents) such that B is A pitched up by that amount.
inline double correlate (const std::vector<double>& A, const std::vector<double>& B, double* peakOut = nullptr)
{
    const double stepC = 0.5;
    const int maxShift = (int) (150.0 / stepC);
    std::vector<double> c ((size_t) (2 * maxShift + 1));
    double best = -1e300; int bi = 0;
    for (int s = -maxShift; s <= maxShift; ++s)
    {
        double acc = 0.0, na = 0.0, nb = 0.0;
        const int j0 = std::max (0, -s), j1 = std::min ((int) A.size(), (int) B.size() - s);
        for (int j = j0; j < j1; ++j) { const double a = A[(size_t) j], b = B[(size_t) (j + s)]; acc += a * b; na += a * a; nb += b * b; }
        c[(size_t) (s + maxShift)] = acc / std::sqrt (na * nb + 1e-30);
        if (c[(size_t) (s + maxShift)] > best) { best = c[(size_t) (s + maxShift)]; bi = s + maxShift; }
    }
    const double y0 = c[(size_t) std::max (0, bi - 1)], y1 = c[(size_t) bi], y2 = c[(size_t) std::min ((int) c.size() - 1, bi + 1)];
    const double d = (y0 - 2 * y1 + y2) != 0.0 ? 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) : 0.0;
    return (bi - maxShift + d) * stepC;
}

