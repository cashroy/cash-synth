#include "AdvancedPanel.h"
#include "CashLookAndFeel.h"
#include "Parameters.h"

namespace cash
{
namespace
{
    constexpr int sectionGap = 10;
    constexpr int titleHeight = 22;
    constexpr int labelHeight = 16;
    constexpr int textBoxHeight = 18;
}

AdvancedPanel::AdvancedPanel (juce::AudioProcessorValueTreeState& s) : state (s)
{
    using namespace param;
    addSection (0, "OSC 1", { { osc1Wave, "Wave" }, { osc1Octave, "Octave" }, { osc1Semi, "Semi" }, { osc1Fine, "Fine" }, { osc1Level, "Level" } });
    addSection (0, "OSC 2", { { osc2Wave, "Wave" }, { osc2Octave, "Octave" }, { osc2Semi, "Semi" }, { osc2Fine, "Fine" }, { osc2Level, "Level" } });
    addSection (0, "SUB / NOISE", { { subLevel, "Sub" }, { noiseLevel, "Noise" } });
    addSection (0, "MASTER", { { velocitySens, "Velocity" }, { masterGain, "Gain" } });

    addSection (1, "FILTER", { { filterType, "Type" }, { filterCutoff, "Cutoff" }, { filterReso, "Reso" },
                               { filterEnvAmt, "Env Amt" }, { filterKeyTrack, "Key Track" } });
    addSection (1, "FILTER / PITCH ENV", { { fltAttack, "Attack" }, { fltDecay, "Decay" }, { fltSustain, "Sustain" },
                                           { fltRelease, "Release" }, { pitchEnvAmt, "Pitch" } });
    addSection (1, "DRIVE", { { driveType, "Type" }, { driveAmount, "Amount" }, { lfoToDrive, "LFO 1" } });

    addSection (2, "AMP ENV", { { ampAttack, "Attack" }, { ampDecay, "Decay" }, { ampSustain, "Sustain" }, { ampRelease, "Release" } });
    addSection (2, "LFO 1", { { lfoWave, "Wave" }, { lfoRate, "Rate" }, { lfoToPitch, "Pitch" }, { lfoToCutoff, "Cutoff" } });
    addSection (2, "LFO 2", { { lfo2Wave, "Wave" }, { lfo2Rate, "Rate" }, { lfo2Amount, "Amount" }, { lfo2Target, "Target" } });
}

AdvancedPanel::~AdvancedPanel() = default;

void AdvancedPanel::addSection (int row, const juce::String& title, std::initializer_list<std::pair<const char*, const char*>> params)
{
    if (static_cast<int> (rows.size()) <= row)
        rows.resize (static_cast<size_t> (row + 1));

    Section section { title, {}, {} };

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
            c->slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            c->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, textBoxHeight);
            c->slider->setPopupDisplayEnabled (false, false, nullptr);
            c->slider->setTooltip (p->getName (64));
            addAndMakeVisible (*c->slider);
            c->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, id, *c->slider);
            // Double-click resets to the parameter default.
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                c->slider->setDoubleClickReturnValue (true, ranged->convertFrom0to1 (ranged->getDefaultValue()));
        }

        section.controls.push_back (c.get());
        controls.push_back (std::move (c));
    }

    rows[static_cast<size_t> (row)].push_back (std::move (section));
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

            g.setColour (colours::gold);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (s.title, s.bounds.withHeight (titleHeight).reduced (10, 0), juce::Justification::centredLeft);
        }
    }
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds();
    const int numRows = static_cast<int> (rows.size());
    if (numRows == 0)
        return;

    const int rowHeight = (area.getHeight() - sectionGap * (numRows - 1)) / numRows;

    for (auto& row : rows)
    {
        auto rowArea = area.removeFromTop (rowHeight);
        area.removeFromTop (sectionGap);

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

            auto inner = s.bounds.reduced (4, 0);
            inner.removeFromTop (titleHeight);
            inner.removeFromBottom (6);
            const int cw = inner.getWidth() / juce::jmax (1, static_cast<int> (s.controls.size()));

            for (auto* c : s.controls)
            {
                auto cell = inner.removeFromLeft (cw);
                c->label.setBounds (cell.removeFromTop (labelHeight));
                if (c->slider != nullptr)
                {
                    c->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (80, cell.getWidth() - 6), textBoxHeight);
                    c->slider->setBounds (cell);
                }
                else
                    c->combo->setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth() - 6, 100), 26));
            }
        }
    }
}
}
