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

    // Reads raw (real-unit) values from anywhere, e.g. Cash Match's offline renderer.
    using RawLookup = std::function<std::atomic<float>* (const char* paramId)>;
    explicit SynthEngine (const RawLookup&);

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi);

    int getNumActiveVoices() const;

private:
    void readParameters();

    struct Lfo
    {
        double phase = 0.0;
        float inc = 0.0f;
        int wave = 0;
        float next();
    };

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
    Raw driveType, driveAmount, lfoToDrive, lfo2Wave, lfo2Rate, lfo2Amount, lfo2Target, pitchEnvAmt;

    juce::Synthesiser synth;
    VoiceContext ctx;
    std::vector<float> lfoBuffer, lfo2Buffer;
    double sampleRate = 44100.0;
    Lfo lfo1, lfo2;
    juce::SmoothedValue<float> gain { 0.5f };
};
}
