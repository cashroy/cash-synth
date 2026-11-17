#include "SynthVoice.h"

namespace cash
{
namespace
{
    constexpr float voiceGain = 0.3f;        // headroom for polyphony
    constexpr float envCutoffRange = 60.0f;  // semitones at 100 % env amount
    constexpr float lfoCutoffRange = 36.0f;  // semitones at 100 % LFO amount
    constexpr int filterUpdateInterval = 8;  // samples between coefficient updates

    // PolyBLEP residual to band-limit the saw/square discontinuities.
    float polyBlep (double t, double dt)
    {
        if (t < dt)
        {
            t /= dt;
            return static_cast<float> (t + t - t * t - 1.0);
        }
        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return static_cast<float> (t * t + t + t + 1.0);
        }
        return 0.0f;
    }

    double advance (double phase, double inc)
    {
        phase += inc;
        return phase >= 1.0 ? phase - std::floor (phase) : phase;
    }

    double semisToRatio (float semis) { return std::exp2 (static_cast<double> (semis) / 12.0); }
}

float CashVoice::oscillator (int wave, double t, double dt)
{
    switch (wave)
    {
        case 0: return static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * t));
        case 1: return static_cast<float> (1.0 - 4.0 * std::abs (t - 0.5));
        case 2: return static_cast<float> (2.0 * t - 1.0) - polyBlep (t, dt);
        default:
        {
            float v = t < 0.5 ? 1.0f : -1.0f;
            v += polyBlep (t, dt);
            v -= polyBlep (std::fmod (t + 0.5, 1.0), dt);
            return v;
        }
    }
}

void CashVoice::Svf::setup (float cutoffHz, float q, double sampleRate)
{
    // Topology-preserving state variable filter (Simper / Cytomic).
    const auto g = static_cast<float> (std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate));
    k = 1.0f / q;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

float CashVoice::Svf::process (float v0, int mode)
{
    const float v3 = v0 - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;

    switch (mode)
    {
        case 1:  return v1;                 // band-pass
        case 2:  return v0 - k * v1 - v2;   // high-pass
        default: return v2;                 // low-pass
    }
}

void CashVoice::setCurrentPlaybackSampleRate (double newRate)
{
    SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);
    if (newRate > 0.0)
    {
        ampEnv.setSampleRate (newRate);
        filterEnv.setSampleRate (newRate);
        cutoff.reset (newRate, 0.02);
    }
}

void CashVoice::startNote (int midiNote, float velocity, juce::SynthesiserSound*, int pitchWheel)
{
    const bool wasIdle = ! ampEnv.isActive();
    note = midiNote;
    bendSemis = bendToSemis (pitchWheel);

    const auto& p = ctx.params;
    velGain = 1.0f - p.velocitySens + p.velocitySens * velocity;

    if (wasIdle)
    {
        filter.reset();
        cutoff.setCurrentAndTargetValue (p.cutoffHz);
        phaseSub = phase1; // keep the sub locked to osc 1 at note start
    }

    ampEnv.setParameters (p.ampEnv);
    filterEnv.setParameters (p.filterEnv);
    ampEnv.noteOn();      // JUCE's ADSR restarts from its current level, so steals don't click
    filterEnv.noteOn();
    filterCountdown = 0;
}

void CashVoice::stopNote (float, bool allowTailOff)
{
    if (allowTailOff)
    {
        ampEnv.noteOff();
        filterEnv.noteOff();
    }
    else
    {
        ampEnv.reset();
        filterEnv.reset();
        clearCurrentNote();
    }
}

void CashVoice::pitchWheelMoved (int newValue)
{
    bendSemis = bendToSemis (newValue);
}

void CashVoice::renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    if (! isVoiceActive())
        return;

    const auto& p = ctx.params;
    const double sr = getSampleRate();
    const float nyquistSafe = static_cast<float> (sr * 0.45);
    const float q = 0.5f + p.resonance * p.resonance * 14.5f;
    const float keySemis = p.keyTrack * static_cast<float> (note - 60);

    ampEnv.setParameters (p.ampEnv);
    filterEnv.setParameters (p.filterEnv);
    cutoff.setTargetValue (p.cutoffHz);

    const double baseInc = juce::MidiMessage::getMidiNoteInHertz (note) / sr;
    const double inc1 = baseInc * semisToRatio (p.osc1Semis);
    const double inc2 = baseInc * semisToRatio (p.osc2Semis);
    const double incSub = baseInc * 0.5;

    const int numChannels = out.getNumChannels();
    const float* lfo = ctx.lfo + (startSample - ctx.lfoBase);

    for (int i = 0; i < numSamples; ++i)
    {
        const float lfoValue = lfo[i];
        const double ratio = semisToRatio (bendSemis + lfoValue * p.lfoToPitch);
        const double d1 = inc1 * ratio, d2 = inc2 * ratio, dSub = incSub * ratio;

        float s = 0.0f;
        if (p.osc1Level > 0.0f)  s += p.osc1Level * oscillator (p.osc1Wave, phase1, d1);
        if (p.osc2Level > 0.0f)  s += p.osc2Level * oscillator (p.osc2Wave, phase2, d2);
        if (p.subLevel > 0.0f)   s += p.subLevel * oscillator (0, phaseSub, dSub);
        if (p.noiseLevel > 0.0f) s += p.noiseLevel * (noise.nextFloat() * 2.0f - 1.0f);

        phase1 = advance (phase1, d1);
        phase2 = advance (phase2, d2);
        phaseSub = advance (phaseSub, dSub);

        const float fEnv = filterEnv.getNextSample();
        const float baseCutoff = cutoff.getNextValue();
        if (--filterCountdown <= 0)
        {
            const float semis = p.filterEnvAmt * fEnv * envCutoffRange + keySemis + lfoValue * p.lfoToCutoff * lfoCutoffRange;
            const float hz = juce::jlimit (20.0f, nyquistSafe, baseCutoff * static_cast<float> (semisToRatio (semis)));
            filter.setup (hz, q, sr);
            filterCountdown = filterUpdateInterval;
        }

        const float y = filter.process (s, p.filterType) * ampEnv.getNextSample() * velGain * voiceGain;

        for (int ch = 0; ch < numChannels; ++ch)
            out.addSample (ch, startSample + i, y);

        if (! ampEnv.isActive())
        {
            clearCurrentNote();
            break;
        }
    }
}
}
