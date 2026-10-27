// Developer tool (not part of ctest): re-runs the FIT of the tuning on cached evidence histograms (tests/hist_cache.cpp) with different choices and scores each
// by consistency under the exact pitch shifts that were cached: the reading at shift s must be the reading at shift 0 plus s.
//   fit_variants <hist dir>
#include "gen/audio_io.h"
#include <centsible/reading.h>
#include "fft.h"
#define private public
#include "spectral_likelihood.h"
#undef private
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

namespace fs = std::filesystem;
using namespace centsible::detail;

struct Rec { float shift; double mt, ml; Hist T, L; };
struct Song { std::string name; std::vector<Rec> recs; };

static std::vector<Song> loadAll (const fs::path& dir)
{
    std::vector<Song> songs;
    for (auto& e : fs::directory_iterator (dir))
    {
        if (e.path().extension() != ".hist") continue;
        Song s;
        const auto nm = e.path().filename().u8string();
        s.name.assign (nm.begin(), nm.end());
        FILE* f = _wfopen (e.path().c_str(), L"rb");
        if (! f) continue;
        for (;;)
        {
            Rec r;
            if (std::fread (&r.shift, sizeof r.shift, 1, f) != 1) break;
            if (std::fread (&r.mt, sizeof r.mt, 1, f) != 1) break;
            if (std::fread (&r.ml, sizeof r.ml, 1, f) != 1) break;
            if (std::fread (r.T.data(), sizeof (float), kBins, f) != (size_t) kBins) break;
            if (std::fread (r.L.data(), sizeof (float), kBins, f) != (size_t) kBins) break;
            s.recs.push_back (r);
        }
        std::fclose (f);
        if (s.recs.size() >= 2) songs.push_back (std::move (s));
    }
    std::sort (songs.begin(), songs.end(), [] (const Song& a, const Song& b) { return a.name < b.name; });
    return songs;
}

// the tuning the plugin would report from this evidence (the pick of SpectralLikelihoodEstimator::estimate, without the later decorations)
static double pick (const Rec& r)
{
    const double minMass = SpectralLikelihoodEstimator::knobs.minMass;
    if (r.mt < minMass && r.ml < minMass) return 1e9;
    bool tonal = r.mt >= minMass;
    Fit f1 = tonal ? fitSingle (r.T, r.mt) : fitSingle (r.L, r.ml);
    if (tonal && r.ml >= minMass && f1.tmpl.stiff > 0.0)
    {
        const Fit fl = fitSingle (r.L, r.ml);
        if (fl.tmpl.wUni <= 0.2 && std::fabs (circWrap (fl.delta - f1.delta)) >= 1.0) { f1 = fl; tonal = false; }
    }
    return polishDelta (tonal ? r.T : r.L, f1.tmpl, f1.delta);
}

struct Variant { const char* name; std::function<void()> apply; };

int main (int argc, char** argv)
{
    if (argc < 2) return 2;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("loading\n");
    const auto songs = loadAll (fs::u8path (argv[1]));
    std::printf ("%zu songs\n", songs.size());
    const FitKnobs defaults = fitKnobs;
    auto reset = [&] { fitKnobs = defaults; };
    auto A = [] (std::initializer_list<double> a) { return [a] { int k = 0; for (double v : a) fitKnobs.alphas[(size_t) k++] = v; fitKnobs.numAlphas = k; }; };
    std::vector<Variant> variants
    {
        { "production", [] {} },
        { "alpha 1.2, 2", A ({ 1.2, 2.0 }) },
        { "alpha 1.0, 1.5, 2.0", A ({ 1.0, 1.5, 2.0 }) },
        { "alpha 1.2, 1.6, 2, 2.5", A ({ 1.2, 1.6, 2.0, 2.5 }) },
        { "alpha 1.5, 2", A ({ 1.5, 2.0 }) },
        { "alpha 1.2, 2, 3", A ({ 1.2, 2.0, 3.0 }) },
        { "alpha 1.2 only", A ({ 1.2 }) },
        { "alpha 1.6 only", A ({ 1.6 }) },
        { "alpha 2 only", A ({ 2.0 }) },
        { "alpha 1.2, 2 + margin 30", [A] { A ({ 1.2, 2.0 }) (); fitKnobs.mixMargin = 30; } },
        { "alpha 1.2, 2 + margin 300", [A] { A ({ 1.2, 2.0 }) (); fitKnobs.mixMargin = 300; } },
        { "alpha 1.2, 2 + no mixtures", [A] { A ({ 1.2, 2.0 }) (); fitKnobs.mixMargin = 1e9; } },
        { "alpha 1.2, 2 + sigma>=0.4", [A] { A ({ 1.2, 2.0 }) (); fitKnobs.sigmas = { 0.4, 1.0, 2.0, 3.5, 5.0 }; fitKnobs.numSigmas = 5; } },
    };
    for (auto& v : variants)
    {
        reset();
        v.apply();
        // picks[song][rec]
        std::vector<std::vector<double>> picks (songs.size());
        std::atomic<size_t> next { 0 };
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::max (2u, std::thread::hardware_concurrency()); ++t)
            pool.emplace_back ([&]
            {
                for (;;)
                {
                    const size_t i = next++;
                    if (i >= songs.size()) return;
                    for (const auto& r : songs[i].recs) picks[i].push_back (pick (r));
                }
            });
        for (auto& t : pool) t.join();
        long cases = 0, in05 = 0, in1 = 0, in3 = 0;
        double worst = 0, sumAbs = 0;
        int songsBad05 = 0, songsBad3 = 0;
        for (size_t i = 0; i < songs.size(); ++i)
        {
            bool bad05 = false, bad3 = false;
            const auto& rec = songs[i].recs;
            if (picks[i][0] > 1e8) continue;
            for (size_t k = 1; k < rec.size(); ++k)
            {
                if (picks[i][k] > 1e8) continue;
                const double e = std::fabs (circWrap (picks[i][k] - picks[i][0] - (double) rec[k].shift));
                ++cases; in05 += e <= 0.5; in1 += e <= 1.0; in3 += e <= 3.0; worst = std::max (worst, e); sumAbs += e;
                bad05 = bad05 || e > 0.5; bad3 = bad3 || e > 3.0;
            }
            songsBad05 += bad05; songsBad3 += bad3;
        }
        std::printf ("%-22s cases %5ld  within 0.5: %5.1f %%  1: %5.1f %%  3: %5.1f %%  max %6.2f  mean %.3f   songs with a case > 0.5: %d, > 3: %d\n", v.name, cases, 100.0 * in05 / cases,
                    100.0 * in1 / cases, 100.0 * in3 / cases, worst, sumAbs / cases, songsBad05, songsBad3);
        std::fflush (stdout);
    }
    return 0;
}
