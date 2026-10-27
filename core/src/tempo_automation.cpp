// Tempo automation: detune over time against tempo over time. See centsible/sections.h.
//
//   1. detune curve: windows of 2 - 4 s, each fitted against a template learned from the whole track (detune_timeline.h);
//   2. tempo curve: a windowed tempogram + Viterbi path (tempo_detector.h), or the tempo map the caller supplies (manual points, host tempo);
//   3. verdict: with x = 1200 log2 (bpm / bpm_ref) the tempo expressed in cents, a VARISPEED has detune = a + x, a TIME-STRETCH has detune = a.
//      Both are fitted as circular regressions (the detune lives on a 100-cent circle, so a 267-cent ramp wraps 2.7 times), and the one whose
//      residual is much tighter wins; the free slope is reported too;
//   4. boundaries: steps of the detune (after removing a varispeed trend) and steps of the tempo are found with a piecewise-linear change-point
//      search that accepts DISCONTINUITIES only (a ramp or a kink is not a boundary); detune steps are then placed to a frame (~0.1 s);
//   5. regions between the boundaries: detune, tempo, behaviour and key (measured against the tuning curve) of each.
#include <centsible/sections.h>
#include "detune_timeline.h"
#include "stats_util.h"
#include "tempo_detector.h"
#include "track_internal.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>

namespace centsible
{
namespace
{
using namespace detail::util;

// ---- one observation = one window with a detune and a tempo --------------------------------------------------------------------------
struct Obs
{
    double t = 0.0;
    double d = 0.0;        // detune wrapped, cents
    double u = 0.0;        // unwrapped
    double x = 0.0;        // tempo in cents relative to the reference tempo, within one octave
    double sigma = 1.0;    // noise of the detune (cents)
    double w = 1.0;        // weight
};

struct Classification
{
    TempoBehaviour behaviour = TempoBehaviour::Unknown;
    double slope = 0.0;               // best-fitting d(detune) / d(tempo cents)
    double sdVarispeed = 50.0, sdFixed = 50.0;
    double R1 = 0.0, R0 = 0.0;
    double offset1 = 0.0;             // detune at x = 0 under the varispeed model (circular mean of d - x)
    double tempoNoise = 0.0;
    double tempoRange = 0.0, pitchRange = 0.0, pitchNoise = 1.0, logLikRatio = 0.0;
    bool tempoVaries = false, pitchMoves = false;
};

// von Mises concentration for a mean resultant length R, and the log-likelihood of n independent angles at that fit (the constant 2 pi term cancels in differences)
double vmKappa (double R)
{
    R = std::clamp (R, 0.0, 0.999);
    if (R < 0.53) return 2.0 * R + R * R * R + 5.0 * std::pow (R, 5.0) / 6.0;
    if (R < 0.85) return -0.4 + 1.39 * R + 0.43 / (1.0 - R);
    return 1.0 / (R * R * R - 4.0 * R * R + 3.0 * R);
}

double logBesselI0 (double k)
{
    if (k < 3.75)
    {
        const double t = (k / 3.75) * (k / 3.75);
        return std::log (1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    return k - 0.5 * std::log (k) + std::log (0.39894228 + 0.01328592 / k + 0.00225319 / (k * k));
}

double vmLogLik (double R, double neff)
{
    const double k = vmKappa (R);
    return neff * (k * R - logBesselI0 (k));
}

// jitter of a detune series around its own running median, and how far its smoothed course ranges
struct PitchStats { double noise = 1.0, range = 0.0; };

PitchStats pitchStats (const std::vector<double>& u)
{
    PitchStats p;
    if (u.size() < 3) return p;
    const auto sm = medianFilter (u, 9);
    std::vector<double> ad;
    for (std::size_t i = 0; i < u.size(); ++i) ad.push_back (std::fabs (u[i] - sm[i]));
    p.noise = std::max (1.0, 1.4826 * medianOf (ad));
    p.range = percentileOf (sm, 0.9) - percentileOf (sm, 0.1);
    return p;
}

// overlap: how many windows share one stretch of audio (3 s windows at a 1 s hop: 3), so that the evidence is not counted that many times over
Classification classify (const std::vector<Obs>& obs, double overlap = 3.0)
{
    Classification c;
    const int n = (int) obs.size();
    if (n < 6) return c;
    std::vector<double> x, u, d, w;
    for (const auto& o : obs) { x.push_back (o.x); u.push_back (o.u); d.push_back (o.d); w.push_back (o.w); }

    // jitter of the tempo curve: median absolute successive difference
    std::vector<double> dx;
    for (int i = 1; i < n; ++i) dx.push_back (x[(size_t) i] - x[(size_t) i - 1]);
    const double med = medianOf (dx);
    std::vector<double> ad;
    for (double v : dx) ad.push_back (std::fabs (v - med));
    c.tempoNoise = std::max (3.0, 1.4826 * medianOf (ad) / std::sqrt (2.0));

    const auto xs = medianFilter (x, 5);
    const auto ps = pitchStats (u);
    c.tempoRange = percentileOf (xs, 0.9) - percentileOf (xs, 0.1);
    c.pitchRange = ps.range;
    c.pitchNoise = ps.noise;
    c.tempoVaries = c.tempoRange >= std::max (45.0, 6.0 * c.tempoNoise);       // 45 cents = 2.6 %
    c.pitchMoves = c.pitchRange >= std::max (10.0, 4.0 * ps.noise);

    // The circular fit of "detune - s * tempo" for a slope s, robust to a tempo track that wanders in a weak stretch: the 15 % worst residuals (against the
    // circular mean) are left out, the same share for both models, before the concentration is measured.
    auto stat = [&] (double s)
    {
        std::vector<double> r ((size_t) n);
        for (int i = 0; i < n; ++i) r[(size_t) i] = wrap100 (d[(size_t) i] - s * x[(size_t) i]);
        const auto first = circularStats (r, w);
        std::vector<double> dev ((size_t) n);
        for (int i = 0; i < n; ++i) dev[(size_t) i] = std::fabs (wrap100 (r[(size_t) i] - first.mean));
        const double cut = percentileOf (dev, 0.85);
        std::vector<double> rk, wk;
        for (int i = 0; i < n; ++i) if (dev[(size_t) i] <= cut) { rk.push_back (r[(size_t) i]); wk.push_back (w[(size_t) i]); }
        return circularStats (rk, wk);
    };
    const auto s0 = stat (0.0), s1 = stat (1.0);
    c.R0 = s0.R; c.R1 = s1.R; c.sdFixed = s0.sd; c.sdVarispeed = s1.sd; c.offset1 = s1.mean;

    if (! c.tempoVaries)
    {
        c.behaviour = c.pitchMoves ? TempoBehaviour::PitchMoves : TempoBehaviour::Constant;
        return c;
    }
    double bestS = 0.0, bestR = -1.0;
    for (double s = -0.5; s <= 1.5001; s += 0.01)
    {
        const double R = stat (s).R;
        if (R > bestR) { bestR = R; bestS = s; }
    }
    for (double s = bestS - 0.01; s <= bestS + 0.0101; s += 0.002)
    {
        const double R = stat (s).R;
        if (R > bestR) { bestR = R; bestS = s; }
    }
    c.slope = bestS;
    const double tolV = std::clamp (2.5 * std::max (c.tempoNoise, ps.noise), 10.0, 25.0);
    const double tolF = std::max (6.0, 2.5 * ps.noise);
    // which model explains the detune better: compare the two circular fits as likelihoods, over the number of independent windows (not the overlapped ones)
    double sw = 0.0, sw2 = 0.0;
    for (double v : w) { sw += v; sw2 += v * v; }
    const double neff = sw2 > 0.0 ? 0.85 * sw * sw / sw2 / std::max (1.0, overlap) : 1.0;
    c.logLikRatio = vmLogLik (c.R1, neff) - vmLogLik (c.R0, neff);
    if (c.sdVarispeed <= tolV && c.logLikRatio >= 4.0 && std::fabs (bestS - 1.0) <= 0.3) c.behaviour = TempoBehaviour::Varispeed;
    else if (c.sdFixed <= tolF && -c.logLikRatio >= 4.0 && std::fabs (bestS) <= 0.3) c.behaviour = TempoBehaviour::TimeStretch;
    else c.behaviour = TempoBehaviour::Mixed;
    return c;
}

// ---- change points ----------------------------------------------------------------------------------------------------------------------
struct SegPoint { double t = 0.0, v = 0.0, sigma = 1.0; };
struct Step { int index = 0; double gain = 0.0, jump = 0.0, time = 0.0; };

// prefix sums for weighted straight-line fits
struct LineSums
{
    std::vector<double> S0, St, Sv, Stt, Stv, Svv;
    double t0 = 0.0;
    explicit LineSums (const std::vector<SegPoint>& p)
    {
        const std::size_t n = p.size();
        for (auto* v : { &S0, &St, &Sv, &Stt, &Stv, &Svv }) v->assign (n + 1, 0.0);
        t0 = n ? p.front().t : 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double w = 1.0 / (p[i].sigma * p[i].sigma), t = p[i].t - t0, v = p[i].v;
            S0[i + 1] = S0[i] + w; St[i + 1] = St[i] + w * t; Sv[i + 1] = Sv[i] + w * v;
            Stt[i + 1] = Stt[i] + w * t * t; Stv[i + 1] = Stv[i] + w * t * v; Svv[i + 1] = Svv[i] + w * v * v;
        }
    }
    struct Fit { double sse = 0.0, a = 0.0, b = 0.0; };    // v = a + b * (t - t0)
    Fit line (int i, int j) const
    {
        const double s0 = S0[(size_t) j] - S0[(size_t) i], st = St[(size_t) j] - St[(size_t) i], sv = Sv[(size_t) j] - Sv[(size_t) i];
        const double stt = Stt[(size_t) j] - Stt[(size_t) i], stv = Stv[(size_t) j] - Stv[(size_t) i], svv = Svv[(size_t) j] - Svv[(size_t) i];
        Fit f;
        const double den = s0 * stt - st * st;
        if (std::fabs (den) > 1e-9 * std::max (1.0, s0 * stt)) f.b = (s0 * stv - st * sv) / den;
        f.a = s0 > 0 ? (sv - f.b * st) / s0 : 0.0;
        f.sse = std::max (0.0, svv - f.a * sv - f.b * stv);
        return f;
    }
};

// A boundary is a DISCONTINUITY: the straight lines fitted just left and just right of it (`near` points each, leaving `skip` points out next to
// the split for step edges that the windows smear) disagree where they meet. A kink (a ramp starting or ending) does not. The split itself is
// chosen by how much two straight lines explain better than one (chi squared gain).
void findStepsIn (const std::vector<SegPoint>& p, const LineSums& ls, int a, int b, double minGain, double minJump, int minPoints, int near, int skip, std::vector<Step>& out)
{
    if (b - a < 2 * minPoints) return;
    const auto whole = ls.line (a, b);
    int best = -1;
    double bestGain = minGain, bestJump = 0.0, bestTime = 0.0;
    for (int k = a + minPoints; k <= b - minPoints; ++k)
    {
        const auto L = ls.line (a, k), R = ls.line (k, b);
        const double gain = whole.sse - L.sse - R.sse;
        if (gain <= bestGain) continue;
        const int la = std::max (a, k - skip - near), lb = std::max (a + 1, k - skip), ra = std::min (b - 1, k + skip), rb = std::min (b, k + skip + near);
        if (lb - la < 2 || rb - ra < 2) continue;
        const auto Ln = ls.line (la, lb), Rn = ls.line (ra, rb);
        const double tb = 0.5 * (p[(size_t) k - 1].t + p[(size_t) k].t) - ls.t0;
        const double jump = (Rn.a + Rn.b * tb) - (Ln.a + Ln.b * tb);
        if (std::fabs (jump) < minJump) continue;
        best = k; bestGain = gain; bestJump = jump; bestTime = tb + ls.t0;
    }
    if (best < 0) return;
    out.push_back ({ best, bestGain, bestJump, bestTime });
    findStepsIn (p, ls, a, best, minGain, minJump, minPoints, near, skip, out);
    findStepsIn (p, ls, best, b, minGain, minJump, minPoints, near, skip, out);
}

std::vector<Step> findSteps (const std::vector<SegPoint>& p, double minGain, double minJump, int minPoints, int near, int skip)
{
    std::vector<Step> out;
    if ((int) p.size() < 2 * minPoints) return out;
    const LineSums ls (p);
    findStepsIn (p, ls, 0, (int) p.size(), minGain, minJump, minPoints, near, skip, out);
    std::sort (out.begin(), out.end(), [] (const Step& a, const Step& b) { return a.time < b.time; });
    return out;
}

// where exactly (to a frame) does the evidence switch from the left plateau's fit to the right plateau's fit? Maximises the likelihood of "frames left of
// the split under the left fit, frames right of it under the right fit" (each plateau with its own template: they may be different instruments).
double refineSplit (const detail::FrameEvidence& fe, const detail::Fit& fitL, int sL, const detail::Fit& fitR, int sR, double t0, double t1, double fallback)
{
    std::size_t lo, hi;
    detail::dt::frameRange (fe, t0, t1, lo, hi);
    if (hi <= lo + 2) return fallback;
    std::vector<double> llL (hi - lo), llR (hi - lo);
    for (std::size_t f = lo; f < hi; ++f)
    {
        llL[f - lo] = detail::logLik (fe.frames[f].hist, fitL.tmpl, sL);
        llR[f - lo] = detail::logLik (fe.frames[f].hist, fitR.tmpl, sR);
    }
    // split after frame m: frames <= m are left
    double total = 0.0;
    for (double v : llR) total += v;
    double best = -1e300, left = 0.0, right = total;
    std::size_t bestM = lo;
    for (std::size_t m = lo; m < hi; ++m)
    {
        left += llL[m - lo];
        right -= llR[m - lo];
        if (left + right > best) { best = left + right; bestM = m; }
    }
    return fe.frames[bestM].t + 0.5 * fe.frameSeconds;
}

// the beat in [t - reach, t + reach] where the beat interval changes most (two-plateau fit of log intervals)
double refineTempoStep (const std::vector<double>& beats, double t, double reach, double fallback)
{
    std::vector<double> bt, li;
    for (std::size_t i = 0; i + 1 < beats.size(); ++i)
        if (beats[i] >= t - 2.0 * reach && beats[i] <= t + 2.0 * reach) { bt.push_back (beats[i + 1]); li.push_back (std::log (beats[i + 1] - beats[i])); }
    if (li.size() < 8) return fallback;
    double best = 1e300, at = fallback;
    for (std::size_t k = 4; k + 4 <= li.size(); ++k)
    {
        if (bt[k] < t - reach || bt[k] > t + reach) continue;
        const double a = medianOf (std::vector<double> (li.begin(), li.begin() + (long) k)), b = medianOf (std::vector<double> (li.begin() + (long) k, li.end()));
        double sse = 0.0;
        for (std::size_t i = 0; i < li.size(); ++i) { const double r = li[i] - (i < k ? a : b); sse += std::fabs (r); }     // L1: robust to a missed beat
        if (sse < best) { best = sse; at = bt[k] - 0.5 * (bt[k] - (k > 0 ? bt[k - 1] : bt[k])); }
    }
    return at;
}

double overlapOf (const std::vector<CurvePoint>& curve, double hop)
{
    double sum = 0.0;
    int n = 0;
    for (const auto& c : curve) if (c.detuneValid) { sum += c.window; ++n; }
    return n ? std::max (1.0, sum / n / std::max (0.1, hop)) : 1.0;
}

std::string bpmText (double bpm) { return fmt ("%.1f", bpm); }
std::string centsText (double c) { return fmt ("%+.1f", c); }
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
TempoAutomationResult analyzeTempoAutomation (const float* mono, std::size_t n, double sr, const AutomationOptions& opt, const ProgressFn& progress)
{
    using namespace detail;
    TempoAutomationResult res;
    res.seconds = (double) n / sr;
    auto report = [&] (double f, const char* stage) { return ! progress || progress (f, stage); };
    if (res.seconds < 12.0)
    {
        res.summary = "The audio is too short (" + fmt ("%.1f", res.seconds) + " s) to follow the tempo and the detune over time.";
        return res;
    }

    // 1. frame evidence and the detune curve
    const auto fe = recordFrames (mono, n, sr, progress, 0.0, 0.4);
    if (fe.cancelled) { res.cancelled = true; return res; }
    DetuneOptions dopt;
    dopt.windowSeconds = opt.windowSeconds;
    dopt.hopSeconds = opt.hopSeconds;
    const auto dc = computeDetuneCurve (fe, 0.0, res.seconds, dopt, progress, 0.4, 0.6);
    if (dc.cancelled) { res.cancelled = true; return res; }

    // 2. tempo
    tempodetail::OnsetEnvelope env;
    tempodetail::TempoTrack tt;
    std::vector<double> beats;
    bool haveTempo = false;
    if (opt.tempoMap && opt.tempoMap->valid())
    {
        res.tempoFromMap = true;
        haveTempo = true;
    }
    else
    {
        if (! report (0.6, "tempo")) { res.cancelled = true; return res; }
        env = tempodetail::computeOnsetEnvelope (mono, n, sr, 0);
        if (env.valid)
        {
            tt = tempodetail::trackTempo (env, opt.tempoWindowSeconds, 1.0, 100.0);
            // second pass: a gentle pull towards the tempo the first pass found AROUND each window (median over +-20 s): a short stretch at another metrical
            // level is pulled back to its surroundings, while a track that switches beat keeps both of its tempi and a varispeed ramp is followed
            if (tt.valid())
            {
                const auto first = tt;
                auto localTempo = [first] (double time)
                {
                    std::vector<double> near;
                    for (std::size_t i = 0; i < first.t.size(); ++i) if (std::fabs (first.t[i] - time) <= 20.0) near.push_back (first.bpm[i]);
                    return near.empty() ? 0.0 : medianOf (near);
                };
                tt = tempodetail::trackTempo (env, opt.tempoWindowSeconds, 1.0, 100.0, localTempo);
            }
            haveTempo = tt.valid();
        }
    }
    if (! report (0.7, "analysing")) { res.cancelled = true; return res; }

    // 3. curve
    res.curve.resize (dc.points.size());
    std::vector<double> bpms;
    for (std::size_t i = 0; i < dc.points.size(); ++i)
    {
        auto& c = res.curve[i];
        const auto& p = dc.points[i];
        c.time = p.time;
        c.window = p.window;
        c.detuneValid = p.valid;
        c.cents = p.cents;
        c.ci = p.ci;
        c.mass = p.mass;
        c.sharpness = p.sharpness;
        if (! haveTempo) continue;
        if (res.tempoFromMap)
        {
            c.tempoValid = true;
            c.bpm = opt.tempoMap->bpmAt (p.time);
        }
        else if (p.time >= tt.t.front() - 1.5 - 1e-9 && p.time <= tt.t.back() + 1.5 + 1e-9)
        {
            // a window whose tempogram does not stand out (no clear pulse) says nothing
            const auto it = std::lower_bound (tt.t.begin(), tt.t.end(), p.time);
            const std::size_t j = std::min<std::size_t> (tt.t.size() - 1, (std::size_t) (it - tt.t.begin()));
            c.tempoValid = (tt.z[j] >= 2.5 || (j > 0 && tt.z[j - 1] >= 2.5)) && ! (j < tt.ambiguous.size() && tt.ambiguous[j]);
            c.bpm = tt.bpmAt (p.time);
        }
        if (c.tempoValid) bpms.push_back (c.bpm);
    }
    int validDetune = 0;
    for (const auto& c : res.curve) validDetune += c.detuneValid;
    if (validDetune < 4)
    {
        res.summary = "Not enough steady pitched content to follow the detune over time.";
        return res;
    }
    res.valid = true;

    // unwrap the detune: every window as close to the previous valid one as the circle allows
    {
        bool have = false;
        double prev = 0.0;
        for (auto& c : res.curve)
        {
            if (! c.detuneValid) continue;
            c.unwrapped = have ? c.cents + 100.0 * std::round ((prev - c.cents) / 100.0) : c.cents;
            prev = c.unwrapped;
            have = true;
        }
    }

    // (A tempogram can settle on another counting of the same pulse - 5:4, 4:3, 6:5 of the song's own tempo - for a stretch; the tempo track marks such
    // windows as ambiguous and they are not trusted above. Octaves are no problem: tempo in cents is folded to one octave.)
    double bref = bpms.empty() ? 0.0 : medianOf (bpms);
    if (! bpms.empty())
    {
        res.medianBpm = bref;
        // the range the tempo covers: the 3rd to 97th percentile, so that a few windows of a fade-out or a silent intro do not stretch it
        auto sorted = bpms;
        std::sort (sorted.begin(), sorted.end());
        const std::size_t trim = sorted.size() >= 30 ? (std::size_t) std::floor (0.03 * (double) sorted.size()) : 0;
        res.minBpm = sorted[trim];
        res.maxBpm = sorted[sorted.size() - 1 - trim];
        for (auto& c : res.curve)
            if (c.tempoValid) c.tempoCents = wrapOctave (1200.0 * std::log2 (c.bpm / bref));
    }

    // 4. verdict over everything
    std::vector<Obs> all;
    for (const auto& c : res.curve)
    {
        if (! c.detuneValid) continue;
        Obs o;
        o.t = c.time; o.d = c.cents; o.u = c.unwrapped; o.x = c.tempoValid ? c.tempoCents : 0.0;
        o.sigma = std::max (0.7, c.ci / 1.96);
        o.w = 1.0 / (o.sigma * o.sigma + 9.0);
        if (c.tempoValid) all.push_back (o);
    }
    Classification verdict;
    if (all.size() >= 6)
        verdict = classify (all, overlapOf (res.curve, opt.hopSeconds));
    else
    {
        // no usable tempo: classify the detune alone
        std::vector<double> us;
        for (const auto& c : res.curve) if (c.detuneValid) us.push_back (c.unwrapped);
        const auto ps = pitchStats (us);
        verdict.pitchRange = ps.range;
        verdict.pitchNoise = ps.noise;
        verdict.pitchMoves = verdict.pitchRange >= std::max (10.0, 4.0 * ps.noise);
        verdict.behaviour = verdict.pitchMoves ? TempoBehaviour::PitchMoves : TempoBehaviour::Constant;
    }
    res.behaviour = verdict.behaviour;
    res.slope = verdict.slope;
    res.spreadVarispeed = verdict.sdVarispeed;
    res.spreadFixed = verdict.sdFixed;
    res.tempoNoise = verdict.tempoNoise;

    // The first and last readings of a track (median of three windows): the detune on a continuous scale and the tempo.
    {
        std::vector<double> firstC, lastC, firstB, lastB;
        int seenD = 0, seenT = 0;
        for (const auto& c : res.curve) if (c.detuneValid && seenD++ < 3) firstC.push_back (c.unwrapped);
        for (const auto& c : res.curve) if (c.tempoValid && seenT++ < 3) firstB.push_back (c.bpm);
        seenD = seenT = 0;
        for (auto it = res.curve.rbegin(); it != res.curve.rend(); ++it) if (it->detuneValid && seenD++ < 3) lastC.push_back (it->unwrapped);
        for (auto it = res.curve.rbegin(); it != res.curve.rend(); ++it) if (it->tempoValid && seenT++ < 3) lastB.push_back (it->bpm);
        res.centsStart = medianOf (firstC); res.centsEnd = medianOf (lastC);
        res.bpmStart = medianOf (firstB); res.bpmEnd = medianOf (lastB);
    }

    // Varispeed: the turns of the 100-cent circle. The detune of a varispeed is continuous, and the tempo says roughly where it must be, so the integer number
    // of turns of every window is chosen by dynamic programming over the whole track: a penalty for a jump of the continuous detune between neighbouring
    // windows, a weak pull towards the value the tempo implies. A noisy stretch cannot make a window slip by a turn (the jump costs far more than the pull),
    // and where the tempo track counted the pulse at another metrical level for a stretch (4:3 is five semitones less 2 cents: the circle cannot see it)
    // the detune stays continuous and that stretch's tempo is moved back to the usual level. The track's ends are then the line the verdict fitted, not the
    // last few noisy windows; the track starts within 50 cents of zero, as for every other verdict.
    if (verdict.behaviour == TempoBehaviour::Varispeed && std::fabs (verdict.slope) > 0.5)
    {
        const double s = verdict.slope;
        std::vector<int> idx;                       // the windows with a detune
        for (int i = 0; i < (int) res.curve.size(); ++i) if (res.curve[(size_t) i].detuneValid) idx.push_back (i);
        const int m = (int) idx.size();
        constexpr int kReach = 12, kStates = 2 * kReach + 1;      // turns relative to the tempo's expectation
        auto expectedOf = [&] (const CurvePoint& c) { return c.tempoValid ? verdict.offset1 + s * c.tempoCents : 0.0; };
        if (m >= 2)
        {
            const double jumpScale = 15.0, pullScale = 150.0;
            std::vector<std::vector<double>> cost ((size_t) m, std::vector<double> ((size_t) kStates, 0.0));
            std::vector<std::vector<int>> back ((size_t) m, std::vector<int> ((size_t) kStates, 0));
            auto uOf = [&] (int wi, int state)       // continuous detune of window idx[wi] at `state` turns from its expectation
            {
                const auto& c = res.curve[(size_t) idx[(size_t) wi]];
                const double e = expectedOf (c);
                return e + wrap100 (c.cents - e) + 100.0 * (state - kReach);
            };
            for (int st = 0; st < kStates; ++st)
            {
                const auto& c = res.curve[(size_t) idx[0]];
                const double pull = c.tempoValid ? 100.0 * (st - kReach) / pullScale : 0.0;
                cost[0][(size_t) st] = pull * pull;
            }
            for (int wi = 1; wi < m; ++wi)
            {
                const auto& c = res.curve[(size_t) idx[(size_t) wi]];
                for (int st = 0; st < kStates; ++st)
                {
                    const double u = uOf (wi, st), pull = c.tempoValid ? 100.0 * (st - kReach) / pullScale : 0.0;
                    double best = 1e300;
                    int bi = 0;
                    for (int pr = 0; pr < kStates; ++pr)
                    {
                        const double jump = (u - uOf (wi - 1, pr)) / jumpScale, v = cost[(size_t) wi - 1][(size_t) pr] + jump * jump;
                        if (v < best) { best = v; bi = pr; }
                    }
                    cost[(size_t) wi][(size_t) st] = best + pull * pull;
                    back[(size_t) wi][(size_t) st] = bi;
                }
            }
            std::vector<int> state ((size_t) m);
            state[(size_t) m - 1] = (int) (std::min_element (cost[(size_t) m - 1].begin(), cost[(size_t) m - 1].end()) - cost[(size_t) m - 1].begin());
            for (int wi = m - 1; wi > 0; --wi) state[(size_t) wi - 1] = back[(size_t) wi][(size_t) state[(size_t) wi]];
            // the dominant level: the median number of turns of the windows with a tempo
            std::vector<double> turns;
            for (int wi = 0; wi < m; ++wi) if (res.curve[(size_t) idx[(size_t) wi]].tempoValid) turns.push_back ((double) (state[(size_t) wi] - kReach));
            const int level = (int) std::lround (medianOf (turns));
            for (int wi = 0; wi < m; ++wi)
            {
                auto& c = res.curve[(size_t) idx[(size_t) wi]];
                c.unwrapped = uOf (wi, state[(size_t) wi]);
                const int shift = state[(size_t) wi] - kReach - level;
                if (c.tempoValid && shift != 0)
                {
                    c.tempoCents += 100.0 * shift / s;      // this stretch's tempo was counted at another metrical level: move it back
                    c.bpm = bref * std::pow (2.0, c.tempoCents / 1200.0);
                }
            }
            // the track starts within 50 cents of zero
            const double turnsFirst = 100.0 * std::round (res.curve[(size_t) idx[0]].unwrapped / 100.0);
            for (int wi = 0; wi < m; ++wi) res.curve[(size_t) idx[(size_t) wi]].unwrapped -= turnsFirst;
            // the line the verdict fitted: u = A + s x, with A the median offset
            std::vector<double> offsets;
            for (const auto& c : res.curve) if (c.detuneValid && c.tempoValid) offsets.push_back (c.unwrapped - s * c.tempoCents);
            if (offsets.size() >= 4)
            {
                const double A = medianOf (offsets);
                std::vector<double> firstX, lastX;
                for (const auto& c : res.curve) if (c.tempoValid && firstX.size() < 4) firstX.push_back (c.tempoCents);
                for (auto it = res.curve.rbegin(); it != res.curve.rend(); ++it) if (it->tempoValid && lastX.size() < 4) lastX.push_back (it->tempoCents);
                const double xStart = medianOf (firstX), xEnd = medianOf (lastX);
                res.centsStart = A + s * xStart;
                res.centsEnd = A + s * xEnd;
                res.bpmStart = bref * std::pow (2.0, xStart / 1200.0);
                res.bpmEnd = bref * std::pow (2.0, xEnd / 1200.0);
            }
        }
    }

    // A track whose tempo stays put is reported at the whole-track tempo estimate (see the regions below for why not at the tempogram's median level).
    if (haveTempo && ! res.tempoFromMap && (verdict.behaviour == TempoBehaviour::Constant || verdict.behaviour == TempoBehaviour::PitchMoves))
    {
        const auto est = detail::estimateTempo (mono, n, sr);
        if (est.valid && (res.medianBpm <= 0.0 || std::fabs (wrapOctave (1200.0 * std::log2 ((double) est.bpm / res.medianBpm))) > 30.0))
            res.medianBpm = res.minBpm = res.maxBpm = res.bpmStart = res.bpmEnd = (double) est.bpm;
    }

    // A varispeed track is ONE tempo moving a few percent, so if the whole-track estimate (octave-agnostic, the one checked against the tracker) differs from the
    // tempogram's level by far more than a ramp can explain, and by a simple ratio, the tempogram counted the pulse at a related metrical level (4:5 of the
    // tempo, say): move the whole tempo curve to the level the estimate knows. (Not done for tracks that switch beat: their tempi really are different.)
    if (verdict.behaviour == TempoBehaviour::Varispeed && bref > 0.0 && ! res.tempoFromMap)
    {
        const auto est = detail::estimateTempo (mono, n, sr);
        if (est.valid)
        {
            double r = (double) est.bpm / bref;
            while (r >= std::sqrt (2.0)) r *= 0.5;
            while (r < 1.0 / std::sqrt (2.0)) r *= 2.0;
            if (std::fabs (std::log (r)) > 0.12)
            {
                static const double ratios[] = { 6.0 / 5, 5.0 / 4, 4.0 / 3, 3.0 / 2, 5.0 / 3, 8.0 / 5, 7.0 / 4, 7.0 / 6 };
                double best = 0.0, bestErr = 1e9;
                for (double q : ratios)
                    for (double f : { q, 1.0 / q })
                    {
                        double g = f;
                        while (g >= std::sqrt (2.0)) g *= 0.5;
                        while (g < 1.0 / std::sqrt (2.0)) g *= 2.0;
                        const double err = std::fabs (std::log (r / g));
                        if (err < bestErr) { bestErr = err; best = g; }
                    }
                if (best > 0.0 && bestErr < 0.04)
                {
                    for (auto& c : res.curve) if (c.tempoValid) c.bpm *= best;
                    bref *= best;
                    res.medianBpm *= best; res.minBpm *= best; res.maxBpm *= best; res.bpmStart *= best; res.bpmEnd *= best;
                }
            }
        }
    }

    // varispeed: where would the track sit on A440?
    const bool varispeed = verdict.behaviour == TempoBehaviour::Varispeed;
    if (varispeed && bref > 0.0)
    {
        const double zero = bref * std::pow (2.0, -wrap100 (verdict.offset1) / 1200.0);     // the zero-offset tempo nearest to the median tempo
        std::vector<double> cands;
        for (int k = -36; k <= 36; ++k)
        {
            const double b = zero * std::pow (2.0, k / 12.0);
            if (b >= res.minBpm * 0.97 && b <= res.maxBpm * 1.03) cands.push_back (b);
        }
        double start = res.bpmStart > 0.0 ? res.bpmStart : bref, best = zero;
        double bestD = 1e300;
        for (int k = -36; k <= 36; ++k)
        {
            const double b = zero * std::pow (2.0, k / 12.0), dist = std::fabs (std::log (b / start));
            if (dist < bestD) { bestD = dist; best = b; }
        }
        res.referenceBpm = best;
        std::sort (cands.begin(), cands.end());
        res.zeroBpms = cands;
        if (opt.referenceBpm > 0.0) res.detuneAtReference = wrap100 (verdict.offset1 + 1200.0 * std::log2 (opt.referenceBpm / bref));
    }

    // 5. boundaries
    // pitch residual: raw detune, or detune with the varispeed trend removed
    std::vector<SegPoint> pitchPts, tempoPts;
    for (const auto& c : res.curve)
    {
        if (! c.detuneValid) continue;
        if (varispeed && ! c.tempoValid) continue;     // the trend cannot be removed where the tempo is unknown
        const double e = c.unwrapped - (varispeed ? verdict.slope * c.tempoCents : 0.0);
        pitchPts.push_back ({ c.time, e, std::max ({ 1.0, c.ci / 1.96, 0.8 * verdict.pitchNoise }) });
    }
    for (const auto& c : res.curve)
        if (c.tempoValid) tempoPts.push_back ({ c.time, c.tempoCents, std::max (6.0, verdict.tempoNoise) });
    // a step of s cents needs to be several times larger than the noise to count; windows overlap, so thresholds are generous
    const auto pitchSteps = findSteps (pitchPts, 60.0, std::max (4.0, 3.0 * verdict.pitchNoise), 4, 5, 0);
    const double tempoJumpMin = std::max (40.0, 5.0 * std::max (6.0, verdict.tempoNoise));
    const auto tempoSteps = findSteps (tempoPts, 60.0, tempoJumpMin, 5, 5, 2);

    struct Cand { double t, strength; bool pitch, tempo; double jump, before, after; double z = 0.0; int passage = 0; };    // passage: 1 = a passage begins here, 2 = it ends here
    std::vector<Cand> cands;
    for (const auto& s : pitchSteps) cands.push_back ({ s.time, s.gain, true, false, s.jump, 0.0, 0.0 });
    if (! tempoSteps.empty() && ! res.tempoFromMap && env.valid)
    {
        beats = tempodetail::trackBeats (env, tt);
    }
    for (const auto& s : tempoSteps)
    {
        double t = s.time;
        if (! res.tempoFromMap && ! beats.empty()) t = refineTempoStep (beats, t, 5.0, t);
        const double before = haveTempo ? (res.tempoFromMap ? opt.tempoMap->bpmAt (t - 2.0) : tt.bpmAt (t - 4.0)) : 0.0;
        const double after = haveTempo ? (res.tempoFromMap ? opt.tempoMap->bpmAt (t + 2.0) : tt.bpmAt (t + 4.0)) : 0.0;
        cands.push_back ({ t, s.gain, false, true, 0.0, before, after });
    }
    // More places to look, from the evidence itself rather than from a template: where the pattern of the tonal evidence changes (a new instrument, or the peaks
    // moving). The windows share one template, and a part of the track whose instruments the template fits badly can read half a semitone off, which hides a
    // step from the curve; the plateaus around these candidates are fitted with templates of their own below, and only real differences of tuning survive.
    std::vector<EvidenceChange> changeSeries;
    double changeThreshold = 1e9, snapThreshold = 1e9;
    if (! varispeed && dc.fit)
    {
        const auto change = evidenceChange (fe, 0.0, res.seconds);
        changeSeries = change;
        if (change.size() > 8)
        {
            std::vector<double> v, ad;
            for (const auto& e : change) v.push_back (e.change);
            const double med = medianOf (v);
            for (double x : v) ad.push_back (std::fabs (x - med));
            const double threshold = std::max (0.2, med + 4.0 * 1.4826 * medianOf (ad));
            changeThreshold = threshold;
            snapThreshold = med + 1.5 * 1.4826 * medianOf (ad);       // a tempo step snaps to a clear local peak of the change, not necessarily an outlier of the track
            std::vector<EvidenceChange> peaks;
            for (std::size_t i = 1; i + 1 < change.size(); ++i)
                if (change[i].change >= threshold && change[i].change >= change[i - 1].change && change[i].change >= change[i + 1].change) peaks.push_back (change[i]);
            std::sort (peaks.begin(), peaks.end(), [] (const EvidenceChange& x, const EvidenceChange& y) { return x.change > y.change; });
            std::vector<double> taken;
            for (const auto& e : peaks)
            {
                bool near = false;
                for (double t : taken) near = near || std::fabs (t - e.time) < 6.0;
                if (near || taken.size() >= 12) continue;
                taken.push_back (e.time);
                cands.push_back ({ e.time, 1000.0 * e.change, true, false, 0.0, 0.0, 0.0 });
            }
        }
    }
    std::sort (cands.begin(), cands.end(), [] (const Cand& a, const Cand& b) { return a.t < b.t; });
    std::vector<Cand> merged;
    for (const auto& c : cands)
    {
        if (! merged.empty() && c.t - merged.back().t < 3.0)
        {
            auto& m = merged.back();
            if (c.pitch && ! m.pitch) m.t = c.t;                 // the pitch step is located more precisely
            m.pitch = m.pitch || c.pitch; m.tempo = m.tempo || c.tempo;
            m.strength += c.strength;
            if (c.pitch) m.jump = c.jump;
            if (c.tempo) { m.before = c.before; m.after = c.after; }
        }
        else merged.push_back (c);
    }
    // A stretch of the track fitted on its own: the template is learned from that stretch (the track's other parts may be other instruments, and the template
    // of the whole track can then lock half a semitone off on them), the evidence is all of its frames. Cached: the checks below ask for the same stretches.
    std::map<std::pair<long long, long long>, StretchFit> plateauCache;
    bool stopped = false;                       // the caller cancelled while the checks were running
    auto plateau = [&] (double a, double b) -> StretchFit
    {
        const auto key = std::make_pair ((long long) std::llround (a * 20.0), (long long) std::llround (b * 20.0));
        const auto it = plateauCache.find (key);
        if (it != plateauCache.end()) return it->second;
        if (stopped) return StretchFit {};
        if (! report (0.7, "checking boundaries")) { stopped = true; return StretchFit {}; }
        const auto r = fitStretch (fe, a, b, nullptr, std::max (12.0, 0.2 * dc.medianMass));
        plateauCache.emplace (key, r);
        return r;
    };

    // Every suggestion is checked against evidence that is not the windows it was found in, and the weakest failing one is dropped until all pass:
    //  * detune step: the pooled evidence of the plateau on each side (frames, not windows) must differ by more than twice the combined 95 % interval
    //    and by 8 cents;
    //  * tempo step: the whole-stretch tempo on each side must differ by at least 2.6 %;
    //  * plateaus: a part that lasts less than 10 s (8 s at either end of the track) is a passage, not a new beat. A dip that comes back counts twice
    //    against it, so both of its boundaries go.
    // A tempo map the caller gave is taken as given.
    {
        const double kMinPlateau = 10.0, kMinEdge = 8.0;
        auto neighbourTimes = [&] (std::size_t i, bool pitchKind, double& tl, double& tr, bool& leftEdge, bool& rightEdge)
        {
            tl = 0.0; tr = res.seconds; leftEdge = rightEdge = true;
            for (std::size_t j = i; j-- > 0;) if (pitchKind ? merged[j].pitch : merged[j].tempo) { tl = merged[j].t; leftEdge = false; break; }
            for (std::size_t j = i + 1; j < merged.size(); ++j) if (pitchKind ? merged[j].pitch : merged[j].tempo) { tr = merged[j].t; rightEdge = false; break; }
        };
        auto drop = [&] (std::size_t i, bool pitchKind)
        {
            if (pitchKind) { merged[i].pitch = false; merged[i].jump = 0.0; merged[i].z = 0.0; }
            else merged[i].tempo = false;
            if (! merged[i].pitch && ! merged[i].tempo) merged.erase (merged.begin() + (long) i);
        };
        for (int guard = 0; guard < 200; ++guard)
        {
            bool dropped = false;
            // ---- detune steps
            {
                int worst = -1;
                double worstScore = 1e300;
                for (std::size_t i = 0; i < merged.size(); ++i)
                {
                    if (! merged[i].pitch) continue;
                    auto& m = merged[i];
                    double tl, tr; bool le, re;
                    neighbourTimes (i, true, tl, tr, le, re);
                    bool ok = (m.t - tl >= (le ? kMinEdge : kMinPlateau)) && (tr - m.t >= (re ? kMinEdge : kMinPlateau));
                    double z = 0.0;
                    if (ok && ! varispeed && dc.fit)
                    {
                        const auto L = plateau (std::max (tl + 0.5, m.t - 40.0), m.t - 0.5);
                        const auto R = plateau (m.t + 0.5, std::min (tr - 0.5, m.t + 40.0));
                        const double jump = L.valid && R.valid ? wrap100 (R.cents - L.cents) : 0.0, comb = L.valid && R.valid ? std::max (0.3, std::hypot (L.ci, R.ci)) : 1.0;
                        z = L.valid && R.valid ? std::fabs (jump) / comb : 0.0;
                        ok = L.valid && R.valid && std::fabs (jump) >= std::max (8.0, 2.0 * comb);
                        if (L.valid && R.valid) m.jump = jump;
                        m.z = z;
                    }
                    else if (ok)
                        z = std::fabs (m.jump);
                    if (! ok && z < worstScore) { worst = (int) i; worstScore = z; }
                }
                if (worst >= 0) { drop ((std::size_t) worst, true); dropped = true; }
            }
            if (dropped) continue;
            // ---- tempo steps
            if (haveTempo && ! res.tempoFromMap)
            {
                int worst = -1;
                double worstScore = 1e300;
                for (std::size_t i = 0; i < merged.size(); ++i)
                {
                    if (! merged[i].tempo) continue;
                    auto& m = merged[i];
                    double tl, tr; bool le, re;
                    neighbourTimes (i, false, tl, tr, le, re);
                    bool ok = (m.t - tl >= (le ? kMinEdge : kMinPlateau)) && (tr - m.t >= (re ? kMinEdge : kMinPlateau));
                    double score = 0.0;
                    if (ok)
                    {
                        auto stretchTempo = [&] (double a, double b) -> double
                        {
                            const std::size_t i0 = (std::size_t) std::llround (std::max (0.0, a) * sr), i1 = std::min (n, (std::size_t) std::llround (b * sr));
                            if (i1 < i0 + (std::size_t) (9.0 * sr)) return 0.0;
                            const auto t = detail::estimateTempo (mono + i0, i1 - i0, sr);
                            return t.valid ? (double) t.bpm : 0.0;
                        };
                        const double bL = stretchTempo (std::max (tl + 1.5, m.t - 40.0), m.t - 1.5), bR = stretchTempo (m.t + 1.5, std::min (tr - 1.5, m.t + 40.0));
                        ok = bL > 0.0 && bR > 0.0;
                        if (ok)
                        {
                            const double c = wrapOctave (1200.0 * std::log2 (bR / bL)), a = std::fabs (c);
                            score = a;
                            ok = a >= 45.0;
                        }
                    }
                    if (! ok && score < worstScore) { worst = (int) i; worstScore = score; }
                }
                if (worst >= 0) { drop ((std::size_t) worst, false); dropped = true; }
            }
            if (! dropped) break;
        }
    }
    // Passages: a part shorter than 25 s that sits at another detune and then comes back to where the song was (a vocal-only verse, a breakdown, a drop with
    // a different instrument). The evidence for it is real, but it is not a switch to a new beat: both of its boundaries are marked as a passage, and the
    // verdict ignores them.
    if (! varispeed && dc.fit)
    {
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < merged.size(); ++i) if (merged[i].pitch) idx.push_back (i);
        for (std::size_t k = 0; k + 1 < idx.size(); ++k)
        {
            auto& a = merged[idx[k]];
            auto& b = merged[idx[k + 1]];
            if (a.passage || b.t - a.t >= 25.0) continue;
            const double before = k > 0 ? merged[idx[k - 1]].t : 0.0, after = k + 2 < idx.size() ? merged[idx[k + 2]].t : res.seconds;
            const auto L = plateau (std::max (before + 0.5, a.t - 40.0), a.t - 0.5);
            const auto R = plateau (b.t + 0.5, std::min (after - 0.5, b.t + 40.0));
            if (! L.valid || ! R.valid) continue;
            if (std::fabs (wrap100 (R.cents - L.cents)) < std::max (4.0, 2.0 * std::hypot (L.ci, R.ci))) { a.passage = 1; b.passage = 2; }
        }
    }
    // Detune steps are placed to a frame (the windows around a step are mixed): the split that makes the frames fit best, the left ones under the template of
    // the plateau on their side and the right ones under the other's.
    if (! varispeed)
    {
        for (std::size_t i = 0; i < merged.size(); ++i)
        {
            if (! merged[i].pitch) continue;
            auto& m = merged[i];
            double tl = 0.0, tr = res.seconds;
            for (std::size_t j = i; j-- > 0;) if (merged[j].pitch) { tl = merged[j].t; break; }
            for (std::size_t j = i + 1; j < merged.size(); ++j) if (merged[j].pitch) { tr = merged[j].t; break; }
            const auto L = plateau (std::max (tl + 0.5, m.t - 40.0), m.t - 0.5);
            const auto R = plateau (m.t + 0.5, std::min (tr - 0.5, m.t + 40.0));
            if (L.valid && R.valid && L.fit && R.fit)
                m.t = refineSplit (fe, *L.fit, L.bin, *R.fit, R.bin, std::max (tl + 1.0, m.t - 4.0), std::min (tr - 1.0, m.t + 4.0), m.t);
        }
    }
    // A tempo step is located from the tempogram, whose windows are 6 s long: where the pattern of the evidence changes clearly right next to it (a new beat
    // changes the instruments too), take that, which is sharper. Boundaries that are now within 5 s of each other are one boundary; the detune step's time,
    // which is placed to a frame, wins.
    if (! varispeed && ! changeSeries.empty())
        for (auto& m : merged)
        {
            if (! m.tempo || m.pitch) continue;
            double best = 0.0, at = m.t;
            for (const auto& e : changeSeries) if (std::fabs (e.time - m.t) <= 4.0 && e.change > best) { best = e.change; at = e.time; }
            if (best >= snapThreshold) m.t = at;
        }
    std::sort (merged.begin(), merged.end(), [] (const Cand& a, const Cand& b) { return a.t < b.t; });
    for (std::size_t i = 1; i < merged.size();)
    {
        auto& a = merged[i - 1];
        auto& b = merged[i];
        if (b.t - a.t < 5.0 && a.passage == 0 && b.passage == 0)
        {
            if (b.pitch && ! a.pitch) a.t = b.t;
            a.pitch = a.pitch || b.pitch;
            a.tempo = a.tempo || b.tempo;
            a.strength += b.strength;
            if (b.pitch) { a.jump = b.jump; a.z = b.z; }
            if (b.tempo) { a.before = b.before; a.after = b.after; }
            merged.erase (merged.begin() + (long) i);
        }
        else ++i;
    }
    if (stopped) { res.cancelled = true; res.valid = false; return res; }
    for (const auto& m : merged)
    {
        SuggestedBoundary b;
        b.passageStart = m.passage == 1;
        b.passageEnd = m.passage == 2;
        b.time = m.t;
        b.strength = m.strength;
        b.detuneJump = m.pitch;
        b.tempoStep = m.tempo;
        b.jumpCents = m.jump;
        b.significance = m.z;
        b.bpmBefore = m.before;
        b.bpmAfter = m.after;
        std::string why;
        if (m.pitch) why = "the detune jumps " + centsText (m.jump) + " cents" + (m.passage == 1 ? " (a passage at another detune begins)" : m.passage == 2 ? " (back from a passage)" : "");
        if (m.tempo)
        {
            if (! why.empty()) why += " and ";
            why += "the tempo steps " + bpmText (m.before) + " -> " + bpmText (m.after) + " BPM";
        }
        b.reason = why;
        res.boundaries.push_back (b);
    }

    // The verdict for a track whose tempo stays put rests on what survived the checks: validated detune steps, or a drift of the detune across the track.
    // (A scatter of a few cents between the sections of an ordinary mix is not a movement of the pitch.)
    if (verdict.behaviour == TempoBehaviour::PitchMoves || verdict.behaviour == TempoBehaviour::Constant)
    {
        int steps = 0;
        for (const auto& m : merged) steps += m.pitch && m.passage == 0;
        std::vector<double> firstQ, lastQ;
        {
            std::vector<double> u;
            for (const auto& c : res.curve) if (c.detuneValid) u.push_back (c.unwrapped);
            const std::size_t q = u.size() / 4;
            if (q >= 3) { firstQ.assign (u.begin(), u.begin() + (long) q); lastQ.assign (u.end() - (long) q, u.end()); }
        }
        const bool drift = ! firstQ.empty() && std::fabs (medianOf (lastQ) - medianOf (firstQ)) >= std::max (10.0, 4.0 * verdict.pitchNoise);
        res.behaviour = (steps > 0 || drift) ? TempoBehaviour::PitchMoves : TempoBehaviour::Constant;
    }

    // 6. regions
    std::vector<double> edges { 0.0 };
    {
        std::vector<double> src;
        if (! opt.boundaries.empty()) src = opt.boundaries;
        else for (const auto& b : res.boundaries) src.push_back (b.time);
        std::sort (src.begin(), src.end());
        for (double b : src)
            if (b > edges.back() + 4.0 && b < res.seconds - 4.0) edges.push_back (b);
        edges.push_back (res.seconds);
    }
    const int regionCount = (int) edges.size() - 1;
    for (int r = 0; r < regionCount; ++r)
    {
        Region reg;
        reg.start = edges[(size_t) r];
        reg.end = edges[(size_t) r + 1];
        std::vector<Obs> ob;
        std::vector<double> dv, wv, bp, uv, tv;
        for (const auto& c : res.curve)
        {
            const double a = c.time - 0.5 * c.window, b = c.time + 0.5 * c.window;
            if (a < reg.start - 1e-9 || b > reg.end + 1e-9) continue;       // windows that cross a boundary mix two parts
            if (c.detuneValid)
            {
                const double sg = std::max (0.7, c.ci / 1.96);
                dv.push_back (c.cents); wv.push_back (1.0 / (sg * sg + 9.0)); uv.push_back (c.unwrapped); tv.push_back (c.time);
                if (c.tempoValid)
                {
                    Obs o;
                    o.t = c.time; o.d = c.cents; o.u = c.unwrapped; o.x = c.tempoCents; o.sigma = sg; o.w = 1.0 / (sg * sg + 9.0);
                    ob.push_back (o);
                }
            }
            if (c.tempoValid) bp.push_back (c.bpm);
        }
        reg.windows = (int) dv.size();
        reg.ok = reg.windows >= 3;
        if (reg.ok)
        {
            const auto cs = circularStats (dv, wv);
            reg.cents = cs.mean;
            reg.ci = std::max (0.05, 1.96 * cs.sd / std::sqrt (std::max (1.0, reg.windows / 3.0)));
            // moving pitch: straight-line fit of the unwrapped detune
            const auto pst = pitchStats (uv);
            reg.pitchMoves = pst.range >= std::max (10.0, 4.0 * pst.noise);
            if (reg.pitchMoves && tv.size() >= 2)
            {
                double mt = 0, mu = 0;
                for (std::size_t i = 0; i < tv.size(); ++i) { mt += tv[i]; mu += uv[i]; }
                mt /= (double) tv.size(); mu /= (double) tv.size();
                double sxx = 0, sxy = 0;
                for (std::size_t i = 0; i < tv.size(); ++i) { sxx += (tv[i] - mt) * (tv[i] - mt); sxy += (tv[i] - mt) * (uv[i] - mu); }
                const double slope = sxx > 0 ? sxy / sxx : 0.0;
                reg.centsStart = mu + slope * (reg.start - mt);
                reg.centsEnd = mu + slope * (reg.end - mt);
            }
            else
            {
                reg.centsStart = reg.centsEnd = reg.cents;
                if (reg.end - reg.start >= 2.0 * opt.windowSeconds)
                {
                    // a steady region: all its frames together, against a template learned from the region itself, give a sharper and truer reading than the
                    // mean of its windows (which share the template of the whole track)
                    const auto sf = plateau (reg.start + 0.5, reg.end - 0.5);
                    if (sf.valid)
                    {
                        const double offset = wrap100 (sf.cents - reg.cents);
                        reg.cents = sf.cents; reg.ci = std::max (0.05, sf.ci); reg.centsStart = reg.centsEnd = sf.cents;
                        if (std::fabs (offset) >= 1.0)
                        {
                            // bring the windows of this region into the same frame, so that the curve, the tuning curve the key is measured against and the
                            // region agree
                            for (auto& c : res.curve)
                                if (c.detuneValid && c.time >= reg.start && c.time < reg.end) { c.cents = wrap100 (c.cents + offset); c.unwrapped += offset; }
                            for (auto& v : dv) v = wrap100 (v + offset);
                            for (auto& v : uv) v += offset;
                            for (auto& o : ob) { o.d = wrap100 (o.d + offset); o.u += offset; }
                        }
                    }
                }
            }
        }
        if (! bp.empty())
        {
            reg.bpm = medianOf (bp);
            reg.bpmMin = *std::min_element (bp.begin(), bp.end());
            reg.bpmMax = *std::max_element (bp.begin(), bp.end());
            reg.tempoMoves = 1200.0 * std::log2 (reg.bpmMax / reg.bpmMin) > 30.0;
        }
        if (! res.tempoFromMap && ! reg.tempoMoves && reg.ok && reg.end - reg.start >= 12.0)
        {
            // a steady region's tempo is the whole-stretch estimate (octave-agnostic, checked against the tracker at 98 %), not the median of tempogram windows,
            // which can sit on a related metrical level for a whole part (4:5 of the tempo, say)
            const std::size_t i0 = (std::size_t) std::llround (reg.start * sr), i1 = std::min (n, (std::size_t) std::llround (reg.end * sr));
            const auto est = detail::estimateTempo (mono + i0, i1 - i0, sr);
            if (est.valid && (reg.bpm <= 0.0 || std::fabs (wrapOctave (1200.0 * std::log2 ((double) est.bpm / reg.bpm))) > 30.0))
                reg.bpm = reg.bpmMin = reg.bpmMax = (double) est.bpm;
        }
        if (ob.size() >= 6) reg.behaviour = classify (ob, overlapOf (res.curve, opt.hopSeconds)).behaviour;
        else if (reg.ok) reg.behaviour = reg.pitchMoves ? TempoBehaviour::PitchMoves : TempoBehaviour::Constant;
        res.regions.push_back (reg);
    }

    if (stopped) { res.cancelled = true; res.valid = false; return res; }
    // keys: measured against the tuning curve (in its final frame, after the regions recentred their windows)
    if (opt.regionKeys)
    {
        keydetail::TuningCurve tcurve;
        for (const auto& c : res.curve) if (c.detuneValid) { tcurve.t.push_back (c.time); tcurve.cents.push_back (c.cents); }
        for (int r = 0; r < regionCount; ++r)
        {
            auto& reg = res.regions[(size_t) r];
            if (! reg.ok || reg.end - reg.start < 6.0) continue;
            if (! report (0.7 + 0.3 * (double) r / (double) regionCount, "key")) { res.cancelled = true; res.valid = false; return res; }
            const double a = reg.start + (r > 0 ? 0.75 : 0.0), b = reg.end - (r < regionCount - 1 ? 0.75 : 0.0);
            const std::size_t i0 = (std::size_t) std::llround (a * sr), i1 = std::min (n, (std::size_t) std::llround (b * sr));
            if (i1 > i0 + (std::size_t) (4.0 * sr))
            {
                keydetail::KeyFeatureOptions ko;
                ko.tuningCurve = tcurve.t.empty() ? nullptr : &tcurve;
                ko.startTime = a;
                reg.key = detail::estimateKey (mono + i0, i1 - i0, sr, reg.cents, ko);
            }
        }
    }

    // 7. words
    std::string s;
    const bool haveBpm = res.medianBpm > 0.0;
    switch (res.behaviour)
    {
        case TempoBehaviour::Varispeed:
            s = "Varispeed: the tempo runs " + bpmText (res.bpmStart) + " -> " + bpmText (res.bpmEnd) + " BPM and the pitch follows it (" + centsText (res.centsStart) + " -> "
                + centsText (res.centsEnd) + " cents; 1200 x log2 (bpm / bpm_ref) tracks the detune to within " + fmt ("%.1f", res.spreadVarispeed) + " cents). ";
            if (res.referenceBpm > 0.0)
            {
                s += "The track sits exactly on A440 (0 cents) at " + bpmText (res.referenceBpm) + " BPM";
                std::string others;
                for (double b : res.zeroBpms) if (std::fabs (b - res.referenceBpm) > 0.05) others += (others.empty() ? "" : ", ") + bpmText (b);
                if (! others.empty()) s += " (also at " + others + " BPM: the same source shifted by whole semitones)";
                s += ". ";
            }
            break;
        case TempoBehaviour::TimeStretch:
            s = "Tempo moves, pitch stays: the tempo goes " + bpmText (res.minBpm) + " - " + bpmText (res.maxBpm) + " BPM but the detune stays near " + centsText (wrap100 (res.centsStart))
                + " cents (spread " + fmt ("%.1f", res.spreadFixed) + "), so the pitch does not follow the tempo (a time-stretch, or a tempo change in the music itself). ";
            break;
        case TempoBehaviour::Constant:
            s = haveBpm ? "Steady tempo (" + bpmText (res.medianBpm) + " BPM) and steady pitch (" : "Steady pitch (";
            s += centsText (wrap100 (res.centsStart)) + " cents). ";
            break;
        case TempoBehaviour::PitchMoves:
            s = std::string (haveBpm ? "The tempo is steady (" + bpmText (res.medianBpm) + " BPM) but the pitch moves: " : "The pitch moves: ") + centsText (res.centsStart) + " -> " + centsText (res.centsEnd) + " cents. ";
            break;
        case TempoBehaviour::Mixed:
            s = "Tempo and detune both move (" + bpmText (res.minBpm) + " - " + bpmText (res.maxBpm) + " BPM, detune " + centsText (res.centsStart) + " -> " + centsText (res.centsEnd)
                + " cents) but not in step like a varispeed or a time-stretch would: look at the regions. ";
            break;
        default: s = "Not enough evidence to judge. "; break;
    }
    {
        std::string passages;
        for (std::size_t i = 0; i < res.boundaries.size(); ++i)
        {
            if (! res.boundaries[i].passageStart) continue;
            for (std::size_t j = i + 1; j < res.boundaries.size(); ++j)
                if (res.boundaries[j].passageEnd)
                {
                    passages += (passages.empty() ? "" : "; ") + formatTimestamp (res.boundaries[i].time) + " - " + formatTimestamp (res.boundaries[j].time) + " (" + centsText (res.boundaries[i].jumpCents) + " cents)";
                    break;
                }
        }
        if (! passages.empty()) s += "Short passages at another detune that then come back: " + passages + ". ";
        std::string steps;
        for (const auto& b : res.boundaries)
            if (! b.passageStart && ! b.passageEnd) steps += (steps.empty() ? "" : "; ") + formatTimestamp (b.time) + " (" + b.reason + ")";
        if (! steps.empty()) s += "Suggested boundaries: " + steps + ".";
    }
    res.summary = s;
    report (1.0, "done");
    return res;
}
} // namespace centsible
