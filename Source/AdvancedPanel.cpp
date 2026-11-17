#include "AdvancedPanel.h"
#include "CashLookAndFeel.h"
#include "Parameters.h"
#include "ParameterMap.h"

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
    setTooltip (p.getName (64) + ": TIME = set by hand, SYNC = note value at the host tempo");
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

//============================================================================== LockToggle
AdvancedPanel::LockToggle::LockToggle (ParamLocks& l, juce::StringArray paramIds) : locks (l), ids (std::move (paramIds))
{
    setTooltip ("Lock: Invest and Jackpot leave this knob alone");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setRepaintsOnMouseActivity (true);
}

void AdvancedPanel::LockToggle::toggle()
{
    const bool lock = ! isLocked();
    for (const auto& id : ids)
        locks.setLocked (id, lock);
}

void AdvancedPanel::LockToggle::mouseDown (const juce::MouseEvent&)
{
    toggle();
}

void AdvancedPanel::LockToggle::paint (juce::Graphics& g)
{
    using namespace colours;
    // A tiny padlock: struck in gold when locked, a faint open outline when not.
    const bool locked = isLocked();
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const auto body = r.withTrimmedTop (r.getHeight() * 0.45f);
    const float sw = body.getWidth() * 0.62f;
    const auto shackle = juce::Rectangle<float> (sw, r.getHeight() * 0.62f)
                             .withCentre ({ body.getCentreX(), r.getY() + r.getHeight() * 0.31f })
                             .translated (0.0f, locked ? 0.0f : -1.5f);
    juce::Path loop;
    loop.startNewSubPath (shackle.getX(), body.getY());
    loop.lineTo (shackle.getX(), shackle.getY() + sw * 0.5f);
    loop.addCentredArc (shackle.getCentreX(), shackle.getY() + sw * 0.5f, sw * 0.5f, sw * 0.5f, 0.0f,
                        -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi);
    loop.lineTo (shackle.getRight(), locked ? body.getY() : body.getY() - 2.5f);

    const auto colour = locked ? gold : goldDim.withAlpha (isMouseOver() ? 0.9f : 0.45f);
    g.setColour (colour);
    g.strokePath (loop, juce::PathStrokeType (1.3f));
    if (locked)
    {
        g.fillRoundedRectangle (body, 1.5f);
        g.setColour (knobBody);
        g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (body.getCentre()));
    }
    else
    {
        g.drawRoundedRectangle (body.reduced (0.5f), 1.5f, 1.0f);
    }
}

//============================================================================== PageTabs
AdvancedPanel::PageTabs::PageTabs (AdvancedPanel& p) : panel (p)
{
    panel.onEffectsSwitched = [this] { repaint(); };
    setTooltip ("Turn the page: synth controls / effects (delay, reverb) and the Mint");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setRepaintsOnMouseActivity (true);
}

void AdvancedPanel::PageTabs::choose (int newPage)
{
    panel.setPage (newPage);
    repaint();
}

bool AdvancedPanel::PageTabs::isEffectsLit() const
{
    for (const auto* id : { param::delayOn, param::reverbOn })
        if (const auto* v = panel.state.getRawParameterValue (id); v != nullptr && v->load() > 0.5f)
            return true;
    return false;
}

void AdvancedPanel::PageTabs::mouseDown (const juce::MouseEvent& e)
{
    choose (e.position.x < static_cast<float> (getWidth()) * 0.5f ? synthPage : effectsPage);
}

void AdvancedPanel::PageTabs::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f;
    g.setColour (knobBody);
    g.fillRoundedRectangle (r, radius);

    const auto left = r.withWidth (r.getWidth() * 0.5f), right = left.withX (left.getRight());
    const bool fx = panel.getPage() == effectsPage;
    const auto lit = fx ? right : left;
    juce::Path pill;
    pill.addRoundedRectangle (r.reduced (1.5f), radius - 1.5f);
    g.saveState();
    g.reduceClipRegion (pill);
    g.setGradientFill (juce::ColourGradient (gold.brighter (0.25f), lit.getX(), lit.getY(), gold.darker (0.3f), lit.getX(), lit.getBottom(), false));
    g.fillRect (lit);
    g.restoreState();

    g.setColour (isMouseOver() ? gold : goldDim);
    g.drawRoundedRectangle (r, radius, 1.0f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (fx ? text.withAlpha (0.6f) : knobBody);
    g.drawText ("SYNTH", left, juce::Justification::centred, false);
    g.setColour (fx ? knobBody : text.withAlpha (0.6f));
    g.drawText ("EFFECTS", right, juce::Justification::centred, false);

    // M12: a lit dot after "EFFECTS" while delay or reverb is on, so you can tell
    // from the synth page.
    if (isEffectsLit())
    {
        const float textW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), "EFFECTS");
        const float d = 6.0f;
        const auto dot = juce::Rectangle<float> (d, d).withCentre ({ juce::jmin (right.getCentreX() + textW * 0.5f + 7.0f, right.getRight() - radius),
                                                                      right.getCentreY() });
        g.setColour (ledGreen.withAlpha (0.35f));
        g.fillEllipse (dot.expanded (2.5f));
        g.setColour (ledGreen);
        g.fillEllipse (dot);
        g.setColour (knobBody.withAlpha (0.8f));
        g.drawEllipse (dot, 0.8f);
    }
}

void AdvancedPanel::setPage (int newPage)
{
    newPage = juce::jlimit (0, 1, newPage);
    if (newPage == page)
        return;
    page = newPage;
    for (size_t i = 0; i < pages.size(); ++i)
        pages[i].setVisible (static_cast<int> (i) == page);
    repaint();
    if (onPageChange)
        onPageChange (page);
}

AdvancedPanel::AdvancedPanel (juce::AudioProcessorValueTreeState& s, ParamLocks* l) : state (s), locks (l)
{
    if (locks != nullptr)
        locks->onChange = [this]
        {
            for (auto& c : controls)
                if (c->lock != nullptr)
                    c->lock->repaint();
        };
    using namespace param;
    for (auto& pg : pages)
    {
        pg.setInterceptsMouseClicks (false, true);
        addChildComponent (pg);
    }
    pages[synthPage].setVisible (true);

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

    // M11, effects page: DELAY then REVERB (the signal order), each with its switch.
    addSection (rowsPerPage + 0, "DELAY", { { delayTime, "Time" }, { delayFeedback, "Feedback" }, { delayTone, "Tone" },
                                            { delayPingPong, "Mode" }, { delayMix, "Mix" } }, delayOn);
    addSection (rowsPerPage + 1, "REVERB", { { reverbSize, "Size" }, { reverbDecay, "Decay" }, { reverbDamp, "Damping" },
                                             { reverbPreDelay, "Pre-Delay" }, { reverbWidth, "Width" }, { reverbMix, "Mix" } }, reverbOn);
    // M13: the Mint, the sound Cash Match measured from a sample (made by sine partials + noise).
    addSection (rowsPerPage + 2, "MINT", { { mintLevel, "Level" }, { mintNoise, "Noise" }, { mintTone, "Tone" },
                                           { mintSpeed, "Speed" }, { mintStart, "Start" }, { mintLoop, "Loop" },
                                           { mintFine, "Fine" } }, mintOn);

    // M10: TIME / SYNC over each LFO rate. M11: and over the delay time and pre-delay.
    addRateSync (lfoRate, lfo1Sync, lfo1Note);
    addRateSync (lfo2Rate, lfo2Sync, lfo2Note);
    addRateSync (delayTime, delaySync, delayNote);
    addRateSync (reverbPreDelay, reverbPreSync, reverbPreNote);
}

std::unique_ptr<juce::Slider> AdvancedPanel::makeKnob (juce::Component& parent, const char* id, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
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
    parent.addAndMakeVisible (*slider);
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
    auto* parent = c->slider->getParentComponent();
    jassert (parent != nullptr);
    c->noteSlider = makeKnob (*parent, noteId, c->noteAttachment);
    c->noteSlider->setAlpha (c->slider->getAlpha());
    c->label.setVisible (false); // the toggle takes the label's place

    auto* p = state.getParameter (syncId);
    jassert (p != nullptr);
    c->rateMode = std::make_unique<RateModeSwitch> (*p, [c] (bool synced)
    {
        c->slider->setVisible (! synced);
        c->noteSlider->setVisible (synced);
    });
    parent->addAndMakeVisible (*c->rateMode);
    if (c->lock != nullptr) // a synced rate is one knob: lock its mode and note too
    {
        c->lock->addId (syncId);
        c->lock->addId (noteId);
        c->lock->toFront (false);
    }
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

AdvancedPanel::~AdvancedPanel()
{
    if (locks != nullptr)
        locks->onChange = nullptr;
}

AdvancedPanel::LockToggle* AdvancedPanel::getLock (const juce::String& paramId) const
{
    for (const auto& c : controls)
        if (c->paramId == paramId || c->noteId == paramId)
            return c->lock.get();
    return nullptr;
}

void AdvancedPanel::addSection (int row, const juce::String& title, std::initializer_list<std::pair<const char*, const char*>> params,
                                const char* switchId)
{
    if (static_cast<int> (rows.size()) <= row)
        rows.resize (static_cast<size_t> (row + 1));

    Section section { title, {}, {}, nullptr };
    auto& parent = pageFor (row);

    for (const auto& [id, name] : params)
    {
        auto* p = state.getParameter (id);
        jassert (p != nullptr);

        auto c = std::make_unique<Control>();
        c->paramId = id;
        c->label.setText (name, juce::dontSendNotification);
        c->label.setJustificationType (juce::Justification::centred);
        parent.addAndMakeVisible (c->label);

        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            c->combo = std::make_unique<juce::ComboBox>();
            c->combo->addItemList (choice->choices, 1);
            c->combo->setTooltip (p->getName (64));
            parent.addAndMakeVisible (*c->combo);
            c->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, id, *c->combo);
        }
        else
        {
            c->slider = makeKnob (parent, id, c->sliderAttachment);
        }

        // M12: a padlock on everything a money move can change (weight > 0).
        if (const int index = ParameterMap::indexOf (id); locks != nullptr && index >= 0 && ParameterMap::spec (index).weight > 0.0f)
        {
            c->lock = std::make_unique<LockToggle> (*locks, juce::StringArray { id });
            parent.addAndMakeVisible (*c->lock);
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
        parent.addAndMakeVisible (*sw);
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
    if (row >= rowsPerPage && onEffectsSwitched)
        onEffectsSwitched();
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
    for (size_t ri = 0; ri < rows.size(); ++ri)
    {
        if (static_cast<int> (ri) / rowsPerPage != page)
            continue;
        for (const auto& s : rows[ri])
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

    if (page == effectsPage)
        paintSignalPath (g);
}

void AdvancedPanel::paintSignalPath (juce::Graphics& g)
{
    // Under the effect rows: an engraved plate showing where the effects sit.
    auto area = getLocalBounds();
    const int rowHeight = (area.getHeight() - rowGap * (rowsPerPage - 1)) / rowsPerPage;
    area.removeFromTop (3 * (rowHeight + rowGap));
    const auto plate = area.toFloat().reduced (0.0f, 4.0f);
    g.setColour (colours::panel.withAlpha (0.55f));
    g.fillRoundedRectangle (plate, 8.0f);
    g.setColour (colours::panelEdge);
    g.drawRoundedRectangle (plate.reduced (0.5f), 8.0f, 1.0f);

    const juce::StringArray steps { "VOICES + MINT", "DELAY", "REVERB", "MASTER GAIN", "OUT" };
    const float boxW = 130.0f, boxH = 34.0f, arrow = juce::jmax (20.0f, (plate.getWidth() - 60.0f - boxW * 5.0f) / 4.0f);
    float x = plate.getCentreX() - (boxW * 5.0f + arrow * 4.0f) * 0.5f;
    const float y = plate.getCentreY() - boxH * 0.5f - 8.0f;
    for (int i = 0; i < steps.size(); ++i)
    {
        const juce::Rectangle<float> box (x, y, boxW, boxH);
        g.setColour (colours::knobBody);
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (colours::goldDim);
        g.drawRoundedRectangle (box, 6.0f, 1.0f);
        g.setColour (colours::gold);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (steps[i], box, juce::Justification::centred, false);
        x += boxW;
        if (i + 1 < steps.size())
        {
            juce::Path a;
            a.addArrow ({ x + 4.0f, box.getCentreY(), x + arrow - 4.0f, box.getCentreY() }, 1.5f, 8.0f, 7.0f);
            g.setColour (colours::goldDim);
            g.fillPath (a);
            x += arrow;
        }
    }
    g.setColour (colours::text.withAlpha (0.6f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Cash Match turns the MINT on: it rebuilds the sample from sine partials + noise, played on any key. Effects stay off.",
                plate.withTop (y + boxH + 8.0f).withHeight (18.0f), juce::Justification::centred, false);
}

void AdvancedPanel::resized()
{
    for (auto& pg : pages)
        pg.setBounds (getLocalBounds());

    const auto full = getLocalBounds();
    if (rows.empty())
        return;

    // Every page uses the synth page's row height: knobs are the same size everywhere.
    const int rowHeight = (full.getHeight() - rowGap * (rowsPerPage - 1)) / rowsPerPage;

    for (size_t ri = 0; ri < rows.size(); ++ri)
    {
        auto& row = rows[ri];
        const int slot = static_cast<int> (ri) % rowsPerPage;
        auto rowArea = full.withTop (full.getY() + slot * (rowHeight + rowGap)).withHeight (rowHeight);

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
                if (c->lock != nullptr)
                    c->lock->setBounds (cell.getRight() - 13, cell.getY() + 1, 11, 13);
            }
        }
    }
}
}
