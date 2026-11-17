#include "SynthVoice.h"
#include "Parameters.h"

namespace param2 = cash::param;

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

float CashVoice::shape (int type, float x, float drive)
{
    if (drive <= 0.0f)
        return x;
    // Up to ~+30 dB into the shaper; fade the wet signal in over the first 25 %
    // so tiny amounts don't jump, and pull the level back so louder != better.
    const float pre = std::exp (drive * 3.5f);
    const float in = x * pre;
    float y;
    switch (type)
    {
        case 1:  y = juce::jlimit (-1.0f, 1.0f, in); break;                                   // hard clip
        case 2:  y = std::sin (in * juce::MathConstants<float>::halfPi); break;              // wavefold
        default: y = std::tanh (in); break;                                                   // soft clip
    }
    const float wet = juce::jmin (1.0f, drive * 4.0f);
    const float makeup = 1.0f / (1.0f + drive * 1.5f);
    return x * (1.0f - wet) + y * wet * makeup * 1.6f;
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

void CashVoice::setSampleRate (double newRate)
{
    if (newRate <= 0.0)
        return;
    sampleRate = newRate;
    ampEnv.setSampleRate (newRate);
    filterEnv.setSampleRate (newRate);
    cutoff.reset (newRate, 0.02);
}

void CashVoice::resetPhases (double offset)
{
    phase1 = phase2 = phaseSub = offset - std::floor (offset);
}

void CashVoice::setUnison (float detuneSemis, float newPan, float gain)
{
    detune = detuneSemis;
    pan = juce::jlimit (-1.0f, 1.0f, newPan);
    unisonGain = gain;
}

void CashVoice::play (int midiNote, float vel, bool retrigger, float fromPitch, Glide glide)
{
    const auto& p = ctx.params;
    note = midiNote;
    target = static_cast<float> (midiNote);

    const bool glides = fromPitch >= 0.0f && glide.seconds > 0.0f;
    pitch = glides ? fromPitch : target;
    if (glides)
    {
        const float samples = static_cast<float> (glide.seconds * sampleRate);
        const float distance = glide.constantRate ? 12.0f : std::abs (target - pitch);
        glideStep = distance / juce::jmax (1.0f, samples);
    }

    if (! retrigger && active)
        return; // legato: envelopes carry on, only the pitch moves

    velocity = vel;
    velGain = 1.0f - p.velocitySens + p.velocitySens * vel;

    if (! active)
    {
        filter.reset();
        cutoff.setCurrentAndTargetValue (p.cutoffHz);
        phaseSub = phase1; // keep the sub locked to osc 1 at note start
    }

    active = true;
    ampEnv.setParameters (p.ampEnv);
    filterEnv.setParameters (p.filterEnv);
    ampEnv.noteOn();      // JUCE's ADSR restarts from its current level, so steals don't click
    filterEnv.noteOn();
    filterCountdown = 0;
}

void CashVoice::release()
{
    keyDown = sustained = false;
    ampEnv.noteOff();
    filterEnv.noteOff();
}

void CashVoice::kill()
{
    keyDown = sustained = false;
    ampEnv.reset();
    filterEnv.reset();
    active = false;
}

void CashVoice::render (juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    if (! active)
        return;

    const auto& p = ctx.params;
    const double sr = sampleRate;
    const float nyquistSafe = static_cast<float> (sr * 0.45);
    const float q = 0.5f + p.resonance * p.resonance * 14.5f;
    const float velSemis = p.velCutoffSemis * (velocity - 1.0f);

    ampEnv.setParameters (p.ampEnv);
    filterEnv.setParameters (p.filterEnv);
    cutoff.setTargetValue (p.cutoffHz);

    const double hzPerSemitoneBase = 440.0 / sr; // A4 = 69
    const double ratio1 = semisToRatio (p.osc1Semis), ratio2 = semisToRatio (p.osc2Semis);
    const float fixedSemis = detune + ctx.bend * p.bendRange - 69.0f;
    const float vibrato = p.lfoToPitch + ctx.modWheel * p.modVibratoSemis;

    // Stereo: centre = 1/1 (same as mono), hard side = 0/2, so a mono sum never changes.
    const int numChannels = out.getNumChannels();
    const float gainL = numChannels > 1 ? 1.0f - pan : 1.0f;
    const float gainR = numChannels > 1 ? 1.0f + pan : 1.0f;

    const float* lfo = ctx.lfo + (startSample - ctx.lfoBase);
    const float* lfo2 = ctx.lfo2 != nullptr ? ctx.lfo2 + (startSample - ctx.lfoBase) : nullptr;
    const float mod2 = lfo2 != nullptr ? p.lfo2Amount : 0.0f;
    auto routed = [&] (int target_, float v) { return p.lfo2Target == target_ ? v * mod2 : 0.0f; };

    for (int i = 0; i < numSamples; ++i)
    {
        if (pitch != target)
            pitch = pitch < target ? juce::jmin (target, pitch + glideStep) : juce::jmax (target, pitch - glideStep);

        const float lfoValue = lfo[i];
        const float lfo2Value = lfo2 != nullptr ? lfo2[i] : 0.0f;
        const float fEnv = filterEnv.getNextSample();

        const float pitchMod = lfoValue * vibrato + fEnv * p.pitchEnvSemis
                             + routed (param2::toPitch, lfo2Value) * mod2 * 12.0f; // amount^2 * 12 st
        const double base = hzPerSemitoneBase * semisToRatio (pitch + fixedSemis + pitchMod);
        const double d1 = base * ratio1, d2 = base * ratio2, dSub = base * 0.5;

        const float osc2Level = p.osc2Level * juce::jmax (0.0f, 1.0f + routed (param2::toOsc2, lfo2Value));

        float s = 0.0f;
        if (p.osc1Level > 0.0f)  s += p.osc1Level * oscillator (p.osc1Wave, phase1, d1);
        if (osc2Level > 0.0f)    s += osc2Level * oscillator (p.osc2Wave, phase2, d2);
        if (p.subLevel > 0.0f)   s += p.subLevel * oscillator (0, phaseSub, dSub);
        if (p.noiseLevel > 0.0f) s += p.noiseLevel * (noise.nextFloat() * 2.0f - 1.0f);

        phase1 = advance (phase1, d1);
        phase2 = advance (phase2, d2);
        phaseSub = advance (phaseSub, dSub);

        const float baseCutoff = cutoff.getNextValue();
        if (--filterCountdown <= 0)
        {
            const float semis = p.filterEnvAmt * fEnv * envCutoffRange + p.keyTrack * (pitch - 60.0f) + velSemis
                              + lfoValue * p.lfoToCutoff * lfoCutoffRange + routed (param2::toCutoff, lfo2Value) * lfoCutoffRange;
            const float hz = juce::jlimit (20.0f, nyquistSafe, baseCutoff * static_cast<float> (semisToRatio (semis)));
            filter.setup (hz, q, sr);
            filterCountdown = filterUpdateInterval;
        }

        float y = filter.process (s, p.filterType);

        const float drive = juce::jlimit (0.0f, 1.0f, p.drive + lfoValue * p.lfoToDrive * 0.5f
                                                      + routed (param2::toDrive, lfo2Value) * 0.5f);
        y = shape (p.driveType, y, drive);

        const float tremolo = 1.0f - 0.5f * mod2 * (p.lfo2Target == param2::toAmp ? 1.0f - lfo2Value : 0.0f);
        y *= ampEnv.getNextSample() * velGain * voiceGain * unisonGain * tremolo;

        const int at = startSample + i;
        if (numChannels > 0) out.addSample (0, at, y * gainL);
        if (numChannels > 1) out.addSample (1, at, y * gainR);
        for (int ch = 2; ch < numChannels; ++ch)
            out.addSample (ch, at, y);

        if (! ampEnv.isActive())
        {
            kill();
            break;
        }
    }
}
}
