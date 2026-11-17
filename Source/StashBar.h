#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class CashSynthAudioProcessor;

namespace cash
{
// Strip above the bill: brand on the left, the Stash (presets) on the right.
//   STASH [ Fat Bass        v ] < >  SAVE  ...
class StashBar final : public juce::Component, private juce::Timer
{
public:
    explicit StashBar (CashSynthAudioProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    void sync();          // current name / modified flag (cheap; the editor polls it)
    void refreshList();   // rescans the stash folder
    void saveAs (const juce::String& name); // no dialogs (the SAVE button asks for a name first)
    void loadIndex (int index);             // index into getFiles()
    void step (int delta);                  // previous / next stash, wrapping

    juce::ComboBox& getCombo() { return combo; }
    juce::TextButton& getSaveButton() { return save; }
    juce::TextButton& getMatchButton() { return match; }
    const juce::Array<juce::File>& getFiles() const { return files; }
    juce::String getStatus() const { return status; }

    std::function<void()> onMoneyMoved, onMatch;

private:
    void timerCallback() override { refreshList(); }
    void askForName();
    void showMenu();
    void afterMove();

    CashSynthAudioProcessor& processor;
    juce::ComboBox combo;
    juce::TextButton prev { "<" }, next { ">" }, save { "SAVE" }, more { "..." }, match { "CASH MATCH" };
    juce::Array<juce::File> files;
    juce::String status;
    juce::Rectangle<int> brandArea, labelArea, statusArea;
};
}
