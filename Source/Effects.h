#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <functional>
#include "Tempo.h"

namespace cash
{
// M11: the effects after the voices: a stereo delay, then a reverb. Each has an
// on/off switch; while a switch is off and its fade has finished the effect does
// no work at all, so the output is bit-for-bit the dry signal (old patches sound
// exactly as before). Turning one on starts from a clean (silent) tail.
//
// Mixes: dry stays at full level up to 50 % and fades to 0 at 100 %; wet rises
// to full level at 50 %. Mix 0 = exactly dry.
class Effects
{
public:
    using Lookup = std::function<std::atomic<float>* (const char* paramId)>;
    explicit Effects (const Lookup&);

    static constexpr double maxDelaySeconds = 4.0;     // synced times past this are clamped
    static constexpr double maxPreDelaySeconds = 1.0;

    void prepare (double sampleRate, int maxBlockSize);
    void reset(); // clears the tails; the next block snaps times/sizes to their targets

    // In place on a stereo buffer (a mono buffer is processed as left only).
    void process (juce::AudioBuffer<float>&, const tempo::Info&);

    // The delay / pre-delay in seconds for the current params at this tempo.
    double delaySeconds (const tempo::Info&) const;
    double preDelaySeconds (const tempo::Info&) const;
    // Roughly how long the effects ring after the voices stop (for the host).
    double tailSeconds() const;

    bool isDelayRunning() const { return delay.running; }
    bool isReverbRunning() const { return reverb.running; }

private:
    struct Raw
    {
        std::atomic<float>* p = nullptr;
        float get() const { return p->load (std::memory_order_relaxed); }
        int asInt() const { return juce::roundToInt (get()); }
    };

    // A power-of-two ring buffer with a fractional (cubic) read.
    struct Line
    {
        std::vector<float> data;
        int mask = 0, write = 0;
        void allocate (int minLength);
        void clear();
        void push (float x) { data[static_cast<size_t> (write)] = x; write = (write + 1) & mask; }
        float read (float delaySamples) const; // 0 = the sample just pushed
        float readInt (int delaySamples) const { return data[static_cast<size_t> ((write - 1 - delaySamples) & mask)]; }
    };

    struct Fade // the on/off gate and the mix, shared by both effects
    {
        juce::SmoothedValue<float> gate { 0.0f }, mix { 0.0f };
    };

    struct Delay
    {
        bool running = false;
        Fade fade;
        juce::SmoothedValue<float> time { 1.0f }, feedback { 0.0f }, pingPong { 0.0f };
        std::array<Line, 2> line;
        std::array<float, 2> lp {}, hp {};
        float lpA = 1.0f, hpA = 0.0f;
    };

    static constexpr int numLines = 8;
    struct Reverb
    {
        bool running = false;
        Fade fade;
        juce::SmoothedValue<float> size { 0.5f }, decay { 2.0f }, preDelay { 0.0f }, width { 1.0f };
        std::array<Line, 2> pre;
        std::array<std::array<Line, 2>, 2> diffuser; // [channel][stage]
        std::array<Line, numLines> fdn;
        std::array<float, numLines> damp {}, gain {};
        float dampA = 1.0f;
    };

    void readTargets (const tempo::Info&);
    void snapAll();
    void processDelay (float* l, float* r, int n);
    void processReverb (float* l, float* r, int n);
    void updateReverbGains();

    static bool startOrStop (bool on, Fade&, bool& running); // true = do work this block

    Raw delayOn, delayTime, delaySync, delayNote, delayFeedback, delayMix, delayPingPong, delayTone;
    Raw reverbOn, reverbSize, reverbDecay, reverbDamp, reverbPreDelay, reverbPreSync, reverbPreNote, reverbWidth, reverbMix;

    double sampleRate = 44100.0;
    bool fresh = true; // first block after prepare/reset: jump to the targets
    tempo::Info lastTempo;
    Delay delay;
    Reverb reverb;
};
}
