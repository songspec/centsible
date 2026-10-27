// Whole-track features (key, tempo, match advice) on synthetic songs of known key and tempo, each pitched by a known detune.
// Key rule: exact key = pass; the RELATIVE major/minor is a known weakness of the method, counted separately and still accepted.
#include "gen/synth.h"
#include <centsible/track.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

using namespace synth;

static const double SR = 44100.0;

// Triads on a 4-chord loop in the given key (tonic pitch class 0 = C), bass root, drums at `beat` seconds per quarter note, everything detuned by `cents`.
static Signal song (double seconds, int tonic, bool minorKey, double beat, double cents, unsigned seed)
{
    struct Chord { int degree; bool minor; };
    const Chord major[4] = { { 0, false }, { 7, false }, { 9, true }, { 5, false } };   // I V vi IV
    const Chord minor[4] = { { 0, true }, { 8, false }, { 3, false }, { 10, false } };  // i VI III VII
    Signal x ((size_t) (seconds * SR), 0.0f);
    const double k = centsToRatio (cents), bar = beat * 4.0;
    for (int n = 0; n * bar < seconds; ++n)
    {
        const auto& c = (minorKey ? minor : major)[n % 4];
        const int rootSemis = (tonic + c.degree) % 12 - 21;                              // C3 = 21 semitones below A4
        const int third = c.minor ? 3 : 4;
        for (int iv : { 0, third, 7 })
        {
            const double f0 = noteHz (rootSemis + iv) * k;
            addStaticNote (x, SR, n * bar, (n + 1) * bar, f0, amplitudes (Timbre::Saw, f0, SR, 16), 0.0, 0.07);
        }
        const double fb = noteHz (rootSemis - 12) * k;
        addStaticNote (x, SR, n * bar, (n + 1) * bar, fb, amplitudes (Timbre::Saw, fb, SR, 12), 0.0, 0.12);
    }
    mixInto (x, drumLoop (SR, seconds, seed, beat), 0.3f);
    return x;
}

int main()
{
    int fails = 0;

    // ---- key + tuning
    struct KeyCase { int tonic; bool minor; double cents; };
    std::vector<KeyCase> cases;
    for (int t : { 0, 2, 4, 5, 7, 9, 11 })
    {
        cases.push_back ({ t, false, 0.0 });
        cases.push_back ({ t, false, 37.0 });
        cases.push_back ({ t, true, -24.0 });
    }
    std::vector<centsible::TrackAnalysis> results (cases.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < std::max (2u, std::thread::hardware_concurrency()); ++w)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < cases.size();)
            {
                const auto x = song (45.0, cases[i].tonic, cases[i].minor, 0.5, cases[i].cents, 5 + (unsigned) i);
                results[i] = centsible::analyzeTrack (x.data(), x.size(), SR);
            }
        });
    for (auto& t : pool) t.join();

    int exact = 0, relative = 0;
    std::printf ("[key + tuning]\n");
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const auto& c = cases[i];
        const auto& r = results[i];
        const bool exactKey = r.key.valid && r.key.root == c.tonic && r.key.minor == c.minor;
        const int relRoot = c.minor ? (c.tonic + 3) % 12 : (c.tonic + 9) % 12;
        const bool relKey = r.key.valid && r.key.root == relRoot && r.key.minor != c.minor;
        const double tuningError = wrap50 (r.tuning.cents - c.cents);
        const bool ok = (exactKey || relKey) && std::fabs (tuningError) <= 0.1;
        exact += exactKey; relative += relKey && ! exactKey;
        fails += ! ok;
        std::printf ("  %-10s detune %+6.1f  -> %-10s%s  tuning error %+.3f  %s\n", centsible::keyName (c.tonic, c.minor).c_str(), c.cents,
                     r.key.valid ? centsible::keyName (r.key.root, r.key.minor).c_str() : "-", exactKey ? "" : relKey ? " (relative)" : " WRONG", tuningError, ok ? "PASS" : "FAIL");
    }
    std::printf ("  -> exact key %d / %d, relative key %d, wrong %d\n\n", exact, (int) cases.size(), relative, (int) cases.size() - exact - relative);

    // ---- tempo
    std::printf ("[tempo]\n");
    int tempoOk = 0, tempoTotal = 0, tempoAlt = 0;
    for (double bpm : { 78.0, 90.0, 100.0, 112.0, 120.0, 128.0, 140.0, 160.0 })
    {
        const auto x = song (45.0, 0, false, 60.0 / bpm, 12.0, 3);
        const auto r = centsible::analyzeTrack (x.data(), x.size(), SR);
        const bool exactTempo = r.tempo.valid && std::fabs (r.tempo.bpm - bpm) <= 1.0;
        const bool altTempo = r.tempo.valid && r.tempo.altBpm > 0.0f && std::fabs (r.tempo.altBpm - bpm) <= 1.0;   // octave ambiguity, reported as such
        const bool ok = exactTempo || altTempo;
        ++tempoTotal; tempoOk += exactTempo; tempoAlt += altTempo && ! exactTempo; fails += ! ok;
        std::printf ("  %6.1f BPM -> %7.2f (alt %6.2f, confidence %.2f)  %s%s\n", bpm, (double) r.tempo.bpm, (double) r.tempo.altBpm, (double) r.tempo.confidence,
                     ok ? "PASS" : "FAIL", altTempo && ! exactTempo ? " (only as the half/double alternative)" : "");
    }
    std::printf ("  -> %d / %d within 1 BPM as the main reading, %d more only as the reported half/double alternative\n\n", tempoOk, tempoTotal, tempoAlt);

    // ---- match advice
    {
        centsible::TrackAnalysis a, b;
        a.tuning.cents = -10.0f; a.tuning.valid = true; a.key.valid = true; a.key.root = 0; a.key.minor = false; a.key.scaleRoot = 0; a.key.degree = 0;     // C major, -10
        b.tuning.cents = 20.0f;  b.tuning.valid = true; b.key.valid = true; b.key.root = 2; b.key.minor = true; b.key.scaleRoot = 5; b.key.degree = 5;      // D minor (relative F major), +20
        const auto m = centsible::matchAdvice (a, b);
        // F major -> C major is -5 semitones; fine tune -30
        const bool ok = std::fabs (m.fineCents + 30.0) < 1e-6 && m.semitones == -5 && std::fabs (m.totalCents + 530.0) < 1e-6;
        fails += ! ok;
        std::printf ("[match advice] fine %+.1f, semitones %+d, total %+.1f  %s\n", m.fineCents, m.semitones, m.totalCents, ok ? "PASS" : "FAIL");
    }

    std::printf ("=== features: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
