#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Parameters.h"

CashSynthAudioProcessor::CashSynthAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "PARAMS", cash::param::createLayout()),
      engine (state)
{
    openBooks();
}

CashSynthAudioProcessor::~CashSynthAudioProcessor()
{
    matcher.stop();
}

void CashSynthAudioProcessor::openBooks()
{
    ledger.clear();
    ledger.record (getPatch(), "Opening balance");
}

void CashSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate;
    engine.prepare (sampleRate, samplesPerBlock);
}

void CashSynthAudioProcessor::releaseResources()
{
    engine.reset();
}

bool CashSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void CashSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const auto tempo = cash::tempo::fromPlayHead (getPlayHead()); // never typed in: always the host's
    lastBpm.store (tempo.bpm);
    lastSigNum.store (tempo.sigNum);
    lastSigDen.store (tempo.sigDen);
    lastPlaying.store (tempo.playing);
    engine.render (buffer, midi, tempo);
    preview.mixInto (buffer, hostRate.load());
    scope.push (buffer);
}

juce::AudioProcessorEditor* CashSynthAudioProcessor::createEditor()
{
    return new CashSynthAudioProcessorEditor (*this);
}

// Knob moves made since the last money move become their own transaction,
// so undo goes back to exactly what you heard.
void CashSynthAudioProcessor::recordHandTweaks()
{
    ledger.record (getPatch(), ledger.size() == 0 ? "Opening balance" : "Hand tweak");
}

void CashSynthAudioProcessor::setPatch (const cash::Patch& patch, const juce::String& memo)
{
    recordHandTweaks();
    cash::applyPatch (state, patch);
    ledger.record (getPatch(), memo);
}

void CashSynthAudioProcessor::invest (const cash::Investment& inv, const cash::LockMask& locks)
{
    const auto memo = "Invested " + juce::String (juce::roundToInt (inv.amount * 100.0f)) + "% @ "
                      + juce::String (juce::roundToInt (inv.risk * 100.0f)) + "% risk";
    setPatch (cash::Mutator::invest (getPatch(), inv, locks), memo);
}

void CashSynthAudioProcessor::hitJackpot (uint64_t seed, const cash::LockMask& locks)
{
    setPatch (cash::Mutator::jackpot (getPatch(), seed, locks), "Jackpot!");
}

bool CashSynthAudioProcessor::undo()
{
    recordHandTweaks();
    if (auto patch = ledger.undo())
    {
        cash::applyPatch (state, *patch);
        return true;
    }
    return false;
}

bool CashSynthAudioProcessor::redo()
{
    if (ledger.current() == nullptr)
        return false;
    if (! cash::samePatch (getPatch(), ledger.current()->patch)) // hand tweaks after an undo win: nothing to redo
    {
        recordHandTweaks();
        return false;
    }
    if (auto patch = ledger.redo())
    {
        cash::applyPatch (state, *patch);
        return true;
    }
    return false;
}

bool CashSynthAudioProcessor::canUndo() const
{
    if (ledger.canUndo())
        return true;
    const auto* now = ledger.current();
    return now != nullptr && ! cash::samePatch (getPatch(), now->patch); // hand tweaks to undo
}

bool CashSynthAudioProcessor::canRedo() const
{
    return ledger.canRedo() && cash::samePatch (getPatch(), ledger.current()->patch);
}

int CashSynthAudioProcessor::bankIt()
{
    const juce::ScopedLock sl (vaultLock);
    return vault.deposit (getPatch());
}

bool CashSynthAudioProcessor::shred (size_t index)
{
    const juce::ScopedLock sl (vaultLock);
    return vault.shred (index);
}

bool CashSynthAudioProcessor::withdraw (size_t index)
{
    if (index >= vault.size())
        return false;
    const auto note = vault.at (index); // copy: setPatch may not touch the vault, but stay safe
    setPatch (note.patch, "Withdrew " + note.serial);
    return true;
}

//============================================================================== Cash Match
void CashSynthAudioProcessor::startMatch (uint64_t seed, cash::MatchBudget budget)
{
    matcher.start (getPatch(), cash::defaultPatch (state), seed, budget);
}

bool CashSynthAudioProcessor::depositMatch()
{
    const auto r = matcher.getResult();
    if (! r.found)
        return false;
    // Matches are made in Hz. An LFO the user had in SYNC stays synced, on the note
    // value nearest the matched rate at the current host tempo.
    auto patch = r.patch;
    const auto now = getPatch();
    const auto tempo = getHostTempo();
    for (auto [syncId, noteId, rateId] : { std::tuple { cash::param::lfo1Sync, cash::param::lfo1Note, cash::param::lfoRate },
                                           std::tuple { cash::param::lfo2Sync, cash::param::lfo2Note, cash::param::lfo2Rate } })
    {
        const auto si = static_cast<size_t> (cash::ParameterMap::indexOf (syncId));
        if (now[si] < 0.5f)
            continue;
        const auto ni = cash::ParameterMap::indexOf (noteId);
        auto* rate = state.getParameter (rateId);
        const double hz = rate->convertFrom0to1 (patch[static_cast<size_t> (cash::ParameterMap::indexOf (rateId))]);
        patch[si] = 1.0f;
        patch[static_cast<size_t> (ni)] = cash::ParameterMap::fromStep (cash::ParameterMap::spec (ni), cash::tempo::nearestNote (hz, tempo));
    }
    setPatch (patch, "Cash Match: " + matcher.getTargetName() + " " + juce::String (r.percent) + "%");
    return true;
}

void CashSynthAudioProcessor::previewTarget()
{
    if (matcher.hasTarget())
        preview.play (matcher.getTargetAudio(), cash::SoundPrint::sampleRate);
}

void CashSynthAudioProcessor::previewMatch()
{
    const auto clip = matcher.getBestRender();
    if (clip.getNumSamples() > 0)
    {
        // Match the target's loudness so the A/B is fair.
        auto level = clip;
        const float a = matcher.getTargetAudio().getMagnitude (0, matcher.getTargetAudio().getNumSamples());
        const float b = level.getMagnitude (0, level.getNumSamples());
        if (b > 1.0e-6f && a > 0.0f)
            level.applyGain (a / b);
        preview.play (level, cash::SoundPrint::sampleRate);
    }
}

//============================================================================== Stash
juce::File CashSynthAudioProcessor::stashIt (const juce::String& name)
{
    const auto patch = getPatch();
    const auto file = stash.save (name, patch);
    if (file != juce::File())
    {
        stashName = cash::Stash::nameOf (file);
        stashPatch = patch;
    }
    return file;
}

bool CashSynthAudioProcessor::loadStash (const juce::File& file)
{
    const auto patch = cash::Stash::load (file, cash::defaultPatch (state));
    if (! patch)
        return false;
    stashName = cash::Stash::nameOf (file);
    setPatch (*patch, "Opened stash: " + stashName);
    stashPatch = getPatch();
    return true;
}

bool CashSynthAudioProcessor::isStashModified() const
{
    return stashName.isNotEmpty() && ! cash::samePatch (getPatch(), stashPatch);
}

//============================================================================== Project state
namespace
{
    const juce::Identifier stateType ("CashSynthState"), vaultType ("Vault"), noteType ("Note"), patchType ("Patch");
}

void CashSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root (stateType);
    root.setProperty ("version", stateVersion, nullptr);
    root.setProperty ("risk", risk, nullptr);
    root.setProperty ("stash", stashName, nullptr);
    root.setProperty ("stashModified", isStashModified(), nullptr);
    root.appendChild (state.copyState(), nullptr);

    juce::ValueTree vaultTree (vaultType);
    {
        const juce::ScopedLock sl (vaultLock);
        for (size_t i = 0; i < vault.size(); ++i)
        {
            const auto& note = vault.at (i);
            juce::ValueTree n (noteType);
            n.setProperty ("time", note.timeMs, nullptr);
            n.appendChild (cash::patchToTree (note.patch, patchType), nullptr);
            vaultTree.appendChild (n, nullptr);
        }
    }
    root.appendChild (vaultTree, nullptr);

    if (const auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

// Expected on the message thread (VST3/AU hosts do this); the vault is guarded
// anyway because getStateInformation may run elsewhere.
void CashSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    const auto root = juce::ValueTree::fromXml (*xml);
    const auto params = root.hasType (stateType) ? root.getChildWithName (state.state.getType())
                                                 : (root.hasType (state.state.getType()) ? root : juce::ValueTree());
    if (! params.isValid())
        return; // not ours: leave everything as it is

    // Older projects lack newer params. APVTS would leave those at their current
    // value, so add them at their defaults: an M6 project loads as M6 sounded.
    auto filled = params.createCopy();
    for (int i = 0; i < cash::ParameterMap::numParams; ++i)
    {
        const auto* id = cash::ParameterMap::spec (i).id;
        if (filled.getChildWithProperty ("id", id).isValid())
            continue;
        if (auto* p = state.getParameter (id))
        {
            juce::ValueTree child ("PARAM");
            child.setProperty ("id", id, nullptr);
            child.setProperty ("value", p->convertFrom0to1 (p->getDefaultValue()), nullptr);
            filled.appendChild (child, nullptr);
        }
    }
    state.replaceState (filled);

    if (root.hasType (stateType))
    {
        setRisk (static_cast<float> (root.getProperty ("risk", 0.5f)));

        const auto fallback = cash::defaultPatch (state);
        {
            const juce::ScopedLock sl (vaultLock);
            vault.clear();
            for (const auto& n : root.getChildWithName (vaultType))
                if (n.hasType (noteType))
                    vault.deposit (cash::patchFromTree (n.getChildWithName (patchType), fallback),
                                   static_cast<juce::int64> (n.getProperty ("time", 0)));
        }

        stashName = root.getProperty ("stash").toString();
        stashPatch = getPatch();
        if (static_cast<bool> (root.getProperty ("stashModified", false)))
            stashPatch.fill (-1.0f); // keep showing "modified" after reload
    }

    openBooks();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CashSynthAudioProcessor();
}
