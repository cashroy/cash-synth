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

    // M8: the small engraved on/off switch in a section title.
    class SectionSwitch final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        SectionSwitch (juce::RangedAudioParameter&, std::function<void (bool)> onChange);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        bool isOn() const { return on; }
        void toggle(); // as if clicked (one host gesture)
        const juce::String paramId;

    private:
        juce::ParameterAttachment attachment;
        std::function<void (bool)> changed;
        bool on = true;
    };

    // Knobs/menus plus the section switches: one control per parameter.
    int getNumControls() const { return static_cast<int> (controls.size() + switches.size()); }
    SectionSwitch* getSwitch (const juce::String& paramId) const;
    float getControlAlpha (const juce::String& paramId) const; // dimmed while its section is off

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
        SectionSwitch* onOff = nullptr;
    };

    void addSection (int row, const juce::String& title, std::initializer_list<std::pair<const char*, const char*>> params,
                     const char* switchId = nullptr);
    void setSectionOn (int row, size_t index, bool on);

    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::vector<Section>> rows;
    std::vector<std::unique_ptr<SectionSwitch>> switches;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};
}
