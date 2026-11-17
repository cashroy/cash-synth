#include "MatchPanel.h"
#include "PluginProcessor.h"
#include "BillArt.h"
#include "CashLookAndFeel.h"
#include "CashCode.h"

namespace cash
{
namespace
{
    const juce::StringArray audioExtensions { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a", ".caf" };

    juce::String stampFor (int percent, bool searching)
    {
        if (searching)      return "PENDING";
        if (percent >= 80)  return "CLEARED";
        if (percent >= 50)  return "PARTIAL";
        return "BOUNCED";
    }

    juce::Colour stampColour (int percent, bool searching)
    {
        if (searching)      return colours::ledAmber.darker (0.3f);
        if (percent >= 80)  return colours::sealGreen;
        if (percent >= 50)  return colours::shiftInk;
        return colours::ledRed.darker (0.2f);
    }
}

MatchPanel::MatchPanel (CashSynthAudioProcessor& p) : processor (p)
{
    for (auto* b : { &load_, &hearSample, &matchIt, &hearMatch, &deposit, &close })
        addAndMakeVisible (*b);

    load_.setTooltip ("Pick a short sample (one note works best). You can also drag a file onto the cheque.");
    hearSample.setTooltip ("Play the sample");
    matchIt.setTooltip ("Search for the patch that sounds closest (runs in the background)");
    hearMatch.setTooltip ("Play the best match so far, at the sample's pitch and level");
    deposit.setTooltip ("Load the best match into the synth (undoable from the Ledger)");

    load_.onClick = [this] { browse(); };
    hearSample.onClick = [this] { processor.previewTarget(); };
    hearMatch.onClick = [this] { processor.previewMatch(); };
    matchIt.onClick = [this]
    {
        if (processor.getMatcher().isSearching())
            processor.getMatcher().stop();
        else
            startMatching();
        sync();
    };
    deposit.onClick = [this]
    {
        if (processor.depositMatch())
        {
            status = "Deposited. Play " + analysis::noteName (processor.getMatcher().getTargetPrint().midiNote) + " to hear it as matched.";
            if (onMoneyMoved)
                onMoneyMoved();
        }
        repaint();
    };
    close.onClick = [this]
    {
        processor.stopPreview();
        if (onClose)
            onClose();
    };

    status = "Drop a sample on the cheque, or LOAD SAMPLE.";
    updateButtons();
}

MatchPanel::~MatchPanel() = default;

bool MatchPanel::isAudioFile (const juce::String& path)
{
    for (const auto& ext : audioExtensions)
        if (path.endsWithIgnoreCase (ext))
            return true;
    return false;
}

bool MatchPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void MatchPanel::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (const auto& f : files)
        if (isAudioFile (f))
        {
            load (juce::File (f));
            return;
        }
}

void MatchPanel::mouseUp (const juce::MouseEvent& e)
{
    if (dropArea.contains (e.getPosition()) && ! processor.getMatcher().hasTarget())
        browse();
}

void MatchPanel::browse()
{
    chooser = std::make_unique<juce::FileChooser> ("Pick a sample to match", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MatchPanel> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                                  safe->load (fc.getResult());
                          });
}

void MatchPanel::load (const juce::File& file)
{
    processor.stopPreview();
    const auto error = processor.getMatcher().loadTarget (file);
    matchWave.setSize (0, 0);
    shownTried = -1;
    if (error.isNotEmpty())
    {
        status = error;
        updateButtons();
        repaint();
        return;
    }
    startMatching();
    sync();
}

void MatchPanel::startMatching()
{
    if (! processor.getMatcher().hasTarget())
        return;
    processor.startMatch (0xC0FFEEull + ++runs * 7919ull);
    status = "Shopping around for your sound...";
}

void MatchPanel::visibilityChanged()
{
    if (isVisible())
    {
        sync();
        startTimerHz (15);
    }
    else
    {
        stopTimer();
    }
}

void MatchPanel::sync()
{
    auto& m = processor.getMatcher();
    const auto r = m.getResult();
    const bool wasSearching = searching;
    searching = m.isSearching();
    found = r.found;
    progress = r.progress;

    if (r.tried != shownTried || wasSearching != searching)
    {
        shownTried = r.tried;
        if (found && percent != r.percent)
            matchWave = m.getBestRender();
        else if (found && matchWave.getNumSamples() == 0)
            matchWave = m.getBestRender();
        percent = r.percent;
        parts = r.parts;
        sectionsOff = r.sectionsOff;

        if (searching)
            status = "Shopping around... " + juce::String (r.tried) + " sounds tried";
        else if (found)
        {
            status = (r.progress >= 1.0f ? "Done: " : "Stopped: ") + juce::String (r.tried) + " sounds tried";
            if (sectionsOff > 0)
                status << ", " << sectionsOff << (sectionsOff == 1 ? " section" : " sections") << " switched off";
            status << ". HEAR MATCH, then DEPOSIT.";
        }
        repaint();
    }
    updateButtons();
}

int MatchPanel::getPartPercent (int part) const
{
    const auto& m = processor.getMatcher();
    if (! found || ! m.hasTarget() || part < 0 || part >= analysis::numParts || ! analysis::partApplies (m.getTargetPrint(), part))
        return -1;
    return analysis::matchPercent (parts[static_cast<size_t> (part)]);
}

void MatchPanel::updateButtons()
{
    const auto& m = processor.getMatcher();
    hearSample.setEnabled (m.hasTarget());
    matchIt.setEnabled (m.hasTarget());
    matchIt.setButtonText (m.isSearching() ? "STOP" : (found ? "MATCH AGAIN" : "MATCH IT"));
    hearMatch.setEnabled (found);
    deposit.setEnabled (found);
}

void MatchPanel::paintWave (juce::Graphics& g, juce::Rectangle<float> r, const juce::AudioBuffer<float>& audio, juce::Colour c, bool filled) const
{
    const int n = audio.getNumSamples();
    if (n == 0 || r.getWidth() < 2.0f)
        return;
    const float peak = juce::jmax (1.0e-6f, audio.getMagnitude (0, n));
    const int columns = static_cast<int> (r.getWidth());

    juce::Path outline;
    for (int col = 0; col < columns; ++col)
    {
        const int a = static_cast<int> (static_cast<juce::int64> (col) * n / columns);
        const int b = juce::jmax (a + 1, static_cast<int> (static_cast<juce::int64> (col + 1) * n / columns));
        float hi = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* x = audio.getReadPointer (ch);
            for (int i = a; i < juce::jmin (b, n); ++i)
                hi = juce::jmax (hi, std::abs (x[i]));
        }
        const float h = hi / peak * r.getHeight() * 0.48f;
        const float px = r.getX() + static_cast<float> (col);
        if (filled)
        {
            g.setColour (c);
            g.drawVerticalLine (static_cast<int> (px), r.getCentreY() - h, r.getCentreY() + h);
        }
        else if (col == 0)
            outline.startNewSubPath (px, r.getCentreY() - h);
        else
            outline.lineTo (px, r.getCentreY() - h);
    }
    if (! filled)
    {
        g.setColour (c);
        g.strokePath (outline, juce::PathStrokeType (1.6f));
        g.strokePath (outline, juce::PathStrokeType (1.6f), juce::AffineTransform::verticalFlip (r.getCentreY() * 2.0f));
    }
}

void MatchPanel::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto& m = processor.getMatcher();
    const auto all = getLocalBounds().toFloat();

    // Cheque paper with a guilloche border.
    g.setColour (paper);
    g.fillRoundedRectangle (all, 8.0f);
    art::drawWaveBand (g, all.reduced (6.0f).withHeight (12.0f), true, 5, 26.0f, inkSoft.withAlpha (0.5f));
    art::drawWaveBand (g, all.reduced (6.0f).removeFromBottom (12.0f), true, 5, 26.0f, inkSoft.withAlpha (0.5f));
    art::drawRosette (g, { all.getRight() - 120.0f, all.getY() + 150.0f }, 30.0f, 110.0f, 18, 6, sealGreen.withAlpha (0.08f));
    g.setColour (ink);
    g.drawRoundedRectangle (all.reduced (1.0f), 8.0f, 2.0f);

    // Header.
    g.setFont (art::serif (26.0f, true, 0.12f));
    g.drawText ("CASH MATCH", 28, 22, 400, 30, juce::Justification::centredLeft, false);
    g.setFont (art::serif (12.0f, false, 0.25f));
    g.setColour (inkSoft);
    g.drawText ("CASHROY SOUND EXCHANGE  -  DEPOSIT A SOUND, WE FIND THE PATCH", 30, 52, 600, 16, juce::Justification::centredLeft, false);
    const auto chequeNo = m.hasTarget() ? CashCode::serialNumber (processor.getPatch()) : juce::String ("No. 000000");
    g.setFont (art::mono (13.0f, true));
    g.setColour (serialInk);
    g.drawText (chequeNo, getWidth() - 340, 26, 220, 20, juce::Justification::centredRight, false);

    // Drop zone.
    const auto drop = dropArea.toFloat();
    g.setColour (dragOver ? sealGreen.withAlpha (0.18f) : paperShade.withAlpha (0.6f));
    g.fillRoundedRectangle (drop, 6.0f);
    juce::Path border;
    border.addRoundedRectangle (drop.reduced (1.0f), 6.0f);
    const float dashes[] = { 7.0f, 5.0f };
    juce::PathStrokeType (1.5f).createDashedStroke (border, border, dashes, 2);
    g.setColour (dragOver ? sealGreen : inkSoft);
    g.fillPath (border);

    if (m.hasTarget())
    {
        auto waveArea = drop.reduced (14.0f, 18.0f);
        paintWave (g, waveArea, m.getTargetAudio(), inkSoft.withAlpha (0.55f), true);
        if (matchWave.getNumSamples() > 0)
            paintWave (g, waveArea, matchWave, shiftInk, false);

        g.setFont (art::serif (11.0f, true, 0.15f));
        g.setColour (inkSoft);
        g.drawText ("SAMPLE", drop.reduced (12.0f, 6.0f), juce::Justification::topLeft, false);
        if (matchWave.getNumSamples() > 0)
        {
            g.setColour (shiftInk);
            g.drawText ("BEST MATCH", drop.reduced (12.0f, 6.0f), juce::Justification::topRight, false);
        }
    }
    else
    {
        g.setColour (ink);
        g.setFont (art::serif (20.0f, true, 0.1f));
        g.drawText ("DROP A SAMPLE HERE", drop.withTrimmedBottom (30.0f), juce::Justification::centred, false);
        g.setFont (art::serif (13.0f));
        g.setColour (inkSoft);
        g.drawText ("one note, the whole thing up to 10 s  -  WAV, AIFF, FLAC, MP3, OGG  -  or click to browse",
                    drop.withTrimmedTop (40.0f), juce::Justification::centred, false);
    }

    // "Pay to the order of" line.
    auto info = infoArea.toFloat();
    g.setColour (inkSoft);
    g.setFont (art::serif (11.0f, true, 0.2f));
    g.drawText ("PAY TO THE ORDER OF", info.removeFromLeft (180.0f), juce::Justification::bottomLeft, false);
    g.setColour (ink);
    g.drawLine (info.getX(), info.getBottom(), info.getRight(), info.getBottom(), 1.0f);
    if (m.hasTarget())
    {
        const auto& tp = m.getTargetPrint();
        juce::String line = m.getTargetName() + "    ";
        line << (tp.pitched ? analysis::noteName (tp.midiNote) + " (" + juce::String (tp.f0, 1) + " Hz)" : juce::String ("no clear pitch: matched at C3"))
             << "    " << juce::String (tp.seconds, 2) << " s";
        // What the analysis heard move.
        if (tp.vibrato.present()) line << "    vibrato " << juce::String (tp.vibrato.rate, 1) << " Hz";
        if (tp.tremolo.present()) line << "    tremolo " << juce::String (tp.tremolo.rate, 1) << " Hz";
        if (tp.wah.present())     line << "    wah " << juce::String (tp.wah.rate, 1) << " Hz";
        if (std::abs (tp.pitchStart) > 0.3f) line << "    pitch drop " << juce::String (-tp.pitchStart, 1) << " st";
        if (tp.width > 0.08f)     line << "    stereo";
        g.setFont (art::serif (16.0f, false));
        g.drawText (line, info.reduced (6.0f, 0.0f), juce::Justification::bottomLeft, true);
    }

    // Amount box: the match %.
    const auto box = amountArea.toFloat();
    g.setColour (paper.brighter (0.4f));
    g.fillRoundedRectangle (box, 6.0f);
    g.setColour (ink);
    g.drawRoundedRectangle (box, 6.0f, 1.5f);
    g.setFont (art::serif (11.0f, true, 0.25f));
    g.setColour (inkSoft);
    g.drawText ("MATCH", box.reduced (12.0f, 8.0f), juce::Justification::topLeft, false);

    g.setColour (ink);
    g.setFont (art::mono (56.0f, true));
    g.drawText (found ? juce::String (percent) + "%" : juce::String ("--%"), box.withTrimmedTop (44.0f).withTrimmedBottom (30.0f), juce::Justification::centred, false);

    // Progress.
    auto bar = box.reduced (16.0f, 0.0f).withHeight (8.0f).withY (box.getBottom() - 26.0f);
    g.setColour (paperShade);
    g.fillRoundedRectangle (bar, 4.0f);
    g.setColour (sealGreen);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 4.0f);

    // Rubber stamp across the box.
    if (found || searching)
    {
        const auto text = stampFor (percent, searching);
        const auto col = stampColour (percent, searching).withAlpha (0.8f);
        juce::Graphics::ScopedSaveState save (g);
        g.addTransform (juce::AffineTransform::rotation (-0.18f, box.getCentreX(), box.getY() + 30.0f));
        const auto stamp = juce::Rectangle<float> (150.0f, 30.0f).withCentre ({ box.getRight() - 95.0f, box.getY() + 30.0f });
        g.setColour (col);
        g.drawRoundedRectangle (stamp, 4.0f, 2.5f);
        g.setFont (art::serif (18.0f, true, 0.2f));
        g.drawText (text, stamp, juce::Justification::centred, false);
    }

    paintParts (g);

    // Memo / status.
    g.setColour (ink);
    g.setFont (art::serif (13.0f));
    g.drawFittedText ("Memo: " + status, memoArea, juce::Justification::topLeft, 3, 0.9f);

    // Dark band for the buttons (where a cheque's MICR line would be).
    g.setColour (background);
    g.fillRoundedRectangle (band.toFloat(), 6.0f);
}

void MatchPanel::paintParts (juce::Graphics& g) const
{
    using namespace colours;
    auto area = partsArea.toFloat();
    g.setColour (inkSoft);
    g.setFont (art::serif (10.0f, true, 0.25f));
    g.drawText ("ITEMISED", area.removeFromTop (14.0f), juce::Justification::centredLeft, false);
    g.setColour (ink.withAlpha (0.4f));
    g.drawHorizontalLine (static_cast<int> (area.getY()), area.getX(), area.getRight());
    area.removeFromTop (4.0f);

    constexpr int columns = 2, rows = (analysis::numParts + 1) / 2;
    const float gap = 14.0f, cellW = (area.getWidth() - gap) / columns, cellH = area.getHeight() / rows;
    for (int part = 0; part < analysis::numParts; ++part)
    {
        const int col = part / rows, row = part % rows;
        auto cell = juce::Rectangle<float> (area.getX() + static_cast<float> (col) * (cellW + gap), area.getY() + static_cast<float> (row) * cellH, cellW, cellH).reduced (0.0f, 2.0f);
        const int pc = getPartPercent (part);
        const bool applies = ! processor.getMatcher().hasTarget() || analysis::partApplies (processor.getMatcher().getTargetPrint(), part);

        auto line = cell.removeFromTop (cell.getHeight() - 6.0f);
        g.setColour (ink);
        g.setFont (art::serif (10.5f, true, 0.12f));
        g.drawText (analysis::partName (part), line, juce::Justification::centredLeft, false);
        g.setFont (art::mono (13.0f, true));
        g.drawText (pc >= 0 ? juce::String (pc) + "%" : juce::String (applies ? "--" : "n/a"), line, juce::Justification::centredRight, false);

        auto bar = cell.withHeight (4.0f).withY (cell.getBottom() - 4.0f);
        g.setColour (paperShade);
        g.fillRoundedRectangle (bar, 2.0f);
        if (pc >= 0)
        {
            g.setColour (pc >= 80 ? sealGreen : (pc >= 50 ? shiftInk : ledRed.darker (0.2f)));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * static_cast<float> (pc) / 100.0f), 2.0f);
        }
    }
}

void MatchPanel::resized()
{
    auto r = getLocalBounds().reduced (24, 20);
    auto header = r.removeFromTop (54);
    close.setBounds (header.removeFromRight (90).withSizeKeepingCentre (90, 30));

    band = r.removeFromBottom (54);
    r.removeFromBottom (12);

    auto right = r.removeFromRight (juce::roundToInt (static_cast<float> (r.getWidth()) * 0.34f));
    r.removeFromRight (20);
    infoArea = r.removeFromBottom (34);
    r.removeFromBottom (10);
    dropArea = r;

    amountArea = right.removeFromTop (juce::jmin (150, right.getHeight() / 2));
    right.removeFromTop (8);
    partsArea = right.removeFromTop (juce::jmin (104, right.getHeight() - 40));
    right.removeFromTop (6);
    memoArea = right;

    auto buttons = band.reduced (12, 10);
    const int w = (buttons.getWidth() - 4 * 12) / 5;
    for (auto* b : { &load_, &hearSample, &matchIt, &hearMatch, &deposit })
    {
        b->setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (12);
    }
}
}
