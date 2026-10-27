// Developer benchmark: the dense-mix class (scattered vocals + 808 + optional exact pad, drums, noise, reverb) with known tuning.
// Prints every case (error, interval) and the summary: mean / median / max error, share inside the reported interval, share within 0.5 cents.
//   bench_dense [seconds=90] [seeds=2]
#include "gen/synth.h"
#include <centsible/analyzer.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

using namespace synth;

int main (int argc, char** argv)
{
    const double SR = 48000.0, seconds = argc > 1 ? std::atof (argv[1]) : 90.0;
    const int seeds = argc > 2 ? std::atoi (argv[2]) : 2;
    struct Mix { const char* name; double scatter; float vocal, pad; };
    const Mix mixes[] = { { "vocal-heavy, quiet pad", 6.0, 0.8f, 0.5f }, { "vocal + 808 only", 6.0, 0.8f, 0.0f }, { "very loose vocal, quiet pad", 9.0, 0.9f, 0.4f } };
    const double offsets[] = { -37.3, -12.8, 8.6, 33.0, 49.7, 88.0 };
    struct Res { double err = 0, ci = 0, conf = 0; bool valid = false; };
    const int n = 3 * 6 * seeds;
    std::vector<Res> res ((size_t) n);
    std::atomic<int> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max (2u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back ([&]
        {
            for (int i; (i = next++) < n;)
            {
                const auto& m = mixes[i / (6 * seeds)];
                const double d = offsets[(i / seeds) % 6];
                const auto x = denseSong (SR, seconds, d, 100 + (unsigned) (i % seeds) * 17, m.scatter, m.vocal, m.pad);
                const auto r = centsible::analyzeBuffer (x.data(), x.size(), SR);
                res[(size_t) i] = { wrap50 (r.cents - d), r.ciCents, r.confidence, r.valid };
            }
        });
    for (auto& t : pool) t.join();

    std::vector<double> errs;
    int inside = 0, within05 = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto& m = mixes[i / (6 * seeds)];
        const auto& r = res[(size_t) i];
        std::printf ("%-28s %+6.1f  error %+7.3f  ci %5.2f  confidence %.2f%s\n", m.name, offsets[(i / seeds) % 6], r.err, r.ci, r.conf, r.valid ? "" : "  (no reading)");
        errs.push_back (std::fabs (r.err));
        inside += std::fabs (r.err) <= r.ci;
        within05 += std::fabs (r.err) <= 0.5;
    }
    std::sort (errs.begin(), errs.end());
    double mean = 0;
    for (double e : errs) mean += e;
    std::printf ("=== dense mixes: %d cases  mean %.3f  median %.3f  p90 %.3f  max %.3f  within 0.5: %d  inside own interval: %d\n", n, mean / n, errs[errs.size() / 2],
                 errs[(size_t) (errs.size() * 0.9)], errs.back(), within05, inside);
    return 0;
}
