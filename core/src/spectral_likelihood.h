#pragma once
// Candidate B: model-based tuning estimator.
//
// Evidence: reassigned (phase-vocoder) frequencies of spectral peaks of the HARMONIC part of the signal (median-filter HPSS), each deposited with a
// SOFT weight (amplitude within its octave band x harmonic mask x prominence x frequency steadiness x amplitude steadiness) into a histogram of its
// offset from the 12-TET grid (cents mod 100), one histogram per register band and per 5-second chunk.
//
// Model: the folded evidence of music tuned at offset delta is a mixture of the notes' fundamentals/octave partials (at delta) and the other
// partials at their KNOWN offsets (+1.96, -13.69, -31.17, +3.91, -48.68 ... cents, stretched by a fitted string stiffness) plus a uniform
// background. The tuning is the maximum-likelihood delta on the circle: harmonic bias is modelled, not filtered.
// Confidence: block bootstrap over chunks. Second tuning: mixture comparison. Song change: likelihood ratio on recent chunks.
#include <centsible/reading.h>
#include "fft.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace centsible::detail
{
inline constexpr int kBins = 400;                 // 0.25 cent per bin; bin centres are the delta grid
inline constexpr double kBinCents = 100.0 / kBins;
inline constexpr int kBands = 3;                  // 0: < 150 Hz   1: 150 - 1500 Hz   2: 1500 - 6000 Hz
inline constexpr int kMaxChunks = 64;
inline constexpr int kHopDiv = 4;                 // frames overlap by (kHopDiv-1)/kHopDiv: hop = window / kHopDiv
inline constexpr int kFrames = 2 * kHopDiv + 1;   // spectral history (about 0.77 s at any hop); the centre frame is the newest minus kHopDiv
inline constexpr int kCentre = kHopDiv;
inline constexpr int kMaxComps = 24;
inline constexpr int kChunkFrames = 59 * kHopDiv / 4;   // ~5 s
// Model constants.
inline constexpr double gStiffPenalty = 15.0;               // log-likelihood a stretched-partial model must gain before it is used (absolute floor)
inline constexpr double gStiffRel = 0.08;                   // ... plus this much per unit of evidence mass (the gain of a spurious extra parameter grows with the data)
inline constexpr double gBackgroundCap = 0.7;
inline constexpr int gHMax = 24;                          // highest partial the template models

using Hist = std::array<float, kBins>;

inline double circWrap (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }
inline int binDist (int a, int b) { int d = std::abs (a - b) % kBins; return std::min (d, kBins - d); }

// ---- the circle model ------------------------------------------------------------------------------------------------------
// One note's evidence: partial h sits at offset o_h = 1200*log2(h * sqrt((1+B h^2)/(1+B))) mod 100 from the note's own grid position, with weight
// h^-alpha (a smooth roll-off, ONE parameter instead of one free weight per partial: with free weights a note's fundamental and its 3rd partial,
// only 1.96 cents apart, cannot be told apart and the fit drifts towards their centroid). Every cluster has width sigma (intonation scatter,
// fitted: sharp data gets a narrow kernel, real music a broad one). A uniform background absorbs the rest.
struct Template
{
    int n = 0;
    std::array<double, kMaxComps> off {}, wn {};   // cluster offsets and their normalised weights (sum = 1)
    double wUni = 0.1, stiff = 0.0, sigma = 2.0, alpha = 1.2;
    double sigma2 = 0.0, rho = 0.0;   // optional broad component: every cluster is (1-rho) N(sigma) + rho N(sigma2): a few precise instruments inside loose ones
    int hMin = 1;           // first partial the notes show in this register (a bass whose fundamentals lie below the band only shows partial 3 or 4 and up)
    // signal: all clusters as ONE density over relative bins (origin = tuning). It does not depend on the background share, so changing the
    // background (EM) only re-mixes it; keeping one table instead of one per cluster keeps a Fit at ~10 kB (fits live on the analysis thread's stack).
    std::array<double, kBins> signal {}, dens {}, logd {};

    double w (int j) const { return (1.0 - wUni) * wn[(size_t) j]; }   // weight of cluster j in the mixture

    void setup (double B, double sigma_, double alpha_, double wUni_, int hMin_ = 1, double sigma2_ = 0.0, double rho_ = 0.0)
    {
        stiff = B; sigma = sigma_; alpha = alpha_; wUni = wUni_; hMin = hMin_; sigma2 = sigma2_; rho = rho_;
        std::array<double, kMaxComps> acc {};
        n = 0;
        for (int h = hMin; h <= gHMax; ++h)
        {
            const double o = circWrap (1200.0 * std::log2 (h * std::sqrt ((1.0 + B * h * h) / (1.0 + B))));
            int found = -1;
            for (int j = 0; j < n; ++j) if (std::fabs (off[(size_t) j] - o) < 0.05) { found = j; break; }
            if (found < 0) { found = n++; off[(size_t) found] = o; acc[(size_t) found] = 0.0; }
            acc[(size_t) found] += std::pow ((double) h, -alpha);
        }
        double sum = 0;
        for (int j = 0; j < n; ++j) sum += acc[(size_t) j];
        for (int j = 0; j < n; ++j) wn[(size_t) j] = acc[(size_t) j] / sum;
        const double norm = kBinCents / (sigma * std::sqrt (2.0 * 3.14159265358979323846));
        const double norm2 = rho > 0.0 ? kBinCents / (sigma2 * std::sqrt (2.0 * 3.14159265358979323846)) : 0.0;
        for (int x = 0; x < kBins; ++x)
        {
            double v = 0.0;
            for (int j = 0; j < n; ++j)
            {
                const double d = circWrap (x * kBinCents - off[(size_t) j]);
                double g = (1.0 - rho) * norm * std::exp (-0.5 * d * d / (sigma * sigma));
                if (rho > 0.0) g += rho * norm2 * std::exp (-0.5 * d * d / (sigma2 * sigma2));
                v += wn[(size_t) j] * g;
            }
            signal[(size_t) x] = v;
        }
        build();
    }

    void setUniform (double u)
    {
        wUni = u;
        build();
    }

    void build()
    {
        for (int x = 0; x < kBins; ++x)
        {
            const double d = wUni / kBins + (1.0 - wUni) * signal[(size_t) x];
            dens[(size_t) x] = d;
            logd[(size_t) x] = std::log (d);
        }
    }
};

struct Fit
{
    double delta = 0.0;     // cents, wrapped to [-50, 50)
    double loglik = -1e300;
    double mass = 0.0;
    int shift = 0;          // grid index of delta
    Template tmpl;
};

inline double logLik (const Hist& H, const Template& t, int s)
{
    double ll = 0.0;
    for (int b = 0; b < kBins; ++b)
        if (H[(size_t) b] > 0.0f) ll += (double) H[(size_t) b] * t.logd[(size_t) (((b - s) % kBins + kBins) % kBins)];
    return ll;
}

inline int scan (const Hist& H, const Template& t, int from, int to, int step, double* bestLL = nullptr)
{
    int best = from;
    double bl = -1e300;
    for (int s = from; s <= to; s += step)
    {
        const int sw = ((s % kBins) + kBins) % kBins;
        const double ll = logLik (H, t, sw);
        if (ll > bl) { bl = ll; best = sw; }
    }
    if (bestLL) *bestLL = bl;
    return best;
}

// EM for the background share only (everything else is fixed by the physics of harmonic sounds)
inline void fitBackground (const Hist& H, Template& t, int s, double mass, int iters)
{
    for (int it = 0; it < iters; ++it)
    {
        double cu = 0.0;
        for (int b = 0; b < kBins; ++b)
        {
            const float hb = H[(size_t) b];
            if (hb <= 0.0f) continue;
            cu += (double) hb * (t.wUni / kBins) / t.dens[(size_t) (((b - s) % kBins + kBins) % kBins)];
        }
        t.setUniform (std::clamp (cu / std::max (1e-9, mass), 0.01, gBackgroundCap));
    }
}

inline double refineDelta (const Hist& H, const Template& t, int s)
{
    const double l0 = logLik (H, t, (s - 1 + kBins) % kBins), l1 = logLik (H, t, s), l2 = logLik (H, t, (s + 1) % kBins);
    const double den = l0 - 2.0 * l1 + l2;
    const double p = den < -1e-12 ? 0.5 * (l0 - l2) / den : 0.0;
    return circWrap (-50.0 + (s + 0.5 + std::clamp (p, -0.5, 0.5)) * kBinCents);
}

// Exact log-likelihood at a continuous shift: the grid scan is quantised to a bin (0.25 cents) and its 3-point parabola is crude for the narrow
// clusters (sigma ~0.4 cents) that clean material produces, so the winning fit is polished on the analytic mixture density instead.
inline double exactLogLik (const Hist& H, const Template& t, double delta)
{
    const double norm = kBinCents / (t.sigma * std::sqrt (2.0 * 3.14159265358979323846)), inv2s2 = 0.5 / (t.sigma * t.sigma);
    const double norm2 = t.rho > 0.0 ? kBinCents / (t.sigma2 * std::sqrt (2.0 * 3.14159265358979323846)) : 0.0, inv2s22 = t.rho > 0.0 ? 0.5 / (t.sigma2 * t.sigma2) : 0.0;
    const double reach = 8.0 * (t.rho > 0.0 ? t.sigma2 : t.sigma);
    double ll = 0.0;
    for (int b = 0; b < kBins; ++b)
    {
        const double hb = (double) H[(size_t) b];
        if (hb <= 0.0) continue;
        const double pos = -50.0 + (b + 0.5) * kBinCents;
        double d = t.wUni / kBins;
        for (int j = 0; j < t.n; ++j)
        {
            const double dd = circWrap (pos - delta - t.off[(size_t) j]);
            if (std::fabs (dd) >= reach) continue;
            double g = (1.0 - t.rho) * norm * std::exp (-dd * dd * inv2s2);
            if (t.rho > 0.0) g += t.rho * norm2 * std::exp (-dd * dd * inv2s22);
            d += t.w (j) * g;
        }
        ll += hb * std::log (d);
    }
    return ll;
}

inline double polishDelta (const Hist& H, const Template& t, double delta0)
{
    constexpr double golden = 0.6180339887498949;
    double lo = delta0 - 0.35, hi = delta0 + 0.35;
    double x1 = hi - golden * (hi - lo), x2 = lo + golden * (hi - lo);
    double f1 = exactLogLik (H, t, x1), f2 = exactLogLik (H, t, x2);
    for (int it = 0; it < 16; ++it)
    {
        if (f1 > f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - golden * (hi - lo); f1 = exactLogLik (H, t, x1); }
        else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + golden * (hi - lo); f2 = exactLogLik (H, t, x2); }
    }
    const double best = 0.5 * (lo + hi);
    return exactLogLik (H, t, best) >= exactLogLik (H, t, delta0) ? circWrap (best) : delta0;
}

inline Fit fitWith (const Hist& H, double mass, double B, double sigma, double alpha, int seedShift = -1, int hMin = 1, double sigma2 = 0.0, double rho = 0.0)
{
    Fit f;
    f.mass = mass;
    f.tmpl.setup (B, sigma, alpha, 0.1, hMin, sigma2, rho);
    int s = seedShift >= 0 ? scan (H, f.tmpl, seedShift - 12, seedShift + 12, 1) : scan (H, f.tmpl, 0, kBins - 1, 4);
    if (seedShift < 0) s = scan (H, f.tmpl, s - 6, s + 6, 1);
    fitBackground (H, f.tmpl, s, mass, 6);
    s = scan (H, f.tmpl, s - 4, s + 4, 1);
    f.shift = s;
    f.loglik = logLik (H, f.tmpl, s);
    f.delta = refineDelta (H, f.tmpl, s);
    return f;
}

// Fits tuning, cluster width, roll-off and (only if it clearly pays for itself) string stiffness.
// allowStiffness = false: the unstretched template only. The upper partials alone are a comb on the circle, and with a free stiffness that comb has other
// explanations half a semitone away; a caller that has no low register to cross-check against (a window of a few seconds) can leave the stiffness out.
// seedShift >= 0 (a grid index): confine every candidate to the tuning near it, to find the template that suits THAT tuning (no stiffness then).
// The search grid and the margin of the two-width template (exposed so that the harnesses can try other choices; production leaves the defaults).
struct FitKnobs
{
    std::array<double, 8> sigmas { 0.25, 0.4, 1.0, 2.0, 3.5, 5.0 };
    std::array<double, 8> alphas { 0.6, 1.2, 2.0, 3.0, 4.5 };   // (a set of 1.2 and 2 alone made the whole-catalog shift test 0.6 points better but produced more confident-wrong songs: 34 vs 25 of 3670; not adopted)
    int numSigmas = 6, numAlphas = 5;
    double mixMargin = 100.0;      // log-likelihood a narrow + broad template must gain over the best single width before it is used (2 extra parameters)
};
inline FitKnobs fitKnobs;

inline Fit fitSingle (const Hist& H, double mass, bool allowStiffness = true, int seedShift = -1)
{
    Fit best;
    for (int si = 0; si < fitKnobs.numSigmas; ++si)
        for (int ai = 0; ai < fitKnobs.numAlphas; ++ai)
        {
            Fit f = fitWith (H, mass, 0.0, fitKnobs.sigmas[(size_t) si], fitKnobs.alphas[(size_t) ai], seedShift);
            if (std::isfinite (f.loglik) && f.loglik > best.loglik) best = f;
        }
    // Two widths: a few precise instruments (synths, 808s, piano) inside loose ones (scattered vocals). One width must compromise between them; a
    // narrow + broad mixture lets the precise part pin the tuning. Adopted only if it beats the single width by a margin (2 extra parameters).
    {
        const int seed = best.shift;
        const double alpha1 = best.tmpl.alpha, margin = fitKnobs.mixMargin;
        double bestLL = best.loglik + margin;
        Fit mixBest;
        bool found = false;
        for (double s1 : { 0.25, 0.4, 0.7, 1.2 })
            for (double s2 : { 4.0, 7.0, 11.0 })
                for (double rho : { 0.3, 0.55, 0.8 })
                {
                    Fit f = fitWith (H, mass, 0.0, s1, alpha1, seed, 1, s2, rho);
                    if (std::isfinite (f.loglik) && f.loglik > bestLL) { bestLL = f.loglik; mixBest = f; found = true; }
                }
        if (found) best = mixBest;
    }

    if (! allowStiffness || seedShift >= 0) return best;

    // Stiffness: the profile likelihood over B is bumpy at narrow cluster widths, so B is found at a wide, smooth width first and then
    // tracked while the width narrows (graduated search). Every candidate is scanned over the whole circle: a stretched string drags the
    // unstretched fit several cents off, so a local seed would miss it. A stretched model is only adopted if it beats the unstretched
    // one by an evidence-proportional margin (a spurious extra parameter gains more with more data).
    const double pen = gStiffPenalty + gStiffRel * mass;
    Fit cur, tmp;
    int hMinUse = 1;
    auto climb = [&] (double B, double sigma, double alpha, double step)
    {
        cur = fitWith (H, mass, B, sigma, alpha, -1, hMinUse);
        for (int it = 0; it < 30 && step > 1.004; ++it)
        {
            bool moved = false;
            tmp = fitWith (H, mass, cur.tmpl.stiff * step, sigma, alpha, -1, hMinUse);
            if (tmp.loglik > cur.loglik) { cur = tmp; moved = true; }
            tmp = fitWith (H, mass, cur.tmpl.stiff / step, sigma, alpha, -1, hMinUse);
            if (tmp.loglik > cur.loglik) { cur = tmp; moved = true; }
            if (! moved) step = std::sqrt (step);
        }
    };
    auto adopt = [&]
    {
        cur.loglik -= pen;
        if (std::isfinite (cur.loglik) && cur.loglik > best.loglik) best = cur;
    };
    {
        double bestB = 0.0, bestLL = -1e300;
        for (int hMin : { 1, 3 })
            for (double B = 1.0e-4; B < 3.2e-3; B *= 1.25)
            {
                const Fit f = fitWith (H, mass, B, 3.0, 1.2, -1, hMin);
                if (std::isfinite (f.loglik) && f.loglik > bestLL) { bestLL = f.loglik; bestB = B; hMinUse = hMin; }
            }
        if (bestB > 0.0)
        {
            double B = bestB;
            for (double sigma : { 3.0, 2.0, 1.0, 0.6, 0.4, 0.25 })
            {
                climb (B, sigma, 1.2, 1.2);
                B = cur.tmpl.stiff;
                adopt();
            }
            for (double alpha : { 0.6, 2.0 })
                for (double sigma : { 3.0, 1.0 })
                {
                    climb (B, sigma, alpha, 1.15);
                    adopt();
                }
        }
    }
    return best;
}

// two-tuning mixture on top of a single fit (same template shape for both tunings)
// Fits live on the analysis thread's stack (several at a time): keep them small.
static_assert (sizeof (Fit) < 16 * 1024, "Fit must stay small enough for a modest thread stack");

struct Fit2 { double delta2 = 0.0, share2 = 0.0, loglik = -1e300; bool ok = false; };

inline Fit2 fitSecond (const Hist& H, const Fit& f1)
{
    Fit2 r;
    const Template& t = f1.tmpl;
    // residual evidence once tuning 1 has explained what it can
    Hist R {};
    double rm = 0.0;
    for (int b = 0; b < kBins; ++b)
    {
        const int x = ((b - f1.shift) % kBins + kBins) % kBins;
        const double expect = f1.mass * 0.94 * t.dens[(size_t) x];
        R[(size_t) b] = (float) std::max (0.0, (double) H[(size_t) b] - expect);
        rm += R[(size_t) b];
    }
    if (rm < 0.04 * f1.mass) return r;
    // candidate second tuning: best template placement on the residual, at least 5 cents from the first
    int best = -1;
    double bl = -1e300;
    for (int s = 0; s < kBins; s += 2)
    {
        if (binDist (s, f1.shift) < 20) continue;
        const double ll = logLik (R, t, s);
        if (ll > bl) { bl = ll; best = s; }
    }
    if (best < 0) return r;
    int s2 = scan (R, t, best - 8, best + 8, 1);
    // EM on the mixture weights (pi1, pi2, uniform), delta2 refined once
    double pi1 = 0.8, pi2 = 0.15, piu = 0.05;
    for (int refine = 0; refine < 2; ++refine)
    {
        for (int it = 0; it < 25; ++it)
        {
            double c1 = 0, c2 = 0, cu = 0;
            for (int b = 0; b < kBins; ++b)
            {
                const float hb = H[(size_t) b];
                if (hb <= 0.0f) continue;
                const double d1 = pi1 * t.dens[(size_t) (((b - f1.shift) % kBins + kBins) % kBins)];
                const double d2 = pi2 * t.dens[(size_t) (((b - s2) % kBins + kBins) % kBins)];
                const double du = piu / kBins;
                const double inv = (double) hb / (d1 + d2 + du);
                c1 += inv * d1; c2 += inv * d2; cu += inv * du;
            }
            const double tot = c1 + c2 + cu;
            pi1 = c1 / tot; pi2 = c2 / tot; piu = cu / tot;
        }
        if (refine == 0)
        {
            double bestLL = -1e300; int bs = s2;
            for (int s = s2 - 12; s <= s2 + 12; ++s)
            {
                double ll = 0;
                for (int b = 0; b < kBins; ++b)
                    if (H[(size_t) b] > 0.0f)
                        ll += (double) H[(size_t) b] * std::log (pi1 * t.dens[(size_t) (((b - f1.shift) % kBins + kBins) % kBins)] + pi2 * t.dens[(size_t) ((((b - s) % kBins) + kBins) % kBins)] + piu / kBins);
                if (ll > bestLL) { bestLL = ll; bs = (s % kBins + kBins) % kBins; }
            }
            s2 = bs;
        }
    }
    double ll = 0;
    for (int b = 0; b < kBins; ++b)
        if (H[(size_t) b] > 0.0f)
            ll += (double) H[(size_t) b] * std::log (pi1 * t.dens[(size_t) (((b - f1.shift) % kBins + kBins) % kBins)] + pi2 * t.dens[(size_t) (((b - s2) % kBins + kBins) % kBins)] + piu / kBins);
    r.delta2 = refineDelta (R, t, s2);
    r.share2 = pi2 / std::max (1e-9, pi1 + pi2);
    r.loglik = ll;
    r.ok = true;
    return r;
}

// ---- competing tunings -----------------------------------------------------------------------------------------------------
// A song can hold two groups of notes a few cents apart (a sampled loop and the playing around it, a pitched vocal over an untouched beat). Under the template the
// single fit chose, the two are nearly tied, so WHICH of them the fit picks can change with nothing but a pitch shift of the whole file (measured on the user's
// catalog: two songs of 104 flipped by 13 - 15 cents under exact shifts). The richest template family (a narrow and a broad width in every note, searched over the
// whole circle) separates the two much better, and its answer does not move with the shift. checkModes returns that answer: the best and the second best tuning
// (at least 5 cents apart), how much better the first is, and how much of the evidence each carries.
struct ModeCheck
{
    bool ok = false;
    double delta1 = 0.0, delta2 = 0.0;   // the best and the second best tuning under the rich template
    double gap = 0.0;                    // log-likelihood advantage of delta1 over delta2 (raw nats; the effective size is gap * effScale)
    double share1 = 0.0, share2 = 0.0;   // their shares of the evidence of the two (sum to 1)
};

inline ModeCheck checkModes (const Hist& H, double mass)
{
    ModeCheck m;
    Fit best;
    for (double s1 : { 0.4, 0.7 })
        for (double s2 : { 7.0, 11.0 })
            for (double rho : { 0.55, 0.8 })
                for (double alpha : { 0.6, 2.0, 3.0 })
                {
                    const Fit f = fitWith (H, mass, 0.0, s1, alpha, -1, 1, s2, rho);
                    if (std::isfinite (f.loglik) && f.loglik > best.loglik) best = f;
                }
    if (! std::isfinite (best.loglik)) return m;
    const Template& t = best.tmpl;

    // the profile over the whole circle at the winning template: the best tuning and the best one at least 5 cents away
    int s1 = 0;
    std::array<double, kBins> ll {};
    for (int s = 0; s < kBins; ++s)
    {
        ll[(size_t) s] = logLik (H, t, s);
        if (ll[(size_t) s] > ll[(size_t) s1]) s1 = s;
    }
    int s2 = -1;
    for (int s = 0; s < kBins; ++s)
        if (binDist (s, s1) >= 20 && (s2 < 0 || ll[(size_t) s] > ll[(size_t) s2])) s2 = s;
    if (s2 < 0) return m;
    m.delta1 = polishDelta (H, t, refineDelta (H, t, s1));
    m.delta2 = polishDelta (H, t, refineDelta (H, t, s2));
    m.gap = exactLogLik (H, t, m.delta1) - exactLogLik (H, t, m.delta2);

    // shares: EM on the weights of the two tunings and a uniform background, all under the same template
    double w1 = 0.45, w2 = 0.45, wu = 0.10;
    for (int it = 0; it < 60; ++it)
    {
        double c1 = 0.0, c2 = 0.0, cu = 0.0;
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
    m.share1 = w1 / (w1 + w2);
    m.share2 = w2 / (w1 + w2);
    m.ok = true;
    return m;
}

// ---- the streaming estimator -----------------------------------------------------------------------------------------------
class SpectralLikelihoodEstimator
{
public:
    // tuning of the statistics (exposed so the harnesses can sweep them; production leaves the defaults)
    struct Knobs
    {
        double effScale = 1.0 / 40.0;     // effective-sample shrink: deposits from one held note are strongly correlated
        double secondDelta = 14.0;        // log-likelihood improvement (after shrinking) needed to report a second tuning
        double secondShare = 0.12;
        double changeDelta = 30.0;        // likelihood ratio needed to declare a new song
        double minMass = 150.0;           // evidence needed before any reading is valid
        bool arbitrate = false;           // when the single fit and the richest template family disagree about the tuning, say so (checkModes): not locked, the interval covers both
        double competeApart = 3.0;        // cents: how far apart the two answers must be to disagree
    };
    static inline Knobs knobs {};

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        order = sr > 150000.0 ? 16 : sr > 50000.0 ? 15 : 14;   // keeps the window near 340 ms at any host rate
        n = 1 << order;
        hop = n / kHopDiv;
        fft = std::make_unique<Fft> (order);
        binHz = sr / n;
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double x = 2.0 * 3.14159265358979323846 * i / n;
            window[(size_t) i] = (float) (0.35875 - 0.48829 * std::cos (x) + 0.14128 * std::cos (2 * x) - 0.01168 * std::cos (3 * x));
        }
        ring.assign ((size_t) n, 0.0f);
        buf.assign ((size_t) n, {});
        cands.reserve (8192);
        const size_t half = (size_t) n / 2 + 1;
        for (auto& s : spec) s.assign (half, {});
        for (auto& m : mags) m.assign (half, 0.0f);
        reset();
    }

    void reset()
    {
        hopCount = 0;
        timeline.clear();
        std::fill (ring.begin(), ring.end(), 0.0f);
        writePos = sinceHop = 0;
        framesSeen = 0;
        head = 0;
        chunkCount = chunkHead = 0;
        for (auto& c : chunks) for (auto& h : c) h.fill (0.0f);
        for (auto& h : cur) h.fill (0.0f);
        curFrames = 0;
        silentFrames = 0;
        hint = false;
        songChanges = 0;
        audioSeconds = 0.0;
        nextHistoryAt = 1.0;
        historyN = 0;
        framesSinceEstimate = 0;
        result = {};
        recentCentsN = 0;
    }

    void hintBoundary() { hint = true; }

    // Offline analysis of how the tuning moves over a song: record the tonal evidence of every analysed frame (never used by the plugin's realtime path).
    struct TimelineFrame { double t; std::array<float, kBins> hist; std::array<float, kBins> low; };    // tonal evidence (150 Hz - 6 kHz) and the low register (< 150 Hz)
    // timelineOnly: skip the chunked estimation (the frame evidence is all the caller wants), which makes recording several times faster
    void enableTimeline (bool on, bool timelineOnly_ = false) { recordTimeline = on; timelineOnly = on && timelineOnly_; timeline.clear(); }
    const std::vector<TimelineFrame>& timelineFrames() const { return timeline; }

    void push (const float* x, int num)
    {
        for (int i = 0; i < num; ++i)
        {
            // a broken upstream plugin can emit NaN / Inf: one such sample would poison the evidence for good, so it is replaced by silence
            const float v = x[i];
            ring[(size_t) writePos] = std::isfinite (v) ? v : 0.0f;
            writePos = (writePos + 1) % n;
            if (++sinceHop >= hop) { sinceHop = 0; processHop(); }
        }
    }

    const centsible::Reading& reading() const { return result; }

private:
    // ---- per hop -----------------------------------------------------------------------------------------------------------
    void processHop()
    {
        ++hopCount;
        double e = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float s = ring[(size_t) ((writePos + i) % n)];
            e += (double) s * s;
            buf[(size_t) i] = { s * window[(size_t) i], 0.0f };
        }
        if (std::sqrt (e / n) < 1.0e-4) { ++silentFrames; framesSeen = 0; return; }
        if ((double) silentFrames * hop / sr >= 2.0) hint = true;
        silentFrames = 0;

        fft->forward (buf.data());
        const int half = n / 2;
        auto& sp = spec[(size_t) head];
        auto& mg = mags[(size_t) head];
        for (int k = 0; k <= half; ++k) { sp[(size_t) k] = buf[(size_t) k]; mg[(size_t) k] = std::abs (buf[(size_t) k]); }
        head = (head + 1) % kFrames;
        if (++framesSeen < kFrames) return;
        processCentre();
        audioSeconds += (double) hop / sr;
        if (timelineOnly) return;
        if (++curFrames >= kChunkFrames) finishChunk();
        else if (++framesSinceEstimate >= 24 * kHopDiv / 4) estimate (false);
    }

    int frameIdx (int offsetFromCentre) const { return ((head - 1 - kCentre + offsetFromCentre) % kFrames + kFrames) % kFrames; }

    void processCentre()
    {
        const int c = frameIdx (0), cp = frameIdx (-1), cn = frameIdx (1);
        const auto& m0 = mags[(size_t) c];
        const int half = n / 2;
        const int kLo = std::max (12, (int) std::ceil (28.0 / binHz)), kHi = std::min (half - 12, (int) (6000.0 / binHz));
        const double dt = (double) hop / sr;

        // candidate peaks
        cands.clear();
        float gmax = 0.0f;
        for (int k = kLo; k <= kHi; ++k) gmax = std::max (gmax, m0[(size_t) k]);
        if (gmax <= 0.0f) return;
        std::array<float, 10> bandMax {};
        auto bandOf = [&] (int k) { return std::clamp ((int) std::floor (std::log2 (std::max (1.0, k * binHz / 32.0))), 0, 9); };
        for (int k = kLo; k <= kHi; ++k)
        {
            const float m = m0[(size_t) k];
            if (m < 0.002f * gmax) continue;
            if (m < m0[(size_t) k - 1] || m < m0[(size_t) k + 1] || m < m0[(size_t) k - 2] || m < m0[(size_t) k + 2]) continue;
            cands.push_back ({ k, m });
            bandMax[(size_t) bandOf (k)] = std::max (bandMax[(size_t) bandOf (k)], m);
        }

        std::array<float, kBins> frameAcc {}, frameLow {};
        std::array<float, 17> fw {};
        std::array<float, kFrames> tw {};
        vizCount = 0;
        for (const auto& cd : cands)
        {
            const int k = cd.k;
            const float m = cd.m;
            // sinusoidal prominence
            const float nb = 0.5f * (m0[(size_t) k - 8] + m0[(size_t) k + 8]) + 1e-20f;
            const double pf = std::clamp ((std::log2 ((double) m / nb) - 1.0) / 3.0, 0.0, 1.0);
            if (pf <= 0.0) continue;

            // harmonic mask (median filtering in time vs. in frequency)
            for (int t = 0; t < kFrames; ++t) tw[(size_t) t] = mags[(size_t) frameIdx (t - kCentre)][(size_t) k];
            std::nth_element (tw.begin(), tw.begin() + kFrames / 2, tw.end());
            const double Hm = tw[(size_t) kFrames / 2];
            for (int j = -8; j <= 8; ++j) fw[(size_t) (j + 8)] = m0[(size_t) (k + j)];
            std::nth_element (fw.begin(), fw.begin() + 8, fw.end());
            const double Pm = fw[8];
            const double mask = Hm * Hm / (Hm * Hm + Pm * Pm + 1e-30);

            // instantaneous frequency before and after the centre frame (coherent over the three main-lobe bins)
            double ifs[2];
            for (int p = 0; p < 2; ++p)
            {
                const auto& A = spec[(size_t) (p == 0 ? c : cn)];
                const auto& B = spec[(size_t) (p == 0 ? cp : c)];
                std::complex<double> cross (0.0, 0.0);
                for (int j = -1; j <= 1; ++j) cross += std::complex<double> (A[(size_t) (k + j)]) * std::conj (std::complex<double> (B[(size_t) (k + j)]));
                const double fk = k * binHz;
                double dphi = std::arg (cross) - 2.0 * 3.14159265358979323846 * fk * dt;
                dphi -= 2.0 * 3.14159265358979323846 * std::round (dphi / (2.0 * 3.14159265358979323846));
                ifs[p] = fk + dphi / (2.0 * 3.14159265358979323846 * dt);
            }
            if (std::fabs (ifs[0] - k * binHz) > 1.2 * binHz || std::fabs (ifs[1] - k * binHz) > 1.2 * binHz) continue;
            const double dev = 1200.0 * std::fabs (std::log2 (ifs[0] / ifs[1]));
            const double stabSigma = 0.8 * 4.0 / kHopDiv;   // frequency noise of a phase difference grows as the frame step shrinks
            const double stab = std::exp (-0.5 * dev * dev / (stabSigma * stabSigma));
            const double ratio = (double) mags[(size_t) cn][(size_t) k] / std::max (1e-20f, mags[(size_t) cp][(size_t) k]);
            const double ampSteady = std::exp (-0.5 * std::pow (std::log2 (ratio) / 0.45, 2.0));
            const double f = 0.5 * (ifs[0] + ifs[1]);

            const float ref = std::max (bandMax[(size_t) bandOf (k)], 0.05f * gmax);
            const double a = std::sqrt (std::min (1.0, (double) m / ref));
            const double weight = a * mask * pf * stab * ampSteady;
            if (weight < 1e-4) continue;

            const int band = f < 150.0 ? 0 : f < 1500.0 ? 1 : 2;
            const double cents = circWrap (1200.0 * std::log2 (f / 440.0));
            const double pos = (cents + 50.0) / kBinCents - 0.5; // bin centres
            const int b0 = (int) std::floor (pos);
            const double fr = pos - b0;
            const double w = weight * (band == 2 ? 0.5 : 1.0);
            cur[(size_t) band][(size_t) ((b0 % kBins + kBins) % kBins)] += (float) (w * (1.0 - fr));
            cur[(size_t) band][(size_t) (((b0 + 1) % kBins + kBins) % kBins)] += (float) (w * fr);
            if (recordTimeline)
            {
                auto& acc = band >= 1 ? frameAcc : frameLow;
                acc[(size_t) ((b0 % kBins + kBins) % kBins)] += (float) (w * (1.0 - fr));
                acc[(size_t) (((b0 + 1) % kBins + kBins) % kBins)] += (float) (w * fr);
            }
            if (vizCount < centsible::Reading::vizPoints && weight > 0.05) viz[(size_t) vizCount++] = { (float) cents, (float) std::min (1.0, weight) };
        }
        result.vizCount = vizCount;
        for (int i = 0; i < vizCount; ++i) result.viz[(size_t) i] = viz[(size_t) i];
        ++result.frameId;
        if (recordTimeline) timeline.push_back ({ ((double) (hopCount - kCentre) * hop - 0.5 * n) / sr, frameAcc, frameLow });
        result.centroidHz = 0.0f;
    }

    // ---- chunks and estimates ----------------------------------------------------------------------------------------------
    void finishChunk()
    {
        chunks[(size_t) chunkHead] = cur;
        chunkHead = (chunkHead + 1) % kMaxChunks;
        chunkCount = std::min (chunkCount + 1, kMaxChunks);
        for (auto& h : cur) h.fill (0.0f);
        curFrames = 0;
        checkSongChange();
        estimate (true);
    }

    const std::array<Hist, kBands>& chunkAt (int i) const // i = 0 oldest .. chunkCount-1 newest
    {
        return chunks[(size_t) (((chunkHead - chunkCount + i) % kMaxChunks + kMaxChunks) % kMaxChunks)];
    }

    static double massOf (const Hist& h) { double m = 0; for (float v : h) m += v; return m; }

    void sumTonal (Hist& out, double& mass, int from, bool withCurrent) const
    {
        out.fill (0.0f);
        for (int i = from; i < chunkCount; ++i)
        {
            const auto& c = chunkAt (i);
            for (int b = 0; b < kBins; ++b) out[(size_t) b] += c[1][(size_t) b] + c[2][(size_t) b];
        }
        if (withCurrent) for (int b = 0; b < kBins; ++b) out[(size_t) b] += cur[1][(size_t) b] + cur[2][(size_t) b];
        mass = massOf (out);
    }

    void sumLow (Hist& out, double& mass, bool withCurrent) const
    {
        out.fill (0.0f);
        for (int i = 0; i < chunkCount; ++i) { const auto& c = chunkAt (i); for (int b = 0; b < kBins; ++b) out[(size_t) b] += c[0][(size_t) b]; }
        if (withCurrent) for (int b = 0; b < kBins; ++b) out[(size_t) b] += cur[0][(size_t) b];
        mass = massOf (out);
    }

    // a different song? (recent chunks fit a different tuning much better than the older evidence does)
    void checkSongChange()
    {
        const int K = hint ? 2 : 4;
        if (chunkCount < K + 3) return;
        Hist older, recent;
        double mo, mr;
        older.fill (0.0f);
        for (int i = 0; i < chunkCount - K; ++i) { const auto& c = chunkAt (i); for (int b = 0; b < kBins; ++b) older[(size_t) b] += c[1][(size_t) b] + c[2][(size_t) b]; }
        mo = massOf (older);
        sumTonal (recent, mr, chunkCount - K, false);
        if (mo < knobs.minMass || mr < 0.5 * knobs.minMass) return;
        const Fit fo = fitSingle (older, mo);
        const int sNew = scan (recent, fo.tmpl, 0, kBins - 1, 2);
        const int sNewF = scan (recent, fo.tmpl, sNew - 6, sNew + 6, 1);
        const double gain = (logLik (recent, fo.tmpl, sNewF) - logLik (recent, fo.tmpl, fo.shift)) * knobs.effScale;
        const double thr = hint ? knobs.changeDelta * 0.15 : knobs.changeDelta;
        // A song change is ABRUPT: the newest chunks differ from the chunks just before them. A tuning that merely drifts (tape wow, a slow retune)
        // also differs from the long-run evidence, but not from the previous few chunks; that is reported as drift, not as a new song.
        bool abrupt = true;
        if (! hint && chunkCount >= 2 * K + 3)
        {
            Hist prev;
            prev.fill (0.0f);
            for (int i = chunkCount - 2 * K; i < chunkCount - K; ++i) { const auto& c = chunkAt (i); for (int b = 0; b < kBins; ++b) prev[(size_t) b] += c[1][(size_t) b] + c[2][(size_t) b]; }
            if (massOf (prev) >= 0.5 * knobs.minMass)
            {
                const int sp = scan (prev, fo.tmpl, 0, kBins - 1, 2);
                const int spF = scan (prev, fo.tmpl, sp - 6, sp + 6, 1);
                abrupt = binDist (spF, sNewF) > 10;   // more than 2.5 cents between neighbouring blocks of chunks
            }
        }
        contested = abrupt && gain > 0.4 * thr;   // a slow drift is not a suspected song change
        if (gain > thr && abrupt && binDist (sNewF, fo.shift) > (hint ? 6 : 12))
        {
            // drop the old evidence, keep the recent chunks
            for (int i = 0; i < K; ++i) scratchChunks[(size_t) i] = chunkAt (chunkCount - K + i);
            for (int i = 0; i < kMaxChunks; ++i) chunks[(size_t) i] = i < K ? scratchChunks[(size_t) i] : std::array<Hist, kBands> {};
            chunkCount = K;
            chunkHead = K % kMaxChunks;
            ++songChanges;
            hint = false;
            contested = false;
            historyN = 0;
            nextHistoryAt = audioSeconds + 1.0;
            recentCentsN = 0;
        }
    }

    // Does the tuning move over the song? Fit the same template to the earlier and the later half of the evidence and test the difference against the
    // bootstrap scatter of both. Conservative: needs >= 8 chunks (40 s), enough evidence in both halves, and a difference of at least 1.5 cents.
    bool measureDrift (const Fit& f1, bool tonal, double& drift) const
    {
        const int nc = chunkCount;
        if (nc < 8) return false;
        const int half = nc / 2;
        auto bandSum = [&] (int from, int to, Hist& out)
        {
            out.fill (0.0f);
            for (int i = from; i < to; ++i)
            {
                const auto& c = chunkAt (i);
                for (int b = 0; b < kBins; ++b) out[(size_t) b] += tonal ? c[1][(size_t) b] + c[2][(size_t) b] : c[0][(size_t) b];
            }
        };
        double est[2], sd[2];
        for (int part = 0; part < 2; ++part)
        {
            const int from = part == 0 ? 0 : half, to = part == 0 ? half : nc;
            Hist H;
            bandSum (from, to, H);
            if (massOf (H) < 0.5 * knobs.minMass) return false;
            const int s0 = scan (H, f1.tmpl, f1.shift - 40, f1.shift + 40, 1);
            est[part] = polishDelta (H, f1.tmpl, refineDelta (H, f1.tmpl, s0));
            std::array<double, 24> draws {};
            std::uint32_t rng = 777u + (std::uint32_t) part;
            Hist S;
            for (int d = 0; d < 24; ++d)
            {
                S.fill (0.0f);
                for (int i = from; i < to; ++i)
                {
                    rng = rng * 1664525u + 1013904223u;
                    const auto& c = chunkAt (from + (int) ((rng >> 8) % (std::uint32_t) (to - from)));
                    for (int b = 0; b < kBins; ++b) S[(size_t) b] += tonal ? c[1][(size_t) b] + c[2][(size_t) b] : c[0][(size_t) b];
                }
                const int sb = scan (S, f1.tmpl, s0 - 16, s0 + 16, 1);
                draws[(size_t) d] = circWrap (refineDelta (S, f1.tmpl, sb) - est[part]);
            }
            double m1 = 0, v = 0;
            for (double x : draws) m1 += x;
            m1 /= 24.0;
            for (double x : draws) v += (x - m1) * (x - m1);
            sd[part] = std::sqrt (v / 23.0);
        }
        drift = circWrap (est[1] - est[0]);
        const double scatter = std::hypot (sd[0], sd[1]);
        return std::fabs (drift) >= std::max (1.5, 3.0 * scatter);
    }

    void estimate (bool chunkDone)
    {
        framesSinceEstimate = 0;
        Hist T, L;
        double mt, ml;
        sumTonal (T, mt, 0, true);
        sumLow (L, ml, true);
        auto& r = result;
        r.songChanges = songChanges;
        r.contested = contested;
        r.seconds = (float) audioSeconds;
        if (mt < knobs.minMass && ml < knobs.minMass) { r.valid = false; r.confidence = 0.0f; return; }

        bool tonal = mt >= knobs.minMass;
        Fit f1 = tonal ? fitSingle (T, mt) : fitSingle (L, ml);
        if (tonal && ml >= knobs.minMass && f1.tmpl.stiff > 0.0)
        {
            // The tonal register needed a stretched-string model. Upper partials alone are a comb on the 100-cent circle and, with free stiffness,
            // that comb is ambiguous (another stiffness and tuning can explain it equally well), whereas the fundamentals in the low register do
            // not depend on stiffness. If the low register is clean and disagrees, it wins. (With an unstretched tonal fit a clean low register
            // is NOT enough: sub-bass often sits a few cents off the body of a mix.)
            const Fit fl = fitSingle (L, ml);
            if (fl.tmpl.wUni <= 0.2 && std::fabs (circWrap (fl.delta - f1.delta)) >= 1.0) { f1 = fl; tonal = false; }
        }
        const Hist& H = tonal ? T : L;
        const double mass = tonal ? mt : ml;

        // Two answers to "which tuning": the single fit above and the richest template family (checkModes) can disagree on a song whose evidence holds two groups of
        // notes a few cents apart (a sampled loop under the playing, a pitched vocal over an untouched beat) or whose partials alias into each other. Which of them the
        // fit lands on can then change with nothing but a pitch shift of the whole file, so the reading says it plainly instead of claiming a tight interval.
        ModeCheck mc;
        if (knobs.arbitrate && tonal && chunkCount >= 4 && f1.tmpl.stiff == 0.0) mc = checkModes (T, mt);
        f1.delta = polishDelta (H, f1.tmpl, f1.delta);
        double cents = f1.delta;

        // confidence interval: block bootstrap over chunks (tonal band), template fixed
        double ci = 50.0;
        const int nc = chunkCount;
        if (nc >= 3)
        {
            std::array<double, 40> draws {};
            std::uint32_t rng = 12345u;
            Hist S;
            for (int d = 0; d < 40; ++d)
            {
                S.fill (0.0f);
                for (int i = 0; i < nc; ++i)
                {
                    rng = rng * 1664525u + 1013904223u;
                    const auto& c = chunkAt ((int) ((rng >> 8) % (std::uint32_t) nc));
                    for (int b = 0; b < kBins; ++b) S[(size_t) b] += tonal ? c[1][(size_t) b] + c[2][(size_t) b] : c[0][(size_t) b];
                }
                const int s = scan (S, f1.tmpl, f1.shift - 16, f1.shift + 16, 1);
                draws[(size_t) d] = circWrap (refineDelta (S, f1.tmpl, s) - f1.delta);
            }
            double m1 = 0, v = 0;
            for (double x : draws) m1 += x;
            m1 /= 40.0;
            for (double x : draws) v += (x - m1) * (x - m1);
            ci = 1.96 * std::sqrt (v / 39.0);
        }
        if (tonal && f1.tmpl.stiff > 0.0) ci = std::max (ci, 0.6);   // a stretched-string fit without the fundamentals to pin it carries model uncertainty
        // the other tuning carries a large share of the evidence: the bootstrap above only knows the tuning it started from, so say it plainly
        bool competing = false;
        double otherDelta = 0.0, otherShare = 0.0;
        if (mc.ok && std::fabs (circWrap (mc.delta1 - cents)) >= knobs.competeApart)
        {
            competing = true;
            otherDelta = mc.delta1;                                        // what the richest family says
            otherShare = std::fabs (circWrap (cents - mc.delta2)) < 3.0 ? mc.share1 : 0.5;
        }
        if (competing) ci = std::max (ci, std::fabs (circWrap (otherDelta - cents)));
        r.competing = competing;
        r.cents = (float) cents;
        r.refHz = (float) (440.0 * std::pow (2.0, cents / 1200.0));
        r.ciCents = (float) ci;
        r.spreadCents = (float) ci;
        r.valid = true;
        {
            double dr = 0.0;
            r.hasDrift = ! contested && measureDrift (f1, tonal, dr);
            r.driftCents = r.hasDrift ? (float) dr : 0.0f;
        }

        // stability of the answer over the last estimates
        recentCents[(size_t) (recentCentsN % 8)] = cents;
        ++recentCentsN;
        double spread = 0.0;
        for (int i = 0; i < std::min (recentCentsN, 8); ++i) spread = std::max (spread, std::fabs (circWrap (recentCents[(size_t) i] - cents)));
        // lock with hysteresis: it takes a tight, steady interval to lock (0.3 cents) but a clearly worse one to let go (0.5), so the readout does not flicker
        if (chunkDone)
            r.locked = r.locked ? (ci <= 0.5 && nc >= 6 && spread <= 0.5 && ! contested) : (ci <= 0.3 && nc >= 6 && spread <= 0.3 && ! contested);
        else
            r.locked = r.locked && ci <= 0.5 && ! contested;
        if (competing) r.locked = false;
        r.settled = r.locked;
        r.confidence = (float) (std::clamp (1.0 - ci / 2.0, 0.0, 1.0) * std::clamp (mass / (4.0 * knobs.minMass), 0.0, 1.0));
        r.dominance = (float) (1.0 + 10.0 * std::clamp (f1.tmpl.w (0), 0.0, 1.0));
        r.notes = (int) std::min (60.0, mass / 40.0);
        r.liveCents = (float) cents;
        r.liveConfidence = r.confidence;

        // second tuning: a genuine mixture of two tunings, or the low register disagreeing with the tonal register
        r.hasSecondary = false;
        r.hasAlt = false;
        if (tonal && chunkCount >= 4)
        {
            const Fit2 f2 = fitSecond (H, f1);
            if (f2.ok)
            {
                const double gain = (f2.loglik - f1.loglik) * knobs.effScale;
                r.hasAlt = true;
                r.altCents = (float) f2.delta2;
                r.hasSecondary = gain > knobs.secondDelta && f2.share2 >= knobs.secondShare && std::fabs (circWrap (f2.delta2 - cents)) >= 5.0;
                r.secondaryCents = (float) f2.delta2;
                r.secondaryShare = (float) f2.share2;
                r.secondaryNotes = (int) std::min (60.0, mass * f2.share2 / 40.0);
            }
        }
        if (tonal && ml >= knobs.minMass && ! r.hasSecondary)
        {
            const Fit fl = fitSingle (L, ml);
            if (std::fabs (circWrap (fl.delta - cents)) >= 4.0 && ml >= 0.25 * mt)
            {
                r.hasSecondary = true; // the low register sits at a different tuning from the tonal content
                r.secondaryCents = (float) fl.delta;
                r.secondaryShare = (float) (ml / (ml + mt));
            }
        }

        if (audioSeconds >= nextHistoryAt)
        {
            nextHistoryAt += 1.0;
            if (historyN == centsible::Reading::historyPoints) { std::copy (hist.begin() + 1, hist.end(), hist.begin()); --historyN; }
            hist[(size_t) historyN++] = (float) cents;
            r.historyCount = historyN;
            std::copy (hist.begin(), hist.begin() + historyN, r.history.begin());
        }
    }

    // ---- state -------------------------------------------------------------------------------------------------------------
    double sr = 44100.0, binHz = 2.9, audioSeconds = 0.0, nextHistoryAt = 1.0;
    int order = 14, n = 16384, hop = 4096;
    std::unique_ptr<Fft> fft;
    std::vector<float> window, ring;
    std::vector<std::complex<float>> buf;
    std::array<std::vector<std::complex<float>>, kFrames> spec;
    std::array<std::vector<float>, kFrames> mags;
    struct CandItem { int k; float m; };
    std::vector<CandItem> cands;
    std::array<std::array<Hist, kBands>, kMaxChunks> scratchChunks {};
    long long hopCount = 0;
    bool recordTimeline = false, timelineOnly = false;
    std::vector<TimelineFrame> timeline;
    int writePos = 0, sinceHop = 0, framesSeen = 0, head = 0, silentFrames = 0, curFrames = 0, framesSinceEstimate = 0;
    std::array<std::array<Hist, kBands>, kMaxChunks> chunks {};
    std::array<Hist, kBands> cur {};
    int chunkCount = 0, chunkHead = 0, songChanges = 0, vizCount = 0, historyN = 0, recentCentsN = 0;
    bool hint = false, contested = false;
    std::array<centsible::VizPeak, centsible::Reading::vizPoints> viz {};
    std::array<float, centsible::Reading::historyPoints> hist {};
    std::array<double, 8> recentCents {};
    centsible::Reading result;
};
} // namespace centsible::detail
