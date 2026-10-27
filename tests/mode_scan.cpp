// Developer diagnostic (not part of ctest): when a song's tonal evidence has two competing tunings, is the answer the same at every exact shift?
//   mode_scan <audio file> <maxSeconds> <shift,shift,...>        (shifts in cents; the audio is resampled by exactly that much, like centsible_shift)
// For every shift it prints, relative to the shift (so a stable answer is the same number on every line):
//   prod   what the production estimator reports (the plugin's number)
//   fitS   what fitSingle picks on the final pooled evidence
//   rich1  the best tuning under the richest template family (narrow + broad widths, searched over the whole circle), rich2 the best one >= 5 cents away
//   gap    log-likelihood advantage of rich1 over rich2 (raw nats; the estimator shrinks nats by 40 for correlated evidence, so gap / 40 is the effective size)
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <centsible/reading.h>
#include "fft.h"
#define private public
#include "spectral_likelihood.h"
#undef private
#include <cstdio>
#include <cstdlib>
#include <sstream>

using namespace centsible::detail;

namespace
{
double rel (double delta, double shift) { return circWrap (delta - shift); }

struct Modes { double d1 = 0, l1 = -1e300, d2 = 0, l2 = -1e300; };

// best and second best tuning (>= 5 cents apart) of one template over the whole circle
Modes topTwo (const Hist& H, const Template& t)
{
    std::vector<double> ll (kBins);
    int s1 = 0;
    for (int s = 0; s < kBins; ++s)
    {
        ll[(size_t) s] = logLik (H, t, s);
        if (ll[(size_t) s] > ll[(size_t) s1]) s1 = s;
    }
    int s2 = -1;
    for (int s = 0; s < kBins; ++s)
        if (binDist (s, s1) >= 32 && (s2 < 0 || ll[(size_t) s] > ll[(size_t) s2])) s2 = s;
    Modes m;
    m.d1 = polishDelta (H, t, refineDelta (H, t, s1));
    m.d2 = polishDelta (H, t, refineDelta (H, t, s2));
    m.l1 = exactLogLik (H, t, m.d1);
    m.l2 = exactLogLik (H, t, m.d2);
    return m;
}

// shares of the evidence of two tunings (and a uniform background) under one template: EM on the mixture weights
void shares (const Hist& H, const Template& t, double d1, double d2, double& p1, double& p2)
{
    const int s1 = ((int) std::lround ((d1 + 50.0) / 0.25 - 0.5) % kBins + kBins) % kBins, s2 = ((int) std::lround ((d2 + 50.0) / 0.25 - 0.5) % kBins + kBins) % kBins;
    double w1 = 0.45, w2 = 0.45, wu = 0.10;
    for (int it = 0; it < 60; ++it)
    {
        double c1 = 0, c2 = 0, cu = 0;
        for (int b = 0; b < kBins; ++b)
        {
            const double hb = (double) H[(size_t) b];
            if (hb <= 0.0) continue;
            const double a = w1 * t.dens[(size_t) (((b - s1) % kBins + kBins) % kBins)], c = w2 * t.dens[(size_t) (((b - s2) % kBins + kBins) % kBins)], u = wu / kBins;
            const double inv = hb / (a + c + u);
            c1 += inv * a; c2 += inv * c; cu += inv * u;
        }
        const double tot = c1 + c2 + cu;
        w1 = c1 / tot; w2 = c2 / tot; wu = cu / tot;
    }
    p1 = w1 / (w1 + w2);
    p2 = w2 / (w1 + w2);
}


// Bayesian average over the template family: P(s) = log sum_k exp (eff * ll_k (s)), ll_k the log-likelihood profile of template k (its own background share fitted).
// The tuning is the maximum of P: a smooth objective, so that a different template winning by a hair does not move the answer to another mode.
struct Marginal { double d1 = 0, d2 = 0, gap = 0; };

Marginal marginalPick (const Hist& H, double mass, double eff)
{
    std::vector<Template> tm;
    for (double sg : { 0.25, 0.4, 1.0, 2.0, 3.5, 5.0 })
        for (double al : { 0.6, 1.2, 2.0, 3.0, 4.5 }) { const Fit f = fitWith (H, mass, 0.0, sg, al); if (std::isfinite (f.loglik)) tm.push_back (f.tmpl); }
    for (double s1 : { 0.4, 0.7 })
        for (double s2 : { 7.0, 11.0 })
            for (double rho : { 0.55, 0.8 })
                for (double al : { 0.6, 2.0, 3.0 }) { const Fit f = fitWith (H, mass, 0.0, s1, al, -1, 1, s2, rho); if (std::isfinite (f.loglik)) tm.push_back (f.tmpl); }
    std::vector<std::vector<double>> ll (tm.size(), std::vector<double> (kBins));
    for (size_t k = 0; k < tm.size(); ++k)
        for (int s = 0; s < kBins; ++s) ll[k][(size_t) s] = eff * logLik (H, tm[k], s);
    std::vector<double> P (kBins);
    for (int s = 0; s < kBins; ++s)
    {
        double mx = -1e300;
        for (size_t k = 0; k < tm.size(); ++k) mx = std::max (mx, ll[k][(size_t) s]);
        double acc = 0;
        for (size_t k = 0; k < tm.size(); ++k) acc += std::exp (ll[k][(size_t) s] - mx);
        P[(size_t) s] = mx + std::log (acc);
    }
    int s1 = 0;
    for (int s = 1; s < kBins; ++s) if (P[(size_t) s] > P[(size_t) s1]) s1 = s;
    int s2 = -1;
    for (int s = 0; s < kBins; ++s) if (binDist (s, s1) >= 32 && (s2 < 0 || P[(size_t) s] > P[(size_t) s2])) s2 = s;
    auto refine = [&] (int s)
    {
        const double a = P[(size_t) ((s + kBins - 1) % kBins)], b = P[(size_t) s], c = P[(size_t) ((s + 1) % kBins)];
        const double den = a - 2 * b + c, p = den < -1e-12 ? 0.5 * (a - c) / den : 0.0;
        return circWrap (-50.0 + (s + 0.5 + std::clamp (p, -0.5, 0.5)) * kBinCents);
    };
    Marginal m;
    m.d1 = refine (s1);
    m.d2 = s2 >= 0 ? refine (s2) : 0.0;
    m.gap = s2 >= 0 ? P[(size_t) s1] - P[(size_t) s2] : 99.0;
    return m;
}

Fit richFit (const Hist& H, double mass)
{
    Fit best;
    for (double s1 : { 0.4, 0.7 })
        for (double s2 : { 7.0, 11.0 })
            for (double rho : { 0.55, 0.8 })
                for (double alpha : { 0.6, 2.0, 3.0 })
                {
                    const Fit f = fitWith (H, mass, 0.0, s1, alpha, -1, 1, s2, rho);
                    if (std::isfinite (f.loglik) && f.loglik > best.loglik) best = f;
                }
    return best;
}
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::printf ("usage: mode_scan <audio> <maxSeconds> <shift,shift,...>\n"); return 2; }
    double sr = 0.0;
    const auto x = loadMono (juce::File (juce::String::fromUTF8 (argv[1])), sr, std::atof (argv[2]));
    std::vector<double> shifts;
    {
        std::stringstream ss (argv[3]);
        std::string tok;
        while (std::getline (ss, tok, ',')) shifts.push_back (std::atof (tok.c_str()));
    }
    std::printf ("%s  (%.0f Hz, %.0f s)\n  shift   prod    fitS    rich1   rich2    gap(raw)  gap/40  share1 share2   f2: delta2 share2 gain/40\n", argv[1], sr, (double) x.size() / sr);
    for (double shift : shifts)
    {
        const auto y = shift == 0.0 ? x : resample (x, std::pow (2.0, shift / 1200.0));
        auto est = std::make_unique<SpectralLikelihoodEstimator>();
        est->prepare (sr);
        for (size_t pos = 0; pos < y.size(); pos += 512) est->push (y.data() + pos, (int) std::min<size_t> (512, y.size() - pos));
        Hist T, L;
        double mt, ml;
        est->sumTonal (T, mt, 0, true);
        est->sumLow (L, ml, true);
        const double prod = est->result.valid ? (double) est->result.cents : 0.0;
        const auto fs = fitSingle (T, mt);
        const auto rf = richFit (T, mt);
        const auto m = topTwo (T, rf.tmpl);
        double p1 = 0, p2 = 0;
        shares (T, rf.tmpl, m.d1, m.d2, p1, p2);
        // what the production second-tuning logic sees on top of fitSingle: the second tuning, its share of (first + second) and the gain in effective nats
        const auto f2 = fitSecond (T, fs);
        const auto mg = marginalPick (T, mt, 1.0 / 40.0);
        std::printf ("  %+5.0f  %+6.2f  %+6.2f  %+6.2f  %+6.2f  %8.1f  %6.2f  %5.2f  %5.2f   %s %+6.2f %5.2f %7.2f\n", shift, rel (prod, shift), rel (fs.delta, shift), rel (m.d1, shift), rel (m.d2, shift),
                    m.l1 - m.l2, (m.l1 - m.l2) / 40.0, p1, p2, f2.ok ? "f2" : "--", f2.ok ? rel (f2.delta2, shift) : 0.0, f2.ok ? f2.share2 : 0.0, f2.ok ? (f2.loglik - fs.loglik) / 40.0 : 0.0);
        std::printf ("%s", "");
        std::printf ("   marg %+6.2f %+6.2f %6.2f\n", rel (mg.d1, shift), rel (mg.d2, shift), mg.gap);
        std::fflush (stdout);
    }
    return 0;
}
