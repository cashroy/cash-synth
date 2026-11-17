#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "SynthEngine.h"
#include "Ledger.h"
#include "Mutator.h"
#include "Vault.h"
#include "ScopeBuffer.h"
#include "Stash.h"
#include "CashMatch.h"
#include "PreviewPlayer.h"

class CashSynthAudioProcessor final : public juce::AudioProcessor
{
public:
    CashSynthAudioProcessor();
    ~CashSynthAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return engine.getEffects().tailSeconds(); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() { return state; }
    const cash::SynthEngine& getEngine() const { return engine; }

    // Money moves (message thread). Each records to the ledger, so it can be undone.
    cash::Patch getPatch() const { return cash::capturePatch (state); }
    void setPatch (const cash::Patch&, const juce::String& memo);
    void invest (const cash::Investment&, const cash::LockMask& locks = {});
    void hitJackpot (uint64_t seed, const cash::LockMask& locks = {});
    bool undo();
    bool redo();
    const cash::Ledger& getLedger() const { return ledger; }
    bool canUndo() const; // true if undo would change the sound (counts unrecorded hand tweaks)
    bool canRedo() const;

    // Vault: patches you chose to keep (saved with the project).
    int bankIt();                // index, or -1 if the vault is full
    bool withdraw (size_t index); // loads a banked patch (undoable)
    bool shred (size_t index);
    const cash::Vault& getVault() const { return vault; }

    // Risk setting the UI invests with (kept here so it survives closing the editor).
    float getRisk() const { return risk; }
    void setRisk (float r) { risk = juce::jlimit (0.0f, 1.0f, r); }

    const cash::ScopeBuffer& getScope() const { return scope; }

    // M10: the host tempo the last block played at (120 BPM, 4/4 without a host).
    cash::tempo::Info getHostTempo() const
    {
        cash::tempo::Info t;
        t.bpm = lastBpm.load();
        t.sigNum = lastSigNum.load();
        t.sigDen = lastSigDen.load();
        t.playing = lastPlaying.load();
        return t;
    }

    // Stash: named presets on disk (shared by every project).
    cash::Stash& getStash() { return stash; }
    juce::File stashIt (const juce::String& name);  // saves the current sound; {} on failure
    bool loadStash (const juce::File&);             // undoable, like any money move
    juce::String getStashName() const { return stashName; } // last stash saved/loaded ("" if none)
    bool isStashModified() const; // sound changed since that stash was saved/loaded

    // Cash Match: analyse a sample, search for the closest patch, deposit it (undoable).
    cash::CashMatcher& getMatcher() { return matcher; }
    void startMatch (uint64_t seed, cash::MatchBudget = {});
    bool depositMatch();                 // applies the best match as a ledger entry
    void previewTarget();                // hear the sample
    void previewMatch();                 // hear the best match (rendered offline)
    void stopPreview() { preview.stop(); }
    bool isPreviewing() const { return preview.isPlaying(); }

    // Project state: version, risk, current stash name, APVTS params, vault.
    // The Ledger is NOT saved: it is session history (like a DAW's undo); after a
    // load it restarts with a fresh "Opening balance" of the loaded sound.
    static constexpr int stateVersion = 1;

private:
    juce::AudioProcessorValueTreeState state;
    cash::SynthEngine engine;
    cash::Ledger ledger;
    cash::Vault vault;
    cash::ScopeBuffer scope;
    float risk = 0.5f;
    cash::Stash stash;
    juce::String stashName;
    cash::Patch stashPatch {};
    juce::CriticalSection vaultLock; // getStateInformation may run off the message thread
    cash::PreviewPlayer preview;
    std::atomic<double> hostRate { 44100.0 };
    std::atomic<double> lastBpm { cash::tempo::defaultBpm };
    std::atomic<int> lastSigNum { 4 }, lastSigDen { 4 };
    std::atomic<bool> lastPlaying { false };
    cash::CashMatcher matcher { state }; // after `state`: it borrows its parameter ranges

    void recordHandTweaks();
    void openBooks(); // clears the ledger and records the current sound as the opening balance

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CashSynthAudioProcessor)
};
