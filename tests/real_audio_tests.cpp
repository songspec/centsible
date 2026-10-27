// Real-audio suite.
//   centsible_real_tests <dir with audio> <results dir> [maxSeconds=180]
// For every file: the unshifted reading, then the file resampled by exact known cents (Kaiser sinc); the detected DIFFERENCE
// must equal the shift. Writes real_baseline.csv, real_shifts.csv and real_summary.md. Prints one short summary.
// NOTE: this proves CONSISTENCY under exact shifts, not absolute correctness of the unshifted reading.
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <atomic>
#include <centsible/analyzer.h>
#include "spectral_likelihood.h"
#include "gen/fit_env.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

// File names in the catalog contain characters outside the ANSI code page (stars, curly quotes): path::string() throws for them on Windows, so names go through UTF-8.
static std::string utf8 (const fs::path& p)
{
    const auto u = p.u8string();
    return std::string (u.begin(), u.end());
}

struct Track { std::string name; double sr = 0, seconds = 0; centsible::Reading base; };   // (the audio itself is not kept: a catalog of hundreds of songs does not fit in memory)
struct Row { std::string track; double shift, expected, detected, error; bool valid; float ci, confidence; bool pass; };

static centsible::Reading analyse (const std::vector<float>& x, double sr)
{
    return centsible::analyzeBuffer (x.data(), x.size(), sr);
}

template <typename F>
static void parallelFor (int count, F fn)
{
    std::atomic<int> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max (2u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back ([&] { for (int i; (i = next++) < count;) fn (i); });
    for (auto& t : pool) t.join();
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: centsible_real_tests <audio dir> <results dir> [maxSeconds]\n"); return 2; }
    const fs::path dir = argv[1], out = argv[2];
    applyFitEnv();
    const double maxSeconds = argc > 3 ? std::atof (argv[3]) : 180.0;
    const std::string filter = argc > 4 ? argv[4] : "";              // only files containing this text
    std::vector<double> shiftArgs;                                       // optional explicit shifts
    for (int i = 5; i < argc; ++i) shiftArgs.push_back (std::atof (argv[i]));
    fs::create_directories (out);

    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator (dir))
    {
        const auto ext = e.path().extension().wstring();
        if ((ext == L".mp3" || ext == L".flac" || ext == L".wav") && utf8 (e.path().filename()).find (filter) != std::string::npos) files.push_back (e.path());
    }
    std::sort (files.begin(), files.end());
    if (files.empty()) { std::printf ("no audio found in %s (nothing to test)\n", utf8 (dir).c_str()); return 0; }

    std::vector<double> shifts = { -49, -37, -12, -3, 3, 12, 37, 50, 88, 112 };
    if (! shiftArgs.empty()) shifts = shiftArgs;
    const int nShifts = (int) shifts.size();
    std::vector<Track> tracks (files.size());
    std::vector<Row> rows (tracks.size() * (size_t) nShifts);
    // one song at a time per worker: decode, read it, read it again at every exact shift, forget the audio
    parallelFor ((int) files.size(), [&] (int i)
    {
        auto& t = tracks[(size_t) i];
        t.name = utf8 (files[(size_t) i].filename());
        const auto audio = loadMono (juce::File (juce::String::fromUTF8 (utf8 (files[(size_t) i]).c_str())), t.sr, maxSeconds);
        t.seconds = (double) audio.size() / t.sr;
        t.base = analyse (audio, t.sr);
        for (int k = 0; k < nShifts; ++k)
        {
            const double c = shifts[(size_t) k];
            const auto shifted = resample (audio, std::pow (2.0, c / 1200.0));
            const auto r = analyse (shifted, t.sr);
            Row row { t.name, c, centsible::wrapCents (c), centsible::wrapCents ((double) r.cents - (double) t.base.cents), 0.0, r.valid && t.base.valid, r.ciCents, r.confidence, false };
            row.error = centsible::wrapCents (row.detected - row.expected);
            row.pass = row.valid && std::fabs (row.error) <= 0.5;
            rows[(size_t) i * (size_t) nShifts + (size_t) k] = row;
        }
    });

    std::ofstream b (out / "real_baseline.csv");
    b << "file,sample_rate,seconds,cents,ci,a4_hz,valid,locked,confidence,secondary_cents,secondary_share\n";
    for (auto& t : tracks)
        b << '"' << t.name << "\"," << t.sr << ',' << t.seconds << ',' << t.base.cents << ',' << t.base.ciCents << ',' << t.base.refHz << ','
          << t.base.valid << ',' << t.base.locked << ',' << t.base.confidence << ',' << (t.base.hasSecondary ? t.base.secondaryCents : 0.0f) << ','
          << (t.base.hasSecondary ? t.base.secondaryShare : 0.0f) << '\n';

    std::ofstream s (out / "real_shifts.csv");
    s << "file,shift_cents,expected_diff,detected_diff,error,ci,confidence,valid,pass\n";
    int total = 0, within = 0, resolvable = 0, resolvableWithin = 0;
    double maxErr = 0.0, maxErrResolvable = 0.0;
    for (auto& r : rows)
    {
        s << '"' << r.track << "\"," << r.shift << ',' << r.expected << ',' << r.detected << ',' << r.error << ',' << r.ci << ',' << r.confidence << ',' << r.valid << ',' << r.pass << '\n';
        ++total; within += std::fabs (r.error) <= 0.5; maxErr = std::max (maxErr, std::fabs (r.error));
        if (r.valid) { ++resolvable; resolvableWithin += std::fabs (r.error) <= 0.5; maxErrResolvable = std::max (maxErrResolvable, std::fabs (r.error)); }
    }

    std::ofstream m (out / "real_summary.md");
    m << "# Real-audio shift test (difference vs unshifted original)\n\n| track | unshifted cents | CI | confidence | worst |error| over 10 shifts | cases >0.5 |\n|---|---|---|---|---|---|\n";
    for (size_t ti = 0; ti < tracks.size(); ++ti)
    {
        double worst = 0; int bad = 0;
        for (int k = 0; k < nShifts; ++k) { const auto& r = rows[ti * (size_t) nShifts + (size_t) k]; worst = std::max (worst, std::fabs (r.error)); bad += std::fabs (r.error) > 0.5; }
        char line[512];
        std::snprintf (line, sizeof line, "| %s | %+.2f | %.2f | %.2f | %.2f | %d |\n", tracks[ti].name.c_str(), (double) tracks[ti].base.cents, (double) tracks[ti].base.ciCents, (double) tracks[ti].base.confidence, worst, bad);
        m << line;
    }
    char sum[512];
    std::snprintf (sum, sizeof sum, "\n**%d cases: %.1f%% within 0.5 cents, max error %.2f cents.** (valid readings only: %d cases, %.1f%% within 0.5, max %.2f)\n",
                   total, 100.0 * within / total, maxErr, resolvable, resolvable ? 100.0 * resolvableWithin / resolvable : 0.0, maxErrResolvable);
    m << sum;
    std::printf ("%s", sum + 1);
    for (size_t ti = 0; ti < tracks.size(); ++ti)
        std::printf ("  %-48s unshifted %+7.2f  confidence %.2f\n", tracks[ti].name.c_str(), (double) tracks[ti].base.cents, (double) tracks[ti].base.confidence);
    const bool pass = total > 0 && 100.0 * within / total >= 99.0 && maxErr <= 1.0;
    std::printf ("acceptance (>=99%% within 0.5, max <= 1.0): %s\n", pass ? "PASS" : "NOT YET");
    return pass ? 0 : 1;
}
