#pragma once

#include <optional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Mutator.h"
#include "SoundPrint.h"
#include "SynthEngine.h"

// Cash Match (M6, v2 in M9): drop in a sample, and a background search finds the
// patch that sounds closest. The match % (and a score per part) is shown on a
// cheque; DEPOSIT applies it. Candidates are rendered on several threads.
namespace cash
{
// Renders a patch offline with a private engine (no APVTS, so it can run on any
// thread). Only uses `ranges` to convert normalised values to real units.
class PatchRenderer
{
public:
    explicit PatchRenderer (const juce::AudioProcessorValueTreeState& ranges);

    // One note at SoundPrint::sampleRate, stereo, full velocity.
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
    static constexpr double maxSeconds = 10.0; // the whole sample, up to this

    explicit CashMatcher (const juce::AudioProcessorValueTreeState& ranges);
    ~CashMatcher() override;

    // Message thread. Both stop any running search. Returns an error ("" = ok).
    juce::String loadTarget (const juce::File&);
    juce::String setTarget (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name);
    void clearTarget();

    bool hasTarget() const { return target.print.valid; }
    const SoundPrint& getTargetPrint() const { return target.print; }
    const juce::AudioBuffer<float>& getTargetAudio() const { return target.audio; } // stereo, SoundPrint::sampleRate
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
        analysis::Parts parts {};  // per part, 0 = identical (analysis::matchPercent for a %)
        int sectionsOff = 0;       // section switches the match turned off
        int tried = 0;
        float progress = 0.0f; // 0..1
        bool found = false;
    };
    Result getResult() const;
    juce::AudioBuffer<float> getBestRender() const; // stereo, SoundPrint::sampleRate

    // Scores one patch against the target (not while a search runs).
    float score (const Patch&);
    analysis::Parts scoreParts (const Patch&);

    int getNumWorkers() const { return numWorkers; }

    // A first guess from the analysis: envelope, noise, brightness, wave, and (M9)
    // pitch env, vibrato/wah/tremolo, unison width and the section switches.
    // `features` (optional) gets the params the analysis measured directly; they
    // can be laid over any patch (`overlay`) without its tone guesses.
    Patch guess (const Patch& defaults, LockMask* features = nullptr) const;
    static Patch overlay (const Patch& base, const Patch& guessed, const LockMask& features);

private:
    struct Eval
    {
        float d = 1.0f;
        analysis::Parts parts {};
    };
    struct Worker
    {
        explicit Worker (const juce::AudioProcessorValueTreeState& r) : renderer (r) {}
        PatchRenderer renderer;
        juce::AudioBuffer<float> scratch;
    };

    void run() override;
    void search();
    void ensureWorkers();
    Eval evaluate (Worker&, const Patch&, juce::AudioBuffer<float>* keep = nullptr);
    std::vector<Eval> evaluateBatch (const std::vector<Patch>&); // in parallel, results in order
    bool cancelled() const { return threadShouldExit() || cancel.load(); }
    float normalised (const char* id, float realValue) const;

    struct Target
    {
        juce::String name;
        juce::AudioBuffer<float> audio;
        SoundPrint print;
    };

    const juce::AudioProcessorValueTreeState& ranges;
    Target target;

    const int numWorkers;
    std::vector<std::unique_ptr<Worker>> workers;  // [0] runs on the search thread itself
    std::unique_ptr<juce::ThreadPool> pool;        // the helpers (after `workers`: stops first)
    std::atomic<int> batchActive { 0 };
    juce::WaitableEvent batchDone;
    std::atomic<int> tried { 0 };

    Patch start0 {}, defaults0 {};
    uint64_t seed0 = 1;
    MatchBudget budget;
    std::atomic<int> expectedEvaluations { 1 };
    std::atomic<bool> cancel { false };

    juce::CriticalSection resultLock;
    Result result;
    juce::AudioBuffer<float> bestRender;
};
}
