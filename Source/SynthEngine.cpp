#include "SynthEngine.h"
#include "Parameters.h"

namespace cash
{
SynthEngine::SynthEngine (juce::AudioProcessorValueTreeState& state)
    : SynthEngine (RawLookup ([&state] (const char* id) { return state.getRawParameterValue (id); }))
{
}

SynthEngine::SynthEngine (const RawLookup& lookup)
{
    auto bind = [&lookup] (Raw& r, const char* id)
    {
        r.p = lookup (id);
        jassert (r.p != nullptr);
    };

    bind (osc1Wave, param::osc1Wave);
    bind (osc1Octave, param::osc1Octave);
    bind (osc1Semi, param::osc1Semi);
    bind (osc1Fine, param::osc1Fine);
    bind (osc1Level, param::osc1Level);
    bind (osc2Wave, param::osc2Wave);
    bind (osc2Octave, param::osc2Octave);
    bind (osc2Semi, param::osc2Semi);
    bind (osc2Fine, param::osc2Fine);
    bind (osc2Level, param::osc2Level);
    bind (subLevel, param::subLevel);
    bind (noiseLevel, param::noiseLevel);
    bind (filterType, param::filterType);
    bind (filterCutoff, param::filterCutoff);
    bind (filterReso, param::filterReso);
    bind (filterEnvAmt, param::filterEnvAmt);
    bind (filterKeyTrack, param::filterKeyTrack);
    bind (fltA, param::fltAttack);
    bind (fltD, param::fltDecay);
    bind (fltS, param::fltSustain);
    bind (fltR, param::fltRelease);
    bind (ampA, param::ampAttack);
    bind (ampD, param::ampDecay);
    bind (ampS, param::ampSustain);
    bind (ampR, param::ampRelease);
    bind (lfoWave, param::lfoWave);
    bind (lfoRate, param::lfoRate);
    bind (lfoToPitch, param::lfoToPitch);
    bind (lfoToCutoff, param::lfoToCutoff);
    bind (velocitySens, param::velocitySens);
    bind (masterGain, param::masterGain);
    bind (driveType, param::driveType);
    bind (driveAmount, param::driveAmount);
    bind (lfoToDrive, param::lfoToDrive);
    bind (lfo2Wave, param::lfo2Wave);
    bind (lfo2Rate, param::lfo2Rate);
    bind (lfo2Amount, param::lfo2Amount);
    bind (lfo2Target, param::lfo2Target);
    bind (pitchEnvAmt, param::pitchEnvAmt);

    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new CashVoice (ctx));
    synth.addSound (new CashSound());

    readParameters();
}

void SynthEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    synth.setCurrentPlaybackSampleRate (newSampleRate);
    lfoBuffer.assign (static_cast<size_t> (juce::jmax (maxBlockSize, 32)), 0.0f);
    lfo2Buffer.assign (lfoBuffer.size(), 0.0f);
    gain.reset (newSampleRate, 0.02);
    readParameters();
    gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGain.get()));
    reset();
}

void SynthEngine::reset()
{
    synth.allNotesOff (0, false);
    lfo1.phase = lfo2.phase = 0.0;
    // Oscillators free-run between notes; a reset makes renders repeatable (Cash Match relies on it).
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<CashVoice*> (synth.getVoice (i)))
            v->resetPhases();
}

int SynthEngine::getNumActiveVoices() const
{
    int n = 0;
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (synth.getVoice (i)->isVoiceActive())
            ++n;
    return n;
}

void SynthEngine::readParameters()
{
    auto& p = ctx.params;
    auto semis = [] (const Raw& oct, const Raw& semi, const Raw& fine)
    { return static_cast<float> (oct.asInt() * 12 + semi.asInt()) + fine.get() / 100.0f; };

    p.osc1Wave = osc1Wave.asInt();
    p.osc2Wave = osc2Wave.asInt();
    p.osc1Semis = semis (osc1Octave, osc1Semi, osc1Fine);
    p.osc2Semis = semis (osc2Octave, osc2Semi, osc2Fine);
    p.osc1Level = osc1Level.get();
    p.osc2Level = osc2Level.get();
    p.subLevel = subLevel.get();
    p.noiseLevel = noiseLevel.get();

    p.filterType = filterType.asInt();
    p.cutoffHz = filterCutoff.get();
    p.resonance = filterReso.get();
    p.filterEnvAmt = filterEnvAmt.get();
    p.keyTrack = filterKeyTrack.get();

    p.filterEnv = { fltA.get(), fltD.get(), fltS.get(), fltR.get() };
    p.ampEnv = { ampA.get(), ampD.get(), ampS.get(), ampR.get() };

    p.lfoToPitch = lfoToPitch.get();
    p.lfoToCutoff = lfoToCutoff.get();
    p.velocitySens = velocitySens.get();

    p.driveType = driveType.asInt();
    p.drive = driveAmount.get();
    p.lfoToDrive = lfoToDrive.get();
    p.lfo2Target = lfo2Target.asInt();
    p.lfo2Amount = lfo2Amount.get();
    p.pitchEnvSemis = pitchEnvAmt.get() * param::pitchEnvRange;

    lfo1.wave = lfoWave.asInt();
    lfo1.inc = static_cast<float> (lfoRate.get() / sampleRate);
    lfo2.wave = lfo2Wave.asInt();
    lfo2.inc = static_cast<float> (lfo2Rate.get() / sampleRate);
}

float SynthEngine::Lfo::next()
{
    const double t = phase;
    phase += inc;
    if (phase >= 1.0)
        phase -= 1.0;

    switch (wave)
    {
        case param::sine:     return static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * t));
        case param::triangle: return static_cast<float> (1.0 - 4.0 * std::abs (t - 0.5));
        case param::saw:      return static_cast<float> (2.0 * t - 1.0);
        default:              return t < 0.5 ? 1.0f : -1.0f;
    }
}

void SynthEngine::render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi)
{
    readParameters();
    gain.setTargetValue (juce::Decibels::decibelsToGain (masterGain.get(), -48.0f));

    const int total = buffer.getNumSamples();
    const int chunk = static_cast<int> (lfoBuffer.size());

    // Render in chunks so the LFO buffer never needs reallocating on the audio thread.
    for (int start = 0; start < total; start += chunk)
    {
        const int n = juce::jmin (chunk, total - start);
        for (int i = 0; i < n; ++i)
        {
            lfoBuffer[static_cast<size_t> (i)] = lfo1.next();
            lfo2Buffer[static_cast<size_t> (i)] = lfo2.next();
        }

        ctx.lfo = lfoBuffer.data();
        ctx.lfo2 = lfo2Buffer.data();
        ctx.lfoBase = start;
        synth.renderNextBlock (buffer, midi, start, n);
    }

    gain.applyGain (buffer, total);
}
}
