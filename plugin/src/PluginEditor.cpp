#include "PluginEditor.h"

namespace
{
    using namespace Palette;
    const juce::Rectangle<float> plateRect { 60.0f, 50.0f, 320.0f, 320.0f };
    constexpr int modeN = 3, modeM = 5;      // plate mode numbers
    constexpr int numGrains = 2600;

    const juce::String minus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    // One decimal: "+12.0", "−23.4" (true minus sign), "0.0". Never "-0.0".
    juce::String signedText (float v, bool asciiMinus = false)
    {
        const int tenths = (int) std::lround ((double) v * 10.0);
        if (tenths == 0) return "0.0";
        return (tenths < 0 ? (asciiMinus ? juce::String ("-") : minus) : juce::String ("+")) + juce::String (std::abs ((double) tenths) / 10.0, 1);
    }

    // Whole cents (the rounded headline): "+10", "-20" with a true minus sign, "0". Never "-0".
    juce::String signedWhole (float v)
    {
        const int r = (int) std::lround (v);
        if (r == 0) return "0";
        return (r < 0 ? minus : juce::String ("+")) + juce::String (std::abs (r));
    }

    // The 95 % half-width: "0.2" below 10 cents, whole cents above; "?" while there is not enough audio for an interval.
    juce::String uncertaintyText (float ci)
    {
        if (ci >= 25.0f) return "?";
        return ci < 10.0f ? juce::String (std::max (0.1f, ci), 1) : juce::String ((int) std::lround (ci));
    }

    juce::String shortName (const juce::String& n, int maxChars)
    {
        const auto stem = juce::File (n).getFileNameWithoutExtension();
        return stem.length() <= maxChars ? stem : stem.substring (0, maxChars - 1) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
    }

    juce::String trackLine (const juce::String& tag, const centsible::TrackAnalysis& a)
    {
        const auto& t = a.tuning;
        juce::String l = tag + "  " + signedText (t.cents) + " " + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2 \xc2\xb1 ")) + uncertaintyText (t.ciCents);
        if (a.key.valid)   l += "   " + juce::String (centsible::keyName (a.key.root, a.key.minor)) + " (" + juce::String (centsible::camelot (a.key.root, a.key.minor)) + ")";        // "D major (10B)": the Camelot code next to the name (compact: the mode is in the readout)
        if (a.tempo.valid) l += "   " + juce::String (juce::roundToInt (a.tempo.bpm)) + " BPM";
        return l;
    }

    float textWidth (const juce::Font& f, const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }

    // Draws `s` with its baseline at y, left edge at x.
    void drawAt (juce::Graphics& g, const juce::Font& f, const juce::String& s, float x, float baseline)
    {
        g.setFont (f);
        g.drawText (s, juce::Rectangle<float> (x, baseline - f.getAscent(), textWidth (f, s) + 8.0f, f.getHeight()),
                    juce::Justification::topLeft, false);
    }

    void drawRight (juce::Graphics& g, const juce::Font& f, const juce::String& s, float right, float baseline)
    {
        drawAt (g, f, s, right - textWidth (f, s), baseline);
    }
}

TuningMatchEditor::TuningMatchEditor (TuningMatchProcessor& p)
    : AudioProcessorEditor (&p), proc (p), holdAtt (p.apvts, "hold", holdButton), reactAtt (p.apvts, "react", reactButton),
      vblank (this, [this] { tick(); })
{
    setLookAndFeel (&laf);
    holdButton.setClickingTogglesState (true);
    reactButton.setClickingTogglesState (true);
    addAndMakeVisible (reactButton);
    addAndMakeVisible (holdButton);
    addAndMakeVisible (resetButton);
    resetButton.onClick = [this] { proc.clearFiles(); proc.resetAnalysis(); panel->clearResults(); };

    panel = std::make_unique<BeatswitchPanel> (proc, laf);
    addChildComponent (*panel);
    addAndMakeVisible (beatswitchButton);
    beatswitchButton.setClickingTogglesState (true);
    beatswitchButton.onClick = [this] { showBeatswitch (beatswitchButton.getToggleState()); };

    grains.resize (numGrains);
    for (auto& gr : grains)
    {
        gr.u = random.nextFloat();
        gr.v = random.nextFloat();
        gr.size = 0.9f + 0.9f * random.nextFloat();
        gr.alpha = 0.30f + 0.60f * random.nextFloat();
    }

    constrainer.setFixedAspectRatio ((double) designW / designH);
    constrainer.setSizeLimits (330, 480, 880, 1280);
    setConstrainer (&constrainer);
    setResizable (true, true);
    setSize ((int) designW, (int) designH);
    snapAnimationsToTarget();
}

TuningMatchEditor::~TuningMatchEditor() { setLookAndFeel (nullptr); }

void TuningMatchEditor::resized()
{
    scale = (float) getWidth() / designW;
    auto place = [this] (juce::Component& c, float x, float y, float w, float h)
    {
        c.setBounds (juce::Rectangle<float> (x * scale, y * scale, w * scale, h * scale).toNearestInt());
    };
    place (holdButton, 60, 654, 62, 30);
    place (reactButton, 130, 654, 62, 30);
    place (resetButton, 200, 654, 62, 30);
    place (beatswitchButton, 270, 654, 110, 30);
    panel->setScale (scale);
    place (*panel, panelX, panelY, BeatswitchPanel::designW, BeatswitchPanel::designH);
}

void TuningMatchEditor::showBeatswitch (bool show)
{
    beatswitchButton.setToggleState (show, juce::dontSendNotification);
    panel->setVisible (show);
    repaint();
}

// ---------------------------------------------------------------------------------------------
// animation

void TuningMatchEditor::step (Spring& s, float dt, float omega, bool wrap50)
{
    if (wrap50 && std::abs (s.target - s.pos) > 50.0f) { s.pos = s.target; s.vel = 0.0f; return; }
    const float a = omega * omega * (s.target - s.pos) - 2.0f * omega * s.vel;
    s.vel += a * dt;
    s.pos += s.vel * dt;
    if (std::abs (s.target - s.pos) < 1.0e-4f && std::abs (s.vel) < 1.0e-3f) { s.pos = s.target; s.vel = 0.0f; }
}

void TuningMatchEditor::updateTargets()
{
    reading = proc.getReading();
    files = proc.fileSnapshot();
    fileMode = files.count > 0;
    matchMode = false;
    if (fileMode)
    {
        reading = centsible::Reading {};   // the live signal is not what is shown while a dropped file is
        const bool allDone = files.count == 1 ? files.slots[0].state == FileAnalyzer::State::done
                                              : files.slots[0].state == FileAnalyzer::State::done && files.slots[1].state == FileAnalyzer::State::done;
        if (allDone && files.count == 1)
            reading = files.slots[0].result.tuning;
        else if (allDone)
        {
            matchMode = true;
            match = centsible::matchAdvice (files.slots[0].result, files.slots[1].result);
            const auto& a = files.slots[0].result.tuning;
            const auto& b = files.slots[1].result.tuning;
            reading.valid = true;
            reading.cents = (float) match.fineCents;
            reading.ciCents = std::hypot (a.ciCents, b.ciCents);
            reading.refHz = 440.0f * std::pow (2.0f, reading.cents / 1200.0f);
            reading.locked = a.locked && b.locked;
            reading.settled = reading.locked;
            reading.confidence = std::min (a.confidence, b.confidence);
            reading.seconds = (float) centsible::kSettleSeconds;
        }
    }
    held = proc.apvts.getRawParameterValue ("hold")->load() >= 0.5f;
    react = proc.apvts.getRawParameterValue ("react")->load() >= 0.5f;
    levelTarget = react ? std::clamp (reading.rms * 5.0f, 0.0f, 1.0f) : 0.0f;
    if (react && reading.onsetCount != lastOnset) { pulse = 1.0f; }
    lastOnset = reading.onsetCount;
    if (react && reading.frameId != lastFrameId)
    {
        lastFrameId = reading.frameId;
        for (int i = 0; i < reading.vizCount; ++i) dots.push_back ({ reading.viz[(size_t) i].cents, reading.viz[(size_t) i].weight, 0.0f });
        if (dots.size() > 400) dots.erase (dots.begin(), dots.begin() + (long) (dots.size() - 400));
    }
    if (! react) dots.clear();

    const bool valid = reading.valid;
    // While locked the number only moves when the reading has clearly moved, so the last digit does not flicker.
    if (valid)
    {
        const bool keep = shownValid && reading.locked
                          && std::fabs ((float) centsible::wrapCents (reading.cents - shownCents)) < std::max (0.15f, 0.5f * reading.ciCents);
        if (! keep) shownCents = reading.cents;
        shownValid = true;
    }
    else
        shownValid = false;
    number.target = valid ? shownCents : 0.0f;
    warp.target = valid ? std::clamp (reading.cents / 50.0f, -1.0f, 1.0f) : 0.0f;

    // how "still" the sand is: restless while uncertain, nearly at rest once settled (or held)
    float c = 0.0f;
    if (valid)
    {
        const float progress = std::clamp (reading.seconds / (float) centsible::kSettleSeconds, 0.0f, 1.0f);
        c = 0.15f + 0.55f * progress * std::clamp (reading.confidence / 0.5f, 0.0f, 1.0f);
        if (reading.contested) c = std::min (c, 0.25f);
        if (reading.locked || held) c = 1.0f;
    }
    calm.target = c;
}

// One animation step of the sand: each grain slides down the slope of the plate's displacement towards a
// nodal line (where the plate does not move), with random shaking that fades as the reading settles.
void TuningMatchEditor::stepSand()
{
    constexpr float pi = juce::MathConstants<float>::pi;
    const float t = warp.pos;
    const float jitter = 0.0006f + (1.0f - calm.pos) * 0.0090f + pulse * 0.0045f;

    for (auto& gr : grains)
    {
        const float a = modeN * pi * gr.u, b = modeM * pi * gr.v, c = modeM * pi * gr.u, d = modeN * pi * gr.v;
        const float f  = std::cos (a) * std::cos (b) - t * std::cos (c) * std::cos (d);
        const float fu = -modeN * pi * std::sin (a) * std::cos (b) + t * modeM * pi * std::sin (c) * std::cos (d);
        const float fv = -modeM * pi * std::cos (a) * std::sin (b) + t * modeN * pi * std::cos (c) * std::sin (d);
        const float g2 = fu * fu + fv * fv + 0.8f;

        gr.u -= 0.5f * f * fu / g2;
        gr.v -= 0.5f * f * fv / g2;
        gr.u += (random.nextFloat() - 0.5f) * 2.0f * jitter;
        gr.v += (random.nextFloat() - 0.5f) * 2.0f * jitter;

        if (gr.u < 0.0f) gr.u = -gr.u;
        if (gr.u > 1.0f) gr.u = 2.0f - gr.u;
        if (gr.v < 0.0f) gr.v = -gr.v;
        if (gr.v > 1.0f) gr.v = 2.0f - gr.v;
        gr.u = std::clamp (gr.u, 0.0f, 1.0f);
        gr.v = std::clamp (gr.v, 0.0f, 1.0f);
    }
}

void TuningMatchEditor::snapAnimationsToTarget()
{
    updateTargets();
    for (auto* s : { &number, &warp, &calm }) { s->pos = s->target; s->vel = 0.0f; }
    copiedFade = 0.0f;
    level = levelTarget;
    for (int i = 0; i < 500; ++i) stepSand();
}

void TuningMatchEditor::tick()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const float dt = lastTick == 0.0 ? 0.0f : (float) std::clamp (now - lastTick, 0.0, 0.05);
    lastTick = now;

    updateTargets();
    level += (levelTarget - level) * (1.0f - std::exp (-dt / (levelTarget > level ? 0.020f : 0.300f)));
    pulse *= std::exp (-dt / 0.18f);
    for (auto& d : dots) d.age += dt;
    dots.erase (std::remove_if (dots.begin(), dots.end(), [] (const Dot& d) { return d.age > 2.5f; }), dots.end());
    step (number, dt, 9.0f, true);
    step (warp, dt, 2.2f);
    step (calm, dt, 1.6f);

    const float sinceCopy = (float) (now - copiedAt);
    copiedFade = sinceCopy < 1.0f ? 1.0f : std::clamp (1.0f - (sinceCopy - 1.0f) / 0.35f, 0.0f, 1.0f);
    if (panel != nullptr && panel->isVisible())
    {
        // the panel covers the plate and everything under it and repaints itself: only the header (the title and the status) is redrawn here
        repaint (0, 0, getWidth(), (int) std::ceil (panelY * scale));
        return;
    }
    stepSand();
    repaint();
}

// ---------------------------------------------------------------------------------------------
// input

void TuningMatchEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool hover = copyRect.contains (toDesign (e.position)) && reading.valid;
    if (hover != copyHover)
    {
        copyHover = hover;
        setMouseCursor (hover ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
}

bool TuningMatchEditor::isInterestedInFileDrag (const juce::StringArray& paths)
{
    for (const auto& p : paths)
        if (FileAnalyzer::isAudioFile (juce::File (p))) return true;
    return false;
}

void TuningMatchEditor::fileDragEnter (const juce::StringArray&, int, int) { dragHover = true; }
void TuningMatchEditor::fileDragExit (const juce::StringArray&) { dragHover = false; }

void TuningMatchEditor::filesDropped (const juce::StringArray& paths, int, int)
{
    dragHover = false;
    proc.analyzeFiles (paths);
    if (panel->isVisible()) panel->setUseLive (false);       // a file dropped while Beatswitch is open is what it works on
}

void TuningMatchEditor::mouseExit (const juce::MouseEvent&)
{
    if (copyHover) { copyHover = false; setMouseCursor (juce::MouseCursor::NormalCursor); }
}

void TuningMatchEditor::mouseUp (const juce::MouseEvent& e)
{
    if (reading.valid && copyRect.contains (toDesign (e.position)) && ! e.mouseWasDraggedSinceMouseDown())
    {
        juce::SystemClipboard::copyTextToClipboard (signedText (matchMode ? shownCents : -shownCents, true)); // plain ASCII "-" for pasting
        copiedAt = juce::Time::getMillisecondCounterHiRes() * 0.001;
        copiedFade = 1.0f;
    }
}

// ---------------------------------------------------------------------------------------------
// painting

void TuningMatchEditor::paint (juce::Graphics& g)
{
    scale = (float) getWidth() / designW;
    g.fillAll (bg);
    g.addTransform (juce::AffineTransform::scale (scale));

    // header
    g.setColour (textDim);
    drawAt (g, laf.font (16.0f, true), "centsible", 60.0f, 34.0f);
    g.setColour (textFaint);
    drawAt (g, laf.font (12.0f), "v1.3", 60.0f + textWidth (laf.font (16.0f, true), "centsible") + 8.0f, 34.0f);

    juce::String status;
    juce::Colour statusColour = textDim;
    if (fileMode && files.busy())                    { status = "analysing " + juce::String (files.count == 2 ? "files" : shortName (files.slots[0].name, 18)) + "..."; }
    else if (fileMode && ! reading.valid)            { status = "no steady pitch found in the file"; statusColour = textFaint; }
    else if (matchMode)                              { status = "match: B onto A"; statusColour = reading.locked ? sand : textDim; }
    else if (fileMode)                               { status = shortName (files.slots[0].name, 20) + "  " + juce::String (files.slots[0].analysisSeconds, 1) + " s"; statusColour = reading.locked ? sand : textDim; }
    else if (held)              status = "held";
    else if (! reading.valid)   { status = "waiting for audio"; statusColour = textFaint; }
    else if (reading.locked)    { status = "LOCKED"; statusColour = sand; }
    else if (reading.contested) status = "something changed, re-listening";
    else if (reading.seconds >= 20.0f && reading.confidence < 0.4f) status = "weak signal";
    else if (reading.seconds >= (float) centsible::kSettleSeconds) status = "refining";
    else
        status = "listening, " + juce::String ((int) reading.seconds) + " s";
    g.setColour (statusColour);
    drawRight (g, laf.font (14.0f, reading.locked), status, 380.0f, 33.0f);

    // confidence ring: fills with the confidence, closes (and glows) when locked
    {
        const float rx = 380.0f - textWidth (laf.font (14.0f, reading.locked), status) - 20.0f, ry = 28.0f, rr = 6.0f;
        juce::Path track, arc;
        track.addEllipse (rx - rr, ry - rr, 2.0f * rr, 2.0f * rr);
        g.setColour (hairline.brighter (0.15f));
        g.strokePath (track, juce::PathStrokeType (1.6f));
        const float frac = reading.valid ? (reading.locked || held ? 1.0f : std::clamp (reading.confidence, 0.04f, 1.0f)) : 0.0f;
        if (frac > 0.0f)
        {
            arc.addCentredArc (rx, ry, rr, rr, 0.0f, 0.0f, frac * juce::MathConstants<float>::twoPi, true);
            g.setColour (reading.locked ? sand : textDim);
            g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    if (panel != nullptr && panel->isVisible())
    {
        if (dragHover)
        {
            g.setColour (sand);
            drawRight (g, laf.font (14.0f), "drop to use in Beatswitch", 380.0f, 33.0f);
        }
        return;       // the Beatswitch panel covers everything below the header
    }
    paintPlate (g);
    if (dragHover)
    {
        g.setColour (sand.withAlpha (0.9f));
        g.drawRoundedRectangle (plateRect.expanded (5.0f), 9.0f, 2.0f);
        g.setColour (sand);
        drawRight (g, laf.font (14.0f), "drop to analyse (two files: match)", 380.0f, 33.0f + 0.0f);
    }
    paintStrip (g);
    paintReadout (g);
    paintCorrection (g);
    if (! matchMode) paintHistory (g);
}

void TuningMatchEditor::paintPlate (juce::Graphics& g)
{
    g.setColour (plate);
    g.fillRoundedRectangle (plateRect, 6.0f);
    g.setColour (sand.withAlpha (0.10f * level + 0.18f * pulse));
    g.drawRoundedRectangle (plateRect.expanded (2.0f + 3.0f * level), 8.0f, 1.2f);
    g.setColour (plateEdge);
    g.drawRoundedRectangle (plateRect.reduced (0.5f), 6.0f, 1.0f);

    // the sand, in three brightness tiers so we only change colour three times
    const auto inner = plateRect.reduced (12.0f);
    for (int tier = 0; tier < 3; ++tier)
    {
        g.setColour (sand.withAlpha (std::min (1.0f, (0.28f + 0.30f * (float) tier) * (0.8f + 0.5f * level + 0.2f * pulse))));
        for (const auto& gr : grains)
        {
            const int gt = gr.alpha < 0.55f ? 0 : gr.alpha < 0.75f ? 1 : 2;
            if (gt != tier) continue;
            g.fillRect (inner.getX() + gr.u * inner.getWidth(), inner.getY() + gr.v * inner.getHeight(), gr.size, gr.size);
        }
    }

    // the bolt the plate is mounted on
    const auto c = plateRect.getCentre();
    g.setColour (plate);
    g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
    g.setColour (textFaint);
    g.drawEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f, 1.0f);
}

void TuningMatchEditor::paintReadout (juce::Graphics& g)
{
    const bool valid = reading.valid;
    const bool firm = reading.locked || held;
    // The big number is rounded to the nearest 5 cents (and says so): a coarse value is right far more often than a decimal one; the exact
    // reading with its interval stays visible next to it, and "shift ... to fix" below is exact. (Match mode shows the exact shift.)
    const bool rounded = ! matchMode;
    const float bigValue = rounded ? std::round (number.pos / 5.0f) * 5.0f : number.pos;
    const juce::String num = valid ? (rounded ? signedWhole (bigValue) : signedText (bigValue)) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
    const auto numFont = laf.font (80.0f);
    const float baseline = 450.0f;

    g.setColour (! valid ? textFaint : firm ? text : textDim);
    drawAt (g, numFont, num, 60.0f, baseline);

    // beside the number: the uncertainty, the unit and the equivalent concert pitch
    const float x = 60.0f + textWidth (numFont, num) + 14.0f;
    if (valid)
    {
        const auto cent = juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));
        if (rounded)
        {
            g.setColour (textDim);
            drawAt (g, laf.font (14.0f), cent + " (cents), rounded to the nearest 5", x, baseline - 52.0f);
            g.setColour (firm ? textDim : textFaint);
            drawAt (g, laf.font (16.0f), "exact " + signedText (number.pos) + " " + cent + juce::String (juce::CharPointer_UTF8 (" \xc2\xb1 ")) + uncertaintyText (reading.ciCents), x, baseline - 31.0f);
        }
        else
        {
            g.setColour (firm ? textDim : textFaint);
            drawAt (g, laf.font (19.0f), juce::String (juce::CharPointer_UTF8 ("\xc2\xb1 ")) + uncertaintyText (reading.ciCents), x, baseline - 52.0f);
            g.setColour (textDim);
            drawAt (g, laf.font (16.0f), cent + " (cents)", x, baseline - 31.0f);
        }
        const float hz = 440.0f * std::pow (2.0f, number.pos / 1200.0f);
        drawAt (g, laf.font (15.0f), matchMode ? juce::String ("shift B") : "A4 = " + juce::String (hz, 1) + " Hz", x, baseline - 10.0f);
    }
    else
    {
        g.setColour (textFaint);
        drawAt (g, laf.font (16.0f), juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")) + " (cents)", x, baseline - 31.0f);
        drawAt (g, laf.font (15.0f), fileMode ? juce::String (files.busy() ? "one moment" : "no pitch found") : juce::String ("play the track"), x, baseline - 10.0f);
        if (! fileMode)
        {
            g.setColour (textFaint.withAlpha (0.8f));
            drawAt (g, laf.font (13.0f), "or drop an audio file here", 60.0f, 471.0f);
        }
    }

    // The same offset can be read two ways (-12.0 from the nearest semitone is +88.0 from the one below), and the plugin cannot know which
    // one the artist used: say both, and what to try if the first does not match.
    float y = 471.0f;
    if (valid && matchMode)
    {
        g.setColour (textDim);
        drawAt (g, laf.font (13.0f), trackLine ("A", files.slots[0].result), 60.0f, y); y += 15.0f;
        drawAt (g, laf.font (13.0f), trackLine ("B", files.slots[1].result), 60.0f, y); y += 15.0f;
        g.setColour (textFaint);
        if (match.keysKnown)
            drawAt (g, laf.font (13.0f), match.semitones == 0 ? juce::String ("same key family: only the fine tune is needed")
                                                              : "for the same key as well: " + (match.semitones > 0 ? juce::String ("+") : minus) + juce::String (std::abs (match.semitones)) + " semitones, "
                                                                    + signedText ((float) match.totalCents) + " " + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")) + " in all", 60.0f, y);
        y += 15.0f;
        drawAt (g, laf.font (13.0f), "A " + shortName (files.slots[0].name, 14) + "   B " + shortName (files.slots[1].name, 14), 60.0f, y);
        return;
    }
    if (valid && std::fabs (reading.cents) >= 0.5f)
    {
        const float c = reading.cents, other = c < 0.0f ? c + 100.0f : c - 100.0f;
        g.setColour (textFaint);
        drawAt (g, laf.font (13.0f), "(= " + signedText (other) + " " + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")) + " from one semitone "
                                         + (c < 0.0f ? "down)" : "up)"), 60.0f, y);
        y += 15.0f;
        drawAt (g, laf.font (13.0f), "if " + signedText (c) + " doesn't work, try " + signedText (other) + " instead", 60.0f, y);
        y += 15.0f;
    }

    // a second tuning, only when it is statistically solid
    if (valid && reading.hasSecondary)
    {
        g.setColour (textDim);
        drawAt (g, laf.font (14.0f), "also heard " + signedText (reading.secondaryCents) + " "
                    + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")) + " (" + juce::String ((int) std::lround (reading.secondaryShare * 100.0f)) + "%)", 60.0f, y);
        y += 15.0f;
    }
    // a tuning that moves over the song: the number is the average, say so
    if (valid && reading.hasDrift)
    {
        g.setColour (textDim);
        drawAt (g, laf.font (14.0f), "drifts " + signedText (reading.driftCents) + " " + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")) + " over the song (reading = average)", 60.0f, y);
        y += 15.0f;
    }
    if (valid && fileMode && files.count == 1)
    {
        const auto& a = files.slots[0].result;
        const auto& k = a.key;
        const auto font = laf.font (14.0f), small = laf.font (13.0f);
        const juce::String tempo = a.tempo.valid ? juce::String (a.tempo.bpm, 1) + " BPM" : juce::String();
        // The key with its Camelot code and how sure the detector is. A relative major / minor pair it cannot tell apart is shown as the pair with the
        // split ("A minor / C major, 60 / 40"); a runner-up that matters goes on a second line when the panel has room for one.
        juce::String keyText, runner;
        if (k.valid)
        {
            if (k.relativeAmbiguous)
                keyText = "key " + juce::String (centsible::keyAmbiguityText (k)) + "  (" + juce::String (centsible::camelot (k.root, k.minor)) + " / "
                          + juce::String (centsible::camelot (centsible::relativeRoot (k), centsible::relativeIsMinor (k))) + ")";
            else
                keyText = "key " + juce::String (centsible::keyLabel (k)) + ", " + juce::String ((int) std::lround (100.0f * k.probability)) + " %";
            const bool runnerIsRelative = k.runnerUpRoot == centsible::relativeRoot (k) && k.runnerUpMinor == centsible::relativeIsMinor (k);
            if (k.runnerUpProbability >= 0.15f && ! runnerIsRelative) runner = juce::String (centsible::runnerUpText (k));
            if (k.betweenKeys && runner.isNotEmpty() && textWidth (small, "between two keys: " + runner) <= 322.0f) runner = "between two keys: " + runner;
        }
        const bool room = y + 15.0f <= 531.0f;
        const juce::String tempoSep = tempo.isEmpty() ? juce::String() : "   " + tempo;
        const bool tempoFits = textWidth (font, keyText + tempoSep) <= 322.0f;
        juce::String line1 = keyText.isEmpty() ? tempo : (tempoFits ? keyText + tempoSep : keyText);
        juce::String line2 = (! tempoFits && keyText.isNotEmpty() ? tempo : juce::String());
        if (runner.isNotEmpty()) line2 += (line2.isEmpty() ? "" : "   ") + runner;
        if (line2.isNotEmpty() && ! room)
        {
            // no room for a second line: the short form of the key and the tempo
            line1 = k.valid ? "key " + juce::String (centsible::keyLabel (k)) + (tempo.isEmpty() ? juce::String() : "   " + tempo) : tempo;
            line2 = juce::String();
        }
        g.setColour (textDim);
        drawAt (g, font, line1, 60.0f, y);
        if (line2.isNotEmpty())
        {
            y += 15.0f;
            g.setColour (textFaint);
            drawAt (g, small, line2, 60.0f, y);
        }
    }
}

void TuningMatchEditor::paintCorrection (juce::Graphics& g)
{
    g.setColour (hairline);
    g.drawLine (60.0f, 540.0f, 380.0f, 540.0f, 1.0f);

    const bool valid = reading.valid;
    const float base = 570.0f;
    const auto labelFont = laf.font (17.0f), valueFont = laf.font (21.0f, true);
    const auto cents = juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));
    float x = 60.0f;
    g.setColour (textDim);
    const juce::String shiftLabel = matchMode ? "shift B by" : "shift";
    drawAt (g, labelFont, shiftLabel, x, base);
    x += textWidth (labelFont, shiftLabel) + 8.0f;

    g.setColour (! valid ? textFaint : (reading.locked || held) ? text : textDim);
    const juce::String value = valid ? signedText (matchMode ? number.pos : -number.pos) + " " + cents : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
    drawAt (g, valueFont, value, x, base);
    x += textWidth (valueFont, value) + 8.0f;
    g.setColour (textDim);
    drawAt (g, labelFont, matchMode ? "to match A" : "to fix", x, base);

    // "copy": plain underlined text, like a link
    copyRect = { 306.0f, 546.0f, 74.0f, 32.0f };
    const auto copyFont = laf.font (17.0f);
    const float w = textWidth (copyFont, "copy");
    g.setColour ((valid ? (copyHover ? text : textDim) : textFaint).withAlpha (1.0f - copiedFade));
    drawRight (g, copyFont, "copy", 380.0f, base);
    g.drawLine (380.0f - w, base + 3.0f, 380.0f, base + 3.0f, 1.0f);
    if (copiedFade > 0.0f)
    {
        g.setColour (sand.withAlpha (copiedFade));
        drawRight (g, copyFont, "copied", 380.0f, base);
    }
}

void TuningMatchEditor::paintHistory (juce::Graphics& g)
{
    g.setColour (textFaint);
    drawAt (g, laf.font (13.0f), "history", 60.0f, 607.0f);
    if (reading.valid)
        drawRight (g, laf.font (13.0f), "now " + signedText (reading.liveCents), 380.0f, 607.0f);

    const juce::Rectangle<float> area (60.0f, 613.0f, 320.0f, 26.0f);
    const int count = reading.historyCount;
    if (! reading.valid || count < 2)
    {
        g.setColour (hairline);
        g.drawLine (area.getX(), area.getCentreY(), area.getRight(), area.getCentreY(), 1.0f);
        return;
    }

    const float last = reading.history[(size_t) count - 1];
    float maxDev = 0.0f;
    for (int i = 0; i < count; ++i)
        maxDev = std::max (maxDev, std::abs ((float) centsible::wrapCents (reading.history[(size_t) i] - last)));
    const float range = std::max (1.0f, maxDev * 1.15f);
    const float midY = area.getCentreY();
    const int span = std::max (count, 24);
    auto pointAt = [&] (int i)
    {
        const float dv = (float) centsible::wrapCents (reading.history[(size_t) i] - last);
        return juce::Point<float> (area.getX() + (float) i / (float) (span - 1) * area.getWidth(), midY - dv / range * area.getHeight() * 0.5f);
    };

    g.setColour (hairline);
    g.drawLine (area.getX(), midY, area.getRight(), midY, 1.0f);

    juce::Path path;
    path.startNewSubPath (pointAt (0));
    for (int i = 1; i < count; ++i) path.lineTo (pointAt (i));
    g.setColour (sand.withAlpha (0.85f));
    g.strokePath (path, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const auto end = pointAt (count - 1);
    g.fillEllipse (end.x - 2.4f, end.y - 2.4f, 4.8f, 4.8f);
}

// A thin strip under the plate: every note vote of the last couple of seconds, placed by its offset from the 12-TET grid
// (-50 .. +50 cents). Votes pile up where the tuning is, so the cluster is literally visible.
void TuningMatchEditor::paintStrip (juce::Graphics& g)
{
    const juce::Rectangle<float> strip (60.0f, 377.0f, 320.0f, 12.0f);
    g.setColour (hairline);
    g.drawLine (strip.getX(), strip.getCentreY(), strip.getRight(), strip.getCentreY(), 1.0f);
    g.drawLine (strip.getCentreX(), strip.getY() + 2.0f, strip.getCentreX(), strip.getBottom() - 2.0f, 1.0f);
    for (const auto& d : dots)
    {
        const float x = strip.getX() + (d.cents + 50.0f) / 100.0f * strip.getWidth();
        const float a = (1.0f - d.age / 2.5f) * (0.15f + 0.6f * d.weight);
        g.setColour (sand.withAlpha (std::clamp (a, 0.0f, 1.0f)));
        g.fillEllipse (x - 1.6f, strip.getCentreY() - 1.6f, 3.2f, 3.2f);
    }
    if (reading.valid)
    {
        // eased marker (the same spring as the number) with the 95 % interval as a soft band around it
        const float x = strip.getX() + (number.pos + 50.0f) / 100.0f * strip.getWidth();
        const float half = std::clamp (reading.ciCents, 0.0f, 25.0f) / 100.0f * strip.getWidth();
        g.setColour (sand.withAlpha (reading.locked || held ? 0.20f : 0.12f));
        g.fillRoundedRectangle (x - half, strip.getY() + 1.0f, std::max (2.0f * half, 1.5f), strip.getHeight() - 2.0f, 2.0f);
        g.setColour (sand);
        g.fillRect (x - 0.75f, strip.getY(), 1.5f, strip.getHeight());
    }
}
