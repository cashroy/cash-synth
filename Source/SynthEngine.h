#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "SynthVoice.h"
#include "Tempo.h"
#include "Effects.h"

namespace cash
{
// Subtractive engine: reads the APVTS once per block, runs the global LFOs and
// owns its 16 voices. Poly (with unison), or Mono / Legato with a held-note stack,
// note priority and glide. MIDI is omni.
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
    // `tempo` = the host's BPM, time signature and song position at the block start
    // (default: 120 BPM, 4/4, stopped). Synced LFO rates follow it; while it plays
    // they lock their phase to the song position.
    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const tempo::Info& tempo = {});

    // M13: the model the Mint plays (owned by the caller, any thread). A model
    // stays in use until the audio thread's next block starts; mayBeUsing() says
    // when it is safe to free a model handed over earlier.
    void setMint (const MintModel* m) { wantedMint.store (m); }
    const MintModel* getMint() const { return wantedMint.load(); }
    bool mayBeUsing (const MintModel* m) const
    {
        const auto* u = usedMint.load();
        return m != nullptr && (u == m || u == busy());
    }

    // M11: delay + reverb after the voices (before the master gain).
    const Effects& getEffects() const { return fx; }

    double getLfoPhase (int which) const { return (which == 0 ? lfo1 : lfo2).phase; } // 0..1, for tests

    int getNumActiveVoices() const;
    int getMonoNote() const { return monoGate ? monoNote : -1; } // sounding mono note, -1 if none
    float getVoicePitch (int index) const { return voices[static_cast<size_t> (index)]->getPitch(); }
    float getVoicePan (int index) const { return voices[static_cast<size_t> (index)]->getPan(); }
    bool isVoiceActive (int index) const { return voices[static_cast<size_t> (index)]->isActive(); }

private:
    void readParameters();
    void handleMidi (const juce::MidiMessage&);
    void renderVoices (juce::AudioBuffer<float>&, int start, int num);

    void snapSmoothers();
    void fillLanes (int numSamples);
    void setChoice (int newValue, int& now, int& prev, Lane fade);

    // Held notes carry over when the voice mode or unison count changes.
    void changeVoicing (int newMode, int newUnison);
    void regroup (int note, int count);           // poly: give a held note `count` voices
    void startPolyNote (int note, float velocity, float fromPitch);

    void noteOn (int note, float velocity);
    void noteOff (int note);
    void setSustain (bool down);
    void releaseAll();
    void killAll();

    // Poly
    CashVoice* allocate (uint64_t notBefore);
    // Mono / Legato
    struct Held { int note; float velocity; };
    const Held* priorityNote() const;
    void monoPlay (const Held&, bool legatoContext);
    void releaseMono();

    CashVoice::Glide glide() const;
    int unisonCount() const;
    void placeUnison (CashVoice&, int index, int count) const;

    struct Lfo
    {
        double phase = 0.0;
        int wave = 0, prevWave = 0;
        bool synced = false;
        double lengthQn = 1.0;   // synced cycle length in quarter notes
        juce::SmoothedValue<float> rate { 1.0f }, gate { 1.0f }, fade { 1.0f };
        static float shape (int wave, double t);
    };
    void fillLfo (Lfo&, std::vector<float>& out, int numSamples, int startSample);

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
    Raw voiceMode, glideTime, glideMode, glideType, notePriority, unisonVoices, unisonDetune, unisonSpread;
    Raw bendRange, modVibrato, velToCutoff;
    Raw osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn;
    Raw lfo1Sync, lfo1Note, lfo2Sync, lfo2Note;
    Raw mintOn, mintLevel, mintNoise, mintTone, mintSpeed, mintFine, mintStart, mintLoop;
    std::array<Raw, layers::numParams> layerRaw; // M14, in layers::ids() order

    std::atomic<const MintModel*> wantedMint { nullptr }, usedMint { nullptr };
    const MintModel* busy() const { return reinterpret_cast<const MintModel*> (&usedMint); } // "picking one up now"

    tempo::Info hostTempo;       // this block's
    bool wasLocked = false;      // the song position was running last block
    double expectedPpq = 0.0;    // where it should be now if nobody seeked

    std::array<juce::SmoothedValue<float>, numLanes> smooth;
    std::vector<float> laneData; // numLanes x chunk

    std::vector<std::unique_ptr<CashVoice>> voices;
    VoiceContext ctx;

    int mode = 0, priority = 0, unison = 1, glideModeNow = 0;
    float glideSeconds = 0.0f;
    bool glideByRate = false;

    std::vector<Held> held;      // keys down, in press order (reserved: no allocation on the audio thread)
    bool sustainDown = false;
    int monoNote = -1;           // the note the mono voices are on
    bool monoGate = false;       // mono voices are sounding (key or pedal holds them)
    float lastPitch = -1.0f;     // last note played, for glide (-1 = none yet)
    uint64_t clock = 0;          // voice age stamps
    std::vector<float> lfoBuffer, lfo2Buffer;
    double sampleRate = 44100.0;
    Lfo lfo1, lfo2;
    juce::SmoothedValue<float> gain { 0.5f };
    Effects fx;
};
}
