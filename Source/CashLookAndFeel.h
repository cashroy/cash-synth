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

    // The bill (M4)
    inline const juce::Colour desk       { 0xff08100c }; // what the bill lies on
    inline const juce::Colour paper      { 0xffe6e9d4 };
    inline const juce::Colour paperShade { 0xffcdd6bb };
    inline const juce::Colour ink        { 0xff1b3a29 }; // engraving green-black
    inline const juce::Colour inkSoft    { 0xff587a62 };
    inline const juce::Colour sealGreen  { 0xff1f6b46 };
    inline const juce::Colour serialInk  { 0xff2b7a4e };
    inline const juce::Colour shiftInk   { 0xffa8742c }; // copper "100"
    inline const juce::Colour ledGreen   { 0xff46e27e };
    inline const juce::Colour ledRed     { 0xffe8503f };
    inline const juce::Colour ledAmber   { 0xffe8b23f };
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
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool isOver, bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int height) override;
};
}
