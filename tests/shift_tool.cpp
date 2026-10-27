// centsible_shift <in audio> <out.wav> <cents> [maxSeconds=180]
// Writes the input (first maxSeconds, mono) resampled by exactly `cents` (Kaiser-sinc) as a 32-bit float WAV. Used by tools/compare_estimators.py so that
// Centsible, librosa and the essentia-style reference all analyse bit-identical samples.
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

int main (int argc, char** argv)
{
    if (argc < 4) { std::printf ("usage: centsible_shift <in> <out.wav> <cents> [maxSeconds]\n"); return 2; }
    double sr = 0;
    auto x = loadMono (juce::File (argv[1]), sr, argc > 4 ? std::atof (argv[4]) : 180.0);
    const double cents = std::atof (argv[3]);
    if (cents != 0.0) x = resample (x, std::pow (2.0, cents / 1200.0));

    FILE* f = std::fopen (argv[2], "wb");
    if (! f) { std::printf ("cannot write %s\n", argv[2]); return 2; }
    const std::uint32_t dataBytes = (std::uint32_t) (x.size() * 4), rate = (std::uint32_t) sr;
    auto u32 = [&] (std::uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (std::uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (4 + (8 + 16) + (8 + 4) + (8 + dataBytes));
    std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (1); u32 (rate); u32 (rate * 4); u16 (4); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((std::uint32_t) x.size());
    std::fwrite ("data", 1, 4, f); u32 (dataBytes);
    std::fwrite (x.data(), 4, x.size(), f);
    std::fclose (f);
    return 0;
}
