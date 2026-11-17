#pragma once

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>

// Drawing helpers for the banknote look: engraved fonts, guilloche linework,
// text on a circle. Linework is original; the front's background is a tinted,
// faded public-domain scan (CREDITS.md) with our own text printed over it.
namespace cash::art
{
juce::Font serif (float height, bool bold = false, float kerning = 0.0f);
juce::Font mono (float height, bool bold = false);

// Closed wavy ring: radius = mid +/- amp, with `petals` lobes.
juce::Path rosetteRing (juce::Point<float> centre, float mid, float amp, int petals, float phase, float squash = 1.0f);

// Several phase-shifted rings stroked thinly: the classic guilloche mesh.
void drawRosette (juce::Graphics&, juce::Point<float> centre, float innerRadius, float outerRadius,
                  int petals, int layers, juce::Colour, float thickness = 0.6f, float squash = 1.0f);

// Interlaced sine lines filling a band (rope border). Horizontal runs left to right.
void drawWaveBand (juce::Graphics&, juce::Rectangle<float> band, bool horizontal, int lines, float wavelength,
                   juce::Colour, float thickness = 0.6f);

// Text bent round a circle, centred on `centreAngle` (0 = top, clockwise).
// onBottom reads left to right along the bottom of the circle instead.
void drawTextAround (juce::Graphics&, const juce::String& text, juce::Point<float> centre, float radius,
                     float centreAngle, const juce::Font&, bool onBottom = false);

// Saw-toothed seal edge.
juce::Path toothedCircle (juce::Point<float> centre, float radius, float toothDepth, int teeth);

// A loose hand-written-looking flourish for signatures.
juce::Path signature (juce::Rectangle<float> area, uint32_t seed);

// A gold coin: reeded edge, embossed face, optional engraved label.
// Used by the Invest coins and by every rotary knob.
void drawCoin (juce::Graphics&, juce::Point<float> centre, float radius, const juce::String& label,
               bool highlight = false, bool enabled = true);

// Whole sides of the bill (static art, cached by the components).
// paintFront uses the tinted public-domain $100 scan (see CREDITS.md) and
// falls back to paintFrontPainted (all original art) if the scan is missing.
void paintFront (juce::Graphics&, juce::Rectangle<float> bill);
void paintFrontPainted (juce::Graphics&, juce::Rectangle<float> bill);
bool hasScan();
juce::Image makeTintedScan(); // invalid if the scan is missing
void paintBack (juce::Graphics&, juce::Rectangle<float> bill);

// Layout shared by the painter and the components that sit on top of the art.
struct FrontLayout
{
    explicit FrontLayout (juce::Rectangle<float> bill);

    juce::Rectangle<float> bill, inner;     // inner = inside the rope frame
    juce::Rectangle<float> title, subtitle;
    juce::Rectangle<float> portrait, ribbon;
    juce::Rectangle<float> leftZone, rightZone;
    juce::Rectangle<float> serialTop, serialBottom;
    juce::Rectangle<float> seal, sealCaption, motto, flip;
    juce::Rectangle<float> voiceSwitch; // POLY / MONO / LEGATO (M7)
    std::array<juce::Point<float>, 4> corners; // TL, TR, BL, BR numeral medallions
    float cornerRadius;
};

constexpr float frameInset = 10.0f;
constexpr float frameWidth = 22.0f;
}
