#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

TEST_CASE ("Processor reports synth basics", "[processor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;

    CHECK (p.getName() == "Cash Synth");
    CHECK (p.acceptsMidi());
    CHECK_FALSE (p.producesMidi());
    CHECK (p.getTotalNumInputChannels() == 0);
    CHECK (p.getTotalNumOutputChannels() == 2);
}

TEST_CASE ("Processor outputs silence without MIDI", "[processor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    p.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            buffer.setSample (ch, i, 0.5f); // garbage the processor must clear

    juce::MidiBuffer midi;
    p.processBlock (buffer, midi);

    CHECK (buffer.getMagnitude (0, 512) == 0.0f);
    p.releaseResources();
}

TEST_CASE ("Editor builds a control for every parameter", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;

    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    REQUIRE (editor != nullptr);
    CHECK (editor->getWidth() > 0);
    CHECK (editor->getHeight() > 0);

    auto* cashEditor = dynamic_cast<CashSynthAudioProcessorEditor*> (editor.get());
    REQUIRE (cashEditor != nullptr);
    CHECK (cashEditor->getAdvancedPanel().getNumControls() == p.getParameters().size());
}

// Hidden (run with "[snapshot]"): renders the editor to $CASH_SNAPSHOT as a PNG for visual checks.
TEST_CASE ("Editor snapshot", "[.][snapshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());

    const auto path = juce::SystemStats::getEnvironmentVariable ("CASH_SNAPSHOT", {});
    REQUIRE (path.isNotEmpty());
    juce::File file (path);
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (stream.openedOk());
    juce::PNGImageFormat png;
    CHECK (png.writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), stream));
}
