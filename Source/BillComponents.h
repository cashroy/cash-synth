#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AdvancedPanel.h"
#include "BillArt.h"
#include "ScopeBuffer.h"

class CashSynthAudioProcessor;

namespace cash
{
// Static art drawn once into an image at the screen's pixel scale.
class ArtCache
{
public:
    template <typename Painter>
    void draw (juce::Graphics& g, juce::Rectangle<int> area, Painter&& painter)
    {
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        if (image.isNull() || area.getWidth() != size.getWidth() || area.getHeight() != size.getHeight() || scale != cachedScale)
        {
            size = area;
            cachedScale = scale;
            image = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt (static_cast<float> (area.getWidth()) * scale)),
                                 juce::jmax (1, juce::roundToInt (static_cast<float> (area.getHeight()) * scale)), true);
            juce::Graphics ig (image);
            ig.addTransform (juce::AffineTransform::scale (scale));
            painter (ig, area.withZeroOrigin().toFloat());
        }
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / cachedScale).translated (area.toFloat().getTopLeft()));
    }

private:
    juce::Image image;
    juce::Rectangle<int> size;
    float cachedScale = 0.0f;
};

// The portrait oval: a live, engraved-looking scope of the output.
class ScopePortrait final : public juce::Component, private juce::Timer
{
public:
    explicit ScopePortrait (const ScopeBuffer&);
    void paint (juce::Graphics&) override;
    void refresh(); // pulls the newest audio (the timer calls this while visible)
    bool hasSignal() const { return peak > 1.0e-4f; }

private:
    void timerCallback() override;

    static constexpr int readSize = 2048, showSize = 900;
    const ScopeBuffer& scope;
    std::array<float, readSize> samples {};
    int start = 0;
    float peak = 0.0f, gain = 1.0f;
};

// The treasury-style seal. Pressing it hits the jackpot.
class JackpotSeal final : public juce::Button
{
public:
    JackpotSeal();
    void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
    bool hitTest (int x, int y) override;
};

// Banknote serial. Click to copy the full CashCode.
class SerialTag final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SerialTag();
    void setSerial (const juce::String&);
    juce::String getSerial() const { return serial; }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void flashCopied();

    std::function<void()> onClick;

private:
    void timerCallback() override;
    juce::String serial;
    bool copied = false;
};

// Small engraved tab that flips the bill over.
class FlipButton final : public juce::Button
{
public:
    FlipButton (const juce::String& text, bool onDark);
    void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

private:
    bool dark;
};

// Engraved three-way switch on the front: POLY / MONO / LEGATO (the voiceMode param).
class VoiceModeSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit VoiceModeSwitch (juce::RangedAudioParameter&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    int getMode() const { return mode; }
    void choose (int newMode); // as if clicked (undoable by the host, notifies it)

private:
    juce::Rectangle<float> segment (int index) const;
    int segmentAt (juce::Point<float>) const;

    juce::ParameterAttachment attachment;
    int mode = 0, hover = -1;
};

class BillFront final : public juce::Component
{
public:
    explicit BillFront (CashSynthAudioProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    void setSerial (const juce::String&);
    JackpotSeal& getJackpot() { return seal; }
    SerialTag& getSerial (int which) { return which == 0 ? serialTop : serialBottom; }
    ScopePortrait& getPortrait() { return portrait; }
    FlipButton& getFlipButton() { return flip; }
    VoiceModeSwitch& getVoiceSwitch() { return voiceSwitch; }

    std::function<void()> onFlip, onMoneyMoved;

private:
    CashSynthAudioProcessor& processor;
    ArtCache art;
    ScopePortrait portrait;
    JackpotSeal seal;
    SerialTag serialTop, serialBottom;
    FlipButton flip { "FLIP FOR KNOBS", false };
    VoiceModeSwitch voiceSwitch;
};

class BillBack final : public juce::Component
{
public:
    explicit BillBack (juce::AudioProcessorValueTreeState&);
    void paint (juce::Graphics&) override;
    void resized() override;

    AdvancedPanel& getAdvancedPanel() { return advanced; }
    FlipButton& getFlipButton() { return flip; }
    std::function<void()> onFlip;

private:
    ArtCache art;
    AdvancedPanel advanced;
    FlipButton flip { "FLIP TO FRONT", true };
};
}
