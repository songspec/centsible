// Offline accuracy suite for the shared analysis core (core/, no DAW, no JUCE).
//   centsible_tests [fail] [results dir]    "fail" prints failing cases only; a results dir gets synthetic_results.csv + synthetic_summary.md
//
// Sections:
//   1 sweep         -200..+200 cents in 0.5 steps on rich harmonic tones and chords                (+-0.1)
//   2 zoo           instruments and conditions, at offsets incl. +88, +112, -150                   (+-0.1; vibrato-only audio may decline)
//   3 songs         fake songs: chords + bass + drums + detuned melody (+ reverb, + noise)         (+-0.2)
//   4 secondary     a genuine second tuning must be found; it must NEVER be reported when absent
//   5 switch        a different song after a song: no carry-over (with and without a silence gap)
//   6 regression    the +88 case, with its dual interpretation
//   7 sample rates  22.05 .. 192 kHz                                                               (+-0.1)
//   8 dense mixes   scattered vocals + 808 (+ quiet exact pad): benchmark of accuracy and interval honesty
//   9 drift         a moving tuning is reported as drift, a steady one never
// Real-audio tests (known shifts of real songs) are in real_audio_tests.cpp, key/tempo/match in features_tests.cpp.
#include "gen/synth.h"
#include "gen/fit_env.h"
#include <centsible/analyzer.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace synth;

static const double SR = 48000.0;

struct Row
{
    std::string section, name;
    double expected = 0, detected = 0, error = 0, tol = 0, ci = 0, confidence = 0;
    bool pass = false, secondary = false, secondaryExpected = false, declined = false;
    std::string note;
};

static std::vector<Row> rows;
static std::mutex rowsMutex;

static centsible::Reading run (const Signal& x, double sr = SR, double* seconds = nullptr)
{
    centsible::Analyzer e;
    e.prepare (sr);
    for (size_t pos = 0; pos < x.size();)
    {
        const size_t len = std::min<size_t> (x.size() - pos, 477); // host-like odd block size
        e.push (x.data() + pos, (int) len);
        pos += len;
    }
    if (seconds) *seconds = (double) x.size() / sr;
    return e.reading();
}

static void record (Row r) { std::lock_guard<std::mutex> l (rowsMutex); rows.push_back (std::move (r)); }

static void parallelFor (int count, const std::function<void (int)>& fn)
{
    std::atomic<int> next { 0 };
    std::vector<std::thread> pool;
    const unsigned workers = std::max (2u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < workers; ++t)
        pool.emplace_back ([&] { for (int i; (i = next++) < count;) fn (i); });
    for (auto& t : pool) t.join();
}

// Checks a single-tuning signal: the dominant tuning within tol, and no secondary tuning reported.
static void checkSingle (const char* section, const std::string& name, const Signal& x, double detune, double tol, bool mayDecline = false)
{
    const auto r = run (x);
    Row row;
    row.section = section; row.name = name;
    row.expected = wrap50 (detune);
    row.detected = r.cents;
    row.error = wrap50 (r.cents - detune);
    row.tol = tol;
    row.secondary = r.hasSecondary;
    row.ci = r.ciCents; row.confidence = r.confidence;
    row.pass = r.valid && std::fabs (row.error) <= tol && ! r.hasSecondary && ! r.hasDrift;
    // honest answers for audio with no steady notes: no reading at all, or a reading the plugin itself flags as weak (confidence < 0.3)
    const bool declinedHonestly = mayDecline && (! r.valid || r.confidence < 0.3f);
    if (declinedHonestly) { row.pass = ! r.hasSecondary; row.declined = true; row.note = r.valid ? "flagged low confidence (weak evidence): honest" : "no reading (no steady notes): honest"; }
    else if (mayDecline && r.valid) row.pass = std::fabs (row.error) <= 3.0 && ! r.hasSecondary;
    if (r.hasSecondary) row.note = "FALSE SECOND TUNING at " + std::to_string (r.secondaryCents);
    if (r.hasDrift) row.note += " FALSE DRIFT " + std::to_string (r.driftCents);
    record (row);
}

int main (int argc, char** argv)
{
    applyFitEnv();
    const bool failuresOnly = argc > 1 && std::string (argv[1]) == "fail";
    // ------------------------------------------------------------------------------------------------ 1 sweep
    {
        std::vector<double> offsets;
        for (double d = -200.0; d <= 200.0 + 1e-9; d += 0.5) offsets.push_back (d);
        const int total = (int) offsets.size() * 2;
        parallelFor (total, [&] (int i)
        {
            const double d = offsets[(size_t) (i / 2)];
            const bool chords = (i % 2) == 1;
            const Signal x = chords ? sawChords (SR, 16.0, d) : sawMelody (SR, 16.0, d);
            char name[64];
            std::snprintf (name, sizeof name, "%s %+.1f", chords ? "saw chords" : "saw melody", d);
            checkSingle ("1 sweep", name, x, d, 0.1);
        });
    }

    // ------------------------------------------------------------------------------------------------ 2 zoo
    {
        struct Item { std::string name; std::function<Signal (double)> make; double tol; bool mayDecline = false; };
        const std::vector<Item> items = {
            { "piano-like, B=4e-4",             [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::Saw, 4e-4); }, 0.1 },
            { "piano-like, B=1e-3 (treble)",  [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::Soft, 1e-3, 1); }, 0.1 },
            { "clarinet (odd partials)",        [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::Odd, 0.0); }, 0.1 },
            { "voice, weak fundamental",        [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::Voice, 0.0); }, 0.1 },
            { "voice, no fundamental",          [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::VoiceNoFundamental, 0.0); }, 0.1 },
            { "partials only (high-passed)",    [] (double d) { return singleVoiceMelody (SR, 24, d, Timbre::PartialsOnly, 0.0, -1); }, 0.1 },
            { "bass 41-62 Hz, B=1e-3",          [] (double d) { return stretchedBass (SR, 24, d, 1e-3); }, 0.1 },
            { "bass 41-62 Hz, B=3e-4",          [] (double d) { return stretchedBass (SR, 24, d, 3e-4); }, 0.1 },
            { "vibrato vocal +-25c (a cappella)", [] (double d) { return vibratoVocal (SR, 30, d, 0.0, 5); }, 0.2, true },
            { "chords + noise (SNR 6 dB)",      [] (double d) { auto x = sawChords (SR, 24, d); addNoise (x, 6.0, 3); return x; }, 0.1 },
            { "chords + drums",                 [] (double d) { auto x = sawChords (SR, 24, d); mixInto (x, drumLoop (SR, 24, 2), 0.5f); return x; }, 0.1 },
            { "chords + reverb tail",           [] (double d) { auto x = sawChords (SR, 24, d); auto w = reverbWet (x, SR); mixInto (x, w, 1.5f); return x; }, 0.1 },
        };
        const double offsets[] = { -150.0, -37.3, -12.8, 0.4, 8.6, 49.7, 88.0, 112.0 };
        const int n = (int) items.size() * 8;
        parallelFor (n, [&] (int i)
        {
            const auto& it = items[(size_t) (i / 8)];
            const double d = offsets[i % 8];
            char name[96];
            std::snprintf (name, sizeof name, "%s @ %+.1f", it.name.c_str(), d);
            checkSingle ("2 zoo", name, it.make (d), d, it.tol, it.mayDecline);
        });
    }

    // ------------------------------------------------------------------------------------------------ 3 songs
    {
        struct Item { std::string name; SongOptions o; };
        SongOptions plain;
        SongOptions noisy; noisy.noiseSnrDb = 12.0;
        SongOptions dry; dry.reverb = 0.0;
        SongOptions wet; wet.reverb = 0.6;
        SongOptions stiff; stiff.bassB = 1.5e-3;
        const std::vector<Item> items = { { "fake song", plain }, { "fake song + noise", noisy }, { "fake song, dry", dry },
                                          { "fake song, very wet", wet }, { "fake song, stiff bass", stiff } };
        const double offsets[] = { -150.0, -37.3, -12.8, 0.4, 8.6, 45.1, 88.0, 112.0 };
        const int n = (int) items.size() * 8;
        parallelFor (n, [&] (int i)
        {
            const auto& it = items[(size_t) (i / 8)];
            const double d = offsets[i % 8];
            char name[96];
            std::snprintf (name, sizeof name, "%s @ %+.1f", it.name.c_str(), d);
            checkSingle ("3 songs", name, fakeSong (SR, 50, d, it.o, 21 + (unsigned) i), d, 0.2);
        });
    }

    // ------------------------------------------------------------------------------------------------ 4 secondary
    {
        struct Case { std::string name; double d1, d2, share; bool expectSecond; };
        const std::vector<Case> cases = {
            { "second tuning +20c, 35% of time", 10.0, 30.0, 0.5, true },
            { "second tuning -25c, 40% of time", -8.0, -33.0, 0.6, true },
            { "second tuning +18c, 40% of time", 0.0, 18.0, 0.6, true },
            { "second tuning, near a semitone", 40.0, -20.0, 0.6, true },
            { "tiny drone only (10% of time)",   0.0, 20.0, 0.12, false },
            { "none (single tuning)",             15.0, 15.0, 0.0, false },
        };
        parallelFor ((int) cases.size(), [&] (int i)
        {
            const auto& c = cases[(size_t) i];
            const Signal x = c.share > 0.0 ? twoTunings (SR, 100.0, c.d1, c.d2, c.share, 9 + (unsigned) i) : fakeSong (SR, 100.0, c.d1, SongOptions {}, 9 + (unsigned) i);
            const auto r = run (x);
            Row row;
            row.section = "4 secondary"; row.name = c.name;
            row.expected = wrap50 (c.d1); row.detected = r.cents; row.error = wrap50 (r.cents - c.d1); row.tol = 0.2;
            row.secondary = r.hasSecondary; row.secondaryExpected = c.expectSecond;
            row.ci = r.ciCents; row.confidence = r.confidence;
            bool ok = std::fabs (row.error) <= row.tol && r.hasSecondary == c.expectSecond;
            if (c.expectSecond && r.hasSecondary)
            {
                const double e2 = wrap50 (r.secondaryCents - c.d2);
                row.note = "secondary " + std::to_string (r.secondaryCents) + " (expected " + std::to_string (wrap50 (c.d2)) + ", share "
                           + std::to_string ((int) (r.secondaryShare * 100)) + "%)";
                ok = ok && std::fabs (e2) <= 0.5;
            }
            else if (r.hasSecondary != c.expectSecond)
                row.note = c.expectSecond ? "SECOND TUNING MISSED" : "FALSE SECOND TUNING at " + std::to_string (r.secondaryCents);
            row.pass = ok;
            record (row);
        });
    }

    // ------------------------------------------------------------------------------------------------ 5 switch
    {
        struct Case { std::string name; double dA, dB; bool gap; };
        const std::vector<Case> cases = {
            { "0 -> +88 (gap)",       0.0,   88.0, true },
            { "0 -> +88 (no gap)",    0.0,   88.0, false },
            { "+30 -> -20 (gap)",    30.0,  -20.0, true },
            { "+30 -> -20 (no gap)", 30.0,  -20.0, false },
            { "-37 -> +12 (no gap)", -37.0,  12.0, false },
            { "0 -> +3 (gap, similar)", 0.0,  3.0, true },
        };
        parallelFor ((int) cases.size(), [&] (int i)
        {
            const auto& c = cases[(size_t) i];
            Signal a = fakeSong (SR, 90.0, c.dA, SongOptions {}, 31 + (unsigned) i);
            const Signal b = fakeSong (SR, 150.0, c.dB, SongOptions {}, 41 + (unsigned) i);
            if (c.gap) a.insert (a.end(), (size_t) (4 * SR), 0.0f);
            const Signal x = concat (a, b);
            centsible::Analyzer e;
            e.prepare (SR);
            size_t pos = 0;
            double firstOk = -1.0;
            const size_t bStart = a.size();
            while (pos < x.size())
            {
                const size_t len = std::min<size_t> (x.size() - pos, 512);
                e.push (x.data() + pos, (int) len);
                pos += len;
                if (pos > bStart && firstOk < 0.0 && std::fabs (wrap50 (e.reading().cents - c.dB)) <= 0.3 && ! e.reading().hasSecondary)
                    firstOk = (double) (pos - bStart) / SR;
            }
            const auto r = e.reading();
            Row row;
            row.section = "5 switch"; row.name = c.name;
            row.expected = wrap50 (c.dB); row.detected = r.cents; row.error = wrap50 (r.cents - c.dB); row.tol = 0.3;
            row.secondary = r.hasSecondary;
            row.ci = r.ciCents; row.confidence = r.confidence;
            row.pass = std::fabs (row.error) <= 0.3 && ! r.hasSecondary && firstOk >= 0.0 && firstOk <= 90.0;
            row.note = firstOk >= 0.0 ? "correct after " + std::to_string ((int) firstOk) + " s of song B" : "never converged";
            if (r.hasSecondary) row.note += "; STALE SECOND TUNING " + std::to_string (r.secondaryCents);
            record (row);
        });
    }

    // ------------------------------------------------------------------------------------------------ 6 regression
    {
        const Signal x = sawChords (SR, 20.0, 88.0);
        const auto r = run (x);
        Row row;
        row.section = "6 regression"; row.name = "+88 cents reads -12 (never 0)";
        row.expected = -12.0; row.detected = r.cents; row.error = r.cents - (-12.0); row.tol = 0.1;
        row.ci = r.ciCents; row.confidence = r.confidence;
        row.pass = std::fabs (row.error) <= 0.1 && std::fabs (r.cents) > 5.0;
        const double other = r.cents + 100.0; // the other reading: the same offset counted from the semitone below
        row.note = "also = " + std::to_string (other) + " from one semitone down";
        record (row);
    }

    // ------------------------------------------------------------------------------------------------ 7 sample rates
    {
        // hosts run at 44.1 .. 192 kHz: same tuning must come out whatever the rate
        const double rates[] = { 22050.0, 32000.0, 44100.0, 88200.0, 96000.0, 192000.0 };
        const double offsets[] = { -23.4, 37.0, 88.0 };
        parallelFor (6 * 3, [&] (int i)
        {
            const double sr = rates[i / 3], d = offsets[i % 3];
            char name[96];
            std::snprintf (name, sizeof name, "chords @ %.0f Hz, detune %+.1f", sr, d);
            const Signal x = sawChords (sr, 24, d);
            const auto r = run (x, sr);
            Row row;
            row.section = "7 sample rates"; row.name = name;
            row.expected = wrap50 (d); row.detected = r.cents; row.error = wrap50 (r.cents - d); row.tol = 0.1;
            row.secondary = r.hasSecondary; row.ci = r.ciCents; row.confidence = r.confidence;
            row.pass = r.valid && std::fabs (row.error) <= row.tol && ! r.hasSecondary;
            record (row);
        });
    }

    // ------------------------------------------------------------------------------------------------ 8 dense mixes (benchmark)
    {
        // The hard class: scattered vocals + 808 (+ a quiet exact pad). The truth is the producer's grid. This section measures how
        // close the plugin gets and how honest its interval is: pass = the error is covered by 1.5x the reported interval (0.5 cent floor). Not a 0.1-cent test.
        struct Mix { std::string name; double scatter; float vocal, pad; };
        const std::vector<Mix> mixes = { { "vocal-heavy, quiet pad", 6.0, 0.8f, 0.5f }, { "vocal + 808 only", 6.0, 0.8f, 0.0f }, { "very loose vocal, quiet pad", 9.0, 0.9f, 0.4f } };
        const double offsets[] = { -37.3, -12.8, 8.6, 33.0, 49.7, 88.0 };
        parallelFor ((int) mixes.size() * 6 * 2, [&] (int i)
        {
            const auto& m = mixes[(size_t) (i / 12)];
            const double d = offsets[(i / 2) % 6];
            const unsigned seed = 100 + (unsigned) (i % 2) * 17;
            char name[112];
            std::snprintf (name, sizeof name, "%s @ %+.1f (seed %u)", m.name.c_str(), d, seed);
            const Signal x = denseSong (SR, 90.0, d, seed, m.scatter, m.vocal, m.pad);
            const auto r = run (x);
            Row row;
            row.section = "8 dense mixes"; row.name = name;
            row.expected = wrap50 (d); row.detected = r.cents; row.error = wrap50 (r.cents - d);
            row.secondary = false; row.ci = r.ciCents; row.confidence = r.confidence;
            row.tol = std::max (0.5, 1.5 * (double) r.ciCents);   // honesty rule: the error must be covered by the interval the plugin reports (0.5 cents floor)
            row.pass = r.valid && std::fabs (row.error) <= row.tol && ! r.hasSecondary;
            row.note = std::fabs (row.error) <= std::max (0.05, (double) r.ciCents) ? "inside CI" : "outside CI";
            record (row);
        });
    }

    // ------------------------------------------------------------------------------------------------ 9 drift
    {
        // A tuning that moves over the song (tape / vinyl wow, a sampled section, a retuned outro) must be reported as drift, a steady one never.
        struct Case { std::string name; double d0, total; bool drifting; };
        const std::vector<Case> cases = {
            { "steady 120 s @ -23.4", -23.4, 0.0, false }, { "steady 120 s @ +37.0", 37.0, 0.0, false }, { "steady 120 s @ +88.0", 88.0, 0.0, false },
            { "drifts +8 over 120 s", 2.0, 8.0, true }, { "drifts -10 over 120 s", 30.0, -10.0, true }, { "drifts +10 over 120 s across the +-50 wrap", 45.0, 10.0, true },
        };
        parallelFor ((int) cases.size(), [&] (int i)
        {
            const auto& c = cases[(size_t) i];
            Signal x;
            for (int k = 0; k < 12; ++k)
            {
                const Signal seg = fakeSong (SR, 10.0, c.d0 + c.total * (k + 0.5) / 12.0, SongOptions {}, 61 + (unsigned) (i * 12 + k));
                x.insert (x.end(), seg.begin(), seg.end());
            }
            const auto r = run (x);
            const double meanTuning = c.d0 + c.total * 0.5;
            Row row;
            row.section = "9 drift"; row.name = c.name;
            row.expected = wrap50 (meanTuning); row.detected = r.cents; row.error = wrap50 (r.cents - meanTuning); row.ci = r.ciCents; row.confidence = r.confidence;
            if (c.drifting)
            {
                row.tol = 2.0;   // the reading is the song's average tuning
                row.pass = r.valid && r.hasDrift && std::fabs (r.driftCents - c.total * 0.5) <= 2.0 && std::fabs (row.error) <= row.tol && ! r.hasSecondary;
                row.note = r.hasDrift ? "drift reported " + std::to_string (r.driftCents) + " (later half minus earlier half, expected " + std::to_string (c.total * 0.5) + ")" : "DRIFT MISSED";
            }
            else
            {
                row.tol = 0.2;
                row.pass = r.valid && ! r.hasDrift && std::fabs (row.error) <= row.tol && ! r.hasSecondary;
                row.note = r.hasDrift ? "FALSE DRIFT " + std::to_string (r.driftCents) : "no drift: correct";
            }
            record (row);
        });
    }

    // ------------------------------------------------------------------------------------------------ report
    std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
    {
        return a.section != b.section ? a.section < b.section : a.name < b.name;
    });

    std::string current;
    int sectionPass = 0, sectionTotal = 0;
    auto flush = [&]
    {
        if (! current.empty()) std::printf ("  -> %d / %d passed\n\n", sectionPass, sectionTotal);
        sectionPass = sectionTotal = 0;
    };
    for (const auto& r : rows)
    {
        if (r.section != current)
        {
            flush();
            current = r.section;
            std::printf ("[%s]\n", current.c_str());
        }
        ++sectionTotal;
        sectionPass += r.pass;
        // the sweep is long: print every row only if it fails, plus a sparse sample of passes
        const bool sweep = current == "1 sweep";
        if (failuresOnly ? ! r.pass : (! r.pass || ! sweep || (int) std::fmod (std::fabs (r.expected) * 2.0, 40.0) == 0))
            std::printf ("  %-46s expected %+8.2f  detected %+8.2f  error %+7.3f  %s %s\n", r.name.c_str(), r.expected, r.detected,
                         r.error, r.pass ? "PASS" : "FAIL", r.note.c_str());
    }
    flush();

    int fails = 0, falseSecond = 0, missedSecond = 0, singleCount = 0;
    double maxErr = 0.0, sumErr = 0.0;
    int errCount = 0;
    for (const auto& r : rows)
    {
        fails += ! r.pass;
        if (r.section == "4 secondary") { if (r.secondary && ! r.secondaryExpected) ++falseSecond; if (! r.secondary && r.secondaryExpected) ++missedSecond; }
        else { ++singleCount; if (r.secondary) ++falseSecond; }
        if (r.section != "4 secondary" && r.section != "8 dense mixes" && r.section != "9 drift" && ! r.declined)   // 8 and 9 are benchmarks with their own rules
        {
            maxErr = std::max (maxErr, std::fabs (r.error));
            sumErr += std::fabs (r.error);
            ++errCount;
        }
    }
    std::printf ("=== SUMMARY: %d cases, %d passed, %d failed\n", (int) rows.size(), (int) rows.size() - fails, fails);
    int declinedCount = 0;
    for (const auto& r : rows) declinedCount += r.declined;
    std::printf ("    max |error| %.3f cents, mean |error| %.4f cents over %d measured cases (%d declined: vibrato-only audio, no steady notes)\n",
                 maxErr, sumErr / std::max (1, errCount), errCount, declinedCount);
    std::printf ("    false second tunings reported: %d, genuine second tunings missed: %d\n", falseSecond, missedSecond);

    // results table for the reports: every case with its confidence interval, plus a per-section summary
    if (argc > 2)
    {
        const std::string dir = argv[2];
        std::filesystem::create_directories (dir);
        std::ofstream csv (dir + "/synthetic_results.csv");
        csv << "section,case,expected_cents,detected_cents,error_cents,ci_cents,confidence,tolerance,pass,note\n";
        for (const auto& r : rows)
            csv << '"' << r.section << "\",\"" << r.name << "\"," << r.expected << ',' << r.detected << ',' << r.error << ',' << r.ci << ',' << r.confidence << ',' << r.tol
                << ',' << (r.pass ? "PASS" : "FAIL") << ",\"" << r.note << "\"\n";
        std::ofstream md (dir + "/synthetic_summary.md");
        md << "# Synthetic suite (" << rows.size() << " cases)\n\n| section | cases | passed | max abs error (cents) | mean abs error | median CI (cents) |\n|---|---|---|---|---|---|\n";
        std::vector<std::string> sections;
        for (const auto& r : rows) if (sections.empty() || sections.back() != r.section) sections.push_back (r.section);
        for (const auto& sec : sections)
        {
            int n = 0, ok = 0, m = 0;
            double mx = 0, sm = 0;
            std::vector<double> cis;
            for (const auto& r : rows)
                if (r.section == sec)
                {
                    ++n; ok += r.pass; cis.push_back (r.ci);
                    if (! r.declined && sec != "4 secondary") { mx = std::max (mx, std::fabs (r.error)); sm += std::fabs (r.error); ++m; }
                }
            std::sort (cis.begin(), cis.end());
            char line[256];
            std::snprintf (line, sizeof line, "| %s | %d | %d | %.3f | %.4f | %.2f |\n", sec.c_str(), n, ok, mx, sm / std::max (1, m), cis.empty() ? 0.0 : cis[cis.size() / 2]);
            md << line;
        }
        char tail[256];
        std::snprintf (tail, sizeof tail, "\n**%d / %d passed. Max |error| %.3f cents, mean %.4f. False second tunings: %d.**\n", (int) rows.size() - fails, (int) rows.size(), maxErr, sumErr / std::max (1, errCount), falseSecond);
        md << tail << "\n## Failures\n\n";
        for (const auto& r : rows)
            if (! r.pass)
            {
                char line[320];
                std::snprintf (line, sizeof line, "- %s / %s: expected %+.2f, detected %+.2f, error %+.3f, CI %.2f %s\n", r.section.c_str(), r.name.c_str(), r.expected, r.detected, r.error, r.ci, r.note.c_str());
                md << line;
            }
    }
    return fails ? 1 : 0;
}
