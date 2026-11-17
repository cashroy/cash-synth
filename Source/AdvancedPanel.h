#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ParamLocks.h"

namespace cash
{
// The "advanced" view: one knob or menu per synth parameter, grouped into
// titled sections. Self-contained so the later Coin UI can show/hide it.
class AdvancedPanel final : public juce::Component
{
public:
    // `locks` = null: no padlocks (a panel without a processor).
    explicit AdvancedPanel (juce::AudioProcessorValueTreeState& state, ParamLocks* locks = nullptr);
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

    // M10: the TIME / SYNC toggle that sits over an LFO rate knob. In SYNC the
    // rate knob gives way to a note-value knob (same place, value box reads "1/4 D").
    class RateModeSwitch final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        RateModeSwitch (juce::RangedAudioParameter&, std::function<void (bool)> onChange);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        bool isSynced() const { return synced; }
        void toggle(); // as if clicked (one host gesture)
        const juce::String paramId;

    private:
        juce::ParameterAttachment attachment;
        std::function<void (bool)> changed;
        bool synced = false;
    };

    // M12: a small padlock in the corner of every knob that money moves can change.
    // Locked = Invest and Jackpot leave it alone (a synced rate locks its TIME/SYNC
    // switch and note value with it).
    class LockToggle final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        LockToggle (ParamLocks&, juce::StringArray ids);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        bool isLocked() const { return locks.isLocked (ids[0]); }
        void toggle();
        void addId (const juce::String& id) { ids.add (id); }

    private:
        ParamLocks& locks;
        juce::StringArray ids;
    };

    // M11: the back of the bill has two pages: the synth (4 rows) and the effects
    // (DELAY, REVERB). Same row height on both, so every knob keeps its size.
    enum Page { synthPage = 0, effectsPage = 1 };
    static constexpr int rowsPerPage = 4;
    void setPage (int page);
    int getPage() const { return page; }
    std::function<void (int)> onPageChange;

    // The engraved SYNTH | EFFECTS plate that turns the page (BillBack places it).
    class PageTabs final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit PageTabs (AdvancedPanel&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void choose (int page);
        bool isEffectsLit() const; // M12: delay or reverb is on (a lit dot on EFFECTS)

    private:
        AdvancedPanel& panel;
    };

    // Knobs/menus, section switches, rate-mode toggles and note knobs: one control per parameter.
    int getNumControls() const;
    SectionSwitch* getSwitch (const juce::String& paramId) const;
    RateModeSwitch* getRateMode (const juce::String& syncParamId) const;
    juce::Slider* getKnob (const juce::String& paramId) const; // rate and note knobs included
    float getControlAlpha (const juce::String& paramId) const; // dimmed while its section is off
    LockToggle* getLock (const juce::String& paramId) const;    // null: not lockable / no locks
    std::function<void()> onEffectsSwitched; // delay or reverb switched on/off (PageTabs repaints)

private:
    struct Control
    {
        juce::String paramId;
        juce::Label label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;

        // M10, LFO rates only: SYNC swaps `slider` (Hz) for `noteSlider`.
        juce::String noteId;
        std::unique_ptr<juce::Slider> noteSlider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noteAttachment;
        std::unique_ptr<RateModeSwitch> rateMode;
        std::unique_ptr<LockToggle> lock; // M12
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
    void paintSignalPath (juce::Graphics&);
    void addRateSync (const char* rateId, const char* syncId, const char* noteId);
    std::unique_ptr<juce::Slider> makeKnob (juce::Component& parent, const char* paramId, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>&);
    juce::Component& pageFor (int row) { return pages[static_cast<size_t> (juce::jlimit (0, 1, row / rowsPerPage))]; }

    juce::AudioProcessorValueTreeState& state;
    ParamLocks* locks;
    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::vector<Section>> rows;
    std::vector<std::unique_ptr<SectionSwitch>> switches;
    std::array<juce::Component, 2> pages; // transparent; the panel paints the section plates
    int page = synthPage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};
}
