#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "AdvancedPanel.h"
#include "CashLookAndFeel.h"

class CashSynthAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CashSynthAudioProcessorEditor (CashSynthAudioProcessor&);
    ~CashSynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    cash::AdvancedPanel& getAdvancedPanel() { return advanced; }

private:
    cash::CashLookAndFeel lookAndFeel; // must outlive the child components
    cash::AdvancedPanel advanced;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CashSynthAudioProcessorEditor)
};
