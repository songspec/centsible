// Quick probe: what does the analyzer read on simple controlled signals? (developer tool, not part of ctest)
#include "gen/synth.h"
#include <centsible/analyzer.h>
#include <cstdio>
#include <string>

using namespace synth;

int main (int argc, char** argv)
{
    const double sr = 48000.0;
    struct Case { const char* name; Signal x; double truth; };
    const double d = 18.0;
    Signal sine ((size_t) (20 * sr), 0.0f);
    for (int n = 0; n < 25; ++n)
    {
        const double f0 = noteHz ((n * 5) % 24 - 12) * centsToRatio (d);
        addStaticNote (sine, sr, n * 0.8, (n + 1) * 0.8, f0, { 1.0 }, 0.0, 0.3);
    }
    Case cases[] = {
        { "pure sine melody", sine, d },
        { "saw melody (24 partials)", sawMelody (sr, 20, d), d },
        { "saw chords", sawChords (sr, 20, d), d },
        { "voice weak fundamental", singleVoiceMelody (sr, 20, d, Timbre::Voice, 0.0), d },
        { "piano B=4e-4", singleVoiceMelody (sr, 20, d, Timbre::Saw, 4e-4), d },
        { "treble piano B=1e-3", singleVoiceMelody (sr, 20, d, Timbre::Soft, 1e-3, 1), d },
        { "stiff bass B=1e-3", stretchedBass (sr, 20, d, 1e-3), d },
        { "stiff bass B=3e-4", stretchedBass (sr, 20, d, 3e-4), d },
        { "bass24 B=1e-3 +0.4", stretchedBass (sr, 24, 0.4, 1e-3), 0.4 },
        { "bass24 B=1e-3 +49.7", stretchedBass (sr, 24, 49.7, 1e-3), 49.7 },
        { "bass24 B=1e-3 +8.6", stretchedBass (sr, 24, 8.6, 1e-3), 8.6 },
        { "bass24 B=0 +8.6", stretchedBass (sr, 24, 8.6, 0.0), 8.6 },
        { "bass24 B=1e-4 +8.6", stretchedBass (sr, 24, 8.6, 1e-4), 8.6 },
        { "bass24 B=3e-4 +8.6", stretchedBass (sr, 24, 8.6, 3e-4), 8.6 },
        { "vibrato vocal +0.4", vibratoVocal (sr, 30, 0.4, 0.0, 5), 0.4 },
        { "vibrato vocal -50", vibratoVocal (sr, 30, -150.0, 0.0, 5), -50.0 },
    };
    for (auto& c : cases)
    {
        if (argc > 1 && std::string (c.name).find (argv[1]) == std::string::npos) continue;
        const auto r = centsible::analyzeBuffer (c.x.data(), c.x.size(), sr);
        std::printf ("%-28s truth %+6.2f  read %+7.3f  error %+6.3f  ci %.2f\n", c.name, c.truth, (double) r.cents, centsible::wrapCents ((double) r.cents - c.truth), (double) r.ciCents);
    }
}
