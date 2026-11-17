#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

class CashSynthAudioProcessor;

namespace cash
{
// Cash Match, drawn as a bank cheque laid over the bill:
//   drop zone (sample waveform, best match on top) | match % box + stamp
//   LOAD SAMPLE  HEAR SAMPLE  MATCH IT  HEAR MATCH  DEPOSIT
class MatchPanel final : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit MatchPanel (CashSynthAudioProcessor&);
    ~MatchPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void visibilityChanged() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

    static bool isAudioFile (const juce::String& path);

    void load (const juce::File&);  // analyse and start matching straight away
    void startMatching();
    void sync();                    // pull progress from the matcher (the timer calls this)

    juce::String getStatus() const { return status; }
    juce::TextButton& getMatchButton() { return matchIt; }
    juce::TextButton& getDepositButton() { return deposit; }
    juce::TextButton& getCloseButton() { return close; }

    std::function<void()> onClose, onMoneyMoved;

private:
    void timerCallback() override { sync(); }
    void browse();
    void updateButtons();
    void paintWave (juce::Graphics&, juce::Rectangle<float>, const juce::AudioBuffer<float>&, juce::Colour, bool filled) const;

    CashSynthAudioProcessor& processor;
    juce::TextButton load_ { "LOAD SAMPLE..." }, hearSample { "HEAR SAMPLE" }, matchIt { "MATCH IT" },
                     hearMatch { "HEAR MATCH" }, deposit { "DEPOSIT" }, close { "CLOSE" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::AudioBuffer<float> matchWave;
    juce::String status;
    int shownTried = -1, percent = 0;
    float progress = 0.0f;
    bool searching = false, found = false, dragOver = false;
    uint64_t runs = 0;
    juce::Rectangle<int> dropArea, amountArea, infoArea, band;
};
}
