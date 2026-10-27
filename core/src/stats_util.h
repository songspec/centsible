// Small numeric helpers shared by the offline analyses (sections.cpp, tempo_automation.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace centsible::detail::util
{
inline constexpr double kPi = 3.14159265358979323846;

inline double wrap100 (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }            // onto the 100-cent circle, [-50, 50)
inline double wrapOctave (double c) { return c - 1200.0 * std::floor ((c + 600.0) / 1200.0); }     // onto one octave, [-600, 600)

inline double medianOf (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

inline double percentileOf (std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort (v.begin(), v.end());
    const double pos = p * (double) (v.size() - 1);
    const std::size_t i = (std::size_t) std::floor (pos);
    const double f = pos - (double) i;
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

inline std::vector<double> medianFilter (const std::vector<double>& v, int width)
{
    std::vector<double> out (v.size());
    const int h = width / 2, n = (int) v.size();
    for (int i = 0; i < n; ++i)
    {
        std::vector<double> w (v.begin() + std::max (0, i - h), v.begin() + std::min (n, i + h + 1));
        out[(size_t) i] = medianOf (w);
    }
    return out;
}

inline std::string fmt (const char* f, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0)
{
    char buf[256];
    std::snprintf (buf, sizeof buf, f, a, b, c, d);
    return buf;
}

// weighted circular statistics on the 100-cent circle (cents in, cents out)
struct Circular
{
    double mean = 0.0;     // [-50, 50)
    double R = 0.0;        // mean resultant length, 0 (spread over the circle) .. 1 (all at one point)
    double sd = 50.0;      // circular standard deviation in cents
};

inline Circular circularStats (const std::vector<double>& cents, const std::vector<double>& w)
{
    double c = 0.0, s = 0.0, sw = 0.0;
    for (std::size_t i = 0; i < cents.size(); ++i)
    {
        const double a = 2.0 * kPi * cents[i] / 100.0;
        c += w[i] * std::cos (a);
        s += w[i] * std::sin (a);
        sw += w[i];
    }
    Circular r;
    if (sw <= 0.0) return r;
    r.R = std::hypot (c, s) / sw;
    r.mean = wrap100 (std::atan2 (s, c) * 100.0 / (2.0 * kPi));
    r.sd = std::sqrt (-2.0 * std::log (std::max (1e-6, std::min (1.0, r.R)))) * 100.0 / (2.0 * kPi);
    return r;
}
} // namespace centsible::detail::util
