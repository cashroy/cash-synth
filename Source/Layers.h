#pragma once

#include <array>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParameterMap.h"

// M14 LAYERS: an editable sound made of normal synth parts. Up to 8 tone layers
// (a one-cycle wavetable each, with its own tuning, level, pan, amp ADSR, filter
// + filter ADSR and LFO depths) and 2 band-passed noise layers, plus a shared
// pitch envelope. Cash Match fits them to a sample (LAYERS mode); every knob is a
// normal automatable parameter. The wavetables themselves are data: they ride in
// a MintModel (tables only, or alongside the Mint's measurements), shared and
// saved the same way.
namespace cash
{
struct MintModel;

namespace layers
{
    inline constexpr int numTone = 8, numNoise = 2;
    inline constexpr int numHarmonics = 64;               // per wavetable
    inline constexpr int tableSize = 2048;                // samples per cycle (+1 guard)
    inline constexpr int numMips = 7;                     // 64, 32, 16, 8, 4, 2, 1 harmonics

    inline constexpr const char* layersOn   = "layersOn";
    inline constexpr const char* detail     = "layerDetail";    // 1..8 tone layers on
    inline constexpr const char* pitchAmt   = "layerPitchAmt";  // +/-48 st at the note start
    inline constexpr const char* pitchTime  = "layerPitchTime"; // time to settle (s)

    enum ToneField
    {
        level, pan, octave, semi, fine,
        attack, decay, sustain, release,
        fltType, cutoff, reso, envAmt, fAttack, fDecay, fSustain, fRelease,
        l1Pitch, l1Level, l1Cutoff, l2Pitch, l2Level, l2Cutoff,
        curve,          // M14 part 2 (CashCode v10): decay + release shape, -100..100 % (0 = exponential)
        numToneFields
    };
    enum NoiseField { nLevel, nCentre, nWidth, nAttack, nDecay, nSustain, nRelease, nPan,
                      nSlope,   // M14 part 2 (CashCode v10): band edges 6 / 12 / 24 / 48 dB per octave
                      numNoiseFields };
    inline constexpr int numSlopes = 4;

    inline constexpr int numParams = 4 + numTone * numToneFields + numNoise * numNoiseFields;
    // The v9 set (before Curve and Slope were appended, in this order, after it).
    inline constexpr int numParamsV9 = numParams - numTone - numNoise;

    inline constexpr float lfoPitchRange = 2.0f;   // semitones at 100 %
    inline constexpr float lfoCutoffRange = 36.0f; // semitones at 100 %
    inline constexpr float envCutoffRange = 60.0f; // semitones at 100 %

    const char* toneId (int layer, int field);   // "lyr1Level" ..
    const char* noiseId (int layer, int field);  // "lnz1Level" ..
    // Where a field sits in ids() (Curve and Slope come after every v9 param).
    int toneIndex (int layer, int field);
    int noiseIndex (int layer, int field);
    // Power response |H(f)|^2 of a noise layer's filter as the Bank builds it (prewarped like its SVFs).
    float noiseResponse (float hz, float centreHz, float octaves, int slope, double sampleRate);
    // Env curve -> exponent of the dB falls: dB(t) = -60 (t / time)^p, p = 4^curve.
    inline float curveExponent (float curve) { return std::exp2 (2.0f * curve); }
    // Every layer param ID in ParameterMap order (shared four, tone layers, noise layers).
    const std::vector<const char*>& ids();
    // ParameterMap entries (weight 0: money moves leave layers alone).
    std::vector<ParamSpec> specs();
    void addParams (juce::AudioProcessorValueTreeState::ParameterLayout&);

    // Wavetable data in a MintModel: harmonic amplitudes [layer * numHarmonics + h-1]
    // (sine amplitude per harmonic, phase in `phases`, the right channel's phase lead
    // in `spreads`: the sample's stereo image). Builds the band-limited tables
    // (numMips per layer, left then right) the voices play.
    void buildTables (const std::vector<float>& harmonics, const std::vector<float>& phases,
                      const std::vector<float>& spreads, std::vector<float>& waves);
    // The tables a voice plays: the model's, or a plain saw for every layer.
    const float* tablesFor (const MintModel*);
    inline const float* table (const float* waves, int layer, int mip, int channel)
    {
        return waves + static_cast<size_t> (((channel * numTone + layer) * numMips + mip) * (tableSize + 1));
    }

    // Per-block settings (real units), filled by the engine.
    struct Tone
    {
        float gain = 0.0f, pan = 0.0f, semis = 0.0f;
        float a = 0.01f, d = 0.3f, s = 1.0f, r = 0.3f, p = 1.0f; // p: env curve exponent
        int type = 0;
        float cutoffHz = 20000.0f, q = 0.707f, envSemis = 0.0f;
        float fa = 0.01f, fd = 0.3f, fs = 1.0f, fr = 0.3f;
        float l1p = 0.0f, l1l = 0.0f, l1c = 0.0f, l2p = 0.0f, l2l = 0.0f, l2c = 0.0f; // st, depth, st
        bool open() const { return type == 0 && cutoffHz >= 19999.0f && envSemis == 0.0f && l1c == 0.0f && l2c == 0.0f; }
    };
    struct Noise
    {
        float gain = 0.0f, centreHz = 1000.0f, octaves = 2.0f, pan = 0.0f;
        float a = 0.01f, d = 0.3f, s = 1.0f, r = 0.3f;
        int slope = 0; // 0 = one band-pass (6 dB/oct edges), 1..3 = 1, 2, 4 high-pass + low-pass pairs
    };
    struct Settings
    {
        int detail = 1;
        float pitchAmt = 0.0f, pitchTime = 0.05f;
        std::array<Tone, numTone> tone {};
        std::array<Noise, numNoise> noise {};
    };

    // Exponential ADSR: linear attack, decay/release fall 60 dB in their time
    // (decay towards the sustain level). Starts from its current level.
    // `curve` exponent p (decay and release): dB(t) = -60 (t / time)^p. p = 1 is the
    // plain exponential (same maths as before); p > 1 holds then drops (a kick), p < 1
    // drops fast then lingers. Still -60 dB at the decay / release time either way.
    struct Env
    {
        enum Stage { idle, att, dec, rel };
        Stage stage = idle;
        float value = 0.0f;
        float aStep = 1.0f, dMul = 0.0f, rMul = 0.0f, sus = 1.0f;
        float a = -1.0f, d = -1.0f, r = -1.0f, p = 1.0f;
        static constexpr int curveSeg = 16;   // samples per curve segment (log-linear inside)
        float g = 1.0f, gMul = 1.0f, lg = 0.0f, tFall = 0.0f, segS = 0.0f, relStart = 0.0f;
        int seg = 0;
        void set (float attackS, float decayS, float sustainLevel, float releaseS, double sampleRate, float curveP = 1.0f);
        void on() { stage = att; }
        void off() { if (stage != idle) { stage = rel; startFall(); relStart = value; } }
        void reset() { stage = idle; value = 0.0f; }
        void startFall() { g = 1.0f; lg = 0.0f; tFall = 0.0f; seg = 0; }
        void nextSegment();
        float next()
        {
            switch (stage)
            {
                case att: value += aStep; if (value >= 1.0f) { value = 1.0f; stage = dec; startFall(); } break;
                case dec:
                    if (p == 1.0f)
                        value = sus + (value - sus) * dMul;
                    else
                    {
                        if (--seg <= 0) nextSegment();
                        g *= gMul;
                        value = sus + (1.0f - sus) * g;
                    }
                    break;
                case rel:
                    if (p == 1.0f)
                        value *= rMul;
                    else
                    {
                        if (--seg <= 0) nextSegment();
                        g *= gMul;
                        value = relStart * g;
                    }
                    if (value < 1.0e-5f) { value = 0.0f; stage = idle; }
                    break;
                case idle: break;
            }
            return value;
        }
    };

    // One voice's layers. Real-time safe.
    class Bank
    {
    public:
        void prepare (double sampleRate);
        void start (const Settings&, uint32_t seed); // note on (envelopes from where they are)
        void release();
        void kill();
        bool isActive() const;

        // One sample. `pitch` = the voice's note (MIDI, fractional, incl. bend/glide/mods).
        // Adds the layers' mid and side (L = mid*(1-p)+side convention: side = -mid*pan).
        void next (const Settings&, const float* waves, float pitch, float lfo1, float lfo2, float& mid, float& side);

    private:
        struct Svf
        {
            float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
            void setup (float hz, float q, double sr);
            float lp (float v0, float& band, float& high);
            float lpOnly (float v0) { float b, h; return lp (v0, b, h); }
            float hpOnly (float v0) { float b, h; lp (v0, b, h); return h; }
            void reset() { ic1 = ic2 = 0.0f; }
        };
        struct ToneState
        {
            double phase = 0.0;
            Env amp, flt;
            Svf svf, svfR;
            float gain = 0.0f;   // smoothed level
            int mip = 0, countdown = 0;
        };
        struct NoiseState
        {
            Env amp;
            Svf svf;
            std::array<Svf, 4> hp, lp; // steeper slopes: HP + LP pairs at the band edges
            float gain = 0.0f, norm = 1.0f;
            int countdown = 0;
        };
        std::array<ToneState, numTone> tones;
        std::array<NoiseState, numNoise> noises;
        double sr = 44100.0;
        float smooth = 0.001f;
        double sinceOn = 0.0;   // seconds since note on (pitch envelope)
        uint32_t rng = 1;
        float nextNoise() { rng = rng * 1664525u + 1013904223u; return static_cast<float> (static_cast<int32_t> (rng)) * (1.0f / 2147483648.0f); }
    };

    // ------------------------------------------------------------- fitting (Cash Match LAYERS)
    // Real-unit values for a patch, plus the wavetables. Built from a MintModel.
    struct Fit
    {
        std::vector<std::pair<const char*, float>> values;  // param id -> real value
        std::vector<float> harmonics, phases, spreads;      // numTone * numHarmonics
        int used = 0;                                       // tone layers that got partials
        float lfo1Hz = 0.0f, lfo2Hz = 0.0f;                 // LFO rates the layers' depths need (0 = LFO unused)
    };
    struct Wobbles { float vibRate = 0, vibDepth = 0, tremRate = 0, tremDepthDb = 0, wahRate = 0, wahDepth = 0; }; // SoundPrint units (st, dB, oct)
    // `key` = the MIDI note the match plays; `detail` = tone layers (1..8).
    Fit fit (const MintModel&, int key, int detail, const Wobbles&);
}
}
