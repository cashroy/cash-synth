#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace cash
{
// The "advanced" view: one knob or menu per synth parameter, grouped into
// titled sections. Self-contained so the later Coin UI can show/hide it.
class AdvancedPanel final : public juce::Component
{
public:
    explicit AdvancedPanel (juce::AudioProcessorValueTreeState& state);
    ~AdvancedPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int getNumControls() const { return static_cast<int> (controls.size()); }

private:
    struct Control
    {
        juce::String paramId;
        juce::Label label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    };

    struct Section
    {
        juce::String title;
        std::vector<Control*> controls;
        juce::Rectangle<int> bounds;
    };

    void addSection (int row, const juce::String& title, std::initializer_list<std::pair<const char*, const char*>> params);

    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::vector<Section>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};
}
