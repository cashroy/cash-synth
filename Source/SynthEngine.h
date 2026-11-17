#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "SynthVoice.h"

namespace cash
{
// Polyphonic subtractive engine: reads the APVTS once per block, runs a global
// LFO, and renders voices through juce::Synthesiser.
class SynthEngine
{
public:
    static constexpr int numVoices = 16;

    explicit SynthEngine (juce::AudioProcessorValueTreeState& state);

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi);

    int getNumActiveVoices() const;

private:
    void readParameters();
    float nextLfo();

    struct Raw
    {
        std::atomic<float>* p = nullptr;
        float get() const { return p->load (std::memory_order_relaxed); }
        int asInt() const { return juce::roundToInt (get()); }
    };

    Raw osc1Wave, osc1Octave, osc1Semi, osc1Fine, osc1Level;
    Raw osc2Wave, osc2Octave, osc2Semi, osc2Fine, osc2Level;
    Raw subLevel, noiseLevel;
    Raw filterType, filterCutoff, filterReso, filterEnvAmt, filterKeyTrack;
    Raw fltA, fltD, fltS, fltR, ampA, ampD, ampS, ampR;
    Raw lfoWave, lfoRate, lfoToPitch, lfoToCutoff;
    Raw velocitySens, masterGain;

    juce::Synthesiser synth;
    VoiceContext ctx;
    std::vector<float> lfoBuffer;
    double sampleRate = 44100.0;
    double lfoPhase = 0.0;
    int lfoWaveNow = 0;
    float lfoInc = 0.0f;
    juce::SmoothedValue<float> gain { 0.5f };
};
}
