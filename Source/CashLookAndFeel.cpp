#include "CashLookAndFeel.h"
#include "BillArt.h"

namespace cash
{
CashLookAndFeel::CashLookAndFeel()
{
    using namespace colours;
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, goldDim);
    setColour (juce::Label::textColourId, text);
    setColour (juce::ComboBox::backgroundColourId, knobBody);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, panelEdge);
    setColour (juce::ComboBox::arrowColourId, gold);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, goldDim);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextEditor::backgroundColourId, knobBody);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, goldDim);
    setColour (juce::CaretComponent::caretColourId, gold);
    setColour (juce::TextButton::textColourOffId, gold);
    setColour (juce::TextButton::textColourOnId, gold);
    setColour (juce::TooltipWindow::backgroundColourId, knobBody);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, goldDim);
}

void CashLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool isOver, bool isDown)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    if (isDown)
        r = r.reduced (1.0f);
    g.setColour (isOver && b.isEnabled() ? colours::panelEdge : colours::knobBody);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (b.isEnabled() ? colours::gold : colours::panelEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.2f);
}

juce::Font CashLookAndFeel::getTextButtonFont (juce::TextButton&, int height)
{
    return juce::FontOptions (juce::Font::getDefaultSerifFontName(), juce::jmin (15.0f, static_cast<float> (height) * 0.55f), juce::Font::bold)
        .withKerningFactor (0.08f);
}

void CashLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    using namespace colours;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float track = juce::jmax (2.5f, radius * 0.12f);
    const float arcRadius = radius - track * 0.5f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // Bipolar parameters fill from the centre, unipolar from the start.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float fromAngle = bipolar ? startAngle + static_cast<float> (slider.valueToProportionOfLength (0.0)) * (endAngle - startAngle)
                                    : startAngle;

    juce::Path bg;
    bg.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (panelEdge);
    g.strokePath (bg, { track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (fromAngle, angle), juce::jmax (fromAngle, angle), true);
    g.setColour (slider.isEnabled() ? gold : goldDim);
    g.strokePath (value, { track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    // The knob itself is a coin; a notch engraved at the rim shows where it points.
    const float coinRadius = radius - track * 1.9f;
    art::drawCoin (g, centre, coinRadius, {}, slider.isMouseOverOrDragging(), slider.isEnabled());

    const auto tip = centre.getPointOnCircumference (coinRadius * 0.7f, angle);
    const auto base = centre.getPointOnCircumference (coinRadius * 0.2f, angle);
    g.setColour (juce::Colour (0xfff6dc86).withAlpha (0.8f));
    g.drawLine ({ base.translated (0.0f, 1.0f), tip.translated (0.0f, 1.0f) }, 2.0f);
    g.setColour (juce::Colour (0xff4a370e));
    g.drawLine ({ base, tip }, 2.2f);
}

void CashLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.hasKeyboardFocus (true) ? colours::gold : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    juce::Path arrow;
    const float ax = static_cast<float> (width) - 14.0f, ay = static_cast<float> (height) * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.fillPath (arrow);
}
}
