#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace cash
{
namespace colours
{
    inline const juce::Colour background { 0xff0f2a1d }; // banknote green
    inline const juce::Colour panel      { 0xff163b29 };
    inline const juce::Colour panelEdge  { 0xff2a5a40 };
    inline const juce::Colour gold       { 0xffd4af37 };
    inline const juce::Colour goldDim    { 0xff7a6726 };
    inline const juce::Colour text       { 0xffe8e2c8 };
    inline const juce::Colour knobBody   { 0xff0b1f15 };
}

class CashLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    CashLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (13.0f); }
    juce::Font getLabelFont (juce::Label&) override { return juce::FontOptions (12.0f); }
};
}
