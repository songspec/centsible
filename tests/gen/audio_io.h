#pragma once
// Shared audio-file loading for the JUCE-based test tools.
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

inline std::vector<float> loadMono (const juce::File& f, double& sr, double maxSeconds = 1.0e9)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { std::printf ("cannot decode %s\n", f.getFullPathName().toRawUTF8()); std::exit (2); }
    sr = r->sampleRate;
    const int n = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (maxSeconds * sr));
    juce::AudioBuffer<float> buf ((int) r->numChannels, n);
    r->read (&buf, 0, n, 0, true, true);
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i) mono[(size_t) i] += buf.getSample (ch, i) / (float) buf.getNumChannels();
    return mono;
}
