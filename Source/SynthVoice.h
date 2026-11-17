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

    // M7
    float bendRange = 2.0f;       // semitones at full wheel
    float modVibratoSemis = 0.0f; // LFO 1 pitch depth added at full mod wheel
    float velCutoffSemis = 0.0f;  // cutoff drop for velocity 0 (scaled by 1 - velocity)
};

struct VoiceContext
{
    SynthParams params;
    const float* lfo = nullptr;  // one LFO 1 value per sample of the current chunk
    const float* lfo2 = nullptr; // same for LFO 2
    int lfoBase = 0;             // buffer sample index that lfo[0] corresponds to
    float bend = 0.0f;           // pitch wheel, -1..1 (shared by every voice)
    float modWheel = 0.0f;       // CC 1, 0..1
};

// One voice. Owned and allocated by SynthEngine (no juce::Synthesiser: it has
// no mono mode, and a gliding mono voice changes note while it plays).
class CashVoice final
{
public:
    explicit CashVoice (const VoiceContext& context) : ctx (context) {}

    struct Glide
    {
        float seconds = 0.0f;
        bool constantRate = false; // true: `seconds` per octave; false: `seconds` for any interval
    };

    void setSampleRate (double newRate);

    // Starts the note (envelopes from their current level, so steals don't click),
    // or with retrigger = false just moves the pitch (legato). `fromPitch` >= 0
    // glides from that pitch (MIDI note number, fractional allowed).
    void play (int midiNote, float velocity, bool retrigger, float fromPitch, Glide);
    void release();
    void kill();
    bool isActive() const { return active; }

    // Unison place: detune offset in semitones, pan -1..1, and a level scale.
    void setUnison (float detuneSemis, float pan, float gain);

    void render (juce::AudioBuffer<float>&, int startSample, int numSamples);
    void resetPhases (double offset); // engine reset only (oscillators free-run otherwise)

    int getNote() const { return note; }
    float getPitch() const { return pitch; }
    float getPan() const { return pan; }

    // Engine bookkeeping.
    bool keyDown = false, sustained = false;
    uint64_t age = 0;

    static float shape (int driveType, float x, float drive); // the drive stage (also used by tests)

private:
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        void setup (float cutoffHz, float q, double sampleRate);
        float process (float in, int mode);
        void reset() { ic1 = ic2 = 0.0f; }
    };

    static float oscillator (int wave, double phase, double inc);

    const VoiceContext& ctx;
    double sampleRate = 44100.0;
    juce::ADSR ampEnv, filterEnv;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 4000.0f };
    Svf filter;
    juce::Random noise;

    double phase1 = 0.0, phase2 = 0.0, phaseSub = 0.0;
    bool active = false;
    int note = 60;
    float velocity = 1.0f, velGain = 1.0f;
    float pitch = 60.0f, target = 60.0f, glideStep = 0.0f; // semitones (MIDI note), per sample
    float detune = 0.0f, pan = 0.0f, unisonGain = 1.0f;
    int filterCountdown = 0;
};
}
