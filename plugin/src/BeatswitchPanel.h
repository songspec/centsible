#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "BeatswitchRunner.h"
#include "PluginProcessor.h"
#include "TuningLookAndFeel.h"

// The Beatswitch panel: a beat switch is where a track changes (new beat, new key, new tempo). The panel asks three things and answers with the two halves side by side:
//   "When does the beat switch?"   a time (1:25, 85, or bar|beat like 33|1) - or press "mark switch" while the song plays and recording
//   "First half BPM" / "Second half BPM"   optional: what you know the tempos to be (shown, and used to say whether the pitch followed the tempo)
//   "add a third part"   a check box for the rare second switch: it shows "When does it switch again?" and "Third part BPM".
// For each half: detune (cents, with its interval), key, tempo; then the difference. "tempo curve" follows detune and tempo over the whole track instead.
// The audio is the one file dropped on the plugin, or what the plugin heard while "record" was on (with the host's tempo). Everything is analysed off the audio thread
// (BeatswitchRunner) with a progress bar; the plugin only reports, it never touches the audio. Drawn in the editor's 320 x 596 design space and scaled with the window.
class BeatswitchPanel : public juce::Component, private juce::Timer
{
public:
    static constexpr float designW = 320.0f, designH = 596.0f;

    BeatswitchPanel (TuningMatchProcessor&, TuningLookAndFeel&);
    ~BeatswitchPanel() override;

    void setScale (float s);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;

    // For tests and scripts: the same actions as the controls.
    void setUseLive (bool live);                                    // which audio: the live recording or the dropped file (pressing record / dropping a file does this)
    void setTimes (const juce::String&);                            // "1:25" (the switch); "1:25, 2:40" also switches the third part on
    void setBpms (const juce::String& first, const juce::String& second, const juce::String& third = {});
    void setTempoText (const juce::String& firstHalfBpm) { setBpms (firstHalfBpm, {}); }
    void setThirdPart (bool on);
    void setUseHostTempo (bool on) { hostTempoForCurve = on; }      // tempo curve: the host's tempo instead of measuring it
    void markSwitch();                                              // live: put the time of the recording so far into the switch box
    void analyse();                                                 // the two (three) parts
    void analyseCurve();                                            // detune and tempo over the whole track
    void useSuggestedCuts();                                        // after a tempo curve: its suggested switches become the switch boxes
    juce::String resultsText() const;                               // the results as plain text (what "copy results" puts on the clipboard)
    void cancel() { runner.cancel(); }
    void poll();                                                    // what the 10 Hz timer does (a test without a message loop calls it by hand)
    void clearResults();                                            // forgets the shown results (not the recording)
    void scrollTo (float y);                                        // design pixels from the top of the results
    float resultsHeight() const { return contentHeight; }
    BeatswitchRunner::Snapshot snapshot() const { return runner.snapshot(); }
    juce::String problem() const { return problemText; }
    bool busy() const { return runner.running(); }
    juce::String sourceText() const { return sourceInfo; }

private:
    void timerCallback() override;
    void updateSourceInfo();
    void layoutControls();
    bool buildJob (BeatswitchRunner::Job&, BeatswitchRunner::Mode);
    void start (BeatswitchRunner::Mode);

    // where things are (they move when the third part is on)
    struct Rows { float switchLabel, switchBox, bpmLabel, bpmBox, thirdToggle, thirdLabel, thirdBox, analyse, status, results; };
    Rows rows() const;

    // painting
    void paintHeader (juce::Graphics&);
    void paintResults (juce::Graphics&, float areaHeight);
    float paintSections (juce::Graphics&, const BeatswitchRunner::Snapshot&, float y);
    float paintTwoHalves (juce::Graphics&, const BeatswitchRunner::Snapshot&, float y);
    float paintAutomation (juce::Graphics&, const BeatswitchRunner::Snapshot&, float y);
    void paintCurve (juce::Graphics&, const centsible::TempoAutomationResult&, juce::Rectangle<float> r);
    float paintParagraph (juce::Graphics&, const juce::String& words, float y, const juce::Font&, juce::Colour, float width = designW, float x = 0.0f);

    TuningMatchProcessor& proc;
    TuningLookAndFeel& laf;
    BeatswitchRunner runner;

    float scale = 1.0f;
    bool live = false, third = false, hostTempoForCurve = true;

    juce::TextButton recordButton { "record" }, clearButton { "clear" }, markButton { "mark switch" };
    juce::TextButton thirdButton { "[   ]  add a third part" };
    juce::TextButton analyseButton { "analyse" }, curveButton { "tempo curve" }, cancelButton { "cancel" };
    juce::TextButton copyButton { "copy results" }, cutsButton { "use as switches" };
    juce::TextEditor switchEditor, switch2Editor, bpm1Editor, bpm2Editor, bpm3Editor;

    juce::String sourceInfo, problemText;
    float scrollY = 0.0f, contentHeight = 0.0f;
    bool recordingShown = false, sourceWarning = false;
    BeatswitchRunner::State shownState = BeatswitchRunner::State::idle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatswitchPanel)
};
