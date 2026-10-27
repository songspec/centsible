// Key detection for popular music (trap, rap, pop, rock), in two stages that mirror how such music is actually built:
//
//   1. SCALE (which seven notes?): sustain-gated spectral-peak chroma -> Temperley's key profiles. The scale is what a loop-based track reliably
//      expresses; its relative major/minor pair shares all seven notes. (Benchmarked against Krumhansl-Kessler, a recollection of Shaath's profile and
//      a catalog-learned profile, alone and as ensembles: Temperley alone was best, see REPORT.md.)
//   2. TONIC (which of the seven is "home", major or minor, or a mode such as Phrygian?): the bass. A small softmax over the scale's seven degrees
//      using the pitch classes of the *notes* found by iterative harmonic cancellation (strongest note, subtract its overtones, repeat), which stops
//      a bass note's third harmonic (a fifth above) from being mistaken for a note of its own; plus a prior over degrees. Its few weights were fitted on
//      songs with known keys and are deliberately simple.
//
// The chroma is measured against the detected TUNING (constant, or a tuning curve for songs whose pitch drifts), so a song tuned 40 cents off the grid
// does not smear its notes between pitch classes. Percussion is suppressed by a sustain gate and, optionally, a harmonic/percussive mask; chroma can be
// aggregated per beat. The result carries probabilities for every key so that the runner-up and the relative major/minor ambiguity can be reported.
//
// Offline code (used by analyzeTrack / analyzeSections): it allocates freely and is never called from the audio thread.
#pragma once
#include "fft.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace centsible::keydetail
{
using Chroma = std::array<double, 12>;

struct KeyFeatures
{
    Chroma peaksGated {};   // gated spectral peaks (all registers), per-unit L2-normalised, square-rooted, level-weighted: the SCALE evidence
    Chroma cancBass {};     // notes below 250 Hz found by harmonic cancellation: the BASS / tonic evidence
    Chroma cancGated {};    // all notes found by harmonic cancellation on gated peaks
    int frames = 0;
    double seconds = 0.0;
};

// Tuning over time (a varispeed song drifts; each frame's pitch classes must be measured against ITS OWN reference pitch).
struct TuningCurve
{
    std::vector<double> t, cents;     // sorted times (s), tuning offsets in cents (wrapped, +-50)
    double at (double time) const
    {
        if (t.empty()) return 0.0;
        if (time <= t.front()) return cents.front();
        if (time >= t.back()) return cents.back();
        const auto it = std::upper_bound (t.begin(), t.end(), time);
        const std::size_t j = (std::size_t) (it - t.begin()), i = j - 1;
        double d = cents[j] - cents[i];
        d -= 100.0 * std::floor ((d + 50.0) / 100.0);          // interpolate the short way round the 100-cent circle
        return cents[i] + d * (time - t[i]) / (t[j] - t[i]);
    }
};

struct KeyFeatureOptions
{
    bool sustainGate = true;                 // weight peaks by how well they persist into the neighbouring frames (suppresses transients)
    bool hpss = false;                       // weight peaks by a harmonic mask: median over 7 frames (sustained) vs median over 17 bins (broadband)
    const std::vector<double>* beats = nullptr;   // beat times (s): aggregate chroma per group of beats instead of per frame
    int beatsPerUnit = 1;
    const TuningCurve* tuningCurve = nullptr;     // overrides the constant tuning when given
    double startTime = 0.0;                  // time of sample 0 on the clock the beats / tuning curve use
};

// ---- feature extraction ------------------------------------------------------------------------------------------------------
inline KeyFeatures computeKeyFeatures (const float* x, std::size_t n, double sr, double tuningCents, const KeyFeatureOptions& opt = {})
{
    KeyFeatures out;
    int order = 4;
    while ((double) (1 << order) < 0.34 * sr) ++order;
    const int N = 1 << order, hop = N / 2;
    if (n < (std::size_t) N * 4) return out;
    const double binHz = sr / N;
    const int kLo = (int) std::ceil (28.0 / binHz), kHi = std::min (N / 2 - 3, (int) (5000.0 / binHz));
    Fft fft (order);
    std::vector<float> window ((size_t) N);
    for (int i = 0; i < N; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
    std::vector<std::complex<float>> buf ((size_t) N);
    const int F = (int) ((n - (std::size_t) N) / (std::size_t) hop) + 1;
    auto magnitudes = [&] (int f, std::vector<float>& m)
    {
        const std::size_t pos = (std::size_t) f * (std::size_t) hop;
        for (int i = 0; i < N; ++i) buf[(size_t) i] = { x[pos + (std::size_t) i] * window[(size_t) i], 0.0f };
        fft.forward (buf.data());
        m.resize ((size_t) N / 2 + 1);
        for (int k = 0; k <= N / 2; ++k) m[(size_t) k] = std::abs (buf[(size_t) k]);
    };

    struct FrameVec { double level, time; Chroma pg, cb, cg; };
    std::vector<FrameVec> frames;
    // magnitude frames f-3 .. f+4 live in a ring (clamped at the ends): enough for the +-1 frame sustain gate and the 7-frame harmonic median
    constexpr int kRing = 8, kSide = 3;
    std::vector<std::vector<float>> ring ((size_t) kRing);
    std::vector<int> ringFrame ((size_t) kRing, -1);
    auto mag = [&] (int f) -> const std::vector<float>&
    {
        f = std::clamp (f, 0, F - 1);
        const std::size_t slot = (std::size_t) (f % kRing);
        if (ringFrame[slot] != f) { magnitudes (f, ring[slot]); ringFrame[slot] = f; }
        return ring[slot];
    };

    struct Pk { double hz, amp, gamp; int bin; };
    std::vector<Pk> pks;
    std::vector<int> binPeak ((size_t) N / 2 + 1, -1);
    std::vector<double> resA;
    std::vector<float> tmp ((size_t) 2 * kSide + 1), tmpb (17);

    for (int f = 0; f < F; ++f)
    {
        for (int j = f - kSide; j <= f + kSide + 1 && j < F; ++j) (void) mag (j);     // fill the ring ahead of use
        const auto& cur = mag (f);
        const auto& prev = mag (f - 1);
        const auto& next = mag (f + 1);
        const double frameTime = opt.startTime + ((double) f * hop + N / 2) / sr;
        const double refHz = 440.0 * std::pow (2.0, (opt.tuningCurve ? opt.tuningCurve->at (frameTime) : tuningCents) / 1200.0);
        double peak = 0;
        for (int k = kLo; k <= kHi; ++k) peak = std::max (peak, (double) cur[(size_t) k]);
        FrameVec fv { peak, frameTime, {}, {}, {} };
        if (peak > 0)
        {
            auto sustain = [&] (int k)
            {
                const double a = std::max ({ (double) prev[(size_t) k - 1], (double) prev[(size_t) k], (double) prev[(size_t) k + 1] });
                const double b = std::max ({ (double) next[(size_t) k - 1], (double) next[(size_t) k], (double) next[(size_t) k + 1] });
                return std::clamp (std::min (a, b) / std::max (1e-20, (double) cur[(size_t) k]), 0.0, 1.0);
            };
            auto harmonicMask = [&] (int k)       // Hm^2 / (Hm^2 + Pm^2): ~1 for a partial that persists, ~0 for a broadband click
            {
                for (int j = -kSide; j <= kSide; ++j) tmp[(size_t) (j + kSide)] = mag (f + j)[(size_t) k];
                std::nth_element (tmp.begin(), tmp.begin() + kSide, tmp.end());
                const double Hm = tmp[(size_t) kSide];
                for (int j = -8; j <= 8; ++j) tmpb[(size_t) (j + 8)] = cur[(size_t) std::clamp (k + j, 0, N / 2)];
                std::nth_element (tmpb.begin(), tmpb.begin() + 8, tmpb.end());
                const double Pm = tmpb[8];
                return Hm * Hm / (Hm * Hm + Pm * Pm + 1e-30);
            };
            auto gateOf = [&] (int k)
            {
                double g = 1.0;
                if (opt.sustainGate) g *= sustain (k);
                if (opt.hpss) g *= harmonicMask (k);
                return g;
            };
            auto addPc = [&] (Chroma& dst, double hz, double w)
            {
                const double s = 12.0 * std::log2 (hz / refHz) + 9.0, nearest = std::round (s), frac = s - nearest;
                dst[(size_t) (((int) nearest % 12 + 12) % 12)] += w * std::max (0.0, 1.0 - 2.0 * std::fabs (frac));
            };
            pks.clear();
            for (int k = kLo; k <= kHi; ++k)
            {
                const double mk = cur[(size_t) k];
                if (mk < 0.02 * peak || mk < cur[(size_t) k - 1] || mk < cur[(size_t) k + 1]) continue;
                const double a = std::log (cur[(size_t) k - 1] + 1e-12), b = std::log (mk + 1e-12), c = std::log ((double) cur[(size_t) k + 1] + 1e-12);
                const double den = a - 2 * b + c, d = den < 0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
                const double hz = (k + d) * binHz, gate = gateOf (k);
                if (mk >= 0.03 * peak && hz >= 80.0) addPc (fv.pg, hz, std::log1p (30.0 * mk / peak) * gate);
                pks.push_back ({ hz, mk / peak, mk / peak * gate, k });
            }
            for (std::size_t j = 0; j < pks.size(); ++j) binPeak[(size_t) pks[j].bin] = (int) j;

            // iterative harmonic cancellation, once on plain and once on gated amplitudes
            for (int pass = 0; pass < 2; ++pass)
            {
                resA.assign (pks.size(), 0.0);
                for (std::size_t j = 0; j < pks.size(); ++j) resA[j] = pass == 0 ? pks[j].amp : pks[j].gamp;
                auto at = [&] (double hz, int& bj)     // strongest residual peak within +-40 cents of hz
                {
                    const double kc = hz / binHz, lo = kc * 0.97716, hi = kc * 1.02329;
                    double best = 0; bj = -1;
                    for (int k = std::max (0, (int) std::floor (lo) - 1); k <= std::min (N / 2, (int) std::ceil (hi) + 1); ++k)
                    {
                        const int j = binPeak[(size_t) k];
                        if (j < 0) continue;
                        if (std::fabs (1200.0 * std::log2 (pks[(size_t) j].hz / hz)) < 40.0 && resA[(size_t) j] > best) { best = resA[(size_t) j]; bj = j; }
                    }
                    return best;
                };
                double first = 0;
                for (int note = 0; note < 8; ++note)
                {
                    int bp = -1; double bs = 0;
                    for (int p = 24; p <= 90; ++p)
                    {
                        const double fp = refHz * std::pow (2.0, (p - 69) / 12.0);
                        double sc = 0;
                        for (int h = 1; h <= 10; ++h) { const double fh = fp * h; if (fh > 5000.0) break; int j; sc += std::pow (0.88, h - 1) * at (fh, j); }
                        if (sc > bs) { bs = sc; bp = p; }
                    }
                    if (bp < 0) break;
                    if (note == 0) first = bs;
                    if (bs < 0.12 * first) break;
                    const double fp = refHz * std::pow (2.0, (bp - 69) / 12.0);
                    double wsum = 0;
                    for (int h = 1; h <= 10; ++h) { if (fp * h > 5000.0) break; wsum += std::pow (0.88, h - 1); }
                    const double gain = bs / wsum;
                    for (int h = 1; h <= 10; ++h)
                    {
                        const double fh = fp * h;
                        if (fh > 5000.0) break;
                        int j; at (fh, j);
                        if (j >= 0) resA[(size_t) j] = std::max (0.0, resA[(size_t) j] - gain * std::pow (0.88, h - 1));
                    }
                    const double act = std::log1p (30.0 * bs / first) / std::log1p (30.0);
                    if (pass == 0) { if (fp < 250.0) fv.cb[(size_t) (bp % 12)] += act; }
                    else fv.cg[(size_t) (bp % 12)] += act;
                }
            }
            for (const auto& p : pks) binPeak[(size_t) p.bin] = -1;
        }
        frames.push_back (fv);
    }

    double maxLevel = 0;
    for (const auto& fv : frames) maxLevel = std::max (maxLevel, fv.level);
    if (maxLevel <= 0) return out;
    auto addNormalised = [] (Chroma& acc, const Chroma& c, double w, double power)
    {
        double nrm = 0;
        for (double v : c) nrm += v * v;
        if (nrm <= 0) return;
        nrm = std::sqrt (nrm);
        for (int i = 0; i < 12; ++i) acc[(size_t) i] += w * (power == 1.0 ? c[(size_t) i] / nrm : std::pow (c[(size_t) i] / nrm, power));
    };
    const bool beatSync = opt.beats != nullptr && opt.beats->size() > 4;
    if (! beatSync)
    {
        for (const auto& fv : frames)
        {
            if (fv.level <= 0.02 * maxLevel) continue;
            const double w = std::sqrt (fv.level / maxLevel);
            addNormalised (out.peaksGated, fv.pg, w, 0.5);
            addNormalised (out.cancBass, fv.cb, w, 1.0);
            addNormalised (out.cancGated, fv.cg, w, 1.0);
            ++out.frames;
        }
    }
    else
    {
        // beat-synchronous: frames are pooled per group of beats (their pre-normalisation vectors summed), then each unit is treated like one frame
        const auto& beats = *opt.beats;
        const int per = std::max (1, opt.beatsPerUnit);
        const int units = (int) ((beats.size() - 1) / (size_t) per);
        struct Unit { double level = 0; int count = 0; Chroma pg {}, cb {}, cg {}; };
        std::vector<Unit> us ((size_t) std::max (1, units));
        std::size_t b = 0;
        for (const auto& fv : frames)
        {
            const double t = fv.time;
            while (b + (size_t) per < beats.size() && beats[b + (size_t) per] <= t) b += (size_t) per;
            if (t < beats.front() || t >= beats[std::min (beats.size() - 1, b + (size_t) per)]) continue;
            const std::size_t u = std::min (us.size() - 1, b / (size_t) per);
            auto& unit = us[u];
            unit.level += fv.level; ++unit.count;
            for (int i = 0; i < 12; ++i) { unit.pg[(size_t) i] += fv.pg[(size_t) i]; unit.cb[(size_t) i] += fv.cb[(size_t) i]; unit.cg[(size_t) i] += fv.cg[(size_t) i]; }
        }
        for (const auto& unit : us)
        {
            if (unit.count == 0) continue;
            const double level = unit.level / unit.count;
            if (level <= 0.02 * maxLevel) continue;
            const double w = std::sqrt (level / maxLevel);
            addNormalised (out.peaksGated, unit.pg, w, 0.5);
            addNormalised (out.cancBass, unit.cb, w, 1.0);
            addNormalised (out.cancGated, unit.cg, w, 1.0);
            ++out.frames;
        }
    }
    out.seconds = (double) n / sr;
    return out;
}

// ---- decision --------------------------------------------------------------------------------------------------------------
inline constexpr std::array<double, 12> kTemperleyMajor { 5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0 };
inline constexpr std::array<double, 12> kTemperleyMinor { 5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0 };
inline constexpr int kMajorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };       // degrees I ii iii IV V vi vii
inline constexpr int kDegreeMinor[7] = { 0, 1, 1, 0, 0, 1, 1 };       // the mode a song "in" that degree is called: I, IV, V major; ii, iii, vi, vii minor

// Stage-2 weights (fitted by tools/key_train.py); the defaults are what shipped.
struct KeyModel
{
    double wBass = 0.0, wNotes = 0.0, wMajorMinor = 0.0;
    std::array<double, 7> degreeBias {};
};

struct KeyDecision
{
    bool valid = false;
    int scale = 0;                 // root of the major scale holding the song's notes (its relative minor is scale + 9)
    int degree = 0;                // 0..6: which degree of that scale is the tonic
    int tonic = 0;
    bool minor = false;
    double scaleConfidence = 0.0;  // 0..1
    double tonicConfidence = 0.0;  // 0..1
    std::array<double, 24> profileScores {};
    std::array<double, 7> modeProb {};   // the seven modes of the chosen scale (index = degree of the tonic: Ionian, Dorian, Phrygian, Lydian, Mixolydian, Aeolian, Locrian), summing to 1
    std::array<double, 24> keyProb {};   // probability of every key: index tonic (0-11 major, 12-23 minor), summed over the scales / modes that give that name
};

inline double pearson12 (const Chroma& a, const std::array<double, 12>& b)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; ++i) { ma += a[(size_t) i]; mb += b[(size_t) i]; }
    ma /= 12; mb /= 12;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < 12; ++i) { const double da = a[(size_t) i] - ma, db = b[(size_t) i] - mb; sab += da * db; saa += da * da; sbb += db * db; }
    return saa > 0 && sbb > 0 ? sab / std::sqrt (saa * sbb) : 0.0;
}

inline KeyDecision decideKey (const KeyFeatures& f, const KeyModel& model)
{
    KeyDecision d;
    double total = 0;
    for (double v : f.peaksGated) total += v;
    if (f.frames < 8 || total <= 0) return d;

    // stage 1: scale = family of the best Temperley key
    std::array<double, 24> sc {};
    for (int t = 0; t < 12; ++t)
    {
        std::array<double, 12> pm {}, pn {};
        for (int pc = 0; pc < 12; ++pc) { pm[(size_t) pc] = kTemperleyMajor[(size_t) ((pc - t + 12) % 12)]; pn[(size_t) pc] = kTemperleyMinor[(size_t) ((pc - t + 12) % 12)]; }
        sc[(size_t) t] = pearson12 (f.peaksGated, pm);
        sc[(size_t) (12 + t)] = pearson12 (f.peaksGated, pn);
    }
    d.profileScores = sc;
    std::array<double, 12> fam {};
    for (int s = 0; s < 12; ++s) fam[(size_t) s] = std::max (sc[(size_t) s], sc[(size_t) (12 + (s + 9) % 12)]);
    const int scale = (int) (std::max_element (fam.begin(), fam.end()) - fam.begin());
    std::array<double, 12> scaleProb {};
    double den = 0;
    for (int s = 0; s < 12; ++s) { scaleProb[(size_t) s] = std::exp (30.0 * (fam[(size_t) s] - fam[(size_t) scale])); den += scaleProb[(size_t) s]; }
    for (auto& p : scaleProb) p /= den;
    d.scaleConfidence = scaleProb[(size_t) scale];

    // stage 2: tonic degree, for every candidate scale (so that the probabilities of ALL keys can be reported, not only the winner's)
    auto norm01 = [] (const Chroma& c) { Chroma o = c; const double m = *std::max_element (c.begin(), c.end()); if (m > 0) for (auto& v : o) v /= m; return o; };
    const Chroma bass = norm01 (f.cancBass), notes = norm01 (f.cancGated);
    for (int s = 0; s < 12; ++s)
    {
        if (scaleProb[(size_t) s] < 1e-4) continue;
        const double mm = sc[(size_t) s] - sc[(size_t) (12 + (s + 9) % 12)];
        std::array<double, 7> s2 {};
        double best = -1e300;
        for (int di = 0; di < 7; ++di)
        {
            const int pc = (s + kMajorScale[di]) % 12;
            double v = model.wBass * bass[(size_t) pc] + model.wNotes * notes[(size_t) pc] + model.degreeBias[(size_t) di];
            if (di == 0) v += model.wMajorMinor * mm;
            if (di == 5) v -= model.wMajorMinor * mm;
            s2[(size_t) di] = v;
            best = std::max (best, v);
        }
        double z = 0;
        for (double v : s2) z += std::exp (v - best);
        for (int di = 0; di < 7; ++di)
        {
            const double p = scaleProb[(size_t) s] * std::exp (s2[(size_t) di] - best) / z;
            d.keyProb[(size_t) (((s + kMajorScale[di]) % 12) + 12 * kDegreeMinor[di])] += p;
        }
        if (s == scale)
        {
            int bd = 0;
            for (int di = 1; di < 7; ++di) if (s2[(size_t) di] > s2[(size_t) bd]) bd = di;
            d.degree = bd;
            d.tonicConfidence = std::exp (s2[(size_t) bd] - best) / z;
            for (int di = 0; di < 7; ++di) d.modeProb[(size_t) di] = std::exp (s2[(size_t) di] - best) / z;
        }
    }
    d.valid = true;
    d.scale = scale;
    d.tonic = (scale + kMajorScale[d.degree]) % 12;
    d.minor = kDegreeMinor[d.degree] == 1;
    return d;
}
} // namespace centsible::keydetail
