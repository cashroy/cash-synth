#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 50;
    constexpr int margin = 12;
}

CashSynthAudioProcessorEditor::CashSynthAudioProcessorEditor (CashSynthAudioProcessor& p)
    : AudioProcessorEditor (&p), advanced (p.getState())
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (advanced);
    setSize (1100, 640);
}

CashSynthAudioProcessorEditor::~CashSynthAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void CashSynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (cash::colours::background);

    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (margin + 4, 0);
    g.setColour (cash::colours::gold);
    g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    g.drawText ("Cash Synth", header, juce::Justification::centredLeft);

    g.setColour (cash::colours::goldDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("by Cashroy", header, juce::Justification::centredRight);
}

void CashSynthAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (headerHeight);
    advanced.setBounds (area.reduced (margin, 0).withTrimmedBottom (margin));
}
