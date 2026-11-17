#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Mint.h"
#include "Layers.h"

namespace cash
{
// Per-sample control lanes. The engine fills one value per sample of the current
// chunk for each lane (smoothed knobs, section on/off gates, choice crossfades),
// so automation never steps; voices only read them.
enum Lane : int
{
    laneOsc1Level, laneOsc2Level, laneSubLevel, laneNoiseLevel,
    laneOsc1Fine, laneOsc2Fine,                 // semitones (the fine part of the tuning)
    laneReso, laneEnvAmt, laneKeyTrack,
    laneLfoPitch, laneLfoCutoff, laneLfoDrive,  // LFO 1 amounts
    laneDrive, laneLfo2Amount, lanePitchEnv,    // pitch env in semitones
    laneDetune, laneSpread, laneModVib, laneVelCutoff,
    laneAmpSustain, laneFltSustain,
    laneMintLevel, laneMintNoise,               // M13 the Mint

    // M8 section gates, 0 = off .. 1 = on (ramped, so switching never clicks).
    laneOsc1On, laneOsc2On, laneSubNoiseOn, laneFilterOn, laneFilterEnvOn, laneDriveOn,
    laneLfo2On, // the LFO 2 buffer is gated already; tremolo's "1 - x" form needs the gate itself
    laneMintOn,
    laneLayersOn, // M14

    // Choice crossfades: weight of the new choice (0 = all previous, 1 = all new).
    laneOsc1Wave, laneOsc2Wave, laneFilterType, laneDriveType,

    // LFO 2 routing weight per target (param::LfoTarget order).
    laneToCutoff, laneToPitch, laneToAmp, laneToDrive, laneToOsc2,

    numLanes
};

// Per-block settings that don't need per-sample smoothing (choices, tuning
// steps, envelope times, things only read at note-on).
struct SynthParams
{
    int   osc1Wave = 2, osc2Wave = 2, prevOsc1Wave = 2, prevOsc2Wave = 2;
    float osc1Steps = 0.0f, osc2Steps = 0.0f; // octave*12 + semi (fine comes from its lane)

    int   filterType = 0, prevFilterType = 0;
    float cutoffHz = 4000.0f;

    juce::ADSR::Parameters ampEnv, filterEnv; // sustain levels come from their lanes

    float velocitySens = 0.7f;
    int   driveType = 0, prevDriveType = 0;   // param::DriveType
    float bendRange = 2.0f;                   // semitones at full wheel
    float mintTone = 0.0f, mintSpeed = 1.0f, mintFine = 0.0f; // M13 (fine in semitones)
    float mintStart = 0.0f, mintLoop = 1.0f;  // M13 polish: start point (0..1 of the hold part), loop length scale
    layers::Settings layers;                  // M14 LAYERS (gate in laneLayersOn)
};

struct VoiceContext
{
    SynthParams params;
    const float* lane[numLanes] {};  // lane[k][i] for chunk sample i
    bool moving[numLanes] {};        // the lane changes somewhere in this chunk
    const float* lfo = nullptr;      // LFO 1 (gated) per sample of the current chunk
    const float* lfo2 = nullptr;     // LFO 2 (gated)
    int lfoBase = 0;                 // buffer sample index that index 0 of the chunk arrays is
    float bend = 0.0f;               // pitch wheel, -1..1 (shared by every voice)
    float modWheel = 0.0f;           // CC 1, 0..1
    const MintModel* mint = nullptr; // M13: the measured sound the Mint plays (owned by the processor)
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

    // Unison place: -1..1 across the stack (scaled live by the detune and spread
    // lanes) and a level scale. A sounding voice (held, or stolen while it rings)
    // glides to its new place; a silent one jumps there. Call before play().
    void setUnison (float position, float gain);

    void render (juce::AudioBuffer<float>&, int startSample, int numSamples);
    void resetPhases (double offset); // engine reset only (oscillators free-run otherwise)

    int getNote() const { return note; }
    float getPitch() const { return pitch; }
    float getPan() const { return pan; }
    float getVelocity() const { return velocity; }

    // Engine bookkeeping.
    bool keyDown = false, sustained = false;
    uint64_t age = 0;

    static float shape (int driveType, float x, float drive); // the drive stage (also used by tests)

private:
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        void setup (float cutoffHz, float q, double sampleRate);
        float process (float in, int mode, int prevMode, float fade); // fade = weight of `mode`
        void reset() { ic1 = ic2 = 0.0f; }
        void copyCoefficients (const Svf& o) { a1 = o.a1; a2 = o.a2; a3 = o.a3; k = o.k; }
    };

    static float oscillator (int wave, double phase, double inc);
    void updateEnvelopes (float ampSustain, float fltSustain);

    const VoiceContext& ctx;
    double sampleRate = 44100.0;

    // M13: the Mint, rendered a few samples ahead (it works in small blocks).
    MintPlayer mint;
    layers::Bank layerBank; // M14
    float mintL[MintPlayer::blockSize] {}, mintR[MintPlayer::blockSize] {};
    int mintAt = 0, mintLen = 0;   // next unread sample / how many are buffered
    uint32_t mintSeed = 1;
    juce::ADSR ampEnv, filterEnv;
    float ampSustainSet = -1.0f, fltSustainSet = -1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 4000.0f };
    juce::SmoothedValue<float> unisonPos { 0.0f }, unisonGain { 1.0f };
    Svf filter;
    Svf filterSide; // M13 polish: the Mint's stereo difference (L-R) gets the same filter (it's linear: = filtering L and R)
    bool filterIdle = false; // the filter was fully bypassed: clear it before it comes back
    juce::Random noise;

    double phase1 = 0.0, phase2 = 0.0, phaseSub = 0.0;
    bool active = false;
    int note = 60;
    float velocity = 1.0f, velGain = 1.0f;
    float pitch = 60.0f, target = 60.0f, glideStep = 0.0f; // semitones (MIDI note), per sample
    float pan = 0.0f;
    int filterCountdown = 0;
};
}
