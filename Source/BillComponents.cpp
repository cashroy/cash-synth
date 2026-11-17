#include "BillComponents.h"
#include "CashCode.h"
#include "CashLookAndFeel.h"
#include "PluginProcessor.h"

namespace cash
{
//============================================================================== ScopePortrait
ScopePortrait::ScopePortrait (const ScopeBuffer& s) : scope (s)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void ScopePortrait::timerCallback()
{
    if (isShowing())
        refresh();
}

void ScopePortrait::refresh()
{
    scope.read (samples.data(), readSize);

    // Trigger on a rising zero crossing so the wave stands still.
    start = readSize - showSize;
    for (int i = 1; i < readSize - showSize; ++i)
        if (samples[static_cast<size_t> (i - 1)] < 0.0f && samples[static_cast<size_t> (i)] >= 0.0f)
        {
            start = i;
            break;
        }

    peak = 0.0f;
    for (int i = start; i < start + showSize; ++i)
        peak = juce::jmax (peak, std::abs (samples[static_cast<size_t> (i)]));

    // Gentle auto-gain so quiet patches still fill the portrait.
    const float target = 0.9f / juce::jmax (0.08f, peak);
    gain += (target - gain) * 0.35f;
    repaint();
}

void ScopePortrait::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto oval = getLocalBounds().toFloat().reduced (2.0f);
    juce::Path clip;
    clip.addEllipse (oval);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (clip);
        g.setGradientFill (juce::ColourGradient (paper.brighter (0.05f), oval.getCentreX(), oval.getCentreY() - oval.getHeight() * 0.1f,
                                                 paperShade.darker (0.08f), oval.getX(), oval.getY(), true));
        g.fillEllipse (oval);

        // Engraving: fine horizontals plus concentric rings.
        g.setColour (ink.withAlpha (0.10f));
        for (float y = oval.getY(); y < oval.getBottom(); y += 3.0f)
            g.drawHorizontalLine (juce::roundToInt (y), oval.getX(), oval.getRight());
        g.setColour (ink.withAlpha (0.12f));
        for (float inset = 8.0f; inset < oval.getWidth() * 0.5f; inset += 7.0f)
            g.drawEllipse (oval.reduced (inset), 0.5f);

        const float midY = oval.getCentreY();
        g.setColour (inkSoft.withAlpha (0.5f));
        g.drawHorizontalLine (juce::roundToInt (midY), oval.getX(), oval.getRight());

        juce::Path wave;
        const float left = oval.getX() + 6.0f, width = oval.getWidth() - 12.0f;
        const float amp = oval.getHeight() * 0.30f;
        for (int i = 0; i < showSize; i += 2)
        {
            const float x = left + width * static_cast<float> (i) / static_cast<float> (showSize - 1);
            const float y = midY - juce::jlimit (-1.2f, 1.2f, samples[static_cast<size_t> (start + i)] * gain) * amp;
            if (i == 0)
                wave.startNewSubPath (x, y);
            else
                wave.lineTo (x, y);
        }
        g.setColour (sealGreen.withAlpha (0.28f));
        g.strokePath (wave, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (ink);
        g.strokePath (wave, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (! hasSignal())
        {
            g.setColour (inkSoft);
            g.setFont (art::serif (12.0f, false, 0.2f));
            g.drawText ("PLAY A NOTE", oval.withTrimmedTop (oval.getHeight() * 0.62f).withHeight (20.0f), juce::Justification::centred);
        }
    }

    g.setColour (ink);
    g.drawEllipse (oval, 2.2f);
    g.drawEllipse (oval.reduced (5.0f), 0.8f);
}

//============================================================================== JackpotSeal
JackpotSeal::JackpotSeal() : juce::Button ("Jackpot")
{
    setTooltip ("Jackpot: re-roll the whole sound (undoable)");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

bool JackpotSeal::hitTest (int x, int y)
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return c.getDistanceFrom ({ static_cast<float> (x), static_cast<float> (y) }) <= static_cast<float> (juce::jmin (getWidth(), getHeight())) * 0.5f;
}

void JackpotSeal::paintButton (juce::Graphics& g, bool isOver, bool isDown)
{
    using namespace colours;
    auto c = getLocalBounds().toFloat().getCentre();
    float r = static_cast<float> (juce::jmin (getWidth(), getHeight())) * 0.5f - 2.0f;
    if (isDown)
        r -= 2.0f;

    const auto green = isOver ? sealGreen.brighter (0.25f) : sealGreen;
    g.setColour (green.withAlpha (0.92f));
    g.fillPath (art::toothedCircle (c, r, 5.0f, 64));

    g.setColour (paper.withAlpha (0.9f));
    g.drawEllipse (juce::Rectangle<float> (r * 1.72f, r * 1.72f).withCentre (c), 1.2f);
    g.drawEllipse (juce::Rectangle<float> (r * 1.18f, r * 1.18f).withCentre (c), 1.0f);
    art::drawRosette (g, c, r * 0.6f, r * 0.84f, 24, 3, paper.withAlpha (0.35f), 0.5f);
    g.setColour (green.darker (0.15f));
    g.fillEllipse (juce::Rectangle<float> (r * 1.14f, r * 1.14f).withCentre (c));

    g.setColour (paper);
    art::drawTextAround (g, "THE CASHROY JACKPOT", c, r * 0.73f, 0.0f, art::serif (r * 0.15f, true, 0.1f));
    art::drawTextAround (g, "SPIN TO WIN", c, r * 0.73f, juce::MathConstants<float>::pi, art::serif (r * 0.15f, true, 0.1f), true);

    g.setFont (art::serif (r * 0.62f, true));
    g.drawText ("$", juce::Rectangle<float> (r, r * 0.8f).withCentre (c.translated (0.0f, -r * 0.08f)), juce::Justification::centred);
    g.setFont (art::serif (r * 0.13f, true, 0.2f));
    g.drawText ("JACKPOT", juce::Rectangle<float> (r, r * 0.2f).withCentre (c.translated (0.0f, r * 0.34f)), juce::Justification::centred);

    // Little stars at 3 and 9 o'clock between the arc texts.
    for (float a : { juce::MathConstants<float>::halfPi, -juce::MathConstants<float>::halfPi })
    {
        const auto p = c.getPointOnCircumference (r * 0.73f, a);
        juce::Path star;
        star.addStar (p, 5, r * 0.03f, r * 0.075f, 0.0f);
        g.fillPath (star);
    }
}

//============================================================================== SerialTag
SerialTag::SerialTag()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Serial number of this sound. Click to copy its CashCode.");
}

void SerialTag::setSerial (const juce::String& s)
{
    if (s != serial)
    {
        serial = s;
        repaint();
    }
}

void SerialTag::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    if (isMouseOver())
    {
        g.setColour (colours::serialInk.withAlpha (0.10f));
        g.fillRoundedRectangle (r, 4.0f);
    }
    g.setColour (colours::serialInk);
    g.setFont (art::mono (copied ? 14.0f : 19.0f, true));
    g.drawText (copied ? "CASHCODE COPIED" : serial, r, juce::Justification::centred, false);
}

void SerialTag::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onClick)
        onClick();
}

void SerialTag::flashCopied()
{
    copied = true;
    repaint();
    startTimer (1200);
}

void SerialTag::timerCallback()
{
    stopTimer();
    copied = false;
    repaint();
}

//============================================================================== FlipButton
FlipButton::FlipButton (const juce::String& text, bool onDark) : juce::Button (text), dark (onDark)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Flip the bill over");
}

void FlipButton::paintButton (juce::Graphics& g, bool isOver, bool isDown)
{
    using namespace colours;
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    if (isDown)
        r = r.reduced (1.0f);
    const auto fg = dark ? gold : ink;
    const auto bg = dark ? knobBody : paperShade;
    g.setColour (isOver ? bg.brighter (dark ? 0.3f : 0.1f) : bg);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (fg);
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.2f);

    // Curved arrow glyph.
    const float s = r.getHeight() * 0.32f;
    const juce::Point<float> c (r.getX() + r.getHeight() * 0.6f, r.getCentreY());
    juce::Path arrow;
    arrow.addCentredArc (c.x, c.y, s, s, 0.0f, -2.4f, 1.6f, true);
    g.strokePath (arrow, juce::PathStrokeType (1.4f));
    const auto tip = c.getPointOnCircumference (s, 1.6f);
    juce::Path head;
    head.addTriangle (tip.translated (-3.5f, -2.0f), tip.translated (2.0f, -3.5f), tip.translated (1.0f, 2.5f));
    g.fillPath (head);

    g.setFont (art::serif (11.0f, true, 0.1f));
    g.drawText (getButtonText(), r.withTrimmedLeft (r.getHeight() * 1.1f).withTrimmedRight (6.0f), juce::Justification::centred, false);
}

//============================================================================== BillFront
BillFront::BillFront (CashSynthAudioProcessor& p) : processor (p), portrait (p.getScope())
{
    addAndMakeVisible (portrait);
    addAndMakeVisible (seal);
    addAndMakeVisible (serialTop);
    addAndMakeVisible (serialBottom);
    addAndMakeVisible (flip);

    seal.onClick = [this]
    {
        processor.hitJackpot (static_cast<uint64_t> (juce::Random::getSystemRandom().nextInt64()));
        if (onMoneyMoved)
            onMoneyMoved();
    };

    for (auto* tag : { &serialTop, &serialBottom })
        tag->onClick = [this, tag]
        {
            juce::SystemClipboard::copyTextToClipboard (CashCode::encode (processor.getPatch()));
            tag->flashCopied();
        };

    flip.onClick = [this] { if (onFlip) onFlip(); };
    setSerial (CashCode::serialNumber (processor.getPatch()));
}

void BillFront::setSerial (const juce::String& s)
{
    serialTop.setSerial (s);
    serialBottom.setSerial (s);
}

void BillFront::paint (juce::Graphics& g)
{
    art.draw (g, getLocalBounds(), [] (juce::Graphics& ig, juce::Rectangle<float> r) { art::paintFront (ig, r); });
}

void BillFront::resized()
{
    const art::FrontLayout L (getLocalBounds().toFloat());
    portrait.setBounds (L.portrait.toNearestInt());
    seal.setBounds (L.seal.toNearestInt());
    serialTop.setBounds (L.serialTop.toNearestInt());
    serialBottom.setBounds (L.serialBottom.toNearestInt());
    flip.setBounds (L.flip.toNearestInt());
}

//============================================================================== BillBack
BillBack::BillBack (juce::AudioProcessorValueTreeState& state) : advanced (state)
{
    addAndMakeVisible (advanced);
    addAndMakeVisible (flip);
    flip.onClick = [this] { if (onFlip) onFlip(); };
}

void BillBack::paint (juce::Graphics& g)
{
    art.draw (g, getLocalBounds(), [] (juce::Graphics& ig, juce::Rectangle<float> r) { art::paintBack (ig, r); });
}

void BillBack::resized()
{
    auto inner = getLocalBounds().reduced (20);
    auto titleRow = inner.removeFromTop (24);
    flip.setBounds (titleRow.removeFromRight (150).reduced (0, 1));
    inner.removeFromTop (6);
    advanced.setBounds (inner);
}
}
