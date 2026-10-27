#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <TMAssets.h>

// Flat and quiet: deep violet-black, soft lavender-white type, one lavender accent.
namespace Palette
{
    inline const juce::Colour bg        { 0xff110e18 };
    inline const juce::Colour plate     { 0xff171322 };
    inline const juce::Colour plateEdge { 0xff2f2745 };
    inline const juce::Colour hairline  { 0xff261f38 };
    inline const juce::Colour text      { 0xffece6f5 };
    inline const juce::Colour textDim   { 0xffa89fbd };
    inline const juce::Colour textFaint { 0xff6a6180 };
    inline const juce::Colour sand      { 0xffb99cff };
}

class TuningLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TuningLookAndFeel()
    {
        regular = juce::Typeface::createSystemTypefaceFor (TMAssets::SourceSansProRegular_otf, (size_t) TMAssets::SourceSansProRegular_otfSize);
        bold    = juce::Typeface::createSystemTypefaceFor (TMAssets::SourceSansProBold_otf, (size_t) TMAssets::SourceSansProBold_otfSize);
        setDefaultSansSerifTypeface (regular);
        setColour (juce::ResizableWindow::backgroundColourId, Palette::bg);
    }

    juce::Font font (float height, bool isBold = false, float tracking = 0.0f) const
    {
        return juce::Font (juce::FontOptions (isBold ? bold : regular).withHeight (height)).withExtraKerningFactor (tracking);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return font ((float) buttonHeight * 0.50f);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.75f);
        const bool on = b.getToggleState();
        if (on || down)
        {
            g.setColour (Palette::sand.withAlpha (down ? 0.22f : 0.14f));
            g.fillRoundedRectangle (r, 3.0f);
        }
        g.setColour (on ? Palette::sand.withAlpha (0.8f) : (highlighted ? Palette::textFaint : Palette::plateEdge));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.getToggleState() ? Palette::sand : (highlighted ? Palette::text : Palette::textDim));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    void drawCornerResizer (juce::Graphics& g, int w, int h, bool highlighted, bool) override
    {
        g.setColour (highlighted ? Palette::textDim : Palette::textFaint);
        const float fw = (float) w, fh = (float) h;
        for (int i = 1; i <= 3; ++i)
        {
            const float o = (float) i * 4.5f;
            g.drawLine (fw - o, fh - 1.5f, fw - 1.5f, fh - o, 1.0f);
        }
    }

private:
    juce::Typeface::Ptr regular, bold;
};
