#include <catch2/catch_test_macros.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "AdvancedPanel.h"
#include "BillComponents.h"
#include "ParameterMap.h"
#include "Parameters.h"

// Milestone 12: lock toggles per knob, the lit EFFECTS tab, the flip animation.
namespace
{
using namespace cash::param;

struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    std::unique_ptr<CashSynthAudioProcessorEditor> editor;

    Rig()
    {
        editor.reset (dynamic_cast<CashSynthAudioProcessorEditor*> (p.createEditor()));
        REQUIRE (editor != nullptr);
    }
    ~Rig() { editor.reset(); }

    float value (const char* id) { return p.getState().getParameter (id)->getValue(); }
};

juce::MemoryBlock saveState (CashSynthAudioProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    return block;
}
}

TEST_CASE ("Padlocks on every knob a money move can change", "[m12][locks]")
{
    Rig rig;
    auto& panel = rig.editor->getAdvancedPanel();

    for (int i = 0; i < cash::ParameterMap::numParams; ++i)
    {
        const auto& spec = cash::ParameterMap::spec (i);
        if (cash::isSectionSwitch (i) || juce::String (spec.id) == delayOn || juce::String (spec.id) == reverbOn)
            continue; // switches have no padlock
        const juce::String id (spec.id);
        if (id.endsWith ("Sync") || id.endsWith ("Note"))
            continue; // locked with their rate knob
        INFO (spec.id);
        if (spec.weight > 0.0f)
            CHECK (panel.getLock (id) != nullptr);
        else
            CHECK (panel.getLock (id) == nullptr); // master gain, voice mode...: never mutated, nothing to lock
    }

    // A panel without a processor has no padlocks.
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor other;
    cash::AdvancedPanel bare (other.getState());
    CHECK (bare.getLock (filterCutoff) == nullptr);
}

TEST_CASE ("Locked knobs survive Invest and Jackpot", "[m12][locks]")
{
    Rig rig;
    auto& panel = rig.editor->getAdvancedPanel();
    auto& locks = rig.p.getLocks();

    auto* cutoffLock = panel.getLock (filterCutoff);
    auto* rateLock = panel.getLock (lfoRate);
    REQUIRE (cutoffLock != nullptr);
    REQUIRE (rateLock != nullptr);
    CHECK_FALSE (cutoffLock->isLocked());

    cutoffLock->toggle();
    rateLock->toggle();
    for (auto* id : { filterCutoff, ampAttack, osc1Wave })
        if (id != filterCutoff)
            panel.getLock (id)->toggle();
    CHECK (cutoffLock->isLocked());
    CHECK (locks.isLocked (filterCutoff));
    CHECK (locks.isLocked (lfo1Sync)); // the rate's lock covers its TIME/SYNC switch and note
    CHECK (locks.isLocked (lfo1Note));
    CHECK (locks.count() == 6);

    const auto lockedIds = { filterCutoff, lfoRate, lfo1Sync, lfo1Note, ampAttack, osc1Wave };
    std::map<juce::String, float> before;
    for (auto* id : lockedIds)
        before[id] = rig.value (id);

    bool othersMoved = false;
    const auto start = rig.p.getPatch();
    for (uint64_t seed = 1; seed <= 12; ++seed)
    {
        if (seed % 2 == 0)
            rig.p.hitJackpot (seed);
        else
            rig.p.invest ({ 1.0f, 1.0f, seed });
        for (auto* id : lockedIds)
        {
            INFO (id << " seed " << seed);
            CHECK (rig.value (id) == before[id]);
        }
    }
    othersMoved = ! cash::samePatch (start, rig.p.getPatch());
    CHECK (othersMoved);

    // Unlock: they move again.
    cutoffLock->toggle();
    CHECK_FALSE (locks.isLocked (filterCutoff));
    bool cutoffMoved = false;
    for (uint64_t seed = 100; seed < 110 && ! cutoffMoved; ++seed)
    {
        rig.p.hitJackpot (seed);
        cutoffMoved = rig.value (filterCutoff) != before[filterCutoff];
    }
    CHECK (cutoffMoved);
}

TEST_CASE ("Locks are saved with the project, not with the sound", "[m12][locks][state]")
{
    juce::MemoryBlock saved, old;
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        CashSynthAudioProcessor p;
        old = saveState (p); // a project with no locks (as before M12)
        p.getLocks().setLocked (filterReso, true);
        p.getLocks().setLocked (delayMix, true);
        saved = saveState (p);
        CHECK (p.getLocks().toString() == "filterReso,delayMix");
    }

    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    CHECK (p.getLocks().isLocked (filterReso));
    CHECK (p.getLocks().isLocked (delayMix));
    CHECK (p.getLocks().count() == 2);

    // Loading a project without locks clears them; unknown IDs are ignored.
    p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    CHECK (p.getLocks().count() == 0);
    p.getLocks().fromString ("noSuchParam, ampDecay ,");
    CHECK (p.getLocks().count() == 1);
    CHECK (p.getLocks().isLocked (ampDecay));

    // Banking keeps the sound only: withdrawing doesn't touch the locks.
    p.bankIt();
    p.getLocks().clear();
    CHECK (p.withdraw (0));
    CHECK (p.getLocks().count() == 0);
}

TEST_CASE ("EFFECTS tab lights while delay or reverb is on", "[m12][ui]")
{
    Rig rig;
    auto& tabs = rig.editor->getBack().getPageTabs();
    auto* delay = rig.editor->getAdvancedPanel().getSwitch (delayOn);
    auto* reverb = rig.editor->getAdvancedPanel().getSwitch (reverbOn);
    REQUIRE (delay != nullptr);
    REQUIRE (reverb != nullptr);

    CHECK_FALSE (tabs.isEffectsLit()); // both off by default
    delay->toggle();
    CHECK (tabs.isEffectsLit());
    reverb->toggle();
    delay->toggle();
    CHECK (tabs.isEffectsLit());
    reverb->toggle();
    CHECK_FALSE (tabs.isEffectsLit());

    // Host automation counts too (raw value, no UI involved).
    rig.p.getState().getParameter (reverbOn)->setValueNotifyingHost (1.0f);
    CHECK (tabs.isEffectsLit());
}

TEST_CASE ("Flip: instant off screen, animated card hides both sides then shows the new one", "[m12][ui]")
{
    Rig rig;
    auto& ed = *rig.editor;

    // Not on screen (tests, offline renders): the state changes at once.
    ed.flip();
    CHECK (ed.isFlipped());
    CHECK (ed.getBack().isVisible());
    CHECK_FALSE (ed.getFront().isVisible());
    CHECK_FALSE (ed.getFlipCard().isTurning());
    ed.flip();
    CHECK_FALSE (ed.isFlipped());
    CHECK (ed.getFront().isVisible());

    // The card itself.
    auto& card = ed.getFlipCard();
    card.start (ed.getFront(), ed.getBack());
    CHECK (card.isTurning());
    CHECK_FALSE (ed.getFront().isVisible());
    CHECK_FALSE (ed.getBack().isVisible());
    CHECK (card.getBounds().contains (ed.getFront().getBounds()));
    juce::Image frame (juce::Image::ARGB, card.getWidth(), card.getHeight(), true);
    {
        juce::Graphics g (frame);
        card.paint (g); // progress 0: the full front, no crash
    }
    CHECK (frame.getPixelAt (card.getWidth() / 2, card.getHeight() / 2).getAlpha() > 0);
    card.finish();
    CHECK_FALSE (card.isTurning());
    CHECK (ed.getBack().isVisible());
    CHECK_FALSE (ed.getFront().isVisible());

    // Back to the front by hand, then a real flip + Cash Match: closing the cheque
    // returns to the side you were on.
    ed.getFront().setVisible (true);
    ed.getBack().setVisible (false);
    ed.flip();
    ed.showMatch (true);
    CHECK (ed.isShowingMatch());
    CHECK_FALSE (ed.getBack().isVisible());
    ed.showMatch (false);
    CHECK (ed.isFlipped());
    CHECK (ed.getBack().isVisible());
    CHECK_FALSE (ed.getFront().isVisible());
}
