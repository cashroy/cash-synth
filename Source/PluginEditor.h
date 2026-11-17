#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "BillComponents.h"
#include "MoneyTray.h"
#include "StashBar.h"
#include "MatchPanel.h"
#include "CashLookAndFeel.h"

// The $100 Cashroy bill on a desk: the Stash bar above, the money tray underneath.
// Flip the bill to get at every knob.
class CashSynthAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            public juce::FileDragAndDropTarget,
                                            private juce::Timer
{
public:
    explicit CashSynthAudioProcessorEditor (CashSynthAudioProcessor&);
    ~CashSynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void flip();
    bool isFlipped() const { return back.isVisible(); }
    void showMatch (bool show); // the Cash Match cheque over the bill
    bool isShowingMatch() const { return match.isVisible(); }

    // Drop an audio file anywhere: opens Cash Match with it.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return match.isInterestedInFileDrag (files); }
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void syncFromProcessor(); // serials, ledger, vault (the timer calls this too)

    cash::AdvancedPanel& getAdvancedPanel() { return back.getAdvancedPanel(); }
    cash::BillFront& getFront() { return front; }
    cash::BillBack& getBack() { return back; }
    cash::MoneyTray& getTray() { return tray; }
    cash::StashBar& getStashBar() { return stashBar; }
    cash::MatchPanel& getMatchPanel() { return match; }

    static constexpr int width = 1100, height = 724;

private:
    void timerCallback() override { syncFromProcessor(); }

    CashSynthAudioProcessor& processor;
    cash::CashLookAndFeel lookAndFeel; // must outlive the child components
    cash::BillFront front;
    cash::BillBack back;
    cash::MoneyTray tray;
    cash::StashBar stashBar;
    cash::MatchPanel match;
    bool backBeforeMatch = false;
    juce::TooltipWindow tooltips { this, 600 };
    cash::Patch shownPatch {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CashSynthAudioProcessorEditor)
};
