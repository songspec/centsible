// Detune over time. The streaming estimator keeps the tonal evidence of every analysed frame (a 400-bin histogram on the 100-cent circle, see
// spectral_likelihood.h); this file turns that record into
//   * a detune curve: one estimate per overlapping window (2 - 4 s), each with a bootstrap confidence interval, and
//   * a fit of one stretch (a section / a region) from its frames.
// The window fits share ONE template (cluster width, partial roll-off, string stiffness), learned in three passes:
//   1. every window against a generic template -> a rough tuning per window;
//   2. all windows shifted onto a common tuning and summed -> the usual full fit (fitSingle) of the track's timbre;
//   3. every window against that template (full circle scan + polish), background share re-fitted per window.
// A varispeed song smears the evidence of a plain whole-song histogram; aligned by pass 1 it does not, so the template stays sharp.
// Offline code; allocates freely.
#pragma once
#include "spectral_likelihood.h"
#include <centsible/progress.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace centsible::detail
{
struct FrameEvidence
{
    std::vector<SpectralLikelihoodEstimator::TimelineFrame> frames;   // centre time (s) + tonal histogram of each analysed frame
    std::vector<float> mass;                                          // total weight of each frame
    double frameSeconds = 0.0;                                        // time between frames
    double seconds = 0.0;                                             // length of the recorded audio
    bool cancelled = false;
};

inline FrameEvidence recordFrames (const float* x, std::size_t n, double sr, const ProgressFn& progress = {}, double progressFrom = 0.0, double progressTo = 1.0)
{
    FrameEvidence fe;
    auto est = std::make_unique<SpectralLikelihoodEstimator>();   // ~0.7 MB of chunk storage: never on the stack
    est->prepare (sr);
    est->enableTimeline (true, true);
    constexpr std::size_t block = 16384;
    for (std::size_t pos = 0; pos < n; pos += block)
    {
        est->push (x + pos, (int) std::min (block, n - pos));
        if (progress && ! progress (progressFrom + (progressTo - progressFrom) * std::min (1.0, (double) (pos + block) / (double) std::max<std::size_t> (1, n)), "listening"))
        {
            fe.cancelled = true;
            return fe;
        }
    }
    fe.frames = est->timelineFrames();
    fe.seconds = (double) n / sr;
    fe.mass.reserve (fe.frames.size());
    for (const auto& f : fe.frames)
    {
        double m = 0.0;
        for (float v : f.hist) m += v;
        fe.mass.push_back ((float) m);
    }
    fe.frameSeconds = fe.frames.size() >= 2 ? (fe.frames.back().t - fe.frames.front().t) / (double) (fe.frames.size() - 1) : 0.0;
    return fe;
}

struct DetunePoint
{
    double time = 0.0;        // window centre (s)
    double cents = 0.0;       // tuning offset from the 12-TET grid, wrapped to [-50, 50)
    double ci = 50.0;         // 95 % half-width (cents)
    double mass = 0.0;        // evidence in the window
    double sharpness = 0.0;   // log-likelihood gain over "no tuning" per unit of evidence: how peaked the window's evidence is
    double window = 0.0;      // seconds of audio the estimate used (widened where the evidence is thin)
    bool valid = false;
};

struct DetuneCurve
{
    std::vector<DetunePoint> points;
    std::unique_ptr<Fit> fit;          // the shared template of pass 2 (null if there was not enough evidence)
    double windowSeconds = 0.0, hopSeconds = 0.0;
    double medianMass = 0.0;
    bool cancelled = false;
};

struct DetuneOptions
{
    double windowSeconds = 3.0;
    double hopSeconds = 1.0;
    double minMass = 0.0;             // a window needs this much evidence (0: derived from the track: a fraction of the median window)
    double minMassFraction = 0.2;     // ... of the median window mass
    double absoluteMinMass = 12.0;    // ... but never less than this
    double targetMass = 100.0;        // a window is widened (by 0.5 s steps, symmetrically) until it holds this much evidence ...
    double maxWindowSeconds = 16.0;   // ... or is this long. Real mixes hold 20 - 60 per 3 s, clean synthetic songs 450.
    double minSharpness = 0.0;        // evidence with no peak on the circle (noise, a drum solo) has a sharpness at or below 0; real windows, even thin or smeared ones, are above
    int bootstrapDraws = 24;
    int blockFrames = 6;              // bootstrap block length (frames overlap by 3/4, so single frames are far from independent)
};

namespace dt
{
inline void addInto (Hist& dst, const Hist& src) { for (int b = 0; b < kBins; ++b) dst[(size_t) b] += src[(size_t) b]; }
inline double massOf (const Hist& h) { double m = 0; for (float v : h) m += v; return m; }

// frame range [lo, hi) with time in [t0, t1)
inline void frameRange (const FrameEvidence& fe, double t0, double t1, std::size_t& lo, std::size_t& hi)
{
    auto cmp = [] (const SpectralLikelihoodEstimator::TimelineFrame& f, double t) { return f.t < t; };
    lo = (std::size_t) (std::lower_bound (fe.frames.begin(), fe.frames.end(), t0, cmp) - fe.frames.begin());
    hi = (std::size_t) (std::lower_bound (fe.frames.begin(), fe.frames.end(), t1, cmp) - fe.frames.begin());
}

// a copy of src rotated so that a peak at bin s ends at bin 0, added into dst
inline void rotateInto (Hist& dst, const Hist& src, int s)
{
    for (int b = 0; b < kBins; ++b) dst[(size_t) b] += src[(size_t) (((b + s) % kBins + kBins) % kBins)];
}

// log-likelihood gain over a flat histogram, per unit of evidence
inline double sharpnessOf (const Hist& H, const Template& t, int s, double mass)
{
    double ll = logLik (H, t, s);
    ll -= mass * std::log (1.0 / kBins);
    return mass > 0.0 ? ll / mass : 0.0;
}

// one window against a template: full-circle scan, background re-fit, polish. Returns the polished delta; `bin` receives the grid index.
inline double fitWindow (const Hist& H, double mass, Template& t, int& bin)
{
    int s = scan (H, t, 0, kBins - 1, 2);
    s = scan (H, t, s - 2, s + 2, 1);
    fitBackground (H, t, s, mass, 4);
    s = scan (H, t, s - 3, s + 3, 1);
    bin = s;
    return polishDelta (H, t, refineDelta (H, t, s));
}
} // namespace dt

// Bootstrap interval (95 % half width) for a set of frames of one stretch: blocks of consecutive frames are redrawn with replacement.
inline double bootstrapCi (const FrameEvidence& fe, std::size_t lo, std::size_t hi, const Template& t, double delta, int bin, int draws, int block, std::uint32_t seed = 4242u, bool lowBand = false)
{
    const std::size_t nf = hi - lo;
    const int nb = (int) std::max<std::size_t> (1, (nf + (std::size_t) block - 1) / (std::size_t) block);
    if (nb < 2) return 50.0;
    std::vector<double> d;
    d.reserve ((size_t) draws);
    std::uint32_t rng = seed;
    Hist S;
    for (int it = 0; it < draws; ++it)
    {
        S.fill (0.0f);
        for (int k = 0; k < nb; ++k)
        {
            rng = rng * 1664525u + 1013904223u;
            const std::size_t b = (std::size_t) ((rng >> 8) % (std::uint32_t) nb);
            const std::size_t a0 = lo + b * (std::size_t) block, a1 = std::min (hi, a0 + (std::size_t) block);
            for (std::size_t f = a0; f < a1; ++f) dt::addInto (S, lowBand ? fe.frames[f].low : fe.frames[f].hist);
        }
        const int s = scan (S, t, bin - 16, bin + 16, 1);
        d.push_back (circWrap (refineDelta (S, t, s) - delta));
    }
    double m = 0.0, v = 0.0;
    for (double x : d) m += x;
    m /= (double) d.size();
    for (double x : d) v += (x - m) * (x - m);
    v /= (double) (d.size() - 1);
    return 1.96 * std::sqrt (v * (double) nb / (double) (nb - 1));
}

inline DetuneCurve computeDetuneCurve (const FrameEvidence& fe, double t0, double t1, const DetuneOptions& opt, const ProgressFn& progress = {}, double progressFrom = 0.0, double progressTo = 1.0)
{
    DetuneCurve out;
    out.windowSeconds = opt.windowSeconds;
    out.hopSeconds = opt.hopSeconds;
    struct Win { double centre = 0.0, width = 0.0; std::size_t lo = 0, hi = 0; Hist H {}; double mass = 0.0; int bin1 = 0; double sharp1 = 0.0; };
    std::vector<double> prefixMass (fe.frames.size() + 1, 0.0);
    for (std::size_t i = 0; i < fe.frames.size(); ++i) prefixMass[i + 1] = prefixMass[i] + (double) fe.mass[i];
    std::vector<Win> wins;
    for (double c = t0 + 0.5 * opt.windowSeconds; c + 0.5 * opt.windowSeconds <= t1 + 1e-9; c += opt.hopSeconds)
    {
        Win w;
        w.centre = c;
        w.width = opt.windowSeconds;
        for (;;)
        {
            dt::frameRange (fe, c - 0.5 * w.width, c + 0.5 * w.width, w.lo, w.hi);
            if (prefixMass[w.hi] - prefixMass[w.lo] >= opt.targetMass || w.width + 0.5 > opt.maxWindowSeconds) break;
            w.width += 0.5;
        }
        for (std::size_t f = w.lo; f < w.hi; ++f) dt::addInto (w.H, fe.frames[f].hist);
        w.mass = dt::massOf (w.H);
        wins.push_back (std::move (w));
    }
    out.points.resize (wins.size());
    for (std::size_t i = 0; i < wins.size(); ++i) { out.points[i].time = wins[i].centre; out.points[i].window = wins[i].width; }
    if (wins.empty()) return out;

    std::vector<double> masses;
    for (const auto& w : wins) masses.push_back (w.mass);
    std::sort (masses.begin(), masses.end());
    out.medianMass = masses[masses.size() / 2];
    const double minMass = opt.minMass > 0.0 ? opt.minMass : std::max (opt.absoluteMinMass, opt.minMassFraction * out.medianMass);
    auto report = [&] (double f, const char* stage) { return ! progress || progress (progressFrom + (progressTo - progressFrom) * f, stage); };

    // pass 1: a generic template
    auto generic = std::make_unique<Template>();
    generic->setup (0.0, 2.0, 1.2, 0.3);
    std::vector<std::size_t> usable;
    for (std::size_t i = 0; i < wins.size(); ++i)
    {
        auto& w = wins[i];
        if (w.mass < minMass) continue;
        int s = scan (w.H, *generic, 0, kBins - 1, 2);
        s = scan (w.H, *generic, s - 2, s + 2, 1);
        w.bin1 = s;
        w.sharp1 = dt::sharpnessOf (w.H, *generic, s, w.mass);
        usable.push_back (i);
        if ((i & 15) == 0 && ! report (0.15 * (double) i / (double) wins.size(), "fitting detune")) { out.cancelled = true; return out; }
    }
    if (usable.empty()) return out;

    // pass 2: align the sharper windows on their own tunings, sum, fit the template
    std::vector<double> sharps;
    for (auto i : usable) sharps.push_back (wins[i].sharp1);
    std::sort (sharps.begin(), sharps.end());
    const double cut = usable.size() >= 6 ? sharps[sharps.size() * 3 / 10] : -1e9;
    auto aligned = std::make_unique<Hist>();
    aligned->fill (0.0f);
    for (auto i : usable)
        if (wins[i].sharp1 >= cut) dt::rotateInto (*aligned, wins[i].H, wins[i].bin1);
    const double alignedMass = dt::massOf (*aligned);
    if (alignedMass < 4.0 * std::max (1.0, minMass)) return out;
    if (! report (0.3, "fitting detune")) { out.cancelled = true; return out; }
    out.fit = std::make_unique<Fit> (fitSingle (*aligned, alignedMass, false));     // windows have no low register to cross-check a stretched template against

    // pass 3: every window against the shared template
    for (std::size_t k = 0; k < usable.size(); ++k)
    {
        const std::size_t i = usable[k];
        auto& w = wins[i];
        auto t = std::make_unique<Template> (out.fit->tmpl);
        int bin = 0;
        const double delta = dt::fitWindow (w.H, w.mass, *t, bin);
        auto& p = out.points[i];
        p.cents = delta;
        p.sharpness = dt::sharpnessOf (w.H, *t, bin, w.mass);
        p.ci = bootstrapCi (fe, w.lo, w.hi, *t, delta, bin, opt.bootstrapDraws, opt.blockFrames, 99u + (std::uint32_t) i);
        p.valid = true;
        w.bin1 = bin;
        if ((k & 7) == 0 && ! report (0.3 + 0.55 * (double) k / (double) usable.size(), "fitting detune")) { out.cancelled = true; return out; }
    }

    // pass 4: a window during a glide (varispeed ramp, pitch bend) holds evidence smeared over slope * window cents; a narrow template fitted to
    // that smear is biased and over-confident. Widen the template by the smear (uniform over W, so sd = slope * W / sqrt 12) and fit again.
    for (std::size_t k = 0; k < usable.size(); ++k)
    {
        const std::size_t i = usable[k];
        const std::size_t a = k > 0 ? usable[k - 1] : i, b = k + 1 < usable.size() ? usable[k + 1] : i;
        if (a == i && b == i) continue;
        const double dtm = out.points[b].time - out.points[a].time;
        if (dtm <= 0.0 || dtm > 4.0 * opt.hopSeconds + 1e-9) continue;
        const double slope = circWrap (out.points[b].cents - out.points[a].cents) / dtm;     // cents per second
        const double smear = std::fabs (slope) * wins[i].width;
        if (smear < 2.0) continue;
        auto& w = wins[i];
        auto t = std::make_unique<Template> (out.fit->tmpl);
        const double grow = smear / std::sqrt (12.0);
        t->setup (t->stiff, std::hypot (t->sigma, grow), t->alpha, t->wUni, t->hMin, t->rho > 0.0 ? std::hypot (t->sigma2, grow) : 0.0, t->rho);
        fitBackground (w.H, *t, w.bin1, w.mass, 4);
        const int s = scan (w.H, *t, w.bin1 - 40, w.bin1 + 40, 1);
        const double delta = polishDelta (w.H, *t, refineDelta (w.H, *t, s));
        auto& p = out.points[i];
        p.cents = delta;
        p.sharpness = dt::sharpnessOf (w.H, *t, s, w.mass);
        p.ci = bootstrapCi (fe, w.lo, w.hi, *t, delta, s, opt.bootstrapDraws, opt.blockFrames, 99u + (std::uint32_t) i);
        w.bin1 = s;
        if ((k & 7) == 0 && ! report (0.85 + 0.15 * (double) k / (double) usable.size(), "fitting detune")) { out.cancelled = true; return out; }
    }
    for (std::size_t i = 0; i < wins.size(); ++i)
    {
        out.points[i].mass = wins[i].mass;
        if (out.points[i].valid && out.points[i].sharpness < opt.minSharpness) out.points[i].valid = false;
        if (out.points[i].valid) out.points[i].ci = std::max (out.points[i].ci, 0.05);   // the synthetic floor of the model, whatever the bootstrap says
    }
    return out;
}

// A whole stretch of frames fitted as one: tuning, interval, evidence. Uses the template of a curve when one is given (else fitSingle on the stretch).
struct StretchFit
{
    bool valid = false;
    double cents = 0.0, ci = 50.0, mass = 0.0;
    std::shared_ptr<const Fit> fit;     // the template that fitted the stretch (its background share adapted) and where it sat on the circle
    int bin = 0;
};

inline StretchFit fitStretch (const FrameEvidence& fe, double t0, double t1, const Fit* shared, double minMass, int draws = 32, int block = 6)
{
    StretchFit r;
    std::size_t lo, hi;
    dt::frameRange (fe, t0, t1, lo, hi);
    auto H = std::make_unique<Hist>(), Lo = std::make_unique<Hist>();
    H->fill (0.0f);
    Lo->fill (0.0f);
    for (std::size_t f = lo; f < hi; ++f) { dt::addInto (*H, fe.frames[f].hist); dt::addInto (*Lo, fe.frames[f].low); }
    const double massTonal = dt::massOf (*H), massLow = dt::massOf (*Lo);
    r.mass = massTonal;
    const bool tonal = massTonal >= minMass;
    if (! tonal && massLow < minMass) return r;
    // Like the streaming estimator: the tonal register decides, unless it needed a stretched-string model (its upper partials alone are a comb with other
    // explanations half a semitone away) and the low register, whose fundamentals do not depend on stiffness, is clean and disagrees: then the low register wins.
    auto fit = std::make_unique<Fit> (shared ? *shared : fitSingle (tonal ? *H : *Lo, tonal ? massTonal : massLow));
    bool useLow = ! tonal;
    if (tonal && ! shared && fit->tmpl.stiff > 0.0 && massLow >= minMass)
    {
        auto fl = std::make_unique<Fit> (fitSingle (*Lo, massLow));
        if (fl->tmpl.wUni <= 0.2 && std::fabs (circWrap (fl->delta - fit->delta)) >= 1.0) { fit = std::move (fl); useLow = true; }
    }
    const Hist& band = useLow ? *Lo : *H;
    const double mass = useLow ? massLow : massTonal;
    int bin = 0;
    r.cents = dt::fitWindow (band, mass, fit->tmpl, bin);
    r.ci = bootstrapCi (fe, lo, hi, fit->tmpl, r.cents, bin, draws, block, 4242u, useLow);
    r.mass = mass;
    r.valid = true;
    r.bin = bin;
    r.fit = std::move (fit);
    return r;
}

// How different is the tonal evidence just after t from the evidence just before it? 1 - the cosine similarity of the two blocks' histograms on the
// circle, each blurred by about a cent. No template is involved, so it sees a step of the tuning (the peaks move) and a new instrument (the pattern
// changes) alike, and it does not depend on a template that suits one part of the track better than another. It only PROPOSES places to look: the
// plateaus on either side are then fitted with their own templates, and a boundary stands only if their tunings differ.
struct EvidenceChange { double time = 0.0, change = 0.0; };

inline std::vector<EvidenceChange> evidenceChange (const FrameEvidence& fe, double t0, double t1, double block = 4.0, double hop = 1.0)
{
    std::vector<EvidenceChange> out;
    const std::size_t nf = fe.frames.size();
    if (nf < 16) return out;
    std::vector<std::vector<double>> P (nf + 1, std::vector<double> ((std::size_t) kBins, 0.0));
    for (std::size_t i = 0; i < nf; ++i)
        for (int b = 0; b < kBins; ++b) P[i + 1][(std::size_t) b] = P[i][(std::size_t) b] + (double) fe.frames[i].hist[(std::size_t) b];
    auto index = [&] (double t) { std::size_t lo, hi; dt::frameRange (fe, t, t + 1e9, lo, hi); (void) hi; return lo; };
    auto blurred = [] (const std::vector<double>& a, const std::vector<double>& b, std::vector<double>& v)
    {
        std::vector<double> d ((std::size_t) kBins);
        for (int i = 0; i < kBins; ++i) d[(std::size_t) i] = b[(std::size_t) i] - a[(std::size_t) i];
        v.assign ((std::size_t) kBins, 0.0);
        static const double k[5] = { 1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16 };
        for (int i = 0; i < kBins; ++i)
            for (int j = -2; j <= 2; ++j) v[(std::size_t) i] += k[j + 2] * d[(std::size_t) (((i + j) % kBins + kBins) % kBins)];
    };
    std::vector<double> L, R;
    for (double t = t0 + block; t <= t1 - block + 1e-9; t += hop)
    {
        const std::size_t i0 = index (t - block), i1 = index (t), i2 = index (t + block);
        blurred (P[i0], P[i1], L);
        blurred (P[i1], P[i2], R);
        double ll = 0.0, rr = 0.0, lr = 0.0;
        for (int b = 0; b < kBins; ++b) { const double x = L[(std::size_t) b], y = R[(std::size_t) b]; ll += x * x; rr += y * y; lr += x * y; }
        EvidenceChange e;
        e.time = t;
        e.change = ll > 1e-6 && rr > 1e-6 ? std::max (0.0, 1.0 - lr / std::sqrt (ll * rr)) : 0.0;
        out.push_back (e);
    }
    return out;
}
} // namespace centsible::detail
