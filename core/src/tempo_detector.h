// Tempo evidence for popular music. Offline code (allocates freely, never on the audio thread).
//   * onset envelope: log-compressed spectral flux (optionally split into a low band, where kick / 808 onsets live, and the rest, where hi-hat
//     subdivisions live), detrended;
//   * TempoCurve: autocorrelation of a stretch of that envelope plus a pulse-train score on a BPM grid (the whole-song tempo uses one curve);
//   * trackTempo: a windowed tempogram + Viterbi path = tempo over time (follows ramps and steps; octave jumps are penalised);
//   * trackBeats: dynamic-programming beat tracker whose period follows the tempo track (Ellis 2007, with a time-varying period).
#pragma once
#include "fft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace centsible::tempodetail
{
struct OnsetEnvelope
{
    std::vector<double> env;     // detrended, zero-mean flux per frame
    double fps = 0.0;            // frames per second
    bool valid = false;
};

// band: 0 = whole spectrum (30 Hz - 8 kHz), 1 = low band (30 - 250 Hz), 2 = everything above 250 Hz
inline OnsetEnvelope computeOnsetEnvelope (const float* x, std::size_t n, double sr, int band = 0)
{
    OnsetEnvelope e;
    int order = 4;
    while ((double) (1 << order) < 0.0464 * sr) ++order;
    const int N = 1 << order, hop = std::max (1, (int) std::lround (sr * 0.0058));
    if (n < (std::size_t) (8.0 * sr)) return e;
    Fft fft (order);
    std::vector<float> window ((size_t) N);
    double wsum = 0.0;
    for (int i = 0; i < N; ++i) { window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N)); wsum += window[(size_t) i]; }
    std::vector<std::complex<float>> buf ((size_t) N);
    const double binHz = sr / N;
    int kLo = std::max (1, (int) std::ceil (30.0 / binHz)), kHi = std::min (N / 2 - 1, (int) (8000.0 / binHz));
    if (band == 1) kHi = std::min (kHi, (int) (250.0 / binHz));
    if (band == 2) kLo = std::max (kLo, (int) std::ceil (250.0 / binHz));
    std::vector<double> prev ((size_t) N / 2 + 1, 0.0), env;
    for (std::size_t pos = 0; pos + (std::size_t) N <= n; pos += (std::size_t) hop)
    {
        for (int i = 0; i < N; ++i) buf[(size_t) i] = { x[pos + (std::size_t) i] * window[(size_t) i], 0.0f };
        fft.forward (buf.data());
        double flux = 0.0;
        for (int k = kLo; k <= kHi; ++k)
        {
            const double v = std::log1p (200.0 * 2.0 * std::abs (buf[(size_t) k]) / wsum);
            flux += std::max (0.0, v - prev[(size_t) k]);
            prev[(size_t) k] = v;
        }
        env.push_back (flux);
    }
    const int F = (int) env.size();
    e.fps = sr / hop;
    if (F < (int) (6.0 * e.fps)) return e;
    std::vector<double> out ((size_t) F), csum ((size_t) F + 1, 0.0);
    const int h = std::max (1, (int) (0.2 * e.fps));
    for (int i = 0; i < F; ++i) csum[(size_t) i + 1] = csum[(size_t) i] + env[(size_t) i];
    for (int i = 0; i < F; ++i)
    {
        const int a = std::max (0, i - h), b = std::min (F, i + h + 1);
        out[(size_t) i] = std::max (0.0, env[(size_t) i] - (csum[(size_t) b] - csum[(size_t) a]) / (b - a));
    }
    double mean = 0.0;
    for (double v : out) mean += v;
    mean /= F;
    for (auto& v : out) v -= mean;
    e.env = std::move (out);
    e.valid = true;
    return e;
}

struct TempoCurve
{
    std::vector<double> acf;     // autocorrelation of the onset envelope, lag in frames, normalised to lag 0
    double fps = 0.0;
    int maxLag = 0;
    bool valid = false;

    double at (double lag) const
    {
        const int i = (int) std::floor (lag);
        if (i < 0 || i + 1 > maxLag) return 0.0;
        const double f = lag - i;
        return acf[(size_t) i] * (1.0 - f) + acf[(size_t) i + 1] * f;
    }
    // pulse train at `bpm`: the beat itself counts double (a train at half the tempo is also periodic, but only one already periodic at ONE beat explains the onsets between its pulses)
    double pulse (double bpm) const
    {
        const double period = 60.0 / bpm * fps;
        double s = 0.0, w = 0.0;
        for (int k = 1; k <= 4; ++k)
        {
            if (k * period > maxLag) break;
            const double wk = k == 1 ? 2.0 : 1.0;
            s += wk * at (k * period);
            w += wk;
        }
        return w > 0.0 ? s / w : 0.0;
    }
};

// autocorrelation of env[a, b) (through an FFT, zero padded), normalised by the overlap and by lag 0; lags up to maxLagSeconds (and at most half the stretch)
inline TempoCurve curveOfStretch (const std::vector<double>& env, double fps, int a, int b, double maxLagSeconds = 5.0)
{
    TempoCurve c;
    c.fps = fps;
    const int F = b - a;
    if (F < (int) (2.0 * fps)) return c;
    int po = 4;
    while ((1 << po) < 2 * F) ++po;
    const int P = 1 << po;
    Fft pf (po);
    std::vector<std::complex<float>> pbuf ((size_t) P, { 0.0f, 0.0f });
    double mean = 0.0;
    for (int i = a; i < b; ++i) mean += env[(size_t) i];
    mean /= F;
    for (int i = 0; i < F; ++i) pbuf[(size_t) i] = { (float) (env[(size_t) (a + i)] - mean), 0.0f };
    pf.forward (pbuf.data());
    for (auto& v : pbuf) v = { std::norm (v), 0.0f };
    pf.inverse (pbuf.data());
    c.maxLag = std::min ({ F - 2, (int) (maxLagSeconds * fps), F / 2 });
    c.acf.assign ((size_t) c.maxLag + 2, 0.0);
    for (int l = 0; l <= c.maxLag + 1 && l < F; ++l) c.acf[(size_t) l] = (double) pbuf[(size_t) l].real() / (double) (F - l);
    if (c.acf[0] <= 0.0) return c;
    for (auto& v : c.acf) v /= c.acf[0];
    c.valid = true;
    return c;
}

inline TempoCurve computeTempoCurve (const float* x, std::size_t n, double sr, int band = 0)
{
    const auto e = computeOnsetEnvelope (x, n, sr, band);
    if (! e.valid) return {};
    auto c = curveOfStretch (e.env, e.fps, 0, (int) e.env.size(), 5.0);
    // the whole-song curve may use lags up to 5 s whatever the length (curveOfStretch halves short stretches only)
    return c;
}

// ---- tempo over time -------------------------------------------------------------------------------------------------------------
struct TempoTrack
{
    std::vector<double> t, bpm, z;   // window centres (s), tempo (BPM), how much the chosen pulse train stands out of the window's scores (z score)
    std::vector<char> ambiguous;     // the song's own tempo (the prior) is still nearly as strong in this window as the tempo the path chose: a metrical level, not a change
    bool valid() const { return ! t.empty(); }
    double bpmAt (double time) const
    {
        if (t.empty()) return 0.0;
        if (time <= t.front()) return bpm.front();
        if (time >= t.back()) return bpm.back();
        const auto it = std::upper_bound (t.begin(), t.end(), time);
        const size_t j = (size_t) (it - t.begin()), i = j - 1;
        const double u = (time - t[i]) / (t[j] - t[i]);
        return std::exp (std::log (bpm[i]) * (1.0 - u) + std::log (bpm[j]) * u);
    }
};

// windowed tempogram + Viterbi on a logarithmic BPM grid (about 0.7 % per step, 55 - 210 BPM). A window's emission is the z score of its pulse-train
// scores; moving between tempi costs lambda * (octaves)^2, so ramps and steps are followed but a jump by an octave is not.
// priorAt (time of the window centre -> BPM, 0 = none): a gentle pull towards the tempo the song has AROUND that time (and its octaves): when two metrical
// levels (5:4, 4:3, a triplet feel) score alike the path stays on the local level instead of flipping between them. A real tempo change scores far above
// the prior and still wins. (The prior is local, not the whole song's, because a track that switches beat has two tempi.)
inline TempoTrack trackTempo (const OnsetEnvelope& e, double windowSeconds = 6.0, double hopSeconds = 1.0, double lambda = 100.0, const std::function<double (double)>& priorAt = {},
                              double priorBonus = 1.5, double priorCents = 60.0)
{
    TempoTrack tr;
    if (! e.valid) return tr;
    const int F = (int) e.env.size(), W = (int) std::lround (windowSeconds * e.fps), H = std::max (1, (int) std::lround (hopSeconds * e.fps));
    if (F < W) return tr;
    constexpr int perOctave = 96;
    const double lo = 55.0, hi = 210.0;
    const int G = (int) std::ceil (std::log2 (hi / lo) * perOctave) + 1;
    std::vector<std::vector<double>> pulse, emis;
    std::vector<double> centre, priors;
    for (int a = 0; a + W <= F; a += H)
    {
        const double priorBpm = priorAt ? priorAt ((a + W * 0.5) / e.fps) : 0.0;
        const auto c = curveOfStretch (e.env, e.fps, a, a + W, windowSeconds * 0.5);
        std::vector<double> p ((size_t) G, 0.0);
        // the same octave-agnostic score as the whole-track tempo (estimateTempo): a pulse train at b is supported by trains at b / 2 and 2 b as well, so a
        // related level (4:5 of the true tempo, say) that only one train supports no longer beats the true one
        if (c.valid) for (int i = 0; i < G; ++i) { const double b = lo * std::pow (2.0, (double) i / perOctave); p[(size_t) i] = c.pulse (0.5 * b) + c.pulse (b) + c.pulse (2.0 * b); }
        double m = 0, s = 0;
        for (double v : p) m += v;
        m /= G;
        for (double v : p) s += (v - m) * (v - m);
        s = std::sqrt (s / G);
        std::vector<double> z ((size_t) G, 0.0);
        for (int i = 0; i < G; ++i) z[(size_t) i] = s > 1e-9 ? std::min (8.0, (p[(size_t) i] - m) / s) : 0.0;
        if (priorBpm > 0.0 && s > 1e-9)
            for (int i = 0; i < G; ++i)
            {
                const double b = lo * std::pow (2.0, (double) i / perOctave);
                double pull = 0.0;
                for (double oct : { 0.5, 1.0, 2.0 })
                {
                    const double d = 1200.0 * std::log2 (b / (priorBpm * oct)) / priorCents;
                    pull = std::max (pull, std::exp (-0.5 * d * d));
                }
                z[(size_t) i] += priorBonus * pull;
            }
        pulse.push_back (std::move (p));
        emis.push_back (std::move (z));
        centre.push_back ((a + W * 0.5) / e.fps);
        priors.push_back (priorBpm);
    }
    const int T = (int) emis.size();
    if (T == 0) return tr;
    // Viterbi (max-sum); transition cost between grid states i, j: lambda * ((i - j) / perOctave)^2, limited to +-perOctave/2 steps for speed
    const int reach = perOctave / 2;
    std::vector<std::vector<double>> score ((size_t) T, std::vector<double> ((size_t) G, -1e300));
    std::vector<std::vector<int>> from ((size_t) T, std::vector<int> ((size_t) G, 0));
    score[0] = emis[0];
    for (int t = 1; t < T; ++t)
        for (int j = 0; j < G; ++j)
        {
            double best = -1e300; int bi = j;
            for (int i = std::max (0, j - reach); i <= std::min (G - 1, j + reach); ++i)
            {
                const double d = (double) (i - j) / perOctave, v = score[(size_t) t - 1][(size_t) i] - lambda * d * d;
                if (v > best) { best = v; bi = i; }
            }
            score[(size_t) t][(size_t) j] = best + emis[(size_t) t][(size_t) j];
            from[(size_t) t][(size_t) j] = bi;
        }
    std::vector<int> path ((size_t) T);
    path[(size_t) T - 1] = (int) (std::max_element (score[(size_t) T - 1].begin(), score[(size_t) T - 1].end()) - score[(size_t) T - 1].begin());
    for (int t = T - 1; t > 0; --t) path[(size_t) t - 1] = from[(size_t) t][(size_t) path[(size_t) t]];
    for (int t = 0; t < T; ++t)
    {
        int i = path[(size_t) t];
        double frac = 0.0;   // parabolic refinement on the window's own pulse curve
        if (i > 0 && i < G - 1)
        {
            const double a = pulse[(size_t) t][(size_t) i - 1], b = pulse[(size_t) t][(size_t) i], c = pulse[(size_t) t][(size_t) i + 1], den = a - 2 * b + c;
            if (den < 0) frac = std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        }
        if (priors[(size_t) t] > 0.0)
        {
            const double priorBpm = priors[(size_t) t];
            // is the song's own tempo (or its octaves) still about as strong here? then the path sits on another counting of the same pulse
            // (within +-5 %: a varispeed ramp moves the true tempo away from the prior by a few percent)
            double priorP = -1e300, nearest = 1e300;
            for (double oct : { 0.5, 1.0, 2.0 })
            {
                const double b = priorBpm * oct;
                if (b * 1.05 < lo || b * 0.95 >= hi) continue;
                const int g0 = std::max (0, (int) std::floor (std::log2 (b * 0.95 / lo) * perOctave)), g1 = std::min (G - 1, (int) std::ceil (std::log2 (b * 1.05 / lo) * perOctave));
                for (int gi = g0; gi <= g1; ++gi) priorP = std::max (priorP, pulse[(size_t) t][(size_t) gi]);
                nearest = std::min (nearest, std::fabs (1200.0 * std::log2 ((lo * std::pow (2.0, (double) i / perOctave)) / b)));
            }
            const double pathP = pulse[(size_t) t][(size_t) i];
            // a path within 8 % of the prior (a varispeed moves the tempo that far) is on the prior's own level, not on another counting of it
            tr.ambiguous.push_back ((char) (pathP > 0.0 && priorP > 0.6 * pathP && nearest > 140.0));
        }
        else
            tr.ambiguous.push_back (0);
        tr.t.push_back (centre[(size_t) t]);
        tr.bpm.push_back (lo * std::pow (2.0, (i + frac) / perOctave));
        tr.z.push_back (emis[(size_t) t][(size_t) i]);
    }
    // put the median tempo into 80 - 160 (the DJ convention), keeping the shape of the path
    std::vector<double> sorted = tr.bpm;
    std::sort (sorted.begin(), sorted.end());
    double med = sorted[sorted.size() / 2], mult = 1.0;
    while (med * mult < 80.0) mult *= 2.0;
    while (med * mult >= 160.0) mult *= 0.5;
    if (mult != 1.0) for (auto& b : tr.bpm) b *= mult;
    return tr;
}

// Beat tracking by dynamic programming: score(t) = onset(t) + max over previous beats of [score - alpha * ln(interval / period(t))^2], with the period
// following the tempo track, so accelerating and decelerating songs are tracked. Returns beat times in seconds.
inline std::vector<double> trackBeats (const OnsetEnvelope& e, const TempoTrack& tt, double alpha = 400.0)
{
    std::vector<double> beats;
    if (! e.valid || ! tt.valid()) return beats;
    const int F = (int) e.env.size();
    double sd = 0.0;
    for (double v : e.env) sd += v * v;
    sd = std::sqrt (sd / F);
    if (sd <= 0) return beats;
    std::vector<double> sc ((size_t) F, 0.0);
    std::vector<int> back ((size_t) F, -1);
    for (int i = 0; i < F; ++i)
    {
        const double bpm = std::max (40.0, tt.bpmAt (i / e.fps));
        const double P = 60.0 / bpm * e.fps;
        const int lo = std::max (1, (int) std::floor (0.5 * P)), hi = (int) std::ceil (2.0 * P);
        double best = 0.0; int bi = -1;                       // 0: start a new chain here
        for (int tau = lo; tau <= hi && i - tau >= 0; ++tau)
        {
            const double l = std::log (tau / P), v = sc[(size_t) (i - tau)] - alpha * l * l;
            if (v > best) { best = v; bi = i - tau; }
        }
        sc[(size_t) i] = e.env[(size_t) i] / sd + best;
        back[(size_t) i] = bi;
    }
    int last = F - 1;
    const int tail = (int) std::lround (e.fps * 60.0 / std::max (40.0, tt.bpm.back()));
    double bv = -1e300;
    for (int i = std::max (0, F - tail); i < F; ++i) if (sc[(size_t) i] > bv) { bv = sc[(size_t) i]; last = i; }
    for (int i = last; i >= 0; i = back[(size_t) i]) { beats.push_back (i / e.fps); if (back[(size_t) i] < 0) break; }
    std::reverse (beats.begin(), beats.end());
    return beats;
}
} // namespace centsible::tempodetail
