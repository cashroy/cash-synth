#pragma once

#include <optional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Mutator.h"
#include "SoundPrint.h"
#include "SynthEngine.h"

// Cash Match (M6): drop in a sample, and a background search finds the patch
// that sounds closest. The match % is shown on a cheque; DEPOSIT applies it.
namespace cash
{
// Renders a patch offline with a private engine (no APVTS, so it can run on any
// thread). Only uses `ranges` to convert normalised values to real units.
class PatchRenderer
{
public:
    explicit PatchRenderer (const juce::AudioProcessorValueTreeState& ranges);

    // One note at SoundPrint::sampleRate, mono, full velocity.
    void render (const Patch&, int midiNote, double holdSeconds, double totalSeconds, juce::AudioBuffer<float>& out);

private:
    std::array<juce::RangedAudioParameter*, ParameterMap::numParams> params {};
    std::array<std::atomic<float>, ParameterMap::numParams> raw {};
    SynthEngine engine;
};

struct MatchBudget
{
    int explore = 48;      // random starting points
    int generations = 14;  // invest rounds around the best few
    int children = 12;     // tries per round
    int polishRounds = 2;  // knob-by-knob fine tuning passes
};

class CashMatcher final : private juce::Thread
{
public:
    static constexpr double maxSeconds = 2.5;

    explicit CashMatcher (const juce::AudioProcessorValueTreeState& ranges);
    ~CashMatcher() override;

    // Message thread. Both stop any running search. Returns an error ("" = ok).
    juce::String loadTarget (const juce::File&);
    juce::String setTarget (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name);
    void clearTarget();

    bool hasTarget() const { return target.print.valid; }
    const SoundPrint& getTargetPrint() const { return target.print; }
    const juce::AudioBuffer<float>& getTargetAudio() const { return target.audio; } // mono, SoundPrint::sampleRate
    juce::String getTargetName() const { return target.name; }

    // Start searching from `from` (the current sound counts as a candidate).
    void start (const Patch& from, const Patch& defaults, uint64_t seed, MatchBudget = {});
    void stop();
    bool isSearching() const { return isThreadRunning(); }
    void runBlocking (const Patch& from, const Patch& defaults, uint64_t seed, MatchBudget = {}); // tests

    struct Result
    {
        Patch patch {};
        int percent = 0;
        float distance = 1.0f;
        int tried = 0;
        float progress = 0.0f; // 0..1
        bool found = false;
    };
    Result getResult() const;
    juce::AudioBuffer<float> getBestRender() const; // mono, SoundPrint::sampleRate

    // Scores one patch against the target (not while a search runs).
    float score (const Patch&);

    // A first guess from the analysis: envelope, noise, brightness, wave.
    Patch guess (const Patch& defaults) const;

private:
    void run() override;
    void search();
    float evaluate (const Patch&, juce::AudioBuffer<float>* keep = nullptr);
    bool cancelled() const { return threadShouldExit() || cancel.load(); }
    float normalised (const char* id, float realValue) const;

    struct Target
    {
        juce::String name;
        juce::AudioBuffer<float> audio;
        SoundPrint print;
    };

    const juce::AudioProcessorValueTreeState& ranges;
    PatchRenderer renderer;
    juce::AudioBuffer<float> scratch;
    Target target;

    Patch start0 {}, defaults0 {};
    uint64_t seed0 = 1;
    MatchBudget budget;
    int expectedEvaluations = 1;
    std::atomic<bool> cancel { false };

    juce::CriticalSection resultLock;
    Result result;
    juce::AudioBuffer<float> bestRender;
};
}
