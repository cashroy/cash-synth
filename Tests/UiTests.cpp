#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "CashCode.h"
#include "Parameters.h"
#include "BillArt.h"
#include "CashLookAndFeel.h"

namespace
{
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

    // Hold a note for a few blocks so there is something for the scope.
    void playNote()
    {
        p.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        for (int block = 0; block < 8; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.9f), 0);
            p.processBlock (buffer, midi);
        }
    }
};

cash::Patch patchWith (float v)
{
    cash::Patch p {};
    p.fill (v);
    return p;
}
}

TEST_CASE ("Vault banks, dedupes, fills up and shreds", "[vault]")
{
    cash::Vault vault (3);
    CHECK (vault.deposit (patchWith (0.1f), 1) == 0);
    CHECK (vault.deposit (patchWith (0.2f), 2) == 1);
    CHECK (vault.deposit (patchWith (0.1f), 3) == 0); // same sound: not banked twice
    CHECK (vault.size() == 2);
    CHECK (vault.at (0).serial == cash::CashCode::serialNumber (patchWith (0.1f)));

    CHECK (vault.deposit (patchWith (0.3f)) == 2);
    CHECK (vault.isFull());
    CHECK (vault.deposit (patchWith (0.4f)) == -1); // full: nothing silently dropped
    CHECK (vault.find (patchWith (0.4f)) == -1);

    CHECK (vault.shred (0));
    CHECK_FALSE (vault.shred (5));
    CHECK (vault.find (patchWith (0.2f)) == 0);
    CHECK (vault.deposit (patchWith (0.4f)) == 2);
}

TEST_CASE ("Processor vault, risk and undo state", "[vault][processor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;

    CHECK (p.getLedger().size() == 1); // opening balance recorded up front
    CHECK_FALSE (p.canUndo());
    CHECK_FALSE (p.canRedo());

    // A hand tweak alone is undoable (back to the opening balance).
    p.getState().getParameter (cash::param::filterCutoff)->setValueNotifyingHost (0.2f);
    CHECK (p.canUndo());
    CHECK (p.undo());
    CHECK (cash::samePatch (p.getPatch(), p.getLedger().at (0).patch));
    CHECK (p.canRedo());

    const auto opening = p.getPatch();
    CHECK (p.bankIt() == 0);
    CHECK (p.bankIt() == 0);
    p.hitJackpot (5);
    CHECK (p.bankIt() == 1);
    CHECK (p.getVault().size() == 2);

    CHECK (p.withdraw (0));
    CHECK (cash::samePatch (p.getPatch(), opening));
    CHECK_THAT (p.getLedger().current()->memo.toStdString(), Catch::Matchers::StartsWith ("Withdrew "));
    CHECK (p.undo()); // withdrawals are undoable
    CHECK_FALSE (cash::samePatch (p.getPatch(), opening));
    CHECK_FALSE (p.withdraw (9));

    CHECK (p.shred (0));
    CHECK (p.getVault().size() == 1);

    p.setRisk (1.7f);
    CHECK (p.getRisk() == 1.0f);
    p.setRisk (-1.0f);
    CHECK (p.getRisk() == 0.0f);
}

TEST_CASE ("Scope buffer taps the output", "[scope]")
{
    cash::ScopeBuffer scope;
    juce::AudioBuffer<float> b (2, 100);
    for (int i = 0; i < 100; ++i)
    {
        b.setSample (0, i, static_cast<float> (i));
        b.setSample (1, i, static_cast<float> (i) + 2.0f);
    }
    scope.push (b);
    CHECK (scope.getNumWritten() == 100);

    std::array<float, 4> last {};
    scope.read (last.data(), 4);
    CHECK (last[0] == 97.0f); // mono mix of (96 + 98) / 2
    CHECK (last[3] == 100.0f);

    Rig rig;
    rig.playNote();
    CHECK (rig.p.getScope().getNumWritten() == 8 * 512);
    auto& portrait = rig.editor->getFront().getPortrait();
    portrait.refresh();
    CHECK (portrait.hasSignal());
}

TEST_CASE ("Bill front is wired to the money moves", "[editor][ui]")
{
    Rig rig;
    auto& ed = *rig.editor;
    auto& p = rig.p;
    CHECK (ed.getWidth() == CashSynthAudioProcessorEditor::width);
    CHECK (ed.getFront().isVisible());
    CHECK_FALSE (ed.isFlipped());

    // Serial shows the live patch.
    const auto serial = [&] { return ed.getFront().getSerial (0).getSerial(); };
    CHECK (serial() == cash::CashCode::serialNumber (p.getPatch()));
    CHECK (ed.getFront().getSerial (1).getSerial() == serial());

    // Jackpot seal.
    const auto before = p.getPatch();
    ed.getFront().getJackpot().onClick();
    CHECK (p.getLedger().current()->memo == "Jackpot!");
    CHECK_FALSE (cash::samePatch (before, p.getPatch()));
    CHECK (serial() == cash::CashCode::serialNumber (p.getPatch()));

    // Coins invest their amount at the dial's risk.
    auto& tray = ed.getTray();
    tray.getRiskDial().setValue (0.8, juce::sendNotificationSync);
    CHECK (p.getRisk() == 0.8f);
    tray.getCoin (1).onClick();
    CHECK (p.getLedger().current()->memo == "Invested 25% @ 80% risk");
    tray.getCoin (3).onClick();
    CHECK (p.getLedger().current()->memo == "Invested 100% @ 80% risk");
    CHECK (tray.getMemo() == "Invested 100% @ 80% risk");

    // Undo / redo buttons.
    CHECK (tray.getUndoButton().isEnabled());
    CHECK_FALSE (tray.getRedoButton().isEnabled());
    tray.getUndoButton().onClick();
    CHECK (p.getLedger().current()->memo == "Invested 25% @ 80% risk");
    CHECK (tray.getRedoButton().isEnabled());
    tray.getRedoButton().onClick();
    CHECK (p.getLedger().current()->memo == "Invested 100% @ 80% risk");

    // Bank It fills a vault slot; clicking a slot withdraws it.
    CHECK (tray.getVaultSlot (0).isEmpty());
    tray.getBankItButton().onClick();
    CHECK (p.getVault().size() == 1);
    CHECK_FALSE (tray.getVaultSlot (0).isEmpty());
    const auto banked = p.getPatch();
    ed.getFront().getJackpot().onClick();
    tray.getVaultSlot (0).onWithdraw (0);
    CHECK (cash::samePatch (p.getPatch(), banked));
    CHECK (tray.getNumVaultSlots() == static_cast<int> (p.getVault().getCapacity()));

    // A hand tweak is picked up by the editor's sync.
    p.getState().getParameter (cash::param::filterReso)->setValueNotifyingHost (0.05f);
    ed.syncFromProcessor();
    CHECK (serial() == cash::CashCode::serialNumber (p.getPatch()));
    CHECK (tray.getMemo() == "Hand tweak (unrecorded)");

    // Flip to the knobs and back.
    ed.getFront().getFlipButton().onClick();
    CHECK (ed.isFlipped());
    CHECK (ed.getBack().isVisible());
    CHECK_FALSE (ed.getFront().isVisible());
    CHECK (ed.getAdvancedPanel().getNumControls() == p.getParameters().size());
    ed.getBack().getFlipButton().onClick();
    CHECK_FALSE (ed.isFlipped());
}

// Hidden (run with "[snapshot]"): renders the editor to $CASH_SNAPSHOT as a PNG,
// plus the flipped side next to it as <name>-back.png.
TEST_CASE ("Editor snapshot", "[.][snapshot]")
{
    Rig rig;
    const auto path = juce::SystemStats::getEnvironmentVariable ("CASH_SNAPSHOT", {});
    REQUIRE (path.isNotEmpty());

    // Make it look lived in: a stash, a few banked notes, a held note on the scope.
    const auto stashDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("CashSnapStash", "");
    rig.p.getStash().setFolder (stashDir);
    rig.p.bankIt();
    rig.p.hitJackpot (11);
    rig.p.bankIt();
    rig.p.invest ({ 0.5f, 0.4f, 3 });
    rig.p.bankIt();
    rig.p.setRisk (0.62f);
    rig.p.setPatch (cash::defaultPatch (rig.p.getState()), "Back to par");
    rig.p.stashIt ("Gold Standard Bass");
    rig.p.invest ({ 0.3f, 0.2f, 2026 });
    rig.editor->getStashBar().refreshList();
    rig.editor->syncFromProcessor();
    rig.editor->getTray().getRiskDial().setValue (0.62, juce::dontSendNotification);
    rig.playNote();
    rig.editor->getFront().getPortrait().refresh();

    const auto write = [&] (const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (stream.openedOk());
        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 2.0f), stream));
    };

    const juce::File front (path);
    write (front);
    rig.editor->flip();
    // M8: show both switch states on the back (on, and dimmed off).
    for (auto* id : { cash::param::lfo2On, cash::param::driveOn })
        rig.p.getState().getParameter (id)->setValueNotifyingHost (0.0f);
    write (front.getSiblingFile (front.getFileNameWithoutExtension() + "-back.png"));
    stashDir.deleteRecursively();
}

TEST_CASE ("Bill background: tinted public-domain scan, painted fallback", "[bill]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    REQUIRE (cash::art::hasScan());
    const auto scan = cash::art::makeTintedScan();
    REQUIRE (scan.isValid());
    CHECK (scan.getWidth() == 1200);
    CHECK (scan.getHeight() == 505);

    // Faded duotone: nothing darker than the faded ink, nothing brighter than paper,
    // and no strong colour anywhere (the real note's blue ribbon and copper are gone).
    const float minBrightness = cash::colours::ink.interpolatedWith (cash::colours::paper, 0.38f).getPerceivedBrightness() - 0.02f;
    float darkest = 1.0f, maxSat = 0.0f;
    for (int y = 0; y < scan.getHeight(); y += 3)
        for (int x = 0; x < scan.getWidth(); x += 3)
        {
            const auto c = scan.getPixelAt (x, y);
            darkest = juce::jmin (darkest, c.getPerceivedBrightness());
            maxSat = juce::jmax (maxSat, c.getSaturation());
        }
    CHECK (darkest >= minBrightness);
    CHECK (maxSat <= juce::jmax (cash::colours::ink.getSaturation(), cash::colours::paper.getSaturation()) + 0.05f);

    // The real serial number is blanked: flat paper tone where it was.
    const auto serialSpot = scan.getPixelAt (150, 122);
    CHECK (serialSpot.getPerceivedBrightness() > 0.75f);

    // Both painters draw without the scan's help.
    juce::Image img (juce::Image::ARGB, 400, 175, true);
    juce::Graphics g (img);
    cash::art::paintFrontPainted (g, img.getBounds().toFloat());
    CHECK (img.getPixelAt (200, 4).getAlpha() == 255);
    cash::art::paintFront (g, img.getBounds().toFloat());
    CHECK (img.getPixelAt (200, 4).getAlpha() == 255);
}
