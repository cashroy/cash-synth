#include "BillComponents.h"
#include "CashCode.h"
#include "CashLookAndFeel.h"
#include "PluginProcessor.h"
#include "Parameters.h"

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

//============================================================================== VoiceModeSwitch
namespace
{
    constexpr float captionHeight = 15.0f;
}

VoiceModeSwitch::VoiceModeSwitch (juce::RangedAudioParameter& param)
    : attachment (param, [this] (float v) { mode = juce::roundToInt (v); repaint(); })
{
    setTooltip ("Voice mode. Poly: chords. Mono: one note, envelopes restart on every key. "
                "Legato: one note, envelopes keep going while keys overlap.");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    attachment.sendInitialUpdate();
}

juce::Rectangle<float> VoiceModeSwitch::segment (int index) const
{
    auto r = getLocalBounds().toFloat().withTrimmedTop (captionHeight).reduced (2.0f);
    const float w = r.getWidth() / 3.0f;
    return r.withX (r.getX() + w * static_cast<float> (index)).withWidth (w);
}

int VoiceModeSwitch::segmentAt (juce::Point<float> p) const
{
    for (int i = 0; i < 3; ++i)
        if (segment (i).contains (p))
            return i;
    return -1;
}

void VoiceModeSwitch::choose (int newMode)
{
    if (newMode >= 0 && newMode < 3 && newMode != mode)
        attachment.setValueAsCompleteGesture (static_cast<float> (newMode));
}

void VoiceModeSwitch::mouseDown (const juce::MouseEvent& e) { choose (segmentAt (e.position)); }
void VoiceModeSwitch::mouseMove (const juce::MouseEvent& e) { const int h = segmentAt (e.position); if (h != hover) { hover = h; repaint(); } }
void VoiceModeSwitch::mouseExit (const juce::MouseEvent&) { hover = -1; repaint(); }

void VoiceModeSwitch::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto all = getLocalBounds().toFloat();

    // Paper plate so the scan underneath doesn't fight the switch.
    g.setColour (paper.withAlpha (0.82f));
    g.fillRoundedRectangle (all, 6.0f);

    g.setColour (inkSoft);
    g.setFont (art::serif (10.0f, true, 0.25f));
    g.drawText ("VOICE", all.withHeight (captionHeight).translated (0.0f, 1.0f), juce::Justification::centred);

    const auto body = segment (0).getUnion (segment (2));
    g.setColour (paperShade);
    g.fillRoundedRectangle (body, 4.0f);

    const auto names = param::voiceModeNames();
    for (int i = 0; i < 3; ++i)
    {
        const auto r = segment (i);
        const bool on = i == mode;
        if (on)
        {
            g.setColour (sealGreen);
            g.fillRoundedRectangle (r.reduced (1.5f), 3.0f);
        }
        else if (i == hover)
        {
            g.setColour (ink.withAlpha (0.08f));
            g.fillRoundedRectangle (r.reduced (1.5f), 3.0f);
        }
        if (i > 0)
        {
            g.setColour (ink.withAlpha (0.4f));
            g.drawVerticalLine (juce::roundToInt (r.getX()), r.getY() + 5.0f, r.getBottom() - 5.0f);
        }
        g.setColour (on ? paper : ink);
        g.setFont (art::serif (12.0f, true, 0.12f));
        g.drawText (names[i].toUpperCase(), r, juce::Justification::centred, false);
    }

    g.setColour (ink);
    g.drawRoundedRectangle (body, 4.0f, 1.2f);
}

//============================================================================== BillFront
BillFront::BillFront (CashSynthAudioProcessor& p)
    : processor (p), portrait (p.getScope()), voiceSwitch (*p.getState().getParameter (param::voiceMode))
{
    addAndMakeVisible (voiceSwitch);
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
    voiceSwitch.setBounds (L.voiceSwitch.toNearestInt());
}

//============================================================================== BillBack
BillBack::BillBack (juce::AudioProcessorValueTreeState& state, ParamLocks* locks) : advanced (state, locks)
{
    addAndMakeVisible (advanced);
    addAndMakeVisible (tabs);
    addAndMakeVisible (flip);
    flip.onClick = [this] { if (onFlip) onFlip(); };
    advanced.onPageChange = [this] (int) { tabs.repaint(); };
}

void BillBack::paint (juce::Graphics& g)
{
    art.draw (g, getLocalBounds(), [] (juce::Graphics& ig, juce::Rectangle<float> r) { art::paintBack (ig, r); });
}

void BillBack::resized()
{
    auto inner = getLocalBounds().reduced (20, 18);
    auto titleRow = inner.removeFromTop (24);
    flip.setBounds (titleRow.removeFromRight (150).reduced (0, 1));
    tabs.setBounds (titleRow.removeFromLeft (234).reduced (0, 1)); // left of the centred title (SYNTH | EFFECTS | LAYERS)
    inner.removeFromTop (4);
    advanced.setBounds (inner);
}
}

//============================================================================== FlipCard
namespace cash
{
namespace
{
    constexpr int flipMargin = 16; // room round the bill for the lift and the shadow
}

FlipCard::FlipCard()
{
    setInterceptsMouseClicks (true, false);
    setVisible (false);
}

void FlipCard::start (juce::Component& from, juce::Component& to)
{
    finish();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (from.getScreenBounds());
    const float scale = juce::jlimit (1.0f, 3.0f, static_cast<float> (display != nullptr ? display->scale : 1.0)
                                                      * juce::Component::getApproximateScaleFactorForComponent (&from));
    from.setVisible (true);
    fromImage = from.createComponentSnapshot (from.getLocalBounds(), true, scale);
    to.setVisible (true);
    toImage = to.createComponentSnapshot (to.getLocalBounds(), true, scale);
    from.setVisible (false);
    to.setVisible (false);
    target = &to;

    setBounds (from.getBounds().expanded (flipMargin));
    progress = 0.0f;
    startMs = juce::Time::getMillisecondCounterHiRes();
    setVisible (true);
    toFront (false);
    startTimerHz (60);
}

void FlipCard::finish()
{
    stopTimer();
    if (auto* t = target.getComponent())
        t->setVisible (true);
    target = nullptr;
    fromImage = {};
    toImage = {};
    setVisible (false);
    if (auto* parent = getParentComponent())
        parent->repaint();
}

void FlipCard::timerCallback()
{
    progress = static_cast<float> (juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - startMs) / durationMs));
    if (progress >= 1.0f)
        finish();
    else
        repaint();
}

void FlipCard::paint (juce::Graphics& g)
{
    if (fromImage.isNull() || toImage.isNull())
        return;

    // Ease in and out, turn through half a circle: the bill narrows to its edge,
    // the other side comes round. It lifts (a touch taller, shadow further away)
    // and darkens as it stands edge-on to the light.
    const float e = progress * progress * (3.0f - 2.0f * progress);
    const float angle = e * juce::MathConstants<float>::pi;
    const float squash = std::abs (std::cos (angle));
    const float lift = std::sin (angle);

    const auto bill = getLocalBounds().reduced (flipMargin).toFloat();
    const float w = juce::jmax (1.0f, bill.getWidth() * squash);
    const float h = bill.getHeight() * (1.0f + 0.035f * lift);
    const auto card = juce::Rectangle<float> (w, h).withCentre (bill.getCentre());

    g.setColour (juce::Colours::black.withAlpha (0.45f - 0.15f * lift));
    g.fillRoundedRectangle (card.translated (3.0f + 7.0f * lift, 5.0f + 9.0f * lift), 8.0f);

    const auto& image = e < 0.5f ? fromImage : toImage;
    g.drawImage (image, card, juce::RectanglePlacement::stretchToFit);
    g.setColour (juce::Colours::black.withAlpha (0.4f * lift));
    g.fillRoundedRectangle (card, 8.0f);
    // The edge catching the light as it comes round.
    g.setColour (colours::gold.withAlpha (0.5f * lift));
    g.drawRoundedRectangle (card.reduced (0.5f), 8.0f, 1.0f);
}
}
