// Runs the real plugin processor and verifies the output is bit-identical to the input,
// and that it still measures a detuned tone while doing so.
#include "PluginProcessor.h"
#include <cstdio>
#include <cstring>
#include <random>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0;

    for (int hold : { 0, 1 })
    {
        TuningMatchProcessor proc;
        const double sr = 48000.0;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        if (auto* p = proc.apvts.getParameter ("hold")) p->setValueNotifyingHost ((float) hold);

        std::mt19937 rng (5);
        std::uniform_real_distribution<float> noise (-0.01f, 0.01f);
        const double detune = -23.0, f0 = 220.0 * std::pow (2.0, detune / 1200.0);
        long long sample = 0, mismatches = 0;
        juce::MidiBuffer midi;

        for (int block = 0; block < 48000 * 14 / 512; ++block)
        {
            juce::AudioBuffer<float> buf (2, 512), ref;
            for (int i = 0; i < 512; ++i, ++sample)
            {
                const double t = sample / sr;
                float v = 0.0f;
                for (int h = 1; h <= 5; ++h) v += (float) (std::sin (2.0 * 3.14159265358979 * f0 * h * t) / h);
                buf.setSample (0, i, 0.3f * v + noise (rng));
                buf.setSample (1, i, 0.3f * v + noise (rng));
            }
            ref.makeCopyOf (buf);
            proc.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                mismatches += std::memcmp (buf.getReadPointer (ch), ref.getReadPointer (ch), 512 * sizeof (float)) != 0;
            if (block % 20 == 0) juce::Thread::sleep (30); // let the analysis thread keep up
        }
        juce::Thread::sleep (500);

        const auto r = proc.getReading();
        std::printf ("hold=%d  non-identical blocks: %lld   reading: %s%.1f cents, A4 = %.1f Hz, conf %.2f, settled %d\n",
                     hold, mismatches, r.valid ? "" : "(none) ", (double) r.cents, (double) r.refHz,
                     (double) r.confidence, (int) r.settled);
        const bool ok = mismatches == 0 && (hold == 1 ? ! r.valid : (r.valid && std::fabs (r.cents - detune) <= 1.0));
        std::printf ("%s\n", ok ? "PASS" : "FAIL");
        fails += ! ok;
        proc.releaseResources();
    }
    return fails ? 1 : 0;
}
