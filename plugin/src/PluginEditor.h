#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "BeatswitchPanel.h"
#include "PluginProcessor.h"
#include "TuningLookAndFeel.h"

// Everything is drawn as vectors in a fixed 440 x 700 design space and scaled to the window, so it stays
// crisp at any size / HiDPI scale. Animation is driven by the display's vblank.
//
// The hero is a Chladni plate: grains of sand wander on a vibrating plate and gather on its nodal lines.
// The grains jitter while the reading is uncertain and settle into crisp lines as it firms up; the plate's
// pattern itself warps with the detune (a square lattice when the song is exactly in tune).
class TuningMatchEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget
{
public:
    explicit TuningMatchEditor (TuningMatchProcessor&);
    ~TuningMatchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // drop one audio file to analyse it (tuning, key, tempo) or two to get match advice
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    // For offscreen rendering (tests/screenshots): jump animated values to their targets and let the sand settle.
    void snapAnimationsToTarget();

    // Beatswitch: the button opens the panel over the plate (what changes inside one track: sections, tempo automation). See BeatswitchPanel.h.
    void showBeatswitch (bool show);
    bool beatswitchShown() const { return panel->isVisible(); }
    BeatswitchPanel& beatswitchPanel() { return *panel; }

    static constexpr float designW = 440.0f, designH = 700.0f;
    static constexpr float panelX = 60.0f, panelY = 44.0f;     // where the panel sits in the design space (it is BeatswitchPanel::designW x designH)

private:
    void tick();
    void updateTargets();
    void stepSand();
    juce::Point<float> toDesign (juce::Point<float> p) const { return p / scale; }

    void paintPlate (juce::Graphics&);
    void paintReadout (juce::Graphics&);
    void paintCorrection (juce::Graphics&);
    void paintHistory (juce::Graphics&);

    TuningMatchProcessor& proc;
    TuningLookAndFeel laf;
    juce::TextButton holdButton { "hold" }, resetButton { "reset" }, reactButton { "react" }, beatswitchButton { "beatswitch" };
    juce::AudioProcessorValueTreeState::ButtonAttachment holdAtt, reactAtt;
    std::unique_ptr<BeatswitchPanel> panel;       // declared after laf: destroyed before it
    juce::ComponentBoundsConstrainer constrainer;
    juce::VBlankAttachment vblank;

    float scale = 1.0f;
    juce::Rectangle<float> copyRect;
    bool copyHover = false;
    double lastTick = 0.0, copiedAt = -10.0;

    centsible::Reading reading;
    bool held = false;
    float shownCents = 0.0f;
    bool shownValid = false;
    FileAnalyzer::Snapshot files;
    bool fileMode = false, matchMode = false, dragHover = false;
    centsible::MatchAdvice match;

    struct Spring { float pos = 0, vel = 0, target = 0; };
    Spring number, warp, calm;
    float copiedFade = 0.0f;
    float level = 0.0f, pulse = 0.0f, levelTarget = 0.0f;
    unsigned lastOnset = 0, lastFrameId = 0;
    struct Dot { float cents, weight, age; };
    std::vector<Dot> dots;
    bool react = true;
    void paintStrip (juce::Graphics&);
    static void step (Spring& s, float dt, float omega, bool wrap50 = false);

    struct Grain { float u, v, size, alpha; };
    std::vector<Grain> grains;
    juce::Random random { 20240917 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuningMatchEditor)
};
