// Beatswitch on real songs: how often does it cry wolf on an untouched song, and does it find what a known edit puts in?
//   beatswitch_eval [--seconds N] [--jobs N] [--csv out.log] [--dump dir] <file | folder>...        untouched songs: verdict and suggested boundaries
//   beatswitch_eval --varispeed r0 r1 tA tB <file>...      each song played at speed r0 until tA s, ramping to r1 at tB s, the pitch following the speed:
//                                                          must read as varispeed, with the right slope, detune change and reference tempo
//   beatswitch_eval --splice <fileB> tA tB <file>...        song A up to tA s, then song B from tB s: a boundary at tA and regions that match each song alone
// The edited modes print PASS / FAIL per song against the exact truth of the edit and exit 1 if any fails.
#include "gen/audio_io.h"
#include "gen/varispeed.h"
#include <centsible/sections.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
// like loadMono (gen/audio_io.h) but a file JUCE cannot decode (m4a, say) is skipped instead of ending the batch
std::vector<float> loadOrEmpty (const juce::File& f, double& sr, double maxSeconds)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return {};
    return loadMono (f, sr, maxSeconds);
}

bool isAudio (const juce::File& f)
{
    const auto e = f.getFileExtension().toLowerCase();
    return e == ".wav" || e == ".flac" || e == ".mp3" || e == ".ogg" || e == ".aif" || e == ".aiff" || e == ".m4a";
}

double wrap100 (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }
double wrapOct (double c) { return c - 1200.0 * std::floor ((c + 600.0) / 1200.0); }

std::string line (const std::string& name, const std::vector<float>& x, double sr, const centsible::TempoAutomationResult& r, const std::string& extra)
{
    char buf[1536];
    std::string b;
    for (const auto& s : r.boundaries)
    {
        char t[96];
        std::snprintf (t, sizeof t, " %s%s%s%s(%+.0f)", centsible::formatTimestamp (s.time).c_str(), s.detuneJump ? "D" : "", s.tempoStep ? "T" : "", s.passageStart || s.passageEnd ? "p" : "", s.jumpCents);
        b += t;
    }
    std::snprintf (buf, sizeof buf, "%-44.44s | %5.0f s | %-9s | slope %5.2f | %6.1f BPM | %+7.1f -> %+7.1f | %d boundaries:%s | %s", name.c_str(), (double) x.size() / sr, centsible::behaviourName (r.behaviour), r.slope,
                   r.medianBpm, r.centsStart, r.centsEnd, (int) r.boundaries.size(), b.c_str(), extra.c_str());
    return buf;
}

std::string fmt (const char* f, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0)
{
    char buf[512];
    std::snprintf (buf, sizeof buf, f, a, b, c, d, e);
    return buf;
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<juce::File> files;
    double maxSeconds = 1.0e9;
    unsigned jobsArg = 0;
    std::string csv, dump;
    bool varispeed = false, splice = false, showRegions = false;
    double r0 = 1.0, r1 = 1.0, tA = 0.0, tB = 0.0;
    juce::File fileB;
    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        if (arg == "--seconds" && a + 1 < argc) maxSeconds = std::atof (argv[++a]);
        else if (arg == "--jobs" && a + 1 < argc) jobsArg = (unsigned) std::atoi (argv[++a]);
        else if (arg == "--regions") showRegions = true;
        else if (arg == "--csv" && a + 1 < argc) csv = argv[++a];
        else if (arg == "--dump" && a + 1 < argc) dump = argv[++a];
        else if (arg == "--varispeed" && a + 4 < argc) { varispeed = true; r0 = std::atof (argv[++a]); r1 = std::atof (argv[++a]); tA = std::atof (argv[++a]); tB = std::atof (argv[++a]); }
        else if (arg == "--splice" && a + 3 < argc) { splice = true; fileB = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[++a])); tA = std::atof (argv[++a]); tB = std::atof (argv[++a]); }
        else
        {
            const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[a]));
            if (f.isDirectory()) { for (const auto& c : f.findChildFiles (juce::File::findFiles, false)) if (isAudio (c)) files.push_back (c); }
            else if (f.existsAsFile()) files.push_back (f);
            else std::printf ("not found: %s\n", argv[a]);
        }
    }
    if (files.empty()) { std::printf ("usage: beatswitch_eval [--seconds N] [--jobs N] [--csv out] [--dump dir] [--varispeed r0 r1 tA tB | --splice fileB tA tB] <file | folder>...\n"); return 2; }
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    std::vector<std::string> out (files.size());
    std::atomic<size_t> next { 0 };
    std::atomic<int> failures { 0 };
    std::mutex io;
    std::vector<std::thread> pool;
    const unsigned jobs = jobsArg ? jobsArg : std::max (1u, std::min (4u, std::thread::hardware_concurrency() / 2));
    for (unsigned w = 0; w < jobs; ++w)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < files.size();)
            {
                double sr = 0.0;
                auto x = loadOrEmpty (files[i], sr, maxSeconds);
                if (x.empty()) { std::lock_guard<std::mutex> g (io); std::printf ("%s | cannot decode, skipped\n", files[i].getFileName().toStdString().c_str()); continue; }
                std::string extra, verdictText;
                bool pass = true;
                centsible::TrackAnalysis baseline;
                std::vector<float> yB;
                double srB = 0.0;
                if (varispeed)
                {
                    baseline = centsible::analyzeTrack (x.data(), x.size(), sr);          // what the song is before the edit: its detune and tempo
                    const double rr0 = r0, rr1 = r1, a = tA, b = tB;
                    auto rate = [=] (double t) { return t <= a ? rr0 : t >= b ? rr1 : rr0 + (rr1 - rr0) * (t - a) / (b - a); };
                    x = resampleVarying (x, sr, rate, std::max (rr0, rr1));
                    extra = fmt ("truth: varispeed %+.1f cents over the ramp, song was %+.1f cents at %.1f BPM", 1200.0 * std::log2 (rr1 / rr0), (double) baseline.tuning.cents, (double) baseline.tempo.bpm);
                }
                else if (splice)
                {
                    yB = loadOrEmpty (fileB, srB, 1.0e9);
                    const std::size_t ia = std::min (x.size(), (std::size_t) (tA * sr));
                    // the two songs may have different sample rates (44.1 and 48 kHz are both in the catalog): song B is converted to song A's rate, keeping its
                    // pitch (joining them as they are would play B 1.088 times too fast, +146 cents)
                    std::vector<float> yUse = yB;
                    if (std::fabs (srB - sr) > 1.0) yUse = resample (yB, srB / sr);
                    const std::size_t ibUse = std::min (yUse.size(), (std::size_t) (tB * sr));
                    // each part analysed alone, the way it will sit in the splice
                    baseline = centsible::analyzeTrack (x.data(), ia, sr);
                    x.resize (ia);
                    x.insert (x.end(), yUse.begin() + (long) ibUse, yUse.end());
                    extra = "truth: boundary at " + centsible::formatTimestamp (tA);
                }
                const auto r = centsible::analyzeTempoAutomation (x.data(), x.size(), sr);

                if (varispeed)
                {
                    const double want = 1200.0 * std::log2 (r1 / r0);
                    const bool okVerdict = r.behaviour == centsible::TempoBehaviour::Varispeed;
                    const bool okSlope = std::fabs (r.slope - 1.0) <= 0.2;
                    const double got = r.centsEnd - r.centsStart;
                    const bool okRange = std::fabs (got - want) <= 15.0;     // the tempo track's grid is 12 cents wide, and the ends are read from it
                    const double expected = (double) baseline.tempo.bpm * std::pow (2.0, -(double) baseline.tuning.cents / 1200.0);
                    const double refErr = r.referenceBpm > 0.0 && expected > 0.0 ? wrap100 (wrapOct (1200.0 * std::log2 (r.referenceBpm / expected))) : 999.0;     // reference tempo is defined modulo a semitone
                    const bool okRef = std::fabs (refErr) <= 20.0;
                    pass = okVerdict && okSlope && okRange && okRef;
                    char t[600];
                    std::snprintf (t, sizeof t, " || %s: verdict %s, slope %.2f, detune change %+.1f (want %+.1f), reference %.2f BPM (want %.2f mod semitone, off %+.0f cents)", pass ? "PASS" : "FAIL",
                                   centsible::behaviourName (r.behaviour), r.slope, got, want, r.referenceBpm, expected, refErr);
                    verdictText = t;
                }
                else if (splice)
                {
                    const std::size_t ib = std::min (yB.size(), (std::size_t) (tB * srB));
                    const auto second = centsible::analyzeTrack (yB.data() + ib, yB.size() - ib, srB);
                    const auto& A = baseline;
                    bool foundBoundary = false;
                    double at = 0.0;
                    for (const auto& s : r.boundaries) if (std::fabs (s.time - tA) <= 1.5) { foundBoundary = true; at = s.time; }
                    const bool wantD = std::fabs (wrap100 ((double) second.tuning.cents - (double) A.tuning.cents)) >= 12.0;
                    bool okD = true;
                    for (const auto& s : r.boundaries) if (std::fabs (s.time - tA) <= 1.5 && wantD && ! s.detuneJump) okD = false;
                    // region levels: the region that ends at the boundary and the one that starts there
                    const centsible::Region* left = nullptr; const centsible::Region* right = nullptr;
                    for (const auto& g : r.regions) { if (std::fabs (g.end - tA) <= 1.5) left = &g; if (std::fabs (g.start - tA) <= 1.5) right = &g; }
                    bool okLevels = left && right && left->ok && right->ok;
                    double eA = 999.0, eB = 999.0;
                    if (okLevels)
                    {
                        eA = wrap100 (left->cents - (double) A.tuning.cents);
                        eB = wrap100 (right->cents - (double) second.tuning.cents);
                        okLevels = std::fabs (eA) <= 3.0 && std::fabs (eB) <= 3.0;
                    }
                    pass = foundBoundary && okD && okLevels;
                    char t[700];
                    std::snprintf (t, sizeof t, " || %s: boundary %s (found %s), levels A %+.1f / B %+.1f (alone: %+.1f / %+.1f), level errors %+.1f / %+.1f, A %.0f BPM B %.0f BPM", pass ? "PASS" : "FAIL",
                                   foundBoundary ? "yes" : "NO", foundBoundary ? centsible::formatTimestamp (at).c_str() : "-", left ? left->cents : 0.0, right ? right->cents : 0.0,
                                   (double) A.tuning.cents, (double) second.tuning.cents, eA, eB, (double) A.tempo.bpm, (double) second.tempo.bpm);
                    verdictText = t;
                }
                if (! pass) ++failures;
                out[i] = line (files[i].getFileName().toStdString(), x, sr, r, extra) + verdictText;
                if (showRegions)
                    for (const auto& g : r.regions)
                    {
                        char t[300];
                        std::snprintf (t, sizeof t, "\n      region %s - %s  ok %d  %+.1f +- %.1f cents%s  %.1f BPM  %s  %s", centsible::formatTimestamp (g.start).c_str(), centsible::formatTimestamp (g.end).c_str(), (int) g.ok, g.cents, g.ci,
                                       g.pitchMoves ? " (moves)" : "", g.bpm, centsible::behaviourName (g.behaviour), g.key.valid ? centsible::keyLabel (g.key).c_str() : "");
                        out[i] += t;
                    }
                if (! dump.empty())
                {
                    std::ofstream f (dump + "/" + files[i].getFileNameWithoutExtension().toStdString() + ".csv");
                    f << "time,detune_valid,cents,ci,unwrapped,mass,tempo_valid,bpm,tempo_cents,sharpness,window\n";
                    for (const auto& c : r.curve) { char l[256]; std::snprintf (l, sizeof l, "%.2f,%d,%.3f,%.3f,%.3f,%.1f,%d,%.3f,%.2f,%.3f,%.1f\n", c.time, (int) c.detuneValid, c.cents, c.ci, c.unwrapped, c.mass, (int) c.tempoValid, c.bpm, c.tempoCents, c.sharpness, c.window); f << l; }
                }
                std::lock_guard<std::mutex> g (io);
                std::printf ("%s\n", out[i].c_str());
                std::fflush (stdout);
            }
        });
    for (auto& t : pool) t.join();
    if (! csv.empty())
    {
        std::ofstream f (csv);
        for (const auto& l : out) f << l << "\n";
    }
    if (varispeed || splice) std::printf ("=== %d of %d failed\n", failures.load(), (int) files.size());
    return failures.load() ? 1 : 0;
}
