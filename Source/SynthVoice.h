#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace cash
{
// Per-block parameter snapshot shared (read-only) by all voices.
struct SynthParams
{
    int   osc1Wave = 2, osc2Wave = 2;
    float osc1Semis = 0.0f, osc2Semis = 0.0f; // octave*12 + semi + fine/100
    float osc1Level = 0.8f, osc2Level = 0.5f;
    float subLevel = 0.0f, noiseLevel = 0.0f;

    int   filterType = 0;
    float cutoffHz = 4000.0f, resonance = 0.2f, filterEnvAmt = 0.3f, keyTrack = 0.0f;

    juce::ADSR::Parameters ampEnv, filterEnv;

    float lfoToPitch = 0.0f;  // semitones
    float lfoToCutoff = 0.0f; // 0..1
    float velocitySens = 0.7f;

    // M6
    int   driveType = 0;          // param::DriveType
    float drive = 0.0f;           // 0..1
    float lfoToDrive = 0.0f;      // LFO 1 -> drive, 0..1
    int   lfo2Target = 0;         // param::LfoTarget
    float lfo2Amount = 0.0f;      // 0..1
    float pitchEnvSemis = 0.0f;   // filter env -> pitch, semitones at full env
};

struct VoiceContext
{
    SynthParams params;
    const float* lfo = nullptr;  // one LFO 1 value per sample of the current chunk
    const float* lfo2 = nullptr; // same for LFO 2
    int lfoBase = 0;             // buffer sample index that lfo[0] corresponds to
};

class CashSound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

class CashVoice final : public juce::SynthesiserVoice
{
public:
    explicit CashVoice (const VoiceContext& context) : ctx (context) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<CashSound*> (s) != nullptr; }
    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int pitchWheel) override;
    void stopNote (float velocity, bool allowTailOff) override;
    void pitchWheelMoved (int newValue) override;
    void controllerMoved (int, int) override {}
    void renderNextBlock (juce::AudioBuffer<float>&, int startSample, int numSamples) override;
    void setCurrentPlaybackSampleRate (double newRate) override;
    void resetPhases() { phase1 = phase2 = phaseSub = 0.0; } // engine reset only (free-running otherwise)

private:
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        void setup (float cutoffHz, float q, double sampleRate);
        float process (float in, int mode);
        void reset() { ic1 = ic2 = 0.0f; }
    };

    static float oscillator (int wave, double phase, double inc);
public:
    static float shape (int driveType, float x, float drive); // the drive stage (also used by tests)
private:
    static float bendToSemis (int wheel) { return (static_cast<float> (wheel) - 8192.0f) / 8192.0f * 2.0f; }

    const VoiceContext& ctx;
    juce::ADSR ampEnv, filterEnv;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 4000.0f };
    Svf filter;
    juce::Random noise;

    double phase1 = 0.0, phase2 = 0.0, phaseSub = 0.0;
    int note = 60;
    float velGain = 1.0f, bendSemis = 0.0f;
    int filterCountdown = 0;
};
}
