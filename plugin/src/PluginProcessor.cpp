#include "PluginProcessor.h"
#include "PluginEditor.h"

TuningMatchProcessor::TuningMatchProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    holdParam = apvts.getRawParameterValue ("hold");
}

juce::AudioProcessorValueTreeState::ParameterLayout TuningMatchProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "hold", 1 }, "Hold", false));
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "react", 1 }, "React", true));
    return l;
}

bool TuningMatchProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == l.getMainInputChannelSet();
}

void TuningMatchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    mono.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    analysis.start (sampleRate);
    setLatencySamples (0);
}

void TuningMatchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Read-only: nothing below writes to `buffer`, so the audio passes through bit-identical.
    if (holdParam->load() >= 0.5f) return;

    const int numCh = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels(), 2);   // never trust the host to agree with itself
    if (numCh == 0) return;

    // While "record" is on, the host's transport (tempo, position, time signature) is handed to the analysis thread without locks: Beatswitch reads bar | beat
    // times and the tempo curve from it. Reading the position is cheap and allocation-free.
    if (analysis.wantsHostInfo())
        if (auto* head = getPlayHead())
            if (auto pos = head->getPosition())
            {
                HostInfo h;
                if (auto bpm = pos->getBpm()) { h.hasBpm = true; h.bpm = *bpm; }
                if (auto ppq = pos->getPpqPosition()) { h.hasPpq = true; h.ppq = *ppq; }
                if (auto t = pos->getTimeInSeconds()) { h.hasTime = true; h.timeSeconds = *t; }
                if (auto ts = pos->getTimeSignature()) { h.numerator = ts->numerator; h.denominator = ts->denominator; }
                h.playing = pos->getIsPlaying();
                analysis.pushHost (h);
            }

    for (int start = 0; start < buffer.getNumSamples(); start += (int) mono.size())
    {
        const int len = juce::jmin ((int) mono.size(), buffer.getNumSamples() - start);
        const float* l = buffer.getReadPointer (0) + start;
        const float* r = buffer.getReadPointer (numCh - 1) + start;
        for (int i = 0; i < len; ++i) mono[(size_t) i] = 0.5f * (l[i] + r[i]);
        analysis.push (mono.data(), len);
    }
}

juce::AudioProcessorEditor* TuningMatchProcessor::createEditor() { return new TuningMatchEditor (*this); }

void TuningMatchProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void TuningMatchProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType())) apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

TuningMatchProcessor::BeatswitchSource TuningMatchProcessor::beatswitchSource (bool live) const
{
    BeatswitchSource src;
    if (live)
    {
        const auto r = analysis.snapshotRecording();
        src.live = true;
        src.audio = r.audio;
        src.sampleRate = r.sampleRate;
        src.seconds = r.seconds;
        src.recordingFull = r.full;
        src.droppedSeconds = r.droppedSeconds;
        src.jumps = r.jumps;
        src.hostTempo = r.hostTempo;
        src.tempoKnown = r.tempoKnown;
        src.barsKnown = r.barsKnown;
        src.firstBarSeconds = r.firstBarSeconds;
        src.beatsPerBar = r.beatsPerBar;
        src.quartersPerBeat = r.quartersPerBeat;
        src.name = "live audio";
    }
    else
    {
        const auto snap = fileAnalyzer.snapshot();
        if (snap.count == 1 && snap.slots[0].audio != nullptr)
        {
            src.audio = snap.slots[0].audio;
            src.sampleRate = snap.slots[0].sampleRate;
            src.seconds = src.sampleRate > 0.0 ? (double) src.audio->size() / src.sampleRate : 0.0;
            src.name = snap.slots[0].name;
        }
    }
    return src;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TuningMatchProcessor(); }
