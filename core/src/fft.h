#pragma once
constexpr double kPi = 3.14159265358979323846;
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

// Minimal radix-2 complex FFT. inverse() is unscaled.
class Fft
{
public:
    explicit Fft (int order) : n (1 << order), bitrev ((size_t) n), tw ((size_t) n / 2)
    {
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < order; ++b)
                if (i & (1 << b)) r |= 1 << (order - 1 - b);
            bitrev[(size_t) i] = r;
        }
        for (int i = 0; i < n / 2; ++i)
        {
            const double a = -2.0 * kPi * i / n;
            tw[(size_t) i] = { (float) std::cos (a), (float) std::sin (a) };
        }
    }

    int size() const { return n; }
    void forward (std::complex<float>* d) const { run (d, false); }
    void inverse (std::complex<float>* d) const { run (d, true); }

private:
    void run (std::complex<float>* d, bool inv) const
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = bitrev[(size_t) i];
            if (j > i) std::swap (d[i], d[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    auto w = tw[(size_t) (j * step)];
                    if (inv) w = std::conj (w);
                    const auto u = d[i + j];
                    const auto v = d[i + j + half] * w;
                    d[i + j] = u + v;
                    d[i + j + half] = u - v;
                }
        }
    }

    int n;
    std::vector<int> bitrev;
    std::vector<std::complex<float>> tw;
};
