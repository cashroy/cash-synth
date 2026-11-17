#include "StashBar.h"
#include "BillArt.h"
#include "CashLookAndFeel.h"
#include "PluginProcessor.h"

namespace cash
{
StashBar::StashBar (CashSynthAudioProcessor& p) : processor (p)
{
    combo.setTextWhenNothingSelected ("- nothing stashed yet -");
    combo.setTextWhenNoChoicesAvailable ("Stash is empty: press SAVE");
    combo.setTooltip ("Your stash: saved sounds");
    combo.onChange = [this]
    {
        const int index = combo.getSelectedItemIndex();
        if (index >= 0 && Stash::nameOf (files[index]) != processor.getStashName())
            loadIndex (index);
    };
    addAndMakeVisible (combo);

    prev.setTooltip ("Previous stash");
    next.setTooltip ("Next stash");
    save.setTooltip ("Stash this sound under a name");
    more.setTooltip ("More stash options");
    prev.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    save.onClick = [this] { askForName(); };
    more.onClick = [this] { showMenu(); };
    match.setTooltip ("Cash Match: drop in a sample and find the patch that sounds like it");
    match.onClick = [this] { if (onMatch) onMatch(); };
    for (auto* b : { &prev, &next, &save, &more, &match })
        addAndMakeVisible (*b);

    refreshList();
    startTimer (1500); // pick up files added outside the plugin
}

void StashBar::refreshList()
{
    const auto newFiles = processor.getStash().list();
    if (newFiles != files)
    {
        files = newFiles;
        combo.clear (juce::dontSendNotification);
        for (int i = 0; i < files.size(); ++i)
            combo.addItem (Stash::nameOf (files[i]), i + 1);
    }
    sync();
}

void StashBar::sync()
{
    const auto name = processor.getStashName();
    int index = -1;
    for (int i = 0; i < files.size(); ++i)
        if (Stash::nameOf (files[i]) == name)
            index = i;

    if (combo.getSelectedItemIndex() != index)
    {
        if (index >= 0)
            combo.setSelectedItemIndex (index, juce::dontSendNotification);
        else
            combo.setSelectedId (0, juce::dontSendNotification);
    }

    const bool any = ! files.isEmpty();
    prev.setEnabled (any);
    next.setEnabled (any);

    juce::String newStatus;
    if (name.isNotEmpty() && index < 0)
        newStatus = "\"" + name + "\" (not in stash)";
    else if (processor.isStashModified())
        newStatus = "CHANGED";
    if (newStatus != status)
    {
        status = newStatus;
        repaint();
    }
}

void StashBar::afterMove()
{
    sync();
    if (onMoneyMoved)
        onMoneyMoved();
}

void StashBar::saveAs (const juce::String& name)
{
    if (processor.stashIt (name) == juce::File())
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Couldn't stash it")
                                          .withMessage ("Couldn't write to " + processor.getStash().getFolder().getFullPathName())
                                          .withButton ("OK"),
                                      nullptr);
        return;
    }
    refreshList();
    afterMove();
}

void StashBar::loadIndex (int index)
{
    if (juce::isPositiveAndBelow (index, files.size()))
        processor.loadStash (files[index]);
    refreshList();
    afterMove();
}

void StashBar::step (int delta)
{
    refreshList();
    if (files.isEmpty())
        return;
    const int current = combo.getSelectedItemIndex();
    const int n = files.size();
    const int target = current < 0 ? (delta > 0 ? 0 : n - 1) : ((current + delta) % n + n) % n;
    loadIndex (target);
}

void StashBar::askForName()
{
    auto* window = new juce::AlertWindow ("Stash it", "Name this sound:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", processor.getStashName().isNotEmpty() ? processor.getStashName() : "My Sound");
    window->addButton ("STASH IT", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<StashBar> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window] (int result)
    {
        if (result != 1 || safe == nullptr)
            return;
        const auto name = window->getTextEditorContents ("name").trim();
        if (name.isEmpty())
            return;

        auto& p = safe->processor;
        const auto file = p.getStash().fileFor (name);
        if (file.existsAsFile() && Stash::nameOf (file) != p.getStashName())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle ("Replace stash?")
                                              .withMessage ("\"" + Stash::nameOf (file) + "\" is already in your stash. Replace it with this sound?")
                                              .withButton ("Replace").withButton ("Cancel"),
                                          [safe, name] (int r) { if (r == 1 && safe != nullptr) safe->saveAs (name); });
            return;
        }
        safe->saveAs (name);
    }), true);
}

void StashBar::showMenu()
{
    const auto name = processor.getStashName();
    const auto current = name.isNotEmpty() ? processor.getStash().fileFor (name) : juce::File();

    juce::PopupMenu menu;
    menu.addItem (1, "Open stash folder");
    menu.addItem (2, "Shred \"" + name + "\" (to the bin)", current.existsAsFile());
    menu.addItem (3, "Rescan");

    juce::Component::SafePointer<StashBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&more), [safe, current] (int result)
    {
        if (safe == nullptr)
            return;
        if (result == 1)
        {
            auto folder = safe->processor.getStash().getFolder();
            folder.createDirectory();
            folder.startAsProcess();
        }
        else if (result == 2)
            Stash::remove (current);
        safe->refreshList();
    });
}

void StashBar::paint (juce::Graphics& g)
{
    using namespace colours;
    g.setColour (gold);
    g.setFont (art::serif (14.0f, true, 0.2f));
    g.drawText ("CASHROY", brandArea, juce::Justification::centredLeft);
    g.setColour (goldDim);
    g.setFont (art::serif (12.0f, false, 0.25f));
    g.drawText ("CASH SYNTH  -  SERIES 2026", brandArea.withTrimmedLeft (100), juce::Justification::centredLeft);

    g.setColour (gold);
    g.setFont (art::serif (13.0f, true, 0.18f));
    g.drawText ("STASH", labelArea, juce::Justification::centredRight);

    if (status.isNotEmpty())
    {
        g.setColour (ledAmber);
        g.setFont (art::serif (11.0f, false, 0.15f));
        g.drawText (status, statusArea, juce::Justification::centredRight);
    }
}

void StashBar::resized()
{
    auto area = getLocalBounds();
    more.setBounds (area.removeFromRight (34));
    area.removeFromRight (6);
    save.setBounds (area.removeFromRight (70));
    area.removeFromRight (6);
    next.setBounds (area.removeFromRight (28));
    area.removeFromRight (2);
    prev.setBounds (area.removeFromRight (28));
    area.removeFromRight (6);
    combo.setBounds (area.removeFromRight (240));
    area.removeFromRight (8);
    labelArea = area.removeFromRight (60);
    area.removeFromRight (8);
    match.setBounds (area.removeFromRight (110));
    area.removeFromRight (10);
    statusArea = area.removeFromRight (120);
    brandArea = area.withTrimmedLeft (4);
}
}
