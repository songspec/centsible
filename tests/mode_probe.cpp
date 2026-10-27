// Throwaway diagnostic: when a song's tonal evidence has two competing tunings, which one does the fit pick, and how much better is it than the other?
//   mode_probe <float32 mono wav from centsible_shift> [seed cents ...]
// For every seed (a tuning in cents) it fits the best template (width, roll-off, two-width mixtures) with the shift confined to +-3 cents of the seed, and prints
// the log-likelihood, next to what the production fitSingle picks. If fitSingle picks the lower of two, the search missed; if the two are close, it is a tie.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>
#include <centsible/reading.h>
#include "fft.h"
#define private public
#include "spectral_likelihood.h"
#undef private

using namespace centsible::detail;

namespace
{
int binOf (double cents) { return (int) std::lround ((cents + 50.0) / 0.25 - 0.5); }

struct Best { double ll = -1e300, delta = 0, sigma = 0, alpha = 0, sigma2 = 0, rho = 0, wUni = 0; };

Best bestNear (const Hist& H, double mass, double seedCents)
{
    Best b;
    const int seed = ((binOf (seedCents) % kBins) + kBins) % kBins;
    auto take = [&] (const Fit& f, double s2, double rho)
    {
        if (std::isfinite (f.loglik) && f.loglik > b.ll)
        {
            b.ll = f.loglik; b.delta = f.delta; b.sigma = f.tmpl.sigma; b.alpha = f.tmpl.alpha; b.sigma2 = s2; b.rho = rho; b.wUni = f.tmpl.wUni;
        }
    };
    for (double sigma : { 0.25, 0.4, 0.7, 1.0, 1.5, 2.0, 3.5, 5.0 })
        for (double alpha : { 0.6, 1.2, 2.0, 3.0, 4.5 })
            take (fitWith (H, mass, 0.0, sigma, alpha, seed), 0.0, 0.0);
    for (double s1 : { 0.25, 0.4, 0.7, 1.2 })
        for (double s2 : { 4.0, 7.0, 11.0 })
            for (double rho : { 0.3, 0.55, 0.8 })
                for (double alpha : { 0.6, 1.2, 2.0, 3.0 })
                    take (fitWith (H, mass, 0.0, s1, alpha, seed, 1, s2, rho), s2, rho);
    return b;
}
}

int main (int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* f = std::fopen (argv[1], "rb");
    if (! f) return 3;
    std::fseek (f, 0, SEEK_END);
    const long size = std::ftell (f);
    std::fseek (f, 24, SEEK_SET);
    unsigned rate = 0;
    std::fread (&rate, 4, 1, f);
    std::fseek (f, 56, SEEK_SET);
    std::vector<float> x ((size_t) (size - 56) / 4);
    std::fread (x.data(), 4, x.size(), f);
    std::fclose (f);

    auto est = std::make_unique<SpectralLikelihoodEstimator>();
    est->prepare ((double) rate);
    for (size_t pos = 0; pos < x.size(); pos += 512) est->push (x.data() + pos, (int) std::min<size_t> (512, x.size() - pos));

    Hist T, L;
    double mt, ml;
    est->sumTonal (T, mt, 0, true);
    est->sumLow (L, ml, true);
    const auto fit = fitSingle (T, mt);
    std::printf ("%s\n  production fitSingle: delta %+.2f  loglik %.1f  sigma %.2f alpha %.2f sigma2 %.1f rho %.2f wUni %.2f  (mass %.0f)\n", argv[1], fit.delta, fit.loglik, fit.tmpl.sigma, fit.tmpl.alpha,
                fit.tmpl.sigma2, fit.tmpl.rho, fit.tmpl.wUni, mt);
    for (int a = 2; a < argc; ++a)
    {
        const double seed = std::atof (argv[a]);
        const auto b = bestNear (T, mt, seed);
        std::printf ("  seed %+6.1f: best loglik %.1f at %+.2f  (sigma %.2f alpha %.2f sigma2 %.1f rho %.2f wUni %.2f)   vs production %+.1f\n", seed, b.ll, b.delta, b.sigma, b.alpha, b.sigma2, b.rho, b.wUni,
                    b.ll - fit.loglik);
    }
    return 0;
}
