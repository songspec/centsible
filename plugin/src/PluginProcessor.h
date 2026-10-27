#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "AnalysisThread.h"
#include "FileAnalyzer.h"

// Analysis-only plugin: the audio buffer is never written to, so output == input exactly.
class TuningMatchProcessor : public juce::AudioProcessor
{
public:
    TuningMatchProcessor();
    ~TuningMatchProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Centsible"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Message thread only: newest snapshot published by the analysis thread.
    const centsible::Reading& getReading() { return analysis.getReading(); }
    void resetAnalysis() { analysis.requestReset(); }

    // Drag and drop: analyse audio files offline with the same core (one file: tuning/key/tempo, two files: match advice). Message thread only.
    void analyzeFiles (const juce::StringArray& paths) { fileAnalyzer.start (paths); }
    void clearFiles() { fileAnalyzer.clear(); }
    FileAnalyzer::Snapshot fileSnapshot() const { return fileAnalyzer.snapshot(); }

    // Beatswitch: the audio to analyse, either the single file that was dropped or what the plugin heard while "record" was on (with the host's tempo).
    struct BeatswitchSource
    {
        std::shared_ptr<const std::vector<float>> audio;
        double sampleRate = 0.0, seconds = 0.0;
        juce::String name;
        bool live = false, recordingFull = false;
        int jumps = 0;                      // live only: how often the host's position jumped while recording
        double droppedSeconds = 0.0;        // live only: audio the recording lost (the analysis thread could not keep up)
        centsible::TempoMap hostTempo;      // live only: the host's tempo while recording
        bool tempoKnown = false;
        bool barsKnown = false;             // live only: bar 1 beat 1 of the host's timeline is known
        double firstBarSeconds = 0.0;
        int beatsPerBar = 4;
        double quartersPerBeat = 1.0;
    };
    BeatswitchSource beatswitchSource (bool live) const;                 // message thread
    void setRecording (bool on) { analysis.setRecording (on); }
    bool isRecording() const { return analysis.isRecording(); }
    double recordedSeconds() const { return analysis.recordedSeconds(); }
    RecordingInfo recordingInfo() const { return analysis.recordingInfo(); }       // cheap: no audio is copied
    void clearRecording() { analysis.clearRecording(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    AnalysisThread analysis;
    FileAnalyzer fileAnalyzer;
    std::vector<float> mono;
    std::atomic<float>* holdParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuningMatchProcessor)
};
