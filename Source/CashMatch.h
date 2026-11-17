#pragma once

#include <optional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Mutator.h"
#include "SoundPrint.h"
#include "SynthEngine.h"
#include "Mint.h"
#include "Layers.h"

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
    void setMint (const MintModel* m) { engine.setMint (m); } // M13: the model a Mint patch plays

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
    // M13: measure the sample into a Mint model and play that (sine partials +
    // noise made by the synth). False = the M9 search over the synth's own knobs only.
    bool mint = true;
    // M14: with the Mint on, which patches to make. EXACT = the Mint plays the
    // measurements; LAYERS = an editable patch of normal synth parts fitted to
    // them (`detail` tone layers). BOTH makes both; the cheque picks one.
    enum class Mode { exact, layers, both };
    Mode mode = Mode::both;
    int detail = layers::numTone;
    bool refit = false; // keep the last result's other patch (Detail changes re-fit the layers only)
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
        std::shared_ptr<const MintModel> mint; // M13: the measured sound the patch plays (null: synth knobs only)
        int tried = 0;
        float progress = 0.0f; // 0..1
        bool found = false;

        // M14: both answers. The fields above are EXACT's when it was made, else LAYERS'.
        struct Option
        {
            Patch patch {};
            int percent = 0;
            float distance = 1.0f;
            analysis::Parts parts {};
            std::shared_ptr<const MintModel> mint; // EXACT: the measurements; LAYERS: the wavetables only
            int layers = 0;                        // LAYERS: tone layers used
            bool found = false;
        };
        Option exact, layered;
        const Option& pick (bool layers) const { return layers ? layered : exact; }
    };
    Result getResult() const;
    juce::AudioBuffer<float> getBestRender() const; // stereo, SoundPrint::sampleRate
    juce::AudioBuffer<float> getRender (bool layers) const; // M14: EXACT's or LAYERS' render

    // Scores one patch against the target (not while a search runs).
    // A Mint patch is scored with the model of the last search (or none).
    float score (const Patch&);
    analysis::Parts scoreParts (const Patch&);

    // M13: the target measured into a Mint model (built on first use, then kept with the target).
    std::shared_ptr<const MintModel> getMintModel();
    // The patch that plays `model` like the target: every synth section off, the Mint on.
    Patch mintPatch (const Patch& defaults, const MintModel&) const;
    // M14: the patch that plays fitted layers: every other section off, layers + any LFOs they use on.
    Patch layersPatch (const Patch& defaults, const MintModel&, const layers::Fit&) const;

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
    void searchMint();
    void searchLayers (const MintModel&);
    const MintModel* modelFor (const Patch&) const; // the model a patch plays when scored
    void useMint (const MintModel*); // on every worker's renderer
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
    std::shared_ptr<const MintModel> targetMint; // measured from `target` (search thread or message thread, never both)
    std::shared_ptr<const MintModel> layerTables; // the last LAYERS fit's wavetables

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
    juce::AudioBuffer<float> bestRender, layersRender;
};
}
