// Host-style stress test of the real plugin processor: every sample rate / block size / channel layout a DAW may use, repeated prepareToPlay,
// reset mid-stream, zero-length blocks, and hostile input (NaN, Inf, full-scale clipping, silence). The output must stay bit-identical to the input,
// nothing may crash, readings must stay finite, and after the garbage the plugin must still find the tuning of a clean tone.
#include "PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static bool finiteReading (const centsible::Reading& r)
{
    return std::isfinite (r.cents) && std::isfinite (r.ciCents) && std::isfinite (r.confidence) && std::isfinite (r.refHz);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0, cases = 0;
    const double detune = 31.0;
    juce::MidiBuffer midi;

    for (double sr : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int block : { 1, 7, 64, 480, 4096, 8192 })
            for (int channels : { 1, 2 })
            {
                if (block == 1 && (sr != 44100.0 || channels != 1)) continue;   // one-sample blocks: one rate, mono, is enough (and slow)
                TuningMatchProcessor proc;
                proc.setPlayConfigDetails (channels, channels, sr, block);
                proc.prepareToPlay (sr, block);
                proc.prepareToPlay (sr, block);              // hosts do call it repeatedly
                const double f0 = 220.0 * std::pow (2.0, detune / 1200.0);
                const long long total = (long long) (sr * (block == 1 ? 12.0 : 16.0));
                long long sinceSleep = 0;
                long long done = 0, bad = 0;
                bool finite = true;
                for (int pass = 0; done < total; ++pass)
                {
                    juce::AudioBuffer<float> buf (channels, block), ref;
                    const long long period = (long long) (sr * 3.0);
                    const bool garbage = done > 0 && done / period != (done + block) / period;   // one hostile block about every 3 s, whatever the block size
                    for (int i = 0; i < block; ++i)
                    {
                        const double t = (double) (done + i) / sr;
                        float v = 0.0f;
                        for (int h = 1; h <= 5; ++h) v += (float) (std::sin (2.0 * 3.14159265358979 * f0 * h * t) / h);
                        v *= 0.3f;
                        if (garbage)
                        {
                            switch (i % 4)
                            {
                                case 0: v = std::numeric_limits<float>::quiet_NaN(); break;
                                case 1: v = std::numeric_limits<float>::infinity(); break;
                                case 2: v = -4.0f; break;
                                default: v = 0.0f; break;
                            }
                        }
                        for (int ch = 0; ch < channels; ++ch) buf.setSample (ch, i, v);
                    }
                    ref.makeCopyOf (buf);
                    proc.processBlock (buf, midi);
                    for (int ch = 0; ch < channels; ++ch)
                        bad += std::memcmp (buf.getReadPointer (ch), ref.getReadPointer (ch), (size_t) block * sizeof (float)) != 0;
                    juce::AudioBuffer<float> empty (channels, 0);
                    if (pass % 50 == 0) proc.processBlock (empty, midi);   // zero-length block
                    done += block;
                    sinceSleep += block;
                    if (sinceSleep >= (long long) (sr * 0.1)) { juce::Thread::sleep (25); sinceSleep = 0; }   // feed about 4x real time: the analysis keeps up
                    if (pass % 400 == 399) finite = finite && finiteReading (proc.getReading());
                }
                juce::Thread::sleep (700);
                const auto r = proc.getReading();
                finite = finite && finiteReading (r);
                const bool tuned = r.valid && std::fabs (r.cents - detune) <= 3.0;   // the hostile blocks cost some evidence: 3 cents is plenty to prove recovery
                const bool ok = bad == 0 && finite && tuned;
                ++cases; fails += ! ok;
                if (! ok)
                    std::printf ("FAIL sr %.0f block %d ch %d: non-identical %lld, finite %d, reading %s%.2f\n", sr, block, channels, bad, (int) finite, r.valid ? "" : "(none) ", (double) r.cents);
                proc.releaseResources();
            }

    // a reset in the middle of a song, then a different tuning
    {
        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        double result[2] = { 0, 0 };
        for (int song = 0; song < 2; ++song)
        {
            const double d = song == 0 ? -20.0 : 35.0, f0 = 220.0 * std::pow (2.0, d / 1200.0);
            for (int b = 0; b < 48000 * 16 / 512; ++b)
            {
                juce::AudioBuffer<float> buf (2, 512);
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (double) ((long long) b * 512 + i) / 48000.0;
                    float v = 0.0f;
                    for (int h = 1; h <= 5; ++h) v += (float) (std::sin (2.0 * 3.14159265358979 * f0 * h * t) / h);
                    buf.setSample (0, i, 0.3f * v); buf.setSample (1, i, 0.3f * v);
                }
                proc.processBlock (buf, midi);
                if (b % 20 == 0) juce::Thread::sleep (25);
            }
            juce::Thread::sleep (600);
            result[song] = proc.getReading().valid ? proc.getReading().cents : 999.0;
            if (song == 0) proc.resetAnalysis();
        }
        const bool ok = std::fabs (result[0] + 20.0) <= 1.0 && std::fabs (result[1] - 35.0) <= 1.5;
        ++cases; fails += ! ok;
        std::printf ("reset mid-stream: song 1 reads %+.2f (truth -20), song 2 reads %+.2f (truth +35)  %s\n", result[0], result[1], ok ? "PASS" : "FAIL");
    }

    std::printf ("=== robustness: %d cases, %d failed\n", cases, fails);
    return fails ? 1 : 0;
}
