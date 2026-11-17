#include "AdvancedPanel.h"
#include "CashLookAndFeel.h"
#include "Parameters.h"

namespace cash
{
namespace
{
    constexpr int sectionGap = 10;  // between sections in a row
    constexpr int rowGap = 6;       // between rows (four rows since M7)
    constexpr int titleHeight = 18;
    constexpr int labelHeight = 14;
    constexpr int textBoxHeight = 16;
    constexpr float dimAlpha = 0.35f; // a switched-off section's knobs (still editable)
}

//============================================================================== SectionSwitch
AdvancedPanel::SectionSwitch::SectionSwitch (juce::RangedAudioParameter& p, std::function<void (bool)> onChange)
    : paramId (p.paramID),
      attachment (p, [this] (float v)
                  {
                      on = v > 0.5f;
                      repaint();
                      if (changed)
                          changed (on);
                  }),
      changed (std::move (onChange))
{
    setTooltip (p.getName (64));
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setRepaintsOnMouseActivity (true);
    attachment.sendInitialUpdate();
}

void AdvancedPanel::SectionSwitch::toggle()
{
    attachment.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

void AdvancedPanel::SectionSwitch::mouseDown (const juce::MouseEvent&)
{
    toggle();
}

void AdvancedPanel::SectionSwitch::paint (juce::Graphics& g)
{
    using namespace colours;
    // An engraved slot with a gold coin-like thumb: right + lit = on.
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f;

    g.setColour (knobBody);
    g.fillRoundedRectangle (r, radius);
    g.setColour (on ? sealGreen.withAlpha (0.55f) : juce::Colours::black.withAlpha (0.25f));
    g.fillRoundedRectangle (r.reduced (1.5f), radius - 1.5f);
    g.setColour (juce::Colours::black.withAlpha (0.5f)); // engraved: dark top edge, light bottom edge
    g.drawHorizontalLine (juce::roundToInt (r.getY() + 1.0f), r.getX() + radius, r.getRight() - radius);
    g.setColour ((on ? gold : goldDim).withAlpha (isMouseOver() ? 1.0f : 0.85f));
    g.drawRoundedRectangle (r, radius, 1.0f);

    const float d = r.getHeight() - 4.0f;
    const auto thumb = juce::Rectangle<float> (d, d).withCentre ({ on ? r.getRight() - radius : r.getX() + radius, r.getCentreY() });
    g.setGradientFill (juce::ColourGradient (on ? gold.brighter (0.35f) : goldDim.brighter (0.2f), thumb.getX(), thumb.getY(),
                                             on ? gold.darker (0.35f) : goldDim.darker (0.4f), thumb.getRight(), thumb.getBottom(), false));
    g.fillEllipse (thumb);
    g.setColour (knobBody.withAlpha (0.7f));
    g.drawEllipse (thumb, 0.8f);

    const auto label = on ? r.withTrimmedRight (d + 2.0f) : r.withTrimmedLeft (d + 2.0f);
    g.setColour (on ? text : text.withAlpha (0.45f));
    g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
    g.drawText (on ? "ON" : "OFF", label, juce::Justification::centred, false);
}

//============================================================================== RateModeSwitch
AdvancedPanel::RateModeSwitch::RateModeSwitch (juce::RangedAudioParameter& p, std::function<void (bool)> onChange)
    : paramId (p.paramID),
      attachment (p, [this] (float v)
                  {
                      synced = juce::roundToInt (v) == param::syncMode;
                      repaint();
                      if (changed)
                          changed (synced);
                  }),
      changed (std::move (onChange))
{
    setTooltip (p.getName (64) + ": TIME = Hz, SYNC = note value at the host tempo");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setRepaintsOnMouseActivity (true);
    attachment.sendInitialUpdate();
}

void AdvancedPanel::RateModeSwitch::toggle()
{
    attachment.setValueAsCompleteGesture (synced ? static_cast<float> (param::timeMode) : static_cast<float> (param::syncMode));
}

void AdvancedPanel::RateModeSwitch::mouseDown (const juce::MouseEvent&)
{
    toggle();
}

void AdvancedPanel::RateModeSwitch::paint (juce::Graphics& g)
{
    using namespace colours;
    // An engraved two-way plate: the lit half is struck in gold.
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f;
    g.setColour (knobBody);
    g.fillRoundedRectangle (r, radius);

    const auto left = r.withWidth (r.getWidth() * 0.5f), right = left.withX (left.getRight());
    const auto lit = synced ? right : left;
    juce::Path pill;
    pill.addRoundedRectangle (r.reduced (1.5f), radius - 1.5f);
    g.saveState();
    g.reduceClipRegion (pill);
    g.setGradientFill (juce::ColourGradient (gold.brighter (0.25f), lit.getX(), lit.getY(), gold.darker (0.3f), lit.getX(), lit.getBottom(), false));
    g.fillRect (lit);
    g.restoreState();

    g.setColour ((isMouseOver() ? gold : goldDim));
    g.drawRoundedRectangle (r, radius, 1.0f);

    g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
    g.setColour (synced ? text.withAlpha (0.5f) : knobBody);
    g.drawText ("TIME", left, juce::Justification::centred, false);
    g.setColour (synced ? knobBody : text.withAlpha (0.5f));
    g.drawText ("SYNC", right, juce::Justification::centred, false);
}

AdvancedPanel::AdvancedPanel (juce::AudioProcessorValueTreeState& s) : state (s)
{
    using namespace param;
    addSection (0, "OSC 1", { { osc1Wave, "Wave" }, { osc1Octave, "Octave" }, { osc1Semi, "Semi" }, { osc1Fine, "Fine" }, { osc1Level, "Level" } }, osc1On);
    addSection (0, "OSC 2", { { osc2Wave, "Wave" }, { osc2Octave, "Octave" }, { osc2Semi, "Semi" }, { osc2Fine, "Fine" }, { osc2Level, "Level" } }, osc2On);
    addSection (0, "SUB / NOISE", { { subLevel, "Sub" }, { noiseLevel, "Noise" } }, subNoiseOn);
    addSection (0, "MASTER", { { velocitySens, "Velocity" }, { masterGain, "Gain" } });

    addSection (1, "FILTER", { { filterType, "Type" }, { filterCutoff, "Cutoff" }, { filterReso, "Reso" },
                               { filterEnvAmt, "Env Amt" }, { filterKeyTrack, "Key Track" } }, filterOn);
    addSection (1, "FILTER / PITCH ENV", { { fltAttack, "Attack" }, { fltDecay, "Decay" }, { fltSustain, "Sustain" },
                                           { fltRelease, "Release" }, { pitchEnvAmt, "Pitch" } }, filterEnvOn);
    addSection (1, "DRIVE", { { driveType, "Type" }, { driveAmount, "Amount" }, { lfoToDrive, "LFO 1" } }, driveOn);

    addSection (2, "AMP ENV", { { ampAttack, "Attack" }, { ampDecay, "Decay" }, { ampSustain, "Sustain" }, { ampRelease, "Release" } });
    addSection (2, "LFO 1", { { lfoWave, "Wave" }, { lfoRate, "Rate" }, { lfoToPitch, "Pitch" }, { lfoToCutoff, "Cutoff" } }, lfo1On);
    addSection (2, "LFO 2", { { lfo2Wave, "Wave" }, { lfo2Rate, "Rate" }, { lfo2Amount, "Amount" }, { lfo2Target, "Target" } }, lfo2On);

    // M8: unison got its own section (and switch); same knobs, same row.
    addSection (3, "VOICE", { { voiceMode, "Mode" }, { notePriority, "Priority" } });
    addSection (3, "UNISON", { { unisonVoices, "Voices" }, { unisonDetune, "Detune" }, { unisonSpread, "Spread" } }, unisonOn);
    addSection (3, "GLIDE", { { glideTime, "Time" }, { glideMode, "When" }, { glideType, "Type" } }, glideOn);
    addSection (3, "PLAY", { { bendRange, "Bend" }, { modVibrato, "Mod Vib" }, { velToCutoff, "Vel > Cut" } });

    // M10: TIME / SYNC over each LFO rate.
    addRateSync (lfoRate, lfo1Sync, lfo1Note);
    addRateSync (lfo2Rate, lfo2Sync, lfo2Note);
}

std::unique_ptr<juce::Slider> AdvancedPanel::makeKnob (const char* id, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
{
    auto* p = state.getParameter (id);
    jassert (p != nullptr);
    auto slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, textBoxHeight);
    slider->setPopupDisplayEnabled (false, false, nullptr);
    // Set here, not only in the look-and-feel: a value box whose size never changes
    // in resized() is never rebuilt, and would keep the default LAF's outline.
    slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    slider->setTooltip (p->getName (64));
    addAndMakeVisible (*slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, id, *slider);
    // Double-click resets to the parameter default.
    slider->setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    return slider;
}

void AdvancedPanel::addRateSync (const char* rateId, const char* syncId, const char* noteId)
{
    auto it = std::find_if (controls.begin(), controls.end(), [rateId] (const auto& c) { return c->paramId == rateId; });
    jassert (it != controls.end() && (*it)->slider != nullptr);
    auto* c = it->get();

    c->noteId = noteId;
    c->noteSlider = makeKnob (noteId, c->noteAttachment);
    c->noteSlider->setAlpha (c->slider->getAlpha());
    c->label.setVisible (false); // the toggle takes the label's place

    auto* p = state.getParameter (syncId);
    jassert (p != nullptr);
    c->rateMode = std::make_unique<RateModeSwitch> (*p, [c] (bool synced)
    {
        c->slider->setVisible (! synced);
        c->noteSlider->setVisible (synced);
    });
    addAndMakeVisible (*c->rateMode);
}

int AdvancedPanel::getNumControls() const
{
    int n = static_cast<int> (controls.size() + switches.size());
    for (const auto& c : controls)
        n += (c->noteSlider != nullptr ? 1 : 0) + (c->rateMode != nullptr ? 1 : 0);
    return n;
}

AdvancedPanel::RateModeSwitch* AdvancedPanel::getRateMode (const juce::String& syncParamId) const
{
    for (const auto& c : controls)
        if (c->rateMode != nullptr && c->rateMode->paramId == syncParamId)
            return c->rateMode.get();
    return nullptr;
}

juce::Slider* AdvancedPanel::getKnob (const juce::String& paramId) const
{
    for (const auto& c : controls)
    {
        if (c->paramId == paramId)
            return c->slider.get();
        if (c->noteId == paramId)
            return c->noteSlider.get();
    }
    return nullptr;
}

AdvancedPanel::~AdvancedPanel() = default;

void AdvancedPanel::addSection (int row, const juce::String& title, std::initializer_list<std::pair<const char*, const char*>> params,
                                const char* switchId)
{
    if (static_cast<int> (rows.size()) <= row)
        rows.resize (static_cast<size_t> (row + 1));

    Section section { title, {}, {}, nullptr };

    for (const auto& [id, name] : params)
    {
        auto* p = state.getParameter (id);
        jassert (p != nullptr);

        auto c = std::make_unique<Control>();
        c->paramId = id;
        c->label.setText (name, juce::dontSendNotification);
        c->label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (c->label);

        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            c->combo = std::make_unique<juce::ComboBox>();
            c->combo->addItemList (choice->choices, 1);
            c->combo->setTooltip (p->getName (64));
            addAndMakeVisible (*c->combo);
            c->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, id, *c->combo);
        }
        else
        {
            c->slider = makeKnob (id, c->sliderAttachment);
        }

        section.controls.push_back (c.get());
        controls.push_back (std::move (c));
    }

    auto& rowSections = rows[static_cast<size_t> (row)];
    const size_t index = rowSections.size();
    rowSections.push_back (std::move (section));

    if (switchId != nullptr)
    {
        auto* p = state.getParameter (switchId);
        jassert (p != nullptr);
        // Created after the section is stored: the attachment's first callback dims it.
        auto sw = std::make_unique<SectionSwitch> (*p, [this, row, index] (bool on) { setSectionOn (row, index, on); });
        addAndMakeVisible (*sw);
        rowSections[index].onOff = sw.get();
        setSectionOn (row, index, sw->isOn());
        switches.push_back (std::move (sw));
    }
}

void AdvancedPanel::setSectionOn (int row, size_t index, bool on)
{
    auto& section = rows[static_cast<size_t> (row)][index];
    for (auto* c : section.controls)
    {
        const float a = on ? 1.0f : dimAlpha;
        c->label.setAlpha (a);
        if (c->slider != nullptr) c->slider->setAlpha (a);
        if (c->combo != nullptr)  c->combo->setAlpha (a);
        if (c->noteSlider != nullptr) c->noteSlider->setAlpha (a);
        if (c->rateMode != nullptr)   c->rateMode->setAlpha (on ? 1.0f : 0.6f);
    }
    repaint (section.bounds);
}

AdvancedPanel::SectionSwitch* AdvancedPanel::getSwitch (const juce::String& paramId) const
{
    for (const auto& sw : switches)
        if (sw->paramId == paramId)
            return sw.get();
    return nullptr;
}

float AdvancedPanel::getControlAlpha (const juce::String& paramId) const
{
    for (const auto& c : controls)
        if (c->paramId == paramId)
            return c->slider != nullptr ? c->slider->getAlpha() : c->combo->getAlpha();
    return 1.0f;
}

void AdvancedPanel::paint (juce::Graphics& g)
{
    for (const auto& row : rows)
    {
        for (const auto& s : row)
        {
            const auto r = s.bounds.toFloat();
            g.setColour (colours::panel);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (colours::panelEdge);
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

            const bool on = s.onOff == nullptr || s.onOff->isOn();
            g.setColour (on ? colours::gold : colours::goldDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (s.title, s.bounds.withHeight (titleHeight + 2).reduced (10, 0), juce::Justification::centredLeft);
        }
    }
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds();
    const int numRows = static_cast<int> (rows.size());
    if (numRows == 0)
        return;

    const int rowHeight = (area.getHeight() - rowGap * (numRows - 1)) / numRows;

    for (auto& row : rows)
    {
        auto rowArea = area.removeFromTop (rowHeight);
        area.removeFromTop (rowGap);

        int units = 0;
        for (const auto& s : row)
            units += static_cast<int> (s.controls.size());

        const int gaps = sectionGap * (static_cast<int> (row.size()) - 1);
        const float unitWidth = static_cast<float> (rowArea.getWidth() - gaps) / static_cast<float> (juce::jmax (1, units));

        for (size_t si = 0; si < row.size(); ++si)
        {
            auto& s = row[si];
            const bool last = si + 1 == row.size();
            const int w = last ? rowArea.getWidth() : juce::roundToInt (unitWidth * static_cast<float> (s.controls.size()));
            s.bounds = rowArea.removeFromLeft (w);
            rowArea.removeFromLeft (sectionGap);

            if (s.onOff != nullptr)
                s.onOff->setBounds (s.bounds.getRight() - 10 - 34, s.bounds.getY() + 4, 34, 13);

            auto inner = s.bounds.reduced (4, 0);
            inner.removeFromTop (titleHeight);
            inner.removeFromBottom (2);
            const int cw = inner.getWidth() / juce::jmax (1, static_cast<int> (s.controls.size()));

            for (auto* c : s.controls)
            {
                auto cell = inner.removeFromLeft (cw);
                const auto labelArea = cell.removeFromTop (labelHeight);
                c->label.setBounds (labelArea);
                if (c->rateMode != nullptr)
                    c->rateMode->setBounds (labelArea.withSizeKeepingCentre (juce::jmin (62, labelArea.getWidth() - 4), 13));
                for (auto* knob : { c->slider.get(), c->noteSlider.get() })
                    if (knob != nullptr)
                    {
                        knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (80, cell.getWidth() - 6), textBoxHeight);
                        knob->setBounds (cell);
                    }
                if (c->combo != nullptr)
                    c->combo->setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth() - 6, 100), 26));
            }
        }
    }
}
}
