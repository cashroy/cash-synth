#include <catch2/catch_test_macros.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "CashCode.h"
#include "Stash.h"

namespace
{
// A throwaway folder, removed afterwards.
struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("CashStashTest", "");
    TempFolder() { dir.createDirectory(); }
    ~TempFolder() { dir.deleteRecursively(); }
};

juce::MemoryBlock saveState (CashSynthAudioProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    return block;
}

void loadState (CashSynthAudioProcessor& p, const juce::MemoryBlock& block)
{
    p.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
}
}

TEST_CASE ("Patch <-> ValueTree round trip is exact and forgiving", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    p.hitJackpot (42);
    const auto patch = p.getPatch();
    const auto defaults = cash::defaultPatch (p.getState());

    const auto tree = cash::patchToTree (patch, "Patch");
    CHECK (cash::samePatch (cash::patchFromTree (tree, defaults), patch, 1.0e-6f));

    // Missing IDs keep the fallback; unknown IDs and junk are ignored.
    auto partial = tree.createCopy();
    partial.removeProperty (cash::ParameterMap::spec (3).id, nullptr);
    partial.setProperty ("fromTheFuture", 0.5, nullptr);
    partial.setProperty (cash::ParameterMap::spec (4).id, "not a number", nullptr);
    const auto read = cash::patchFromTree (partial, defaults);
    CHECK (read[3] == defaults[3]);
    CHECK (read[5] == patch[5]);
    CHECK (std::isfinite (read[4]));

    // Out-of-range values are clamped.
    auto wild = tree.createCopy();
    wild.setProperty (cash::ParameterMap::spec (0).id, 7.0, nullptr);
    CHECK (cash::patchFromTree (wild, defaults)[0] <= 1.0f);
}

TEST_CASE ("Stash saves, lists, loads, overwrites and shreds", "[stash]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempFolder tmp;
    cash::Stash stash (tmp.dir.getChildFile ("Stash")); // created on first save
    CHECK (stash.list().isEmpty());

    CashSynthAudioProcessor p;
    const auto defaults = cash::defaultPatch (p.getState());
    p.hitJackpot (1);
    const auto a = p.getPatch();
    p.hitJackpot (2);
    const auto b = p.getPatch();

    const auto fileB = stash.save ("bass 10", b);
    const auto fileA = stash.save ("Bass 2", a);
    REQUIRE (fileA.existsAsFile());
    REQUIRE (fileB.existsAsFile());
    CHECK (fileA.getFileExtension() == cash::Stash::extension);

    const auto files = stash.list();
    REQUIRE (files.size() == 2);
    CHECK (cash::Stash::nameOf (files[0]) == "Bass 2"); // natural order: 2 before 10
    CHECK (cash::Stash::nameOf (files[1]) == "bass 10");

    auto loaded = cash::Stash::load (fileA, defaults);
    REQUIRE (loaded.has_value());
    CHECK (cash::samePatch (*loaded, a, 1.0e-6f));

    // Same name overwrites.
    CHECK (stash.save ("Bass 2", b) == fileA);
    CHECK (stash.list().size() == 2);
    CHECK (cash::samePatch (*cash::Stash::load (fileA, defaults), b, 1.0e-6f));

    // Names that aren't legal file names still save.
    CHECK (stash.save ("A/B: \"C\"?", a).existsAsFile());
    CHECK (stash.save ("   ", a).getFileNameWithoutExtension() == "Untitled");

    // Not a stash.
    const auto junk = tmp.dir.getChildFile ("junk.cashstash");
    junk.replaceWithText ("<NotAStash/>");
    CHECK_FALSE (cash::Stash::load (junk, defaults).has_value());
    CHECK_FALSE (cash::Stash::load (tmp.dir.getChildFile ("missing.cashstash"), defaults).has_value());

    // Hand-written file with only a CashCode.
    const auto codeOnly = tmp.dir.getChildFile ("code.cashstash");
    codeOnly.replaceWithText ("<CashStash version=\"1\" code=\"" + cash::CashCode::encode (a) + "\"/>");
    REQUIRE (cash::Stash::load (codeOnly, defaults).has_value());

    CHECK (cash::Stash::remove (fileB));
    CHECK_FALSE (fileB.existsAsFile());
    CHECK_FALSE (cash::Stash::remove (fileB));
}

TEST_CASE ("Project state saves knobs, risk and vault; ledger restarts", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor a;
    a.hitJackpot (7);
    REQUIRE (a.bankIt() == 0);
    a.invest ({ 0.6f, 0.9f, 99 });
    REQUIRE (a.bankIt() == 1);
    a.invest ({ 0.3f, 0.2f, 5 });
    a.setRisk (0.83f);
    const auto patch = a.getPatch();
    const auto block = saveState (a);

    CashSynthAudioProcessor b;
    b.hitJackpot (1234); // history that should vanish on load
    b.hitJackpot (4321);
    REQUIRE (b.getLedger().size() == 3);
    loadState (b, block);

    CHECK (cash::samePatch (b.getPatch(), patch));
    CHECK (b.getRisk() == 0.83f);
    REQUIRE (b.getVault().size() == 2);
    for (size_t i = 0; i < 2; ++i)
    {
        CHECK (cash::samePatch (b.getVault().at (i).patch, a.getVault().at (i).patch, 1.0e-6f));
        CHECK (b.getVault().at (i).serial == a.getVault().at (i).serial);
        CHECK (b.getVault().at (i).timeMs == a.getVault().at (i).timeMs);
    }

    // Fresh books: one opening balance of the loaded sound, nothing to undo.
    REQUIRE (b.getLedger().size() == 1);
    CHECK (b.getLedger().current()->memo == "Opening balance");
    CHECK (cash::samePatch (b.getLedger().current()->patch, patch));
    CHECK_FALSE (b.canUndo());
    CHECK_FALSE (b.canRedo());

    // Saving again gives the same project.
    CashSynthAudioProcessor c;
    loadState (c, saveState (b));
    CHECK (cash::samePatch (c.getPatch(), patch));
    CHECK (c.getVault().size() == 2);
}

TEST_CASE ("Bad or foreign state is ignored; bare APVTS state still loads", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    p.hitJackpot (3);
    p.bankIt();
    const auto patch = p.getPatch();

    const char garbage[] = "definitely not a plugin state";
    p.setStateInformation (garbage, static_cast<int> (sizeof (garbage)));
    p.setStateInformation (nullptr, 0);
    juce::MemoryBlock foreign;
    p.copyXmlToBinary (juce::XmlElement ("SomeOtherSynth"), foreign);
    loadState (p, foreign);
    CHECK (cash::samePatch (p.getPatch(), patch));
    CHECK (p.getVault().size() == 1);
    CHECK (p.getLedger().size() == 2); // untouched

    // Just the PARAMS tree (what a plain APVTS plugin would save).
    CashSynthAudioProcessor q;
    juce::MemoryBlock bare;
    if (auto xml = p.getState().copyState().createXml())
        q.copyXmlToBinary (*xml, bare);
    loadState (q, bare);
    CHECK (cash::samePatch (q.getPatch(), patch));
    CHECK (q.getLedger().size() == 1);
}

TEST_CASE ("Processor stash: save, load (undoable), modified flag, saved with project", "[stash]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempFolder tmp;
    CashSynthAudioProcessor p;
    p.getStash().setFolder (tmp.dir);

    CHECK (p.getStashName().isEmpty());
    CHECK_FALSE (p.isStashModified());

    p.hitJackpot (10);
    const auto first = p.getPatch();
    const auto file = p.stashIt ("Gold Lead");
    REQUIRE (file.existsAsFile());
    CHECK (p.getStashName() == "Gold Lead");
    CHECK_FALSE (p.isStashModified());

    p.invest ({ 1.0f, 1.0f, 77 });
    const auto second = p.getPatch();
    CHECK (p.isStashModified());

    REQUIRE (p.loadStash (file));
    CHECK (cash::samePatch (p.getPatch(), first));
    CHECK_FALSE (p.isStashModified());
    CHECK (p.getLedger().current()->memo == "Opened stash: Gold Lead");
    REQUIRE (p.undo());
    CHECK (cash::samePatch (p.getPatch(), second));
    CHECK (p.isStashModified());

    CHECK_FALSE (p.loadStash (tmp.dir.getChildFile ("nope.cashstash")));

    // The name (and the changed flag) come back with the project.
    CashSynthAudioProcessor q;
    loadState (q, saveState (p));
    CHECK (q.getStashName() == "Gold Lead");
    CHECK (q.isStashModified());
    REQUIRE (p.redo());
    CashSynthAudioProcessor r;
    loadState (r, saveState (p));
    CHECK_FALSE (r.isStashModified());
}

TEST_CASE ("Stash bar lists, saves, steps and shows changes", "[stash][editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempFolder tmp;
    CashSynthAudioProcessor p;
    p.getStash().setFolder (tmp.dir);
    std::unique_ptr<CashSynthAudioProcessorEditor> editor (dynamic_cast<CashSynthAudioProcessorEditor*> (p.createEditor()));
    REQUIRE (editor != nullptr);
    auto& bar = editor->getStashBar();
    bar.refreshList();

    CHECK (bar.getCombo().getNumItems() == 0);
    CHECK (bar.getStatus().isEmpty());

    p.hitJackpot (1);
    bar.saveAs ("Alpha");
    const auto alpha = p.getPatch();
    p.hitJackpot (2);
    bar.saveAs ("Beta");
    const auto beta = p.getPatch();
    REQUIRE (bar.getCombo().getNumItems() == 2);
    CHECK (bar.getCombo().getText() == "Beta");

    bar.step (1); // wraps to Alpha
    CHECK (p.getStashName() == "Alpha");
    CHECK (cash::samePatch (p.getPatch(), alpha));
    CHECK (bar.getCombo().getText() == "Alpha");

    bar.step (-1); // wraps back to Beta
    CHECK (cash::samePatch (p.getPatch(), beta));

    bar.getCombo().setSelectedItemIndex (0, juce::sendNotificationSync); // picking from the list loads it
    CHECK (p.getStashName() == "Alpha");

    p.invest ({ 0.5f, 0.5f, 3 });
    editor->syncFromProcessor();
    CHECK (bar.getStatus() == "CHANGED");
    CHECK (bar.getCombo().getText() == "Alpha"); // still shows where you started

    // Deleted behind our back: the name stays, flagged as not in the stash.
    cash::Stash::remove (p.getStash().fileFor ("Alpha"));
    bar.refreshList();
    CHECK (bar.getCombo().getNumItems() == 1);
    CHECK (bar.getStatus().contains ("not in stash"));
    editor.reset();
}
