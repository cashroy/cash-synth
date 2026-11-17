#include "MoneyTray.h"
#include "BillArt.h"
#include "CashCode.h"
#include "CashLookAndFeel.h"
#include "PluginProcessor.h"

namespace cash
{
namespace
{
    constexpr int titleHeight = 22;
    constexpr int gap = 10;
    const juce::String cent = juce::String::charToString (static_cast<juce::juce_wchar> (0x00A2));

    juce::Colour riskColour (float r)
    {
        return r < 0.5f ? colours::ledGreen.interpolatedWith (colours::ledAmber, r * 2.0f)
                        : colours::ledAmber.interpolatedWith (colours::ledRed, (r - 0.5f) * 2.0f);
    }

    void paintMiniBill (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& serial, bool glow)
    {
        using namespace colours;
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 2.0f);
        g.setColour (glow ? paper.brighter (0.05f) : paper.darker (0.04f));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (ink.withAlpha (0.75f));
        g.drawRoundedRectangle (r.reduced (2.5f), 1.5f, 0.8f);
        g.setColour (inkSoft.withAlpha (0.5f));
        g.drawEllipse (juce::Rectangle<float> (r.getHeight() * 0.62f, r.getHeight() * 0.7f).withCentre (r.getCentre()), 0.8f);

        g.setColour (ink);
        g.setFont (art::serif (juce::jmax (7.0f, r.getHeight() * 0.24f), true));
        const auto corner = r.reduced (4.0f, 3.0f);
        g.drawText ("100", corner, juce::Justification::topLeft, false);
        g.drawText ("100", corner, juce::Justification::bottomRight, false);
        if (serial.isNotEmpty())
        {
            g.setColour (serialInk);
            g.setFont (art::mono (juce::jmax (7.0f, r.getHeight() * 0.22f), true));
            g.drawFittedText (serial, r.reduced (r.getHeight() * 0.55f, 0.0f).withTrimmedTop (r.getHeight() * 0.3f).toNearestInt(),
                              juce::Justification::centred, 1, 0.6f);
        }
    }
}

//============================================================================== CoinButton
CoinButton::CoinButton (const juce::String& d, float a) : juce::Button ("Invest " + d), denomination (d), amount (a)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Invest " + d + ": nudge " + juce::String (juce::roundToInt (a * 100.0f)) + "% of the sound at the current risk (undoable)");
}

bool CoinButton::hitTest (int x, int y)
{
    const float r = static_cast<float> (getWidth()) * 0.5f;
    return juce::Point<float> (r, r).getDistanceFrom ({ static_cast<float> (x), static_cast<float> (y) }) <= r
        || y > getWidth(); // caption
}

void CoinButton::paintButton (juce::Graphics& g, bool isOver, bool isDown)
{
    const float w = static_cast<float> (getWidth());
    float r = w * 0.5f - 2.0f;
    if (isDown)
        r -= 2.0f;
    art::drawCoin (g, { w * 0.5f, w * 0.5f }, r, denomination, isOver);

    g.setColour (isOver ? colours::gold : colours::goldDim);
    g.setFont (art::serif (12.0f, true, 0.1f));
    g.drawText (juce::String (juce::roundToInt (amount * 100.0f)) + "%",
                getLocalBounds().withTrimmedTop (getWidth()), juce::Justification::centred, false);
}

//============================================================================== RiskDial
RiskDial::RiskDial() : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox)
{
    setRange (0.0, 1.0, 0.01);
    // 9 o'clock to 3 o'clock over the top. JUCE wants both angles in [0, 4pi), so use 1.5pi..2.5pi
    // rather than -pi/2..pi/2 (the negative start tripped the jassert in Slider::setRotaryParameters).
    setRotaryParameters (1.5f * juce::MathConstants<float>::pi, 2.5f * juce::MathConstants<float>::pi, true);
    setDoubleClickReturnValue (true, 0.5);
    setValue (0.5, juce::dontSendNotification);
    setTooltip ("Risk: how wild each investment is. High risk swaps waves and filter types.");
    setMouseCursor (juce::MouseCursor::UpDownLeftRightResizeCursor);
}

juce::String RiskDial::rating (float r)
{
    if (r < 0.2f) return "T-BILLS";
    if (r < 0.45f) return "BLUE CHIPS";
    if (r < 0.7f) return "GROWTH";
    if (r < 0.9f) return "PENNY STOCKS";
    return "CRYPTO";
}

void RiskDial::paint (juce::Graphics& g)
{
    using namespace colours;
    const float v = static_cast<float> (getValue());
    auto area = getLocalBounds().toFloat();
    auto led = area.removeFromBottom (30.0f).reduced (8.0f, 2.0f);

    const float radius = juce::jmin (area.getWidth() * 0.5f - 18.0f, area.getHeight() - 16.0f);
    const juce::Point<float> c (area.getCentreX(), area.getBottom() - 6.0f);

    // Coloured gauge.
    const int segs = 36;
    for (int i = 0; i < segs; ++i)
    {
        const float t0 = static_cast<float> (i) / segs, t1 = static_cast<float> (i + 1) / segs;
        juce::Path seg;
        seg.addCentredArc (c.x, c.y, radius, radius, 0.0f, -juce::MathConstants<float>::halfPi + t0 * juce::MathConstants<float>::pi,
                           -juce::MathConstants<float>::halfPi + t1 * juce::MathConstants<float>::pi + 0.01f, true);
        g.setColour (riskColour (t0).withAlpha (t0 <= v ? 1.0f : 0.25f));
        g.strokePath (seg, juce::PathStrokeType (8.0f));
    }
    g.setColour (goldDim);
    for (int i = 0; i <= 10; ++i)
    {
        const float a = -juce::MathConstants<float>::halfPi + juce::MathConstants<float>::pi * static_cast<float> (i) / 10.0f;
        g.drawLine ({ c.getPointOnCircumference (radius - 13.0f, a), c.getPointOnCircumference (radius - (i % 5 == 0 ? 22.0f : 17.0f), a) }, 1.0f);
    }
    g.setFont (art::serif (10.0f, true, 0.1f));
    g.setColour (ledGreen.withAlpha (0.85f));
    g.drawText ("SAFE", juce::Rectangle<float> (c.x - radius + 10.0f, c.y - 14.0f, 40.0f, 12.0f), juce::Justification::centredLeft, false);
    g.setColour (ledRed.withAlpha (0.85f));
    g.drawText ("YOLO", juce::Rectangle<float> (c.x + radius - 50.0f, c.y - 14.0f, 40.0f, 12.0f), juce::Justification::centredRight, false);

    // Needle and hub.
    const float angle = -juce::MathConstants<float>::halfPi + v * juce::MathConstants<float>::pi;
    g.setColour (gold);
    g.drawLine ({ c, c.getPointOnCircumference (radius - 6.0f, angle) }, 2.5f);
    art::drawCoin (g, c, 9.0f, {}, isMouseOverOrDragging());

    // Ticker readout.
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (led, 4.0f);
    g.setColour (goldDim.withAlpha (0.6f));
    g.drawRoundedRectangle (led, 4.0f, 1.0f);
    const auto col = riskColour (v);
    juce::Path tri;
    const auto tc = juce::Point<float> (led.getX() + 14.0f, led.getCentreY());
    tri.addTriangle (tc.translated (-5.0f, 4.0f), tc.translated (5.0f, 4.0f), tc.translated (0.0f, -5.0f));
    g.setColour (col);
    g.fillPath (tri);
    g.setFont (art::mono (14.0f, true));
    g.drawText (juce::String (juce::roundToInt (v * 100.0f)) + "%  " + rating (v), led.withTrimmedLeft (26.0f), juce::Justification::centredLeft, false);
}

//============================================================================== BankItButton
BankItButton::BankItButton() : juce::Button ("Bank It")
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Bank It: keep this sound in the vault");
}

void BankItButton::setState (bool full, bool banked)
{
    if (full != vaultFull || banked != alreadyBanked)
    {
        vaultFull = full;
        alreadyBanked = banked;
        setTooltip (banked ? "This sound is already in the vault"
                           : full ? "The vault is full: right-click a note to shred it"
                                  : "Bank It: keep this sound in the vault");
        repaint();
    }
}

void BankItButton::paintButton (juce::Graphics& g, bool isOver, bool isDown)
{
    using namespace colours;
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    if (isDown)
        r = r.reduced (2.0f);

    auto labelArea = r.removeFromBottom (18.0f);

    // A small stack of bills in a currency strap.
    const float bh = juce::jmin (r.getHeight() * 0.62f, r.getWidth() * 0.45f);
    auto bill = juce::Rectangle<float> (r.getWidth() - 6.0f, bh).withCentre (r.getCentre());
    for (int i = 3; i >= 0; --i)
        paintMiniBill (g, bill.translated (static_cast<float> (i) * 2.0f, static_cast<float> (i) * -3.0f + 4.0f), {}, i == 0 && isOver);

    const auto strap = bill.translated (0.0f, 4.0f).withSizeKeepingCentre (bill.getWidth() * 0.3f, bill.getHeight() + 6.0f);
    const auto strapColour = vaultFull && ! alreadyBanked ? ledRed.darker (0.3f) : alreadyBanked ? sealGreen : shiftInk;
    g.setColour (isOver ? strapColour.brighter (0.2f) : strapColour);
    g.fillRect (strap);
    g.setColour (paper);
    g.drawRect (strap.reduced (1.5f, 0.0f), 0.6f);
    g.setFont (art::serif (strap.getWidth() * 0.7f, true));
    g.drawText ("$", strap, juce::Justification::centred, false);

    g.setColour (isOver ? gold.brighter (0.2f) : (alreadyBanked ? ledGreen : vaultFull ? ledRed : gold));
    g.setFont (art::serif (13.0f, true, 0.12f));
    g.drawText (alreadyBanked ? "BANKED" : vaultFull ? "VAULT FULL" : "BANK IT", labelArea, juce::Justification::centred, false);
}

//============================================================================== VaultSlot
VaultSlot::VaultSlot (int i) : index (i) {}

void VaultSlot::setNote (const BankedNote* note, bool isCurrent)
{
    const auto s = note != nullptr ? note->serial : juce::String();
    if (s != serial || isCurrent != current)
    {
        serial = s;
        current = isCurrent;
        setTooltip (s.isEmpty() ? juce::String ("Empty slot: press Bank It")
                                : s + "\nClick to withdraw. Right-click to copy its CashCode or shred it.");
        setMouseCursor (s.isEmpty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void VaultSlot::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (2.0f);
    if (isEmpty())
    {
        juce::Path outline;
        outline.addRoundedRectangle (r.reduced (1.0f), 3.0f);
        juce::Path dashed;
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (colours::panelEdge);
        g.fillPath (dashed);
        g.setFont (art::serif (9.0f, false, 0.2f));
        g.drawText ("EMPTY", r, juce::Justification::centred, false);
        return;
    }
    paintMiniBill (g, r, serial, isMouseOver());
    if (current)
    {
        g.setColour (colours::gold);
        g.drawRoundedRectangle (r.expanded (1.0f), 3.0f, 2.0f);
    }
}

void VaultSlot::mouseUp (const juce::MouseEvent& e)
{
    if (isEmpty() || ! e.mouseWasClicked())
        return;

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Withdraw " + serial);
        menu.addItem (2, "Copy CashCode");
        menu.addSeparator();
        menu.addItem (3, "Shred");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer<VaultSlot> (this)] (int choice)
                            {
                                if (safe == nullptr) return;
                                auto& s = *safe;
                                if (choice == 1 && s.onWithdraw) s.onWithdraw (s.index);
                                if (choice == 2 && s.onCopy) s.onCopy (s.index);
                                if (choice == 3 && s.onShred) s.onShred (s.index);
                            });
    }
    else if (onWithdraw)
    {
        onWithdraw (index);
    }
}

//============================================================================== MoneyTray
MoneyTray::MoneyTray (CashSynthAudioProcessor& p) : processor (p)
{
    const std::array<std::pair<juce::String, float>, numCoins> denominations { {
        { "10" + cent, 0.10f }, { "25" + cent, 0.25f }, { "50" + cent, 0.50f }, { "$1", 1.0f } } };

    for (size_t i = 0; i < coins.size(); ++i)
    {
        coins[i] = std::make_unique<CoinButton> (denominations[i].first, denominations[i].second);
        auto* coin = coins[i].get();
        coin->onClick = [this, coin]
        {
            processor.invest ({ coin->getAmount(), processor.getRisk(), static_cast<uint64_t> (juce::Random::getSystemRandom().nextInt64()) });
            afterMove();
        };
        addAndMakeVisible (*coin);
    }

    risk.setValue (processor.getRisk(), juce::dontSendNotification);
    risk.onValueChange = [this] { processor.setRisk (static_cast<float> (risk.getValue())); };
    addAndMakeVisible (risk);

    undo.setTooltip ("Undo the last transaction");
    redo.setTooltip ("Redo");
    undo.onClick = [this] { processor.undo(); afterMove(); };
    redo.onClick = [this] { processor.redo(); afterMove(); };
    addAndMakeVisible (undo);
    addAndMakeVisible (redo);

    bankIt.onClick = [this] { processor.bankIt(); afterMove(); };
    addAndMakeVisible (bankIt);

    for (int i = 0; i < static_cast<int> (processor.getVault().getCapacity()); ++i)
    {
        auto slot = std::make_unique<VaultSlot> (i);
        slot->onWithdraw = [this] (int idx) { processor.withdraw (static_cast<size_t> (idx)); afterMove(); };
        slot->onShred = [this] (int idx) { processor.shred (static_cast<size_t> (idx)); afterMove(); };
        slot->onCopy = [this] (int idx)
        {
            if (static_cast<size_t> (idx) < processor.getVault().size())
                juce::SystemClipboard::copyTextToClipboard (CashCode::encode (processor.getVault().at (static_cast<size_t> (idx)).patch));
        };
        addAndMakeVisible (*slot);
        slots.push_back (std::move (slot));
    }

    sync();
}

void MoneyTray::afterMove()
{
    sync();
    if (onMoneyMoved)
        onMoneyMoved();
}

void MoneyTray::sync()
{
    undo.setEnabled (processor.canUndo());
    redo.setEnabled (processor.canRedo());

    const auto& ledger = processor.getLedger();
    const auto patch = processor.getPatch();
    juce::String newMemo, newEntry;
    if (const auto* t = ledger.current())
    {
        const bool tweaked = ! samePatch (patch, t->patch);
        newMemo = tweaked ? juce::String ("Hand tweak (unrecorded)") : t->memo;
        newEntry = "Entry " + juce::String (static_cast<int> (ledger.getPosition()) + 1) + " of " + juce::String (static_cast<int> (ledger.size()));
    }

    const auto& vault = processor.getVault();
    const int currentIndex = vault.find (patch);
    for (size_t i = 0; i < slots.size(); ++i)
        slots[i]->setNote (i < vault.size() ? &vault.at (i) : nullptr, static_cast<int> (i) == currentIndex);
    bankIt.setState (vault.isFull(), currentIndex >= 0);
    const auto newCount = juce::String (static_cast<int> (vault.size())) + " / " + juce::String (static_cast<int> (vault.getCapacity()));

    if (std::abs (risk.getValue() - processor.getRisk()) > 1.0e-4 && ! risk.isMouseButtonDown())
        risk.setValue (processor.getRisk(), juce::dontSendNotification);

    if (newMemo != memo || newEntry != entry || newCount != vaultCount)
    {
        memo = newMemo;
        entry = newEntry;
        vaultCount = newCount;
        repaint();
    }
}

void MoneyTray::paint (juce::Graphics& g)
{
    using namespace colours;
    for (const auto& s : sections)
    {
        const auto r = s.bounds.toFloat();
        g.setColour (panel.darker (0.35f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        g.setColour (gold);
        g.setFont (art::serif (13.0f, true, 0.18f));
        g.drawText (s.title, s.bounds.withHeight (titleHeight).reduced (12, 0), juce::Justification::centredLeft);
    }

    g.setColour (text);
    g.setFont (art::serif (12.0f, false));
    auto memoBox = memoArea;
    const auto entryBox = memoBox.removeFromBottom (14);
    g.drawFittedText (memo, memoBox, juce::Justification::centred, 2);
    g.setColour (goldDim);
    g.setFont (art::serif (10.5f, false, 0.1f));
    g.drawText (entry, entryBox, juce::Justification::centred);

    g.drawText (vaultCount + " NOTES", vaultCountArea, juce::Justification::centred);
}

void MoneyTray::resized()
{
    auto area = getLocalBounds();
    const std::array<int, 4> widths { 372, 230, 160, 0 };
    for (size_t i = 0; i < sections.size(); ++i)
    {
        sections[i].bounds = widths[i] > 0 ? area.removeFromLeft (widths[i]) : area;
        area.removeFromLeft (gap);
    }

    // Coins, sizes rising with the amount, sat on a common baseline.
    {
        auto inner = sections[0].bounds.reduced (12, 0).withTrimmedTop (titleHeight).withTrimmedBottom (8);
        const std::array<int, numCoins> sizes { 62, 72, 84, 96 };
        int total = 0;
        for (auto s : sizes) total += s;
        const int spacing = (inner.getWidth() - total) / (numCoins + 1);
        int x = inner.getX() + spacing;
        for (size_t i = 0; i < coins.size(); ++i)
        {
            const int h = sizes[i] + 18;
            coins[i]->setBounds (x, inner.getCentreY() + (sizes.back() + 18) / 2 - h, sizes[i], h);
            x += sizes[i] + spacing;
        }
    }

    risk.setBounds (sections[1].bounds.withTrimmedTop (titleHeight).reduced (4, 4));

    {
        auto inner = sections[2].bounds.withTrimmedTop (titleHeight).reduced (12, 6);
        auto buttons = inner.removeFromTop (30);
        undo.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 3));
        buttons.removeFromLeft (6);
        redo.setBounds (buttons);
        inner.removeFromTop (6);
        memoArea = inner;
    }

    {
        auto inner = sections[3].bounds.withTrimmedTop (titleHeight).reduced (10, 6);
        auto left = inner.removeFromLeft (96);
        vaultCountArea = left.removeFromBottom (16);
        bankIt.setBounds (left);
        inner.removeFromLeft (8);
        const int cols = 2, rows = (static_cast<int> (slots.size()) + cols - 1) / cols;
        const int cw = inner.getWidth() / cols, rh = inner.getHeight() / juce::jmax (1, rows);
        for (int i = 0; i < static_cast<int> (slots.size()); ++i)
            slots[static_cast<size_t> (i)]->setBounds (inner.getX() + (i % cols) * cw, inner.getY() + (i / cols) * rh, cw, rh);
    }
}
}
