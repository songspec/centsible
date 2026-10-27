#pragma once
// Lets the harnesses try other fit choices without rebuilding (production never sets these):
//   CENTSIBLE_ALPHAS="1.2,2"   the roll-off exponents the fit tries       CENTSIBLE_SIGMAS="0.4,1,2"   the cluster widths
//   CENTSIBLE_MIXMARGIN=100    the log-likelihood margin of the narrow + broad template
#include "spectral_likelihood.h"
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

inline void applyFitEnv()
{
    auto list = [] (const char* name, std::array<double, 8>& dst, int& n)
    {
        const char* e = std::getenv (name);
        if (! e || ! *e) return;
        std::stringstream ss (e);
        std::string tok;
        std::vector<double> v;
        while (std::getline (ss, tok, ',')) v.push_back (std::atof (tok.c_str()));
        if (v.empty() || v.size() > 8) return;
        for (size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
        n = (int) v.size();
    };
    auto& k = centsible::detail::fitKnobs;
    list ("CENTSIBLE_ALPHAS", k.alphas, k.numAlphas);
    list ("CENTSIBLE_SIGMAS", k.sigmas, k.numSigmas);
    if (const char* m = std::getenv ("CENTSIBLE_MIXMARGIN")) k.mixMargin = std::atof (m);
    if (const char* a = std::getenv ("CENTSIBLE_ARBITRATE")) centsible::detail::SpectralLikelihoodEstimator::knobs.arbitrate = std::atoi (a) != 0;
}
