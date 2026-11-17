#include "BillArt.h"
#include "CashLookAndFeel.h"
#include "BinaryData.h"

namespace cash::art
{
namespace
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    void drawCentred (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, const juce::Font& f, juce::Colour c)
    {
        g.setColour (c);
        g.setFont (f);
        g.drawText (text, r, juce::Justification::centred, false);
    }

    void paintPaperTexture (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour line, float spacing)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (line);
        for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += spacing)
            g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 0.4f);
    }

    void paintFrame (juce::Graphics& g, juce::Rectangle<float> bill, float inset, float width, juce::Colour fill, juce::Colour lines, juce::Colour edge)
    {
        const auto outer = bill.reduced (inset);
        const auto inner = outer.reduced (width);

        juce::Path band;
        band.addRectangle (outer);
        band.addRectangle (inner);
        band.setUsingNonZeroWinding (false);
        g.setColour (fill);
        g.fillPath (band);

        const int strands = 6;
        const float wl = width * 1.6f;
        drawWaveBand (g, { outer.getX(), outer.getY(), outer.getWidth(), width }, true, strands, wl, lines, 0.6f);
        drawWaveBand (g, { outer.getX(), inner.getBottom(), outer.getWidth(), width }, true, strands, wl, lines, 0.6f);
        drawWaveBand (g, { outer.getX(), inner.getY(), width, inner.getHeight() }, false, strands, wl, lines, 0.6f);
        drawWaveBand (g, { inner.getRight(), inner.getY(), width, inner.getHeight() }, false, strands, wl, lines, 0.6f);

        // Corner knots.
        for (auto c : { outer.getTopLeft(), outer.getTopRight(), outer.getBottomLeft(), outer.getBottomRight() })
        {
            const auto p = c + juce::Point<float> (c.x < bill.getCentreX() ? width * 0.5f : -width * 0.5f,
                                                   c.y < bill.getCentreY() ? width * 0.5f : -width * 0.5f);
            g.setColour (fill);
            g.fillEllipse (juce::Rectangle<float> (width, width).withCentre (p));
            drawRosette (g, p, width * 0.12f, width * 0.48f, 8, 4, lines, 0.6f);
        }

        g.setColour (edge);
        g.drawRect (outer, 1.4f);
        g.drawRect (inner, 1.0f);
    }
}

juce::Font serif (float height, bool bold, float kerning)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), height, bold ? juce::Font::bold : juce::Font::plain)
                           .withKerningFactor (kerning));
}

juce::Font mono (float height, bool bold)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Path rosetteRing (juce::Point<float> c, float mid, float amp, int petals, float phase, float squash)
{
    juce::Path p;
    const int steps = juce::jlimit (90, 720, petals * 24);
    for (int i = 0; i <= steps; ++i)
    {
        const float t = twoPi * static_cast<float> (i) / static_cast<float> (steps);
        const float n = static_cast<float> (petals);
        const float r = mid + amp * (0.75f * std::sin (n * t + phase) + 0.25f * std::sin (3.0f * n * t - 2.0f * phase));
        const juce::Point<float> pt (c.x + r * std::sin (t), c.y - r * std::cos (t) * squash);
        if (i == 0)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
    }
    p.closeSubPath();
    return p;
}

void drawRosette (juce::Graphics& g, juce::Point<float> centre, float innerRadius, float outerRadius,
                  int petals, int layers, juce::Colour colour, float thickness, float squash)
{
    const float mid = (innerRadius + outerRadius) * 0.5f;
    const float amp = (outerRadius - innerRadius) * 0.5f;
    g.setColour (colour);
    for (int k = 0; k < layers; ++k)
    {
        const float phase = twoPi * static_cast<float> (k) / static_cast<float> (layers * juce::jmax (1, petals / 4));
        g.strokePath (rosetteRing (centre, mid, amp, petals, phase, squash), juce::PathStrokeType (thickness));
    }
}

void drawWaveBand (juce::Graphics& g, juce::Rectangle<float> band, bool horizontal, int lines, float wavelength,
                   juce::Colour colour, float thickness)
{
    const float length = horizontal ? band.getWidth() : band.getHeight();
    const float across = horizontal ? band.getHeight() : band.getWidth();
    const float amp = across * 0.36f;
    g.setColour (colour);

    for (int k = 0; k < lines; ++k)
    {
        const float phase = twoPi * static_cast<float> (k) / static_cast<float> (lines);
        juce::Path p;
        for (float s = 0.0f; s <= length; s += 1.5f)
        {
            const float o = amp * std::sin (twoPi * s / wavelength + phase);
            const juce::Point<float> pt = horizontal ? juce::Point<float> (band.getX() + s, band.getCentreY() + o)
                                                     : juce::Point<float> (band.getCentreX() + o, band.getY() + s);
            if (s == 0.0f)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.strokePath (p, juce::PathStrokeType (thickness));
    }
}

void drawTextAround (juce::Graphics& g, const juce::String& text, juce::Point<float> c, float radius,
                     float centreAngle, const juce::Font& font, bool onBottom)
{
    g.setFont (font);
    const float total = juce::GlyphArrangement::getStringWidth (font, text);
    const float span = total / radius;
    float a = onBottom ? centreAngle + span * 0.5f : centreAngle - span * 0.5f;
    const float h = font.getHeight();

    for (auto ch : text)
    {
        const auto glyph = juce::String::charToString (ch);
        const float w = juce::GlyphArrangement::getStringWidth (font, glyph);
        const float step = w / radius;
        const float mid = onBottom ? a - step * 0.5f : a + step * 0.5f;

        juce::Graphics::ScopedSaveState save (g);
        if (onBottom)
        {
            g.addTransform (juce::AffineTransform::rotation (mid - juce::MathConstants<float>::pi, c.x, c.y));
            g.drawText (glyph, juce::Rectangle<float> (c.x - w, c.y + radius - h * 0.5f, w * 2.0f, h), juce::Justification::centred, false);
        }
        else
        {
            g.addTransform (juce::AffineTransform::rotation (mid, c.x, c.y));
            g.drawText (glyph, juce::Rectangle<float> (c.x - w, c.y - radius - h * 0.5f, w * 2.0f, h), juce::Justification::centred, false);
        }
        a = onBottom ? a - step : a + step;
    }
}

juce::Path toothedCircle (juce::Point<float> c, float radius, float toothDepth, int teeth)
{
    juce::Path p;
    const int points = teeth * 2;
    for (int i = 0; i < points; ++i)
    {
        const float t = twoPi * static_cast<float> (i) / static_cast<float> (points);
        const float r = (i % 2 == 0) ? radius : radius - toothDepth;
        const juce::Point<float> pt (c.x + r * std::sin (t), c.y - r * std::cos (t));
        if (i == 0)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
    }
    p.closeSubPath();
    return p;
}

juce::Path signature (juce::Rectangle<float> area, uint32_t seed)
{
    juce::Random rng (static_cast<juce::int64> (seed));
    juce::Path p;
    const float midY = area.getCentreY();
    float x = area.getX();
    p.startNewSubPath (x, midY);
    const int strokes = 7;
    const float step = area.getWidth() / static_cast<float> (strokes);
    for (int i = 0; i < strokes; ++i)
    {
        const float h = area.getHeight() * (0.25f + 0.25f * rng.nextFloat());
        const float nx = x + step;
        p.cubicTo (x + step * 0.2f, midY - h * 1.4f, x + step * (0.9f + 0.4f * rng.nextFloat()), midY - h,
                   nx - step * 0.3f, midY + h * (rng.nextBool() ? 0.6f : -0.2f));
        x = nx;
    }
    p.quadraticTo (x + step * 0.3f, midY + area.getHeight() * 0.35f, x - step * 2.0f, midY + area.getHeight() * 0.2f);
    return p;
}

void drawCoin (juce::Graphics& g, juce::Point<float> c, float r, const juce::String& label, bool highlight, bool enabled)
{
    const juce::Colour light (0xfff6dc86), mid (0xffd4af37), dark (0xff8c6a1d), deep (0xff5a4312);
    const auto circle = [&] (float radius) { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c); };
    const float sat = enabled ? 1.0f : 0.35f;

    // Drop shadow, then the reeded edge.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (circle (r).translated (1.0f, 2.0f));
    g.setColour (dark.withMultipliedSaturation (sat));
    g.fillEllipse (circle (r));
    g.setColour (deep.withMultipliedSaturation (sat).withAlpha (0.8f));
    const int reeds = juce::jlimit (24, 120, juce::roundToInt (r * 2.2f));
    for (int i = 0; i < reeds; ++i)
    {
        const float a = twoPi * static_cast<float> (i) / static_cast<float> (reeds);
        g.drawLine ({ c.getPointOnCircumference (r * 0.9f, a), c.getPointOnCircumference (r - 0.5f, a) }, 0.8f);
    }

    // Face.
    auto faceLight = light, faceMid = mid;
    if (highlight)
    {
        faceLight = faceLight.brighter (0.15f);
        faceMid = faceMid.brighter (0.15f);
    }
    g.setGradientFill (juce::ColourGradient (faceLight.withMultipliedSaturation (sat), c.x - r * 0.4f, c.y - r * 0.5f,
                                             faceMid.withMultipliedSaturation (sat).darker (0.25f), c.x + r * 0.6f, c.y + r * 0.8f, true));
    g.fillEllipse (circle (r * 0.88f));
    g.setColour (deep.withMultipliedSaturation (sat).withAlpha (0.55f));
    g.drawEllipse (circle (r * 0.76f), juce::jmax (0.6f, r * 0.025f));
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.drawEllipse (circle (r * 0.87f), 0.7f);

    if (label.isNotEmpty())
    {
        const auto box = circle (r * 0.7f);
        g.setFont (serif (r * (label.length() > 2 ? 0.48f : 0.6f), true));
        g.setColour (light.withAlpha (0.9f));
        g.drawText (label, box.translated (0.0f, 1.0f), juce::Justification::centred, false);
        g.setColour (deep);
        g.drawText (label, box, juce::Justification::centred, false);
    }
}

FrontLayout::FrontLayout (juce::Rectangle<float> b) : bill (b)
{
    inner = bill.reduced (frameInset + frameWidth);
    title = inner.withTrimmedTop (4.0f).withHeight (36.0f).withSizeKeepingCentre (640.0f, 36.0f);
    subtitle = title.withY (title.getBottom()).withHeight (18.0f);

    const float pw = 250.0f;
    const float ph = juce::jmin (280.0f, inner.getHeight() - 130.0f);
    portrait = juce::Rectangle<float> (pw, ph).withCentre ({ inner.getCentreX(), subtitle.getBottom() + 8.0f + ph * 0.5f });
    ribbon = juce::Rectangle<float> (400.0f, 30.0f).withCentre ({ inner.getCentreX(), portrait.getBottom() + 22.0f });

    cornerRadius = 42.0f;
    corners = { juce::Point<float> (inner.getX() + 52.0f, inner.getY() + 50.0f),
                juce::Point<float> (inner.getRight() - 52.0f, inner.getY() + 50.0f),
                juce::Point<float> (inner.getX() + 52.0f, inner.getBottom() - 50.0f),
                juce::Point<float> (inner.getRight() - 52.0f, inner.getBottom() - 50.0f) };

    leftZone = juce::Rectangle<float>::leftTopRightBottom (inner.getX() + 110.0f, portrait.getY(), portrait.getX() - 24.0f, inner.getBottom() - 8.0f);
    rightZone = juce::Rectangle<float>::leftTopRightBottom (portrait.getRight() + 24.0f, portrait.getY(), inner.getRight() - 110.0f, inner.getBottom() - 8.0f);

    serialTop = leftZone.withHeight (28.0f).translated (0.0f, 4.0f);
    motto = rightZone.withHeight (26.0f).translated (0.0f, 2.0f);

    const float sealSize = 152.0f;
    seal = juce::Rectangle<float> (sealSize, sealSize).withCentre ({ rightZone.getCentreX(), portrait.getCentreY() + 4.0f });
    sealCaption = rightZone.withY (seal.getBottom() + 2.0f).withHeight (16.0f);
    serialBottom = juce::Rectangle<float>::leftTopRightBottom (ribbon.getRight() + 24.0f, inner.getBottom() - 74.0f, rightZone.getRight(), inner.getBottom() - 46.0f);
    flip = juce::Rectangle<float> (156.0f, 24.0f).withPosition (rightZone.getRight() - 156.0f, inner.getBottom() - 38.0f);
    // Under the legal-tender text (serialTop + 8 + 30 high), centred in the left zone.
    voiceSwitch = juce::Rectangle<float> (228.0f, 48.0f).withCentre ({ leftZone.getCentreX(), serialTop.getBottom() + 38.0f + 22.0f + 24.0f });
}

namespace
{
    void drawPortraitRing (juce::Graphics& g, const FrontLayout& L)
    {
        using namespace colours;
        const float squash = L.portrait.getHeight() / L.portrait.getWidth();
        drawRosette (g, L.portrait.getCentre(), L.portrait.getWidth() * 0.5f + 2.0f, L.portrait.getWidth() * 0.5f + 9.0f,
                     48, 4, ink.withAlpha (0.8f), 0.6f, squash);
    }

    void drawRibbon (juce::Graphics& g, const FrontLayout& L)
    {
        using namespace colours;
        auto r = L.ribbon;
        juce::Path ribbon;
        const float notch = 12.0f;
        ribbon.startNewSubPath (r.getX() - notch, r.getY());
        ribbon.lineTo (r.getRight() + notch, r.getY());
        ribbon.lineTo (r.getRight(), r.getCentreY());
        ribbon.lineTo (r.getRight() + notch, r.getBottom());
        ribbon.lineTo (r.getX() - notch, r.getBottom());
        ribbon.lineTo (r.getX(), r.getCentreY());
        ribbon.closeSubPath();
        g.setColour (paperShade);
        g.fillPath (ribbon);
        g.setColour (ink);
        g.strokePath (ribbon, juce::PathStrokeType (1.2f));
        drawCentred (g, "ONE HUNDRED CASH", r, serif (20.0f, true, 0.14f), ink);
    }

    juce::Rectangle<float> legalTextArea (const FrontLayout& L)
    {
        return L.leftZone.withY (L.serialTop.getBottom() + 8.0f).withHeight (30.0f);
    }

    void drawLegalText (juce::Graphics& g, const FrontLayout& L)
    {
        g.setColour (colours::ink);
        g.setFont (serif (10.5f, false, 0.08f));
        g.drawFittedText ("THIS NOTE IS LEGAL TENDER\nFOR ALL SOUNDS, PUBLIC AND PRIVATE", legalTextArea (L).toNearestInt(),
                          juce::Justification::centred, 2);
    }

    juce::Rectangle<float> signatureArea (const FrontLayout& L)
    {
        return juce::Rectangle<float>::leftTopRightBottom (L.leftZone.getX() + 20.0f, L.inner.getBottom() - 70.0f,
                                                           L.ribbon.getX() - 30.0f, L.inner.getBottom() - 36.0f);
    }

    void drawSignature (juce::Graphics& g, const FrontLayout& L)
    {
        using namespace colours;
        const auto sig = signatureArea (L);
        g.setColour (ink);
        g.strokePath (signature (sig, 0xC0FFEE), juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawHorizontalLine (juce::roundToInt (sig.getBottom() + 2.0f), sig.getX() - 10.0f, sig.getRight() + 10.0f);
        drawCentred (g, "Treasurer of the Synth", sig.withY (sig.getBottom() + 4.0f).withHeight (14.0f).expanded (30.0f, 0.0f),
                     serif (11.0f, false, 0.05f), inkSoft);
    }

    void drawTitles (juce::Graphics& g, const FrontLayout& L)
    {
        drawCentred (g, "THE UNITED STATES OF CASHROY", L.title, serif (31.0f, true, 0.06f), colours::ink);
        drawCentred (g, "SYNTHESIZER  RESERVE  NOTE", L.subtitle, serif (12.5f, false, 0.25f), colours::inkSoft);
    }

    void drawMottoAndCaption (juce::Graphics& g, const FrontLayout& L)
    {
        drawCentred (g, "IN SYNTH WE TRUST", L.motto, serif (17.0f, true, 0.12f), colours::ink);
        drawCentred (g, "PRESS THE SEAL FOR A JACKPOT", L.sealCaption, serif (10.0f, false, 0.12f), colours::inkSoft);
    }

    // Soft paper patch so our text reads cleanly over the scan.
    void plate (juce::Graphics& g, juce::Rectangle<float> r, float alpha = 0.8f)
    {
        // Feathered: a few nested rounded rects build up to `alpha` in the middle.
        constexpr int steps = 4;
        for (int i = 0; i < steps; ++i)
        {
            const auto inner = r.reduced (static_cast<float> (i) * 2.0f);
            g.setColour (colours::paper.withAlpha (1.0f - std::pow (1.0f - alpha, 1.0f / steps)));
            g.fillRoundedRectangle (inner, juce::jmin (10.0f, inner.getHeight() * 0.5f));
        }
    }

    //------------------------------------------------------------------ scan
    // Areas of the 1200x505 source scan that are blanked before use:
    // the real serial numbers and the real signatures.
    const juce::Rectangle<int> scanMasks[] = {
        { 58, 102, 270, 42 },  // top-left serial
        { 825, 336, 250, 44 }, // right serial
        { 60, 312, 185, 95 },  // signatures + titles
        { 838, 398, 60, 40 },  // series year
    };

    juce::Image loadScan()
    {
        return juce::ImageCache::getFromMemory (CashAssets::hundred_obverse_gray_jpg, CashAssets::hundred_obverse_gray_jpgSize);
    }
}

bool hasScan()
{
    return loadScan().isValid();
}

juce::Image makeTintedScan()
{
    const auto src = loadScan();
    if (! src.isValid())
        return {};

    // Duotone: black -> faded ink, white -> paper. Never shown in full colour or contrast.
    const auto dark = colours::ink.interpolatedWith (colours::paper, 0.38f);
    const auto light = colours::paper;
    juce::Image out (juce::Image::ARGB, src.getWidth(), src.getHeight(), false);
    {
        const juce::Image::BitmapData in (src, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst (out, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < src.getHeight(); ++y)
            for (int x = 0; x < src.getWidth(); ++x)
            {
                const float l = in.getPixelColour (x, y).getPerceivedBrightness();
                dst.setPixelColour (x, y, dark.interpolatedWith (light, std::pow (l, 0.8f)));
            }
    }

    // Blank each mask with the paper tone round it, then feather the edges.
    const auto toSrc = juce::AffineTransform::scale (static_cast<float> (src.getWidth()) / 1200.0f, static_cast<float> (src.getHeight()) / 505.0f);
    juce::ImageConvolutionKernel blur (9);
    blur.createGaussianBlur (3.0f);
    for (const auto& m : scanMasks)
    {
        const auto r = m.toFloat().transformedBy (toSrc).getSmallestIntegerContainer().getIntersection (out.getBounds());
        const auto ring = r.expanded (3).getIntersection (out.getBounds());
        float red = 0.0f, green = 0.0f, blue = 0.0f;
        int n = 0;
        for (int x = ring.getX(); x < ring.getRight(); ++x)
            for (int y : { ring.getY(), ring.getBottom() - 1 })
            {
                const auto c = out.getPixelAt (x, y);
                red += c.getFloatRed(); green += c.getFloatGreen(); blue += c.getFloatBlue();
                ++n;
            }
        const auto tone = n > 0 ? juce::Colour::fromFloatRGBA (red / static_cast<float> (n), green / static_cast<float> (n), blue / static_cast<float> (n), 1.0f)
                                : light;
        out.clear (r, tone);
        const auto edge = r.expanded (6).getIntersection (out.getBounds());
        blur.applyToImage (out, out, edge);
    }
    return out;
}

void paintFront (juce::Graphics& g, juce::Rectangle<float> b)
{
    const auto scan = makeTintedScan();
    if (! scan.isValid())
    {
        paintFrontPainted (g, b);
        return;
    }

    using namespace colours;
    const FrontLayout L (b);

    juce::Path shape;
    shape.addRoundedRectangle (b, 6.0f);
    g.setColour (paper);
    g.fillPath (shape);
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (shape);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (scan, b, juce::RectanglePlacement::stretchToFit);
    }

    // Paper halo round the portrait so the scope takes the place of the face.
    {
        const auto c = L.portrait.getCentre();
        const auto halo = L.portrait.expanded (L.portrait.getWidth() * 0.32f, L.portrait.getHeight() * 0.14f);
        juce::ColourGradient grad (paper, c.x, c.y, paper.withAlpha (0.0f), halo.getRight(), c.y, true);
        grad.addColour (0.72, paper.withAlpha (0.92f));
        g.setGradientFill (grad);
        g.fillEllipse (halo);
    }
    drawPortraitRing (g, L);

    g.setColour (ink.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);

    plate (g, L.title.getUnion (L.subtitle).expanded (14.0f, 2.0f));
    drawTitles (g, L);
    drawRibbon (g, L);
    plate (g, legalTextArea (L).withSizeKeepingCentre (legalTextArea (L).getWidth() - 20.0f, 34.0f));
    drawLegalText (g, L);
    plate (g, signatureArea (L).expanded (34.0f, 4.0f).withTrimmedBottom (-16.0f), 0.7f);
    drawSignature (g, L);
    plate (g, L.motto.withSizeKeepingCentre (230.0f, L.motto.getHeight()));
    plate (g, L.sealCaption.withSizeKeepingCentre (250.0f, L.sealCaption.getHeight()));
    drawMottoAndCaption (g, L);
}

void paintFrontPainted (juce::Graphics& g, juce::Rectangle<float> b)
{
    using namespace colours;
    const FrontLayout L (b);

    // Paper.
    g.setGradientFill (juce::ColourGradient (paper, b.getCentreX(), b.getCentreY(), paperShade, b.getX(), b.getY(), true));
    g.fillRoundedRectangle (b, 6.0f);
    paintPaperTexture (g, b.reduced (frameInset), ink.withAlpha (0.045f), 3.0f);

    // Faint background guilloche.
    drawRosette (g, L.portrait.getCentre(), 150.0f, 186.0f, 30, 10, inkSoft.withAlpha (0.16f), 0.5f, 0.78f);
    drawRosette (g, L.leftZone.getCentre().translated (0.0f, 10.0f), 46.0f, 86.0f, 20, 9, inkSoft.withAlpha (0.30f), 0.5f);
    drawRosette (g, L.seal.getCentre(), 78.0f, 108.0f, 26, 8, inkSoft.withAlpha (0.24f), 0.5f);

    paintFrame (g, b, frameInset, frameWidth, paperShade, ink.withAlpha (0.75f), ink);

    drawTitles (g, L);

    // Corner numerals.
    for (size_t i = 0; i < L.corners.size(); ++i)
    {
        const auto c = L.corners[i];
        const float r = L.cornerRadius;
        g.setColour (paper);
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
        drawRosette (g, c, r * 0.62f, r, 16, 6, ink.withAlpha (0.7f), 0.55f);
        g.setColour (ink);
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c), 1.2f);
        g.setColour (paper);
        g.fillEllipse (juce::Rectangle<float> (r * 1.25f, r * 1.25f).withCentre (c));
        const auto colour = (i == 3) ? shiftInk : ink;
        drawCentred (g, "100", juce::Rectangle<float> (r * 2.0f, r).withCentre (c), serif (25.0f, true), colour);
    }

    // Ring round the portrait (the component draws the oval and the live scope).
    drawPortraitRing (g, L);

    drawRibbon (g, L);

    // Left zone: legal text, big copper 100, signature.
    {
        drawLegalText (g, L);

        const auto c = L.leftZone.getCentre().translated (0.0f, 10.0f);
        g.setColour (paper.withAlpha (0.85f));
        g.fillEllipse (juce::Rectangle<float> (64.0f, 64.0f).withCentre (c));
        drawCentred (g, "100", juce::Rectangle<float> (180.0f, 70.0f).withCentre (c), serif (56.0f, true), shiftInk);
        const auto series = juce::Rectangle<float> (104.0f, 16.0f).withCentre (c.translated (0.0f, 54.0f));
        g.setColour (paper);
        g.fillRoundedRectangle (series, 8.0f);
        drawCentred (g, "SERIES 2026", series, serif (11.0f, false, 0.2f), inkSoft);

        drawSignature (g, L);
    }

    drawMottoAndCaption (g, L);
}

void paintBack (juce::Graphics& g, juce::Rectangle<float> b)
{
    using namespace colours;
    g.setGradientFill (juce::ColourGradient (panel, b.getCentreX(), b.getCentreY(), background, b.getX(), b.getY(), true));
    g.fillRoundedRectangle (b, 6.0f);
    paintPaperTexture (g, b.reduced (8.0f), gold.withAlpha (0.035f), 3.0f);
    paintFrame (g, b, 6.0f, 12.0f, background, goldDim.withAlpha (0.8f), goldDim);

    const auto titleRow = b.reduced (20.0f, 18.0f).withHeight (24.0f);
    drawCentred (g, "THE UNITED STATES OF CASHROY  -  ADVANCED CONTROLS", titleRow, serif (15.0f, true, 0.12f), gold);
}
}
