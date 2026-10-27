// Developer probe (not in ctest): record the frame evidence of a synthetic song with moving tempo / pitch and print the windowed detune curve.
//   curve_probe [varispeed|stretch|const|step] [seconds]
#include "gen/warp_synth.h"
#include "detune_timeline.h"
#include "tempo_detector.h"
#include <centsible/sections.h>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace synth;

int main (int argc, char** argv)
{
    if (argc > 1 && ! std::strcmp (argv[1], "burst"))     // timing check: a note from 4 s to 6 s, the evidence centroid must sit at 5 s
    {
        Signal b (44100 * 10, 0.0f);
        addStaticNote (b, 44100.0, 4.0, 6.0, noteHz (0), amplitudes (Timbre::Saw, noteHz (0), 44100.0, 12), 0.0, 0.2);
        const auto fe = centsible::detail::recordFrames (b.data(), b.size(), 44100.0);
        double m = 0.0, tm = 0.0;
        for (size_t i = 0; i < fe.frames.size(); ++i) { m += fe.mass[i]; tm += fe.mass[i] * fe.frames[i].t; }
        std::printf ("burst 4..6 s: evidence centroid at %.3f s (frames %d)\n", tm / m, (int) fe.frames.size());
        return 0;
    }
    if (argc > 1 && ! std::strcmp (argv[1], "glide"))    // frame-level timing of the pitch measurement: a pure glide of 6 cents per second
    {
        const double sr0 = 44100.0;
        Signal g ((size_t) (sr0 * 20), 0.0f);
        const double slopeArg = argc > 2 ? std::atof (argv[2]) : 6.0;
        Plan gp { [] (double) { return 120.0; }, [slopeArg] (double t) { return slopeArg * t; } };
        addPlannedNote (g, sr0, 0.0, 20.0, 0.0, gp, 0.2, 12);
        const auto fe = centsible::detail::recordFrames (g.data(), g.size(), sr0);
        {   // single frames against a generic template
            auto tg = std::make_unique<centsible::detail::Template>();
            tg->setup (0.0, 1.0, 1.2, 0.3);
            for (double lag : { -0.093, 0.0, 0.093, 0.186 })
            {
                double e = 0.0; int k = 0;
                for (const auto& f : fe.frames)
                {
                    if (f.t < 4.0 || f.t > 16.0) continue;
                    int s0 = centsible::detail::scan (f.hist, *tg, 0, centsible::detail::kBins - 1, 2);
                    s0 = centsible::detail::scan (f.hist, *tg, s0 - 2, s0 + 2, 1);
                    e += wrap50 (centsible::detail::refineDelta (f.hist, *tg, s0) - gp.cents (f.t - lag)); ++k;
                }
                std::printf ("single frames, lag %+.3f s: mean signed error %+.3f cents over %d frames", lag, k ? e / k : 0.0, k);
                std::printf (" ");
                std::puts ("");
            }
        }
        centsible::detail::DetuneOptions o2;
        const auto cv = centsible::detail::computeDetuneCurve (fe, 0.0, 20.0, o2);
        for (double lag : { 0.0, 0.1, 0.2, 0.3 })
        {
            double e = 0.0; int k = 0;
            for (const auto& p : cv.points) if (p.valid && p.time > 4.0 && p.time < 16.0) { e += wrap50 (p.cents - gp.cents (p.time - lag)); ++k; }
            std::printf ("glide lag %.1f s: mean signed error %+.3f cents over %d windows\n", lag, k ? e / k : 0.0, k);
        }
        return 0;
    }
    if (argc > 1 && ! std::strcmp (argv[1], "noise"))     // sharpness of the windows of white noise (no tuning at all)
    {
        std::mt19937 rng (7);
        std::normal_distribution<float> g (0.0f, 0.1f);
        std::vector<float> n ((size_t) (60.0 * 44100.0));
        for (auto& v : n) v = g (rng);
        const auto fe = centsible::detail::recordFrames (n.data(), n.size(), 44100.0);
        const auto cv = centsible::detail::computeDetuneCurve (fe, 0.0, 60.0, {});
        for (const auto& p : cv.points) if (p.valid) std::printf ("t %.1f mass %.1f ci %.2f sharp %.3f window %.1f", p.time, p.mass, p.ci, p.sharpness, p.window), std::puts ("");
        return 0;
    }
    const char* mode = argc > 1 ? argv[1] : "varispeed";
    const double seconds = argc > 2 ? std::atof (argv[2]) : 100.0, sr = 44100.0;
    Plan plan;
    if (! std::strcmp (mode, "varispeed")) plan = rampPlan (120.0, 140.0, 20.0, seconds - 20.0, true, 10.0, 120.0);
    else if (! std::strcmp (mode, "stretch")) plan = rampPlan (120.0, 140.0, 20.0, seconds - 20.0, false, 23.4, 120.0);
    else if (! std::strcmp (mode, "step")) plan = { [=] (double) { return 120.0; }, [=] (double t) { return t < seconds / 2 ? 5.0 : -20.0; } };
    else plan = constantPlan (120.0, 23.4);
    ChordSongSpec spec;
    spec.seconds = seconds;
    spec.tonic = 9; spec.minor = true;
    if (argc > 3 && ! std::strcmp (argv[3], "auto"))      // the full tempo-automation result: curve and boundaries
    {
        const auto x = renderChordSong (spec, plan, sr);
        const auto r = centsible::analyzeTempoAutomation (x.data(), x.size(), sr);
        for (const auto& c : r.curve)
            std::printf ("%6.1f  d %s %+8.2f (u %+8.2f) ci %5.2f   t %s bpm %7.2f x %+8.1f\n", c.time, c.detuneValid ? "ok" : "--", c.cents, c.unwrapped, c.ci, c.tempoValid ? "ok" : "--", c.bpm, c.tempoCents);
        std::printf ("%s\n", r.summary.c_str());
        return 0;
    }
    if (argc > 3 && ! std::strcmp (argv[3], "tempo"))     // tempo track and beat tracker against the plan
    {
        const auto x = renderChordSong (spec, plan, sr);
        const auto env = centsible::tempodetail::computeOnsetEnvelope (x.data(), x.size(), sr, 0);
        const double win = argc > 4 ? std::atof (argv[4]) : 6.0;
        const auto tt = centsible::tempodetail::trackTempo (env, win, 1.0, 100.0);
        double se = 0.0; int k = 0;
        for (size_t i = 0; i < tt.t.size(); ++i)
        {
            const double truth = plan.bpm (tt.t[i]), err = 1200.0 * std::log2 (tt.bpm[i] / truth);
            se += std::fabs (err); ++k;
            if (i % 4 == 0) std::printf ("%6.1f  tempo %7.2f  truth %7.2f  err %+6.1f cents  z %.1f\n", tt.t[i], tt.bpm[i], truth, err, tt.z[i]);
        }
        std::printf ("tempo track: mean |err| %.1f cents over %d windows\n", k ? se / k : 0.0, k);
        const auto beats = centsible::tempodetail::trackBeats (env, tt);
        const auto grid = beatGrid (plan, seconds);
        double be = 0.0; int bk = 0;
        for (double b : beats) { double best = 1e9; for (double g : grid) best = std::min (best, std::fabs (g - b)); be += best; ++bk; }
        std::printf ("beats found %d (grid %d), mean distance to nearest true beat %.0f ms\n", (int) beats.size(), (int) grid.size(), 1000.0 * be / std::max (1, bk));
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto x = renderChordSong (spec, plan, sr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto fe = centsible::detail::recordFrames (x.data(), x.size(), sr);
    const auto t2 = std::chrono::steady_clock::now();
    centsible::detail::DetuneOptions opt;
    if (argc > 3) opt.hopSeconds = std::atof (argv[3]);
    const auto curve = centsible::detail::computeDetuneCurve (fe, 0.0, seconds, opt);
    const auto t3 = std::chrono::steady_clock::now();
    auto sec = [] (auto a, auto b) { return std::chrono::duration<double> (b - a).count(); };
    std::printf ("render %.1f s, record %.1f s (%d frames), curve %.1f s, median window mass %.1f\n", sec (t0, t1), sec (t1, t2), (int) fe.frames.size(), sec (t2, t3), curve.medianMass);
    if (curve.fit) std::printf ("template: sigma %.2f alpha %.2f B %.2e wUni %.2f\n", curve.fit->tmpl.sigma, curve.fit->tmpl.alpha, curve.fit->tmpl.stiff, curve.fit->tmpl.wUni);
    double sumErr = 0.0, maxErr = 0.0; int cover = 0, nv = 0;
    for (const auto& p : curve.points)
    {
        const double truth = wrap50 (plan.cents (p.time));
        const double err = p.valid ? wrap50 (p.cents - truth) : 0.0;
        if (p.valid) { sumErr += std::fabs (err); maxErr = std::max (maxErr, std::fabs (err)); cover += std::fabs (err) <= p.ci; ++nv; }
        std::printf ("%6.1f  %s  est %+7.2f +-%5.2f  truth %+7.2f  err %+6.2f  mass %6.1f sharp %.2f\n", p.time, p.valid ? "ok " : "-- ", p.cents, p.ci, truth, err, p.mass, p.sharpness);
    }
    for (double lag : { -0.4, -0.3, -0.2, -0.1, 0.0, 0.1, 0.2, 0.3, 0.4 })
    {
        double e = 0.0; int k = 0;
        for (const auto& p : curve.points) if (p.valid && p.time > 24.0 && p.time < 76.0) { e += wrap50 (p.cents - wrap50 (plan.cents (p.time - lag))); ++k; }
        std::printf ("lag %+.1f s: mean signed error %+.3f over %d windows\n", lag, k ? e / k : 0.0, k);
    }
    std::printf ("windows %d valid %d, mean |err| %.2f, max %.2f, within CI %d / %d\n", (int) curve.points.size(), nv, nv ? sumErr / nv : 0.0, maxErr, cover, nv);
    return 0;
}
