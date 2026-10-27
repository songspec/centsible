// Throwaway diagnostic: dumps the tonal evidence histogram of a float32 mono WAV (as written by centsible_shift) and the fitted model.
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
#include <cstdio>
#include <vector>

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

    auto est = std::make_unique<centsible::detail::SpectralLikelihoodEstimator>();
    est->prepare ((double) rate);
    for (size_t pos = 0; pos < x.size(); pos += 512) est->push (x.data() + pos, (int) std::min<size_t> (512, x.size() - pos));

    centsible::detail::Hist T, L;
    double mt, ml;
    est->sumTonal (T, mt, 0, true);
    est->sumLow (L, ml, true);
    const auto fit = centsible::detail::fitSingle (T, mt);
    std::printf ("rate %u  tonal mass %.0f  low mass %.0f  fit delta %+.2f  sigma %.2f alpha %.2f stiff %.1e wUni %.2f\n", rate, mt, ml, fit.delta, fit.tmpl.sigma, fit.tmpl.alpha, fit.tmpl.stiff, fit.tmpl.wUni);

    // 1-cent smoothed histogram (4 bins), printed as share of the mass, with the fitted tuning marked
    std::printf ("cents  share%%(1-cent bins)\n");
    for (int c = 0; c < 100; ++c)
    {
        double v = 0.0;
        for (int b = c * 4; b < c * 4 + 4; ++b) v += T[(size_t) b];
        const double cents = -50.0 + c + 0.5;
        std::printf ("%+6.1f %5.2f ", cents, 100.0 * v / mt);
        const int bars = (int) std::lround (100.0 * v / mt * 8.0);
        for (int i = 0; i < std::min (bars, 70); ++i) std::putchar ('#');
        if (std::fabs (cents - fit.delta) < 0.5) std::printf ("  <== fitted tuning");
        std::putchar ('\n');
    }
    return 0;
}
