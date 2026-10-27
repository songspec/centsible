#include "BeatswitchPanel.h"

namespace
{
    using namespace Palette;

    // The panel's layout, in its 320 x 596 design space (BeatswitchPanel::rows says where the lower rows are).
    constexpr float sourceY = 22.0f, buttonsY = 46.0f, buttonsH = 24.0f, editH = 28.0f;

    const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
    const juce::String centSign (juce::CharPointer_UTF8 ("\xc2\xa2"));
    const juce::String arrowSign (juce::CharPointer_UTF8 ("\xe2\x86\x92"));
    const juce::String dashSign (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
    const juce::String timesSign (juce::CharPointer_UTF8 ("\xc3\x97"));
    const juce::String plusMinus (juce::CharPointer_UTF8 ("\xc2\xb1"));
    const juce::String approxSign (juce::CharPointer_UTF8 ("\xe2\x89\x88"));
    const juce::String dotSign (juce::CharPointer_UTF8 ("\xc2\xb7"));
    const juce::Colour warn { 0xffe3a0a0 };

    // "+17.0", "-3.4" with a true minus sign, "0.0" (never "-0.0")
    juce::String signedText (double v, int decimals = 1)
    {
        const double k = std::pow (10.0, decimals);
        const double r = std::round (std::fabs (v) * k) / k;
        const juce::String body = decimals > 0 ? juce::String (r, decimals) : juce::String ((int) std::lround (r));
        if (r == 0.0) return body;
        return (v < 0.0 ? minusSign : juce::String ("+")) + body;
    }

    // "3:07", "1:02:03", or with tenths "3:07.4"
    juce::String clockText (double t, bool tenths = false)
    {
        if (! (t > 0.0)) t = 0.0;
        const long long tenthsTotal = (long long) std::floor (t * 10.0 + 0.5);
        const long long whole = tenths ? tenthsTotal / 10 : (long long) std::floor (t + 0.5);
        const int h = (int) (whole / 3600), m = (int) ((whole / 60) % 60), s = (int) (whole % 60);
        juce::String out = h > 0 ? juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2)
                                 : juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
        if (tenths) out += "." + juce::String ((int) (tenthsTotal % 10));
        return out;
    }

    juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    // the key with its Camelot code and how sure the detector is: a relative pair it cannot tell apart is shown as the pair
    juce::String keyLine (const centsible::KeyEstimate& k)
    {
        if (! k.valid) return "key: not enough pitched content";
        if (k.relativeAmbiguous)
            return "key " + toJuce (centsible::keyAmbiguityText (k)) + "  (" + toJuce (centsible::camelot (k.root, k.minor)) + " / "
                   + toJuce (centsible::camelot (centsible::relativeRoot (k), centsible::relativeIsMinor (k))) + ")";
        return "key " + toJuce (centsible::keyLabel (k)) + ", " + juce::String ((int) std::lround (100.0f * k.probability)) + " %";
    }

    juce::String runnerLine (const centsible::KeyEstimate& k)
    {
        if (! k.valid) return {};
        const bool runnerIsRelative = k.runnerUpRoot == centsible::relativeRoot (k) && k.runnerUpMinor == centsible::relativeIsMinor (k);
        if (k.runnerUpProbability >= 0.15f && ! runnerIsRelative) return toJuce (centsible::runnerUpText (k));
        return {};
    }

    juce::String behaviourText (centsible::TempoBehaviour b)
    {
        switch (b)
        {
            case centsible::TempoBehaviour::Varispeed: return "varispeed";
            case centsible::TempoBehaviour::TimeStretch: return "time-stretched";
            case centsible::TempoBehaviour::PitchMoves: return "the pitch moves";
            case centsible::TempoBehaviour::Constant: return "steady";
            case centsible::TempoBehaviour::Mixed: return "tempo and pitch both move";
            default: return "not enough to tell";
        }
    }

    // the tempo of a part: what the user typed (and, when the audio says something else, what was heard), else what was heard
    juce::String bpmText (const std::vector<double>& typed, size_t i, const centsible::TempoEstimate& heard)
    {
        const double t = i < typed.size() ? typed[i] : 0.0;
        if (t > 0.0)
        {
            const double h = heard.valid ? (double) heard.bpm : 0.0;
            const bool differs = h > 0.0 && std::fabs (h - t) / t > 0.03 && std::fabs (h * 2.0 - t) / t > 0.03 && std::fabs (h * 0.5 - t) / t > 0.03;
            return juce::String (t, t == std::floor (t) ? 0 : 1) + " BPM" + (differs ? "  (heard " + juce::String (h, 1) + ")" : juce::String());
        }
        return heard.valid ? juce::String (heard.bpm, 1) + " BPM" : juce::String ("tempo unknown");
    }

    // both tempos typed: did the pitch move by what the tempo change would do (a varispeed), or not (a time-stretch / a tempo change in the music)?
    juce::String varispeedText (const std::vector<double>& typed, size_t from, const centsible::SectionDelta& d)
    {
        if (typed.size() <= from + 1 || typed[from] <= 0.0 || typed[from + 1] <= 0.0 || ! d.tuningKnown) return {};
        const double ratio = typed[from + 1] / typed[from];
        const double expected = 1200.0 * std::log2 (ratio);                                  // cents a varispeed would add
        const double expectedMod = expected - 100.0 * std::floor ((expected + 50.0) / 100.0);  // the detune only shows the part inside one semitone
        const double tol = std::max (3.0, 2.5 * d.ci);
        const double offVarispeed = std::fabs (d.cents - expectedMod), offStretch = std::fabs (d.cents);
        const auto head = "tempo " + timesSign + juce::String (ratio, 3) + " = " + signedText (expected, 0) + " " + centSign + " as a varispeed: ";
        if (std::fabs (expected) < 6.0) return {};
        if (offVarispeed <= tol && offStretch > tol) return head + "the pitch moved by that much: varispeed";
        if (offStretch <= tol && offVarispeed > tol) return head + "the pitch did not move: time-stretch, or a tempo change in the music";
        return head + "the pitch moved, but not by that amount";
    }

    // the grey hint inside an empty field (repainted only when it changes)
    void setPlaceholder (juce::TextEditor& e, const juce::String& hint)
    {
        if (e.getTextToShowWhenEmpty() == hint) return;
        e.setTextToShowWhenEmpty (hint, textFaint);
        e.repaint();
    }

    // one line of text in a box, ellipsised when it does not fit
    void put (juce::Graphics& g, const juce::Font& f, const juce::String& s, float x, float y, float w, juce::Justification j = juce::Justification::centredLeft, float h = 16.0f)
    {
        g.setFont (f);
        g.drawText (s, juce::Rectangle<float> (x, y, w, h), j, true);
    }
}

BeatswitchPanel::BeatswitchPanel (TuningMatchProcessor& p, TuningLookAndFeel& l) : proc (p), laf (l)
{
    setOpaque (true);
    for (auto* b : { &recordButton, &clearButton, &markButton, &thirdButton, &analyseButton, &curveButton, &cancelButton, &copyButton, &cutsButton })
        addAndMakeVisible (*b);
    thirdButton.setClickingTogglesState (true);
    thirdButton.onClick = [this] { setThirdPart (thirdButton.getToggleState()); };
    recordButton.onClick = [this]
    {
        const bool on = ! proc.isRecording();
        proc.setRecording (on);
        if (on) live = true;
        problemText = {};
        updateSourceInfo();
        layoutControls();
        repaint();
    };
    clearButton.onClick = [this] { proc.clearRecording(); updateSourceInfo(); repaint(); };
    markButton.onClick = [this] { markSwitch(); };
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (resultsText()); copyButton.setButtonText ("copied"); };
    cutsButton.onClick = [this] { useSuggestedCuts(); };
    copyButton.setVisible (false);
    cutsButton.setVisible (false);
    analyseButton.onClick = [this] { analyse(); };
    analyseButton.setToggleState (true, juce::dontSendNotification);       // the primary action: drawn with the accent
    curveButton.onClick = [this] { analyseCurve(); };
    cancelButton.onClick = [this] { runner.cancel(); };
    cancelButton.setVisible (false);

    for (auto* e : { &switchEditor, &switch2Editor, &bpm1Editor, &bpm2Editor, &bpm3Editor })
    {
        addAndMakeVisible (*e);
        e->setMultiLine (false);
        e->setReturnKeyStartsNewLine (false);
        e->setScrollbarsShown (false);
        e->setSelectAllWhenFocused (false);
        e->setColour (juce::TextEditor::backgroundColourId, plate);
        e->setColour (juce::TextEditor::outlineColourId, plateEdge);
        e->setColour (juce::TextEditor::focusedOutlineColourId, sand.withAlpha (0.7f));
        e->setColour (juce::TextEditor::textColourId, text);
        e->setColour (juce::TextEditor::highlightColourId, sand.withAlpha (0.30f));
        e->setColour (juce::TextEditor::highlightedTextColourId, text);
        e->setColour (juce::CaretComponent::caretColourId, sand);
        e->onReturnKey = [this] { analyse(); };
        e->onTextChange = [this] { if (problemText.isNotEmpty()) { problemText = {}; repaint(); } };
    }
    setPlaceholder (switchEditor, "e.g.  1:25   or   33|1");
    setPlaceholder (switch2Editor, "e.g.  2:40");
    switch2Editor.setVisible (false);
    bpm3Editor.setVisible (false);
    updateSourceInfo();
}

BeatswitchPanel::~BeatswitchPanel() { stopTimer(); }

void BeatswitchPanel::visibilityChanged()
{
    if (isVisible()) { startTimerHz (10); updateSourceInfo(); layoutControls(); }
    else stopTimer();
}

void BeatswitchPanel::setScale (float s)
{
    scale = s;
    layoutControls();
}

void BeatswitchPanel::setUseLive (bool l)
{
    live = l;
    problemText = {};
    updateSourceInfo();
    layoutControls();
    repaint();
}

void BeatswitchPanel::setTimes (const juce::String& t)
{
    juce::StringArray parts;
    parts.addTokens (t, ",;", "");
    parts.trim();
    switchEditor.setText (parts.size() > 0 ? parts[0] : juce::String(), juce::dontSendNotification);
    if (parts.size() > 1 && parts[1].isNotEmpty())
    {
        setThirdPart (true);
        switch2Editor.setText (parts[1], juce::dontSendNotification);
    }
    problemText = {};
}

void BeatswitchPanel::setBpms (const juce::String& a, const juce::String& b, const juce::String& c)
{
    bpm1Editor.setText (a, juce::dontSendNotification);
    bpm2Editor.setText (b, juce::dontSendNotification);
    bpm3Editor.setText (c, juce::dontSendNotification);
    problemText = {};
}

void BeatswitchPanel::setThirdPart (bool on)
{
    third = on;
    thirdButton.setToggleState (on, juce::dontSendNotification);
    thirdButton.setButtonText (on ? "[x]  third part" : "[   ]  add a third part");
    layoutControls();
    repaint();
}

void BeatswitchPanel::clearResults()
{
    if (runner.running()) runner.cancel();       // (does not wait for it: the message thread stays free; the panel then says "cancelled")
    else runner.clear();
    scrollY = 0.0f;
    problemText = {};
    layoutControls();
    repaint();
}

void BeatswitchPanel::scrollTo (float y)
{
    scrollY = std::max (0.0f, y);
    repaint();
}

void BeatswitchPanel::resized() { layoutControls(); }

BeatswitchPanel::Rows BeatswitchPanel::rows() const
{
    Rows r {};
    r.switchLabel = 80.0f;  r.switchBox = 98.0f;
    r.bpmLabel = 136.0f;    r.bpmBox = 154.0f;
    r.thirdToggle = 192.0f;
    float y = 228.0f;
    if (third) { r.thirdLabel = 228.0f; r.thirdBox = 246.0f; y = 284.0f; }
    else r.thirdLabel = r.thirdBox = -100.0f;
    r.analyse = y;
    r.status = y + 38.0f;
    r.results = y + 68.0f;
    return r;
}

void BeatswitchPanel::layoutControls()
{
    auto place = [this] (juce::Component& c, float x, float y, float w, float h)
    {
        c.setBounds (juce::Rectangle<float> (x * scale, y * scale, w * scale, h * scale).toNearestInt());
    };
    const auto r = rows();
    const bool running = runner.running();
    place (recordButton, 0, buttonsY, 70, buttonsH);
    place (clearButton, 76, buttonsY, 52, buttonsH);
    place (markButton, 134, buttonsY, 100, buttonsH);
    place (switchEditor, 0, r.switchBox, 320, editH);
    place (bpm1Editor, 0, r.bpmBox, 156, editH);
    place (bpm2Editor, 164, r.bpmBox, 156, editH);
    place (thirdButton, 0, r.thirdToggle, 150, buttonsH);
    place (switch2Editor, 0, r.thirdBox, 156, editH);
    place (bpm3Editor, 164, r.thirdBox, 156, editH);
    switch2Editor.setVisible (third);
    bpm3Editor.setVisible (third);
    place (analyseButton, 0, r.analyse, 200, 30);
    place (curveButton, 208, r.analyse, 112, 30);
    place (cancelButton, 244, r.analyse, 76, 30);
    place (copyButton, 0, r.status, 96, 22);
    place (cutsButton, 104, r.status, 120, 22);
    {
        const auto sn = runner.snapshot();
        const bool done = sn.state == BeatswitchRunner::State::done && problemText.isEmpty();
        bool anyCut = false;
        if (done && sn.mode == BeatswitchRunner::Mode::tempo)
            for (const auto& b : sn.automation.boundaries) anyCut = anyCut || (! b.passageStart && ! b.passageEnd);
        copyButton.setVisible (done);
        if (! done) copyButton.setButtonText ("copy results");
        cutsButton.setVisible (anyCut);
    }
    recordButton.setButtonText (proc.isRecording() ? "stop" : "record");
    recordButton.setToggleState (proc.isRecording(), juce::dontSendNotification);
    analyseButton.setVisible (! running);
    curveButton.setVisible (! running);
    cancelButton.setVisible (running);
    const auto font = laf.font (14.0f * scale);
    for (auto* e : { &switchEditor, &switch2Editor, &bpm1Editor, &bpm2Editor, &bpm3Editor })
    {
        e->applyFontToAllText (font);
        e->setFont (font);
        e->setIndents ((int) std::lround (8.0f * scale), (int) std::lround (6.0f * scale));
        e->setBorder (juce::BorderSize<int> (0, 0, 0, 0));
    }
}

void BeatswitchPanel::updateSourceInfo()
{
    juce::String info;
    juce::String bpmHint = "optional";
    if (live)
    {
        const auto ri = proc.recordingInfo();
        juce::String bpm;
        if (ri.tempoKnown)
        {
            bpm = "  " + dotSign + "  host " + (ri.bpmMax - ri.bpmMin > 0.05 ? juce::String (ri.bpmMin, 1) + dashSign + juce::String (ri.bpmMax, 1) : juce::String (ri.bpmMax, 1)) + " BPM";
            bpmHint = "optional (host: " + juce::String (ri.bpmMax, 0) + ")";
        }
        if (proc.isRecording()) info = "recording " + clockText (ri.seconds) + bpm;
        else if (ri.seconds > 0.5) info = "recording: " + clockText (ri.seconds) + (ri.full ? " (full)" : "") + bpm;
        else info = "press record, then play the song in your DAW";
        sourceWarning = ri.droppedSeconds > 0.02;
        if (sourceWarning) info = "gaps! " + info;
        if (ri.jumps > 0) { info = "seek! " + info; sourceWarning = true; }
        if (proc.apvts.getRawParameterValue ("hold")->load() >= 0.5f) { info = "hold is on: not listening"; sourceWarning = true; }
    }
    else
    {
        sourceWarning = false;
        const auto files = proc.fileSnapshot();
        if (files.count == 0) info = "drop an audio file on the plugin, or press record and play the song";
        else if (files.count > 1) info = "two files dropped: drop just one for Beatswitch";
        else if (files.slots[0].state == FileAnalyzer::State::running) info = "reading " + files.slots[0].name + "...";
        else if (files.slots[0].audio != nullptr && files.slots[0].sampleRate > 0.0)
            info = files.slots[0].name + "   " + clockText ((double) files.slots[0].audio->size() / files.slots[0].sampleRate);
        else info = files.slots[0].name + " could not be read";
    }
    for (auto* e : { &bpm1Editor, &bpm2Editor, &bpm3Editor }) setPlaceholder (*e, bpmHint);
    if (info != sourceInfo) { sourceInfo = info; repaint(); }
    if (recordingShown != proc.isRecording()) { recordingShown = proc.isRecording(); layoutControls(); }
}

void BeatswitchPanel::timerCallback() { poll(); }

void BeatswitchPanel::poll()
{
    updateSourceInfo();
    const auto state = runner.snapshot().state;
    if (state != shownState) { shownState = state; layoutControls(); repaint(); }
    else if (state == BeatswitchRunner::State::running) repaint();
}

bool BeatswitchPanel::buildJob (BeatswitchRunner::Job& job, BeatswitchRunner::Mode mode)
{
    problemText = {};
    const auto src = proc.beatswitchSource (live);
    if (! src.audio || src.sampleRate <= 0.0 || src.audio->size() < (size_t) (4.0 * src.sampleRate))
    {
        if (live) problemText = "record at least a few seconds first (press record, then play the song)";
        else if (proc.fileSnapshot().busy()) problemText = "the file is still being read, one moment";
        else problemText = "no audio yet: drop one audio file on the plugin, or press record and play the song";
        return false;
    }
    job.audio = src.audio;
    job.sampleRate = src.sampleRate;
    job.sourceName = src.name;
    job.mode = mode;
    if (live && src.jumps > 0)
        job.warning = "The host's position jumped " + juce::String (src.jumps) + " time(s) while recording (a seek or a loop): bar | beat times after the first jump do not fit the recording. Clear it and record one pass.";
    else if (live && src.droppedSeconds > 0.02)
        job.warning = "The recording lost " + juce::String (src.droppedSeconds, 1) + " s of audio (the plugin could not keep up while recording): everything after the first gap is "
                      "shifted in time. Clear it and record again.";
    else if (live && src.recordingFull)
        job.warning = "The recording is full (about 11 minutes): the rest was not kept.";

    // the tempos the user typed
    const int parts = third ? 3 : 2;
    juce::TextEditor* bpmEditors[3] = { &bpm1Editor, &bpm2Editor, &bpm3Editor };
    const char* bpmNames[3] = { "First half BPM", "Second half BPM", "Third part BPM" };
    for (int i = 0; i < parts; ++i)
    {
        const auto txt = bpmEditors[i]->getText().trim();
        double bpm = 0.0;
        if (txt.isNotEmpty())
        {
            const auto parsed = centsible::parseTempoText (txt.toStdString(), true);
            if (! parsed.ok || parsed.map.points().size() != 1) { problemText = juce::String (bpmNames[i]) + ": '" + txt + "' is not a tempo (a number of BPM between 20 and 400, like 140)"; return false; }
            bpm = parsed.map.points().front().bpm;
        }
        job.typedBpm.push_back (bpm);
    }

    // bar | beat: the host's tempo when it was recorded, else the first half's typed tempo
    const bool hostTempo = live && src.tempoKnown;
    centsible::TempoMap barsMap = hostTempo ? src.hostTempo : (job.typedBpm[0] > 0.0 ? centsible::TempoMap::constant (job.typedBpm[0]) : centsible::TempoMap());
    job.time.beatsPerBar = live ? src.beatsPerBar : 4;
    job.time.quartersPerBeat = live ? src.quartersPerBeat : 1.0;
    job.time.firstBarSeconds = (hostTempo && src.barsKnown) ? src.firstBarSeconds : 0.0;
    job.barsMap = barsMap;

    if (mode == BeatswitchRunner::Mode::tempo)
    {
        if (hostTempo && hostTempoForCurve) job.tempoMap = src.hostTempo;
        job.time.tempoMap = nullptr;
        return true;
    }

    centsible::TimeContext ctx = job.time;
    ctx.tempoMap = barsMap.valid() ? &barsMap : nullptr;
    juce::TextEditor* switchEditors[2] = { &switchEditor, &switch2Editor };
    const char* switchNames[2] = { "When does the beat switch?", "When does it switch again?" };
    for (int i = 0; i < (third ? 2 : 1); ++i)
    {
        const auto txt = switchEditors[i]->getText().trim();
        if (txt.isEmpty()) { problemText = i == 0 ? "type when the beat switches (like 1:25), or press mark switch while the song plays" : "type when it switches again (like 2:40), or switch the third part off"; return false; }
        const auto parsed = centsible::parseTimestampList (txt.toStdString(), ctx);
        if (! parsed.ok) { problemText = toJuce (parsed.error); return false; }
        if (parsed.seconds.size() != 1) { problemText = juce::String (switchNames[i]) + ": one time only here"; return false; }
        const double t = parsed.seconds[0];
        if (t <= 0.0 || t >= src.seconds - 0.1)
        {
            problemText = "'" + toJuce (centsible::formatTimestamp (t)) + "' is outside the audio (0:00 " + dashSign + " " + clockText (src.seconds, true) + ")";
            return false;
        }
        if (! job.boundaries.empty() && t <= job.boundaries.back() + 1.0) { problemText = "the second switch must come after the first one"; return false; }
        job.boundaries.push_back (t);
    }
    job.time.tempoMap = nullptr;       // (the runner shows bar | beat from job.barsMap)
    return true;
}

void BeatswitchPanel::start (BeatswitchRunner::Mode mode)
{
    if (runner.running()) return;
    BeatswitchRunner::Job job;
    if (! buildJob (job, mode)) { repaint(); return; }
    scrollY = 0.0f;
    runner.start (std::move (job));
    shownState = BeatswitchRunner::State::running;
    layoutControls();
    repaint();
}

void BeatswitchPanel::analyse() { start (BeatswitchRunner::Mode::sections); }
void BeatswitchPanel::analyseCurve() { start (BeatswitchRunner::Mode::tempo); }

void BeatswitchPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float areaH = designH - rows().results;
    scrollY = std::clamp (scrollY - w.deltaY * 120.0f, 0.0f, std::max (0.0f, contentHeight - areaH));
    repaint();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// painting

float BeatswitchPanel::paintParagraph (juce::Graphics& g, const juce::String& words, float y, const juce::Font& font, juce::Colour colour, float width, float x)
{
    if (words.isEmpty()) return 0.0f;
    juce::AttributedString as;
    as.append (words, font, colour);
    as.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (as, width);
    layout.draw (g, juce::Rectangle<float> (x, y, width, layout.getHeight() + 2.0f));
    return layout.getHeight() + 2.0f;
}

void BeatswitchPanel::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.addTransform (juce::AffineTransform::scale (scale));
        paintHeader (g);
    }

    // the results: scrollable, clipped to their own area
    const float resultsY = rows().results;
    const float areaH = designH - resultsY;
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (juce::Rectangle<float> (0.0f, resultsY * scale, designW * scale, areaH * scale).toNearestInt());
    g.addTransform (juce::AffineTransform::scale (scale).translated (0.0f, (resultsY - scrollY) * scale));
    paintResults (g, areaH);
}

void BeatswitchPanel::paintHeader (juce::Graphics& g)
{
    const auto r = rows();
    g.setColour (textDim);
    put (g, laf.font (16.0f, true), "beatswitch", 0.0f, 0.0f, 120.0f, juce::Justification::centredLeft, 20.0f);

    // the song
    g.setColour (sourceWarning ? warn : live && proc.isRecording() ? sand : textDim);
    put (g, laf.font (13.0f), sourceInfo, 0.0f, sourceY, 320.0f, juce::Justification::centredLeft, 18.0f);

    // the questions
    g.setColour (text);
    put (g, laf.font (14.0f, true), "When does the beat switch?", 0.0f, r.switchLabel, 320.0f, juce::Justification::centredLeft, 17.0f);
    g.setColour (textDim);
    put (g, laf.font (13.0f), "First half BPM", 0.0f, r.bpmLabel, 156.0f, juce::Justification::centredLeft, 16.0f);
    put (g, laf.font (13.0f), "Second half BPM", 164.0f, r.bpmLabel, 156.0f, juce::Justification::centredLeft, 16.0f);
    if (third)
    {
        g.setColour (text);
        put (g, laf.font (14.0f, true), "When does it switch again?", 0.0f, r.thirdLabel, 156.0f, juce::Justification::centredLeft, 17.0f);
        g.setColour (textDim);
        put (g, laf.font (13.0f), "Third part BPM", 164.0f, r.thirdLabel, 156.0f, juce::Justification::centredLeft, 17.0f);
    }

    // progress, or what went wrong
    const auto s = runner.snapshot();
    if (s.state == BeatswitchRunner::State::running)
    {
        const juce::Rectangle<float> bar (0.0f, r.analyse + 10.0f, 236.0f, 10.0f);
        g.setColour (hairline);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (sand.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withWidth (std::max (6.0f, (float) s.progress * bar.getWidth())), 3.0f);
        g.setColour (textDim);
        put (g, laf.font (12.0f), s.stage + "   " + juce::String ((int) std::lround (100.0 * s.progress)) + " %   " + juce::String (s.elapsed, 0) + " s", 0.0f, r.status, 320.0f,
             juce::Justification::centredLeft, 16.0f);
    }
    else if (problemText.isNotEmpty())
    {
        juce::AttributedString as;
        as.append (problemText, laf.font (12.5f), warn);
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout (as, 320.0f);
        layout.draw (g, juce::Rectangle<float> (0.0f, r.status, 320.0f, r.results - r.status - 2.0f));
    }
}

void BeatswitchPanel::paintResults (juce::Graphics& g, float areaHeight)
{
    const auto s = runner.snapshot();
    float y = 4.0f;
    if (s.state == BeatswitchRunner::State::idle)
    {
        y += paintParagraph (g, "Play the song with record on and press mark switch when the beat switches (or type the time). Then analyse: you get the first half and the second half "
                                "side by side, each with its detune, key and tempo, and what changed between them.",
                             y, laf.font (13.0f), textFaint);
        y += 4.0f;
        y += paintParagraph (g, "The BPM boxes are optional. If you type both, it also says whether the pitch moved with the tempo (a varispeed) or not (a time-stretch). "
                                "tempo curve follows detune and tempo over the whole song instead.",
                             y, laf.font (12.0f), textFaint);
        contentHeight = y + 12.0f;
        return;
    }
    if (s.state == BeatswitchRunner::State::running)
    {
        paintParagraph (g, "analysing " + s.sourceName + " (" + clockText (s.audioSeconds) + ")...", y, laf.font (13.0f), textFaint);
        contentHeight = 40.0f;
        return;
    }
    if (s.state == BeatswitchRunner::State::cancelled)
    {
        paintParagraph (g, "cancelled", y, laf.font (13.0f), textFaint);
        contentHeight = 40.0f;
        return;
    }

    if (contentHeight > areaHeight)
    {
        // a thin scroll indicator on the right edge, drawn first so that the text lies over it (the height is the one of the previous paint)
        const float trackH = areaHeight - 8.0f, barH = std::max (24.0f, trackH * areaHeight / contentHeight);
        const float barY = scrollY + 4.0f + (trackH - barH) * (scrollY / std::max (1.0f, contentHeight - areaHeight));
        g.setColour (textFaint.withAlpha (0.35f));
        g.fillRoundedRectangle (318.0f, barY, 2.0f, barH, 1.0f);
    }

    y = s.mode == BeatswitchRunner::Mode::sections ? paintSections (g, s, y) : paintAutomation (g, s, y);
    g.setColour (textFaint);
    put (g, laf.font (12.0f), s.sourceName + " " + dotSign + " analysed in " + juce::String (s.elapsed, 1) + " s", 0.0f, y + 6.0f, 320.0f, juce::Justification::centredLeft, 14.0f);
    y += 24.0f;
    if (s.warning.isNotEmpty()) y += 4.0f + paintParagraph (g, s.warning, y, laf.font (12.5f), warn);
    contentHeight = y + 8.0f;
    scrollY = std::clamp (scrollY, 0.0f, std::max (0.0f, contentHeight - areaHeight));
}

void BeatswitchPanel::useSuggestedCuts()
{
    const auto sn = runner.snapshot();
    std::vector<double> times;
    for (const auto& b : sn.automation.boundaries)
        if (! b.passageStart && ! b.passageEnd) times.push_back (b.time);
    if (times.empty()) { problemText = "no switch was suggested"; repaint(); return; }
    switchEditor.setText (clockText (times[0], true), juce::dontSendNotification);
    if (times.size() > 1)
    {
        setThirdPart (true);
        switch2Editor.setText (clockText (times[1], true), juce::dontSendNotification);
    }
    else setThirdPart (false);
    problemText = {};
    analyse();
}

juce::String BeatswitchPanel::resultsText() const
{
    const auto s = runner.snapshot();
    juce::String t = "Centsible beatswitch: " + s.sourceName + "\n";
    if (s.state != BeatswitchRunner::State::done) return t + "(no results)\n";
    auto cents = [] (double v, double ci) { return signedText (v) + " " + plusMinus + " " + juce::String (std::max (0.1, ci), 1) + " " + centSign; };
    if (s.mode == BeatswitchRunner::Mode::sections)
    {
        for (std::size_t i = 0; i < s.sections.sections.size(); ++i)
        {
            const auto& sec = s.sections.sections[i];
            t += toJuce (sec.label) + "  " + clockText (sec.start, true) + " - " + clockText (sec.end, true);
            const auto bars = s.bars (sec.start);
            if (bars.isNotEmpty()) t += "  (" + bars + ")";
            if (! sec.ok) { t += "  " + toJuce (sec.note) + "\n"; }
            else
            {
                const auto& tu = sec.analysis.tuning;
                t += "  " + (tu.valid ? cents (tu.cents, tu.ciCents) : juce::String ("no steady pitch"));
                t += "  " + (sec.analysis.key.valid ? toJuce (centsible::keyLabel (sec.analysis.key)) : juce::String ("key unknown"));
                if (sec.analysis.tempo.valid) t += "  " + juce::String (sec.analysis.tempo.bpm, 1) + " BPM";
                t += "\n";
            }
            if (i < s.sections.deltas.size() && s.sections.deltas[i].tuningKnown)
            {
                const auto& d = s.sections.deltas[i];
                t += "   " + toJuce (centsible::sectionLabel (d.from)) + " -> " + toJuce (centsible::sectionLabel (d.to)) + ": " + cents (d.cents, d.ci) + (d.significant ? " (significant)" : " (within the error)") + "\n";
            }
        }
    }
    else
    {
        t += toJuce (s.automation.summary) + "\n";
        int n = 0;
        for (const auto& r : s.automation.regions)
            t += "region " + juce::String (++n) + "  " + clockText (r.start, true) + " - " + clockText (r.end, true) + "  " + (r.ok ? cents (r.pitchMoves ? r.centsEnd : r.cents, r.ci) : juce::String ("too little pitched audio"))
                 + (r.bpm > 0 ? "  " + juce::String (r.bpm, 1) + " BPM" : juce::String()) + (r.key.valid ? "  " + toJuce (centsible::keyLabel (r.key)) : juce::String()) + "\n";
    }
    if (s.warning.isNotEmpty()) t += s.warning + "\n";
    return t;
}

void BeatswitchPanel::markSwitch()
{
    const double t = proc.recordedSeconds();
    if (t < 1.0) { problemText = "record first: press record, play the song, and press mark switch when the beat switches"; repaint(); return; }
    problemText = {};
    live = true;
    // the first press is the switch; a second press goes to "switch again" when the third part is on, otherwise it replaces the switch
    if (third && switchEditor.getText().trim().isNotEmpty() && switch2Editor.getText().trim().isEmpty()) switch2Editor.setText (clockText (t, true), juce::dontSendNotification);
    else switchEditor.setText (clockText (t, true), juce::dontSendNotification);
    updateSourceInfo();
    repaint();
}

// One cut: the two halves side by side, then the difference
float BeatswitchPanel::paintTwoHalves (juce::Graphics& g, const BeatswitchRunner::Snapshot& s, float y)
{
    const auto& r = s.sections;
    const float colW = 154.0f;
    float bottom = y;
    for (int i = 0; i < 2; ++i)
    {
        const auto& sec = r.sections[(size_t) i];
        const float x = i == 0 ? 0.0f : 166.0f;
        float yy = y;
        g.setColour (plate);
        g.fillRoundedRectangle (x - 4.0f, y - 3.0f, colW + 8.0f, 140.0f, 4.0f);
        g.setColour (sand);
        put (g, laf.font (13.0f, true), i == 0 ? "first half" : "second half", x, yy, colW, juce::Justification::centredLeft, 16.0f);
        yy += 16.0f;
        g.setColour (textFaint);
        put (g, laf.font (12.0f), clockText (sec.start, true) + " " + dashSign + " " + clockText (sec.end, true), x, yy, colW, juce::Justification::centredLeft, 14.0f);
        yy += 18.0f;
        if (! sec.ok)
        {
            yy += paintParagraph (g, toJuce (sec.note), yy, laf.font (12.0f), warn, colW, x);
        }
        else
        {
            const auto& t = sec.analysis.tuning;
            g.setColour (t.valid ? text : textFaint);
            put (g, laf.font (22.0f, true), t.valid ? signedText (t.cents) + " " + centSign : juce::String ("no pitch"), x, yy, colW, juce::Justification::centredLeft, 26.0f);
            yy += 26.0f;
            if (t.valid)
            {
                g.setColour (textFaint);
                put (g, laf.font (12.0f), plusMinus + " " + juce::String (std::max (0.1f, t.ciCents), 1) + " " + centSign, x, yy, colW, juce::Justification::centredLeft, 14.0f);
            }
            yy += 16.0f;
            const auto& k = sec.analysis.key;
            g.setColour (textDim);
            put (g, laf.font (13.0f), k.valid ? toJuce (centsible::keyLabel (k)) : juce::String ("key unknown"), x, yy, colW, juce::Justification::centredLeft, 16.0f);
            yy += 16.0f;
            if (k.valid)
            {
                g.setColour (textFaint);
                put (g, laf.font (11.5f), toJuce (centsible::modeSummary (k, 2)), x, yy, colW, juce::Justification::centredLeft, 14.0f);
                g.setColour (textDim);
                yy += 15.0f;
            }
            put (g, laf.font (13.0f), bpmText (s.typedBpm, (size_t) i, sec.analysis.tempo), x, yy, colW, juce::Justification::centredLeft, 16.0f);
            yy += 16.0f;
        }
        bottom = std::max (bottom, yy);
    }
    y = y + 144.0f;
    if (! r.deltas.empty())
    {
        const auto& d = r.deltas[0];
        g.setColour (sand);
        put (g, laf.font (14.0f, true), "A " + arrowSign + " B", 0.0f, y, 60.0f, juce::Justification::centredLeft, 17.0f);
        if (d.tuningKnown)
        {
            g.setColour (d.significant ? text : textDim);
            put (g, laf.font (14.0f, true), signedText (d.cents) + " " + plusMinus + " " + juce::String (std::max (0.1, d.ci), 1) + " " + centSign, 60.0f, y, 140.0f, juce::Justification::centredLeft, 17.0f);
            g.setColour (d.significant ? sand : textFaint);
            put (g, laf.font (12.0f), d.significant ? "different tuning" : "same tuning", 200.0f, y, 120.0f, juce::Justification::centredRight, 17.0f);
        }
        y += 19.0f;
        juce::String second;
        if (d.keysKnown) second = d.sameKey ? "same key" : d.sameScale ? "same scale (relative key or mode)" : "scale " + signedText (d.semitones, 0) + " semitones";
        if (d.tempoKnown) second += (second.isEmpty() ? juce::String() : "  " + dotSign + "  ") + (std::fabs (d.tempoCents) < 3.0 ? juce::String ("same tempo") : "tempo " + timesSign + juce::String (d.tempoRatio, 3));
        if (second.isNotEmpty()) y += paintParagraph (g, second, y, laf.font (12.0f), textDim);
        const auto vari = varispeedText (s.typedBpm, 0, d);
        if (vari.isNotEmpty()) y += paintParagraph (g, vari, y, laf.font (12.0f), sand);
        if (d.tuningKnown && d.significant && std::fabs (d.cents) >= 0.5)
            y += paintParagraph (g, "shift B by " + signedText (-d.cents) + " " + centSign + " to match A", y, laf.font (12.0f), textDim);
    }
    return y + 6.0f;
}

float BeatswitchPanel::paintSections (juce::Graphics& g, const BeatswitchRunner::Snapshot& s, float y)
{
    const auto& r = s.sections;
    if (r.sections.empty())
    {
        paintParagraph (g, "nothing to analyse", y, laf.font (13.0f), textDim);
        return y + 20.0f;
    }
    if (r.sections.size() == 2) return paintTwoHalves (g, s, y);
    for (std::size_t i = 0; i < r.sections.size(); ++i)
    {
        const auto& sec = r.sections[i];
        const auto label = toJuce (sec.label);
        const auto bars = s.bars (sec.start);
        g.setColour (text);
        put (g, laf.font (16.0f, true), label, 0.0f, y, 24.0f, juce::Justification::centredLeft, 19.0f);
        g.setColour (textDim);
        put (g, laf.font (13.0f), clockText (sec.start, true) + " " + dashSign + " " + clockText (sec.end, true) + (bars.isNotEmpty() ? "   " + bars : juce::String()), 24.0f, y + 1.0f, 170.0f,
            juce::Justification::centredLeft, 18.0f);
        if (! sec.ok)
        {
            y += 21.0f;
            y += paintParagraph (g, toJuce (sec.note), y, laf.font (12.0f), warn);
            y += 8.0f;
        }
        else
        {
            const auto& t = sec.analysis.tuning;
            if (t.valid)
            {
                g.setColour (text);
                put (g, laf.font (16.0f, true), signedText (t.cents) + " " + plusMinus + " " + juce::String (std::max (0.1f, t.ciCents), 1) + " " + centSign, 150.0f, y, 170.0f,
                     juce::Justification::centredRight, 19.0f);
            }
            else
            {
                g.setColour (textFaint);
                put (g, laf.font (13.0f), "no steady pitch", 150.0f, y, 170.0f, juce::Justification::centredRight, 19.0f);
            }
            y += 21.0f;
            g.setColour (textDim);
            put (g, laf.font (13.0f), keyLine (sec.analysis.key), 0.0f, y, 210.0f, juce::Justification::centredLeft, 16.0f);
            if (sec.analysis.tempo.valid)
                put (g, laf.font (12.0f), bpmText (s.typedBpm, i, sec.analysis.tempo), 214.0f, y, 106.0f, juce::Justification::centredRight, 16.0f);
            y += 16.0f;
            const auto runnerUp = runnerLine (sec.analysis.key);
            const bool drifts = t.valid && t.hasDrift;
            if (runnerUp.isNotEmpty() || drifts)
            {
                g.setColour (textFaint);
                put (g, laf.font (12.0f), runnerUp + (drifts ? (runnerUp.isNotEmpty() ? juce::String ("   ") : juce::String()) + "drifts " + signedText (t.driftCents) + " " + centSign : juce::String()), 0.0f, y, 320.0f,
                     juce::Justification::centredLeft, 14.0f);
                y += 14.0f;
            }
            y += 6.0f;
        }

        if (i < r.deltas.size())
        {
            const auto& d = r.deltas[i];
            g.setColour (hairline);
            g.drawLine (0.0f, y, 320.0f, y, 1.0f);
            y += 6.0f;
            const auto head = toJuce (centsible::sectionLabel (d.from)) + " " + arrowSign + " " + toJuce (centsible::sectionLabel (d.to));
            g.setColour (sand);
            put (g, laf.font (14.0f, true), head, 0.0f, y, 64.0f, juce::Justification::centredLeft, 17.0f);
            if (d.tuningKnown)
            {
                g.setColour (d.significant ? text : textDim);
                put (g, laf.font (14.0f, true), signedText (d.cents) + " " + plusMinus + " " + juce::String (std::max (0.1, d.ci), 1) + " " + centSign, 64.0f, y, 130.0f, juce::Justification::centredLeft, 17.0f);
                g.setColour (d.significant ? sand : textFaint);
                put (g, laf.font (12.0f), d.significant ? "significant" : "within the error", 194.0f, y, 126.0f, juce::Justification::centredRight, 17.0f);
            }
            else
            {
                g.setColour (textFaint);
                put (g, laf.font (13.0f), "detune unknown", 64.0f, y, 256.0f, juce::Justification::centredLeft, 17.0f);
            }
            y += 18.0f;
            juce::String second;
            if (d.keysKnown)
                second = d.sameKey ? "same key" : d.sameScale ? "same scale (relative key or mode)"
                                                              : "scale " + signedText (d.semitones, 0) + " semitones, " + signedText (d.totalCents) + " " + centSign + " in all";
            if (d.tempoKnown)
                second += (second.isEmpty() ? juce::String() : "  " + dotSign + "  ")
                          + (std::fabs (d.tempoCents) < 3.0 ? juce::String ("same tempo")
                                                            : "tempo " + timesSign + juce::String (d.tempoRatio, 3) + " (" + signedText (d.tempoCents, 0) + " " + centSign + " as a varispeed)");
            if (second.isNotEmpty())
                y += paintParagraph (g, second, y, laf.font (12.0f), textDim);
            const auto vari = varispeedText (s.typedBpm, i, d);
            if (vari.isNotEmpty()) y += paintParagraph (g, vari, y, laf.font (12.0f), sand);
            if (d.tuningKnown && d.significant && std::fabs (d.cents) >= 0.5)
                y += paintParagraph (g, "shift " + toJuce (centsible::sectionLabel (d.to)) + " by " + signedText (-d.cents) + " " + centSign + " to match " + toJuce (centsible::sectionLabel (d.from)),
                                     y, laf.font (12.0f), textDim);
            y += 8.0f;
        }
    }
    return y;
}

void BeatswitchPanel::paintCurve (juce::Graphics& g, const centsible::TempoAutomationResult& r, juce::Rectangle<float> box)
{
    // detune above (on a continuous scale), tempo below, one shared time axis; boundaries as ticks across both, short passages shaded
    const float legendH = 15.0f, gutter = 40.0f, gap = 5.0f, axisH = 13.0f;
    const float plotsH = box.getHeight() - legendH - axisH;
    const float hDetune = (plotsH - gap) * 0.58f, hTempo = plotsH - gap - hDetune;
    const juce::Rectangle<float> a (box.getX() + gutter, box.getY() + legendH, box.getWidth() - gutter, hDetune), b (a.getX(), a.getBottom() + gap, a.getWidth(), hTempo);
    const double t1 = std::max (1.0, r.seconds);
    auto xOf = [&] (double t) { return a.getX() + (float) (std::clamp (t / t1, 0.0, 1.0)) * a.getWidth(); };

    // the scales: the 2nd to 98th percentile of what was measured with some room, so that the odd stray window (the fade-out at the end of a track)
    // does not squash the rest; strays are clipped at the edge of their plot
    auto range = [] (std::vector<double> v, double& lo, double& hi)
    {
        if (v.empty()) return false;
        std::sort (v.begin(), v.end());
        const std::size_t trim = v.size() >= 30 ? (std::size_t) std::floor (0.02 * (double) v.size()) : 0;
        lo = v[trim];
        hi = v[v.size() - 1 - trim];
        const double pad = 0.08 * (hi - lo);
        lo -= pad;
        hi += pad;
        return true;
    };
    std::vector<double> detunes, tempi;
    for (const auto& c : r.curve)
    {
        if (c.detuneValid) detunes.push_back (c.unwrapped);
        if (c.tempoValid) tempi.push_back (c.bpm);
    }
    double dMin = -10.0, dMax = 10.0, bMin = 100.0, bMax = 120.0;
    range (detunes, dMin, dMax);
    range (tempi, bMin, bMax);
    if (dMax - dMin < 24.0) { const double mid = 0.5 * (dMax + dMin); dMin = mid - 12.0; dMax = mid + 12.0; }
    if (bMax - bMin < 4.0) { const double mid = 0.5 * (bMax + bMin); bMin = mid - 2.0; bMax = mid + 2.0; }
    auto yDetune = [&] (double v) { return a.getBottom() - 3.0f - (float) ((v - dMin) / (dMax - dMin)) * (a.getHeight() - 6.0f); };
    auto yTempo = [&] (double v) { return b.getBottom() - 3.0f - (float) ((v - bMin) / (bMax - bMin)) * (b.getHeight() - 6.0f); };

    g.setColour (plate);
    g.fillRoundedRectangle (a, 3.0f);
    g.fillRoundedRectangle (b, 3.0f);

    // short passages at another detune that then come back
    for (std::size_t i = 0; i < r.boundaries.size(); ++i)
        if (r.boundaries[i].passageStart)
            for (std::size_t j = i + 1; j < r.boundaries.size(); ++j)
                if (r.boundaries[j].passageEnd)
                {
                    g.setColour (sand.withAlpha (0.08f));
                    g.fillRect (xOf (r.boundaries[i].time), a.getY(), xOf (r.boundaries[j].time) - xOf (r.boundaries[i].time), b.getBottom() - a.getY());
                    break;
                }

    // a line at every semitone in the detune plot, and at 0 cents
    g.setColour (hairline);
    for (double v = std::ceil (dMin / 100.0) * 100.0; v <= dMax; v += 100.0)
        g.drawLine (a.getX(), yDetune (v), a.getRight(), yDetune (v), v == 0.0 ? 1.0f : 0.8f);

    auto trace = [&] (bool detune, juce::Colour colour, float thick)
    {
        juce::Path p;
        bool pen = false;
        double lastT = -1e9;
        for (const auto& c : r.curve)
        {
            if (! (detune ? c.detuneValid : c.tempoValid)) continue;
            const float x = xOf (c.time), y = detune ? yDetune (c.unwrapped) : yTempo (c.bpm);
            if (! pen || c.time - lastT > 3.5) p.startNewSubPath (x, y); else p.lineTo (x, y);
            pen = true;
            lastT = c.time;
        }
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion ((detune ? a : b).toNearestInt());
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    trace (true, sand, 1.5f);
    trace (false, textDim, 1.3f);

    // the boundaries
    for (const auto& bd : r.boundaries)
    {
        const bool passage = bd.passageStart || bd.passageEnd;
        const float x = xOf (bd.time);
        g.setColour (passage ? textFaint : sand.withAlpha (0.9f));
        g.drawLine (x, a.getY(), x, b.getBottom(), 1.0f);
        juce::Path tri;
        tri.addTriangle (x - 3.0f, b.getBottom() + 1.0f, x + 3.0f, b.getBottom() + 1.0f, x, b.getBottom() + 6.0f);
        g.fillPath (tri);
    }

    // the legend (in the colours of the lines), the scales in the left gutter, the time axis below
    const auto small = laf.font (11.0f);
    g.setColour (sand);
    put (g, small, "detune (" + centSign + ")", a.getX(), box.getY(), 90.0f, juce::Justification::centredLeft, legendH - 2.0f);
    g.setColour (textDim);
    put (g, small, "tempo (BPM)", a.getX() + 90.0f, box.getY(), 90.0f, juce::Justification::centredLeft, legendH - 2.0f);
    g.setColour (textFaint);
    const float gx = box.getX(), gw = gutter - 5.0f;
    put (g, small, signedText (dMax - 0.08 * (dMax - dMin), 0), gx, a.getY(), gw, juce::Justification::centredRight, 12.0f);
    put (g, small, signedText (dMin + 0.08 * (dMax - dMin), 0), gx, a.getBottom() - 12.0f, gw, juce::Justification::centredRight, 12.0f);
    put (g, small, juce::String (bMax - 0.08 * (bMax - bMin), 1), gx, b.getY(), gw, juce::Justification::centredRight, 12.0f);
    put (g, small, juce::String (bMin + 0.08 * (bMax - bMin), 1), gx, b.getBottom() - 12.0f, gw, juce::Justification::centredRight, 12.0f);
    put (g, small, "0:00", a.getX(), b.getBottom() + 4.0f, 60.0f, juce::Justification::centredLeft, 12.0f);
    put (g, small, clockText (r.seconds), a.getRight() - 60.0f, b.getBottom() + 4.0f, 60.0f, juce::Justification::centredRight, 12.0f);
}

float BeatswitchPanel::paintAutomation (juce::Graphics& g, const BeatswitchRunner::Snapshot& s, float y)
{
    const auto& r = s.automation;
    if (! r.valid)
    {
        g.setColour (textDim);
        const auto words = toJuce (r.summary);
        return y + paintParagraph (g, words.isNotEmpty() ? words : juce::String ("not enough pitched, rhythmic audio to follow the detune and the tempo"), y, laf.font (13.0f), textDim);
    }
    g.setColour (text);
    put (g, laf.font (17.0f, true), behaviourText (r.behaviour), 0.0f, y, 320.0f, juce::Justification::centredLeft, 21.0f);
    y += 23.0f;
    if (r.behaviour == centsible::TempoBehaviour::Varispeed)
    {
        g.setColour (sand);
        put (g, laf.font (14.0f, true), juce::String (r.bpmStart, 1) + " " + arrowSign + " " + juce::String (r.bpmEnd, 1) + " BPM  " + approxSign + "  " + signedText (r.centsStart, 0) + " " + arrowSign + " "
                                            + signedText (r.centsEnd, 0) + " " + centSign, 0.0f, y, 320.0f, juce::Justification::centredLeft, 18.0f);
        y += 19.0f;
        if (r.referenceBpm > 0.0)
        {
            g.setColour (textDim);
            juce::String also;
            for (const double b : r.zeroBpms)
                if (std::fabs (b - r.referenceBpm) > 0.05) also += (also.isEmpty() ? juce::String() : juce::String (", ")) + juce::String (b, 1);
            put (g, laf.font (13.0f), "0 " + centSign + " (exactly on A440) at " + juce::String (r.referenceBpm, 1) + " BPM", 0.0f, y, 320.0f, juce::Justification::centredLeft, 16.0f);
            y += 16.0f;
            if (also.isNotEmpty())
            {
                g.setColour (textFaint);
                put (g, laf.font (12.0f), "also at " + also + " BPM (a whole number of semitones away)", 0.0f, y, 320.0f, juce::Justification::centredLeft, 14.0f);
                y += 14.0f;
            }
        }
    }
    else if (r.medianBpm > 0.0)
    {
        // the tempo as a range only where it really moves; a steady track gets its one tempo (a stray window at a fade-out is not a tempo change)
        const bool steadyTempo = r.behaviour == centsible::TempoBehaviour::Constant || r.behaviour == centsible::TempoBehaviour::PitchMoves;
        juce::String words = steadyTempo || r.maxBpm - r.minBpm < 0.05 ? juce::String (r.medianBpm, 1) + " BPM"
                                                                       : juce::String (r.minBpm, 1) + dashSign + juce::String (r.maxBpm, 1) + " BPM";
        if (r.behaviour == centsible::TempoBehaviour::PitchMoves || r.behaviour == centsible::TempoBehaviour::TimeStretch || r.behaviour == centsible::TempoBehaviour::Mixed)
            words += "   detune " + signedText (r.centsStart, 0) + (std::fabs (r.centsEnd - r.centsStart) >= 1.0 ? " " + arrowSign + " " + signedText (r.centsEnd, 0) : juce::String()) + " " + centSign;
        g.setColour (textDim);
        put (g, laf.font (13.0f), words, 0.0f, y, 320.0f, juce::Justification::centredLeft, 16.0f);
        y += 17.0f;
    }

    // the first sentence of the summary says it in words
    {
        const auto& all = r.summary;
        const auto cut = all.find (". ");
        const auto first = toJuce (cut == std::string::npos ? all : all.substr (0, cut + 1));
        y += 2.0f;
        y += paintParagraph (g, first, y, laf.font (12.0f), textDim) + 6.0f;
    }

    paintCurve (g, r, juce::Rectangle<float> (0.0f, y, 320.0f, 140.0f));
    y += 150.0f;

    // the regions, with what starts each one
    g.setColour (textFaint);
    put (g, laf.font (12.0f), r.regions.size() > 1 ? "regions" : "one region, no switch found", 0.0f, y, 260.0f, juce::Justification::centredLeft, 14.0f);
    y += 18.0f;
    int n = 0;
    for (const auto& reg : r.regions)
    {
        ++n;
        const auto bars = s.bars (reg.start);
        g.setColour (sand);
        put (g, laf.font (14.0f, true), juce::String (n), 0.0f, y, 16.0f, juce::Justification::centredLeft, 17.0f);
        g.setColour (textDim);
        put (g, laf.font (13.0f), clockText (reg.start, true) + " " + dashSign + " " + clockText (reg.end, true) + (bars.isNotEmpty() ? "   " + bars : juce::String()), 16.0f, y, 170.0f,
             juce::Justification::centredLeft, 17.0f);
        if (! reg.ok)
        {
            g.setColour (textFaint);
            put (g, laf.font (13.0f), "too little pitched audio", 150.0f, y, 170.0f, juce::Justification::centredRight, 17.0f);
            y += 21.0f;
        }
        else
        {
            g.setColour (text);
            put (g, laf.font (14.0f, true), reg.pitchMoves ? signedText (reg.centsStart, 0) + " " + arrowSign + " " + signedText (reg.centsEnd, 0) + " " + centSign
                                                           : signedText (reg.cents) + " " + plusMinus + " " + juce::String (std::max (0.1, reg.ci), 1) + " " + centSign,
                 150.0f, y, 170.0f, juce::Justification::centredRight, 17.0f);
            y += 18.0f;
            juce::String second;
            // a range only where the tempo really moves (the classifier knows the jitter of the tempo track); flagged time-stretched when the pitch does not follow it
            const bool moves = reg.tempoMoves && (reg.behaviour == centsible::TempoBehaviour::Varispeed || reg.behaviour == centsible::TempoBehaviour::TimeStretch
                                                  || reg.behaviour == centsible::TempoBehaviour::Mixed);
            if (reg.bpm > 0.0)
                second = (moves ? juce::String (reg.bpmMin, 1) + dashSign + juce::String (reg.bpmMax, 1) : juce::String (reg.bpm, 1)) + " BPM";
            if (moves) second += ", " + behaviourText (reg.behaviour);
            if (reg.key.valid) second += (second.isEmpty() ? juce::String() : "   ") + keyLine (reg.key);
            g.setColour (textFaint);
            if (second.isNotEmpty())
            {
                put (g, laf.font (12.0f), second, 16.0f, y, 304.0f, juce::Justification::centredLeft, 14.0f);
                y += 14.0f;
            }
        }
        // what starts this region (the boundary that was found there)
        for (const auto& bd : r.boundaries)
            if (n > 1 && std::fabs (bd.time - reg.start) < 0.02 && ! bd.reason.empty())
            {
                y += paintParagraph (g, toJuce (bd.reason), y, laf.font (12.0f), textFaint, 304.0f, 16.0f);
                break;
            }
        y += 6.0f;
    }
    return y;
}
