#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <centsible/track.h>
#include <memory>
#include <vector>

// Drag-and-drop analysis: decodes up to two audio files on a background thread and runs the SAME core as the live plugin (centsible::analyzeTrack),
// faster than real time. One file = its tuning, key and tempo; two files = match advice (second file onto the first). Never runs on the audio thread.
class FileAnalyzer : private juce::Thread
{
public:
    enum class State { idle, running, done, failed };

    struct Slot
    {
        State state = State::idle;
        juce::String name;
        centsible::TrackAnalysis result;
        double analysisSeconds = 0.0;   // wall-clock time the analysis took
        std::shared_ptr<const std::vector<float>> audio;   // the decoded mono audio, kept for Beatswitch when ONE file was dropped (two files: not kept)
        double sampleRate = 0.0;
    };

    struct Snapshot
    {
        int count = 0;
        Slot slots[2];
        bool busy() const { for (int i = 0; i < count; ++i) if (slots[i].state == State::running) return true; return false; }
    };

    FileAnalyzer() : juce::Thread ("Centsible file analysis", (size_t) 2 * 1024 * 1024) {}
    ~FileAnalyzer() override { stopThread (10000); }

    static bool isAudioFile (const juce::File& f)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg" || ext == ".aif" || ext == ".aiff" || ext == ".m4a";
    }

    // Message thread. Starts analysing the first two audio files of the list (any previous run is abandoned).
    void start (const juce::StringArray& paths)
    {
        stopThread (10000);
        juce::Array<juce::File> files;
        for (const auto& p : paths)
            if (isAudioFile (juce::File (p)) && files.size() < 2) files.add (juce::File (p));
        const juce::ScopedLock sl (lock);
        pending = files;
        state = {};
        state.count = files.size();
        for (int i = 0; i < files.size(); ++i) { state.slots[i].state = State::running; state.slots[i].name = files[i].getFileName(); }
        if (files.size() > 0) startThread();
    }

    void clear()
    {
        stopThread (10000);
        const juce::ScopedLock sl (lock);
        pending.clear();
        state = {};
    }

    Snapshot snapshot() const
    {
        const juce::ScopedLock sl (lock);
        return state;
    }

private:
    void run() override
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        juce::Array<juce::File> files;
        { const juce::ScopedLock sl (lock); files = pending; }

        for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            Slot slot;
            slot.name = files[i].getFileName();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (files[i]));
            if (reader != nullptr && reader->lengthInSamples > 0)
            {
                const int n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) 1 << 30);
                juce::AudioBuffer<float> buf ((int) reader->numChannels, n);
                reader->read (&buf, 0, n, 0, true, true);
                std::vector<float> mono ((size_t) n, 0.0f);
                const float k = 1.0f / (float) buf.getNumChannels();
                for (int c = 0; c < buf.getNumChannels(); ++c)
                {
                    const float* src = buf.getReadPointer (c);
                    for (int s = 0; s < n; ++s) mono[(size_t) s] += src[s] * k;
                }
                slot.result = centsible::analyzeTrack (mono.data(), mono.size(), reader->sampleRate);
                slot.state = slot.result.tuning.valid ? State::done : State::failed;
                slot.sampleRate = reader->sampleRate;
                if (files.size() == 1) slot.audio = std::make_shared<const std::vector<float>> (std::move (mono));
            }
            else
                slot.state = State::failed;
            slot.analysisSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001;
            const juce::ScopedLock sl (lock);
            state.slots[i] = slot;
        }
    }

    juce::CriticalSection lock;
    juce::Array<juce::File> pending;
    Snapshot state;
};
