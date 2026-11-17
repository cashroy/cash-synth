#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Parameters.h"

CashSynthAudioProcessor::CashSynthAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "PARAMS", cash::param::createLayout()),
      engine (state)
{
}

void CashSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
}

void CashSynthAudioProcessor::releaseResources()
{
    engine.reset();
}

bool CashSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void CashSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    engine.render (buffer, midi);
}

juce::AudioProcessorEditor* CashSynthAudioProcessor::createEditor()
{
    return new CashSynthAudioProcessorEditor (*this);
}

void CashSynthAudioProcessor::getStateInformation (juce::MemoryBlock&) {}

void CashSynthAudioProcessor::setStateInformation (const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CashSynthAudioProcessor();
}
