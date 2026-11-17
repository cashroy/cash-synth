#include "PluginEditor.h"
#include "CashCode.h"

namespace
{
    constexpr int margin = 12;
    constexpr int billHeight = 470;
    constexpr int stashHeight = 26;
}

CashSynthAudioProcessorEditor::CashSynthAudioProcessorEditor (CashSynthAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), front (p), back (p.getState()), tray (p), stashBar (p), match (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (front);
    addChildComponent (back);
    addAndMakeVisible (tray);
    addAndMakeVisible (stashBar);
    addChildComponent (match);

    front.onFlip = back.onFlip = [this] { flip(); };
    front.onMoneyMoved = tray.onMoneyMoved = stashBar.onMoneyMoved = match.onMoneyMoved = [this] { syncFromProcessor(); };
    stashBar.onMatch = [this] { showMatch (! match.isVisible()); };
    match.onClose = [this] { showMatch (false); };

    shownPatch = processor.getPatch();
    setSize (width, height);
    startTimerHz (15);
}

CashSynthAudioProcessorEditor::~CashSynthAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void CashSynthAudioProcessorEditor::showMatch (bool show)
{
    if (show == match.isVisible())
        return;
    if (show)
    {
        backBeforeMatch = back.isVisible();
        front.setVisible (false);
        back.setVisible (false);
        match.setVisible (true);
    }
    else
    {
        match.setVisible (false);
        back.setVisible (backBeforeMatch);
        front.setVisible (! backBeforeMatch);
    }
}

void CashSynthAudioProcessorEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    showMatch (true);
    match.filesDropped (files, x, y);
}

void CashSynthAudioProcessorEditor::flip()
{
    if (match.isVisible())
        showMatch (false);
    const bool toBack = ! back.isVisible();
    back.setVisible (toBack);
    front.setVisible (! toBack);
}

void CashSynthAudioProcessorEditor::syncFromProcessor()
{
    const auto patch = processor.getPatch();
    if (! cash::samePatch (patch, shownPatch))
    {
        shownPatch = patch;
        front.setSerial (cash::CashCode::serialNumber (patch));
    }
    tray.sync();
    stashBar.sync();
}

void CashSynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Dark desk with a soft pool of light under the bill.
    const auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (cash::colours::background.darker (0.2f), r.getCentreX(), r.getHeight() * 0.35f,
                                             cash::colours::desk, 0.0f, r.getBottom(), true));
    g.fillAll();
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (front.getBounds().toFloat().translated (3.0f, 5.0f), 8.0f);
}

void CashSynthAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    stashBar.setBounds (area.removeFromTop (stashHeight));
    area.removeFromTop (8);
    const auto bill = area.removeFromTop (billHeight);
    front.setBounds (bill);
    back.setBounds (bill);
    match.setBounds (bill);
    area.removeFromTop (margin);
    tray.setBounds (area);
}
