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

    constexpr float param_unisonDetuneRange = param2::unisonDetuneRange;
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

float CashVoice::Svf::process (float v0, int mode, int prevMode, float fade)
{
    const float v3 = v0 - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;

    // One SVF gives all three responses, so a type change is a plain crossfade.
    auto pick = [&] (int m)
    {
        switch (m)
        {
            case 1:  return v1;                 // band-pass
            case 2:  return v0 - k * v1 - v2;   // high-pass
            default: return v2;                 // low-pass
        }
    };
    const float now = pick (mode);
    return fade >= 1.0f ? now : now * fade + pick (prevMode) * (1.0f - fade);
}

void CashVoice::setSampleRate (double newRate)
{
    if (newRate <= 0.0)
        return;
    sampleRate = newRate;
    ampEnv.setSampleRate (newRate);
    filterEnv.setSampleRate (newRate);
    cutoff.reset (newRate, 0.02);
    unisonPos.reset (newRate, 0.02);
    unisonGain.reset (newRate, 0.02);
    ampSustainSet = fltSustainSet = -1.0f;
}

void CashVoice::resetPhases (double offset)
{
    phase1 = phase2 = phaseSub = offset - std::floor (offset);
    // Noise too, so an offline render sounds the same on whichever engine makes it (M9: parallel Cash Match).
    noise.setSeed (0x5EEDC0DEll + static_cast<juce::int64> (offset * 1000.0));
}

void CashVoice::setUnison (float position, float gain)
{
    position = juce::jlimit (-1.0f, 1.0f, position);
    if (active)
    {
        unisonPos.setTargetValue (position);
        unisonGain.setTargetValue (gain);
    }
    else
    {
        unisonPos.setCurrentAndTargetValue (position);
        unisonGain.setCurrentAndTargetValue (gain);
    }
}

void CashVoice::updateEnvelopes (float ampSustain, float fltSustain)
{
    // Called per block, and every few samples while a sustain level is being
    // automated (JUCE's ADSR jumps straight to a new sustain level).
    auto a = ctx.params.ampEnv;
    auto f = ctx.params.filterEnv;
    a.sustain = ampSustainSet = ampSustain;
    f.sustain = fltSustainSet = fltSustain;
    ampEnv.setParameters (a);
    filterEnv.setParameters (f);
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
        filterIdle = false;
        cutoff.setCurrentAndTargetValue (p.cutoffHz);
        phaseSub = phase1; // keep the sub locked to osc 1 at note start
    }

    active = true;
    updateEnvelopes (ctx.lane[laneAmpSustain] != nullptr ? ctx.lane[laneAmpSustain][0] : p.ampEnv.sustain,
                     ctx.lane[laneFltSustain] != nullptr ? ctx.lane[laneFltSustain][0] : p.filterEnv.sustain);
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

    const int off = startSample - ctx.lfoBase;
    auto lane = [&] (int k) { return ctx.lane[k] + off; };
    const float *osc1L = lane (laneOsc1Level), *osc2L = lane (laneOsc2Level), *subL = lane (laneSubLevel), *noiseL = lane (laneNoiseLevel);
    const float *fine1 = lane (laneOsc1Fine), *fine2 = lane (laneOsc2Fine);
    const float *reso = lane (laneReso), *envAmt = lane (laneEnvAmt), *keyTrack = lane (laneKeyTrack);
    const float *lfoPitch = lane (laneLfoPitch), *lfoCutoff = lane (laneLfoCutoff), *lfoDrive = lane (laneLfoDrive);
    const float *driveL = lane (laneDrive), *amount2 = lane (laneLfo2Amount), *pitchEnv = lane (lanePitchEnv);
    const float *detune = lane (laneDetune), *spread = lane (laneSpread), *modVib = lane (laneModVib), *velCut = lane (laneVelCutoff);
    const float *ampSus = lane (laneAmpSustain), *fltSus = lane (laneFltSustain);
    const float *on1 = lane (laneOsc1On), *on2 = lane (laneOsc2On), *onSub = lane (laneSubNoiseOn);
    const float *onFilter = lane (laneFilterOn), *onFltEnv = lane (laneFilterEnvOn), *onDrive = lane (laneDriveOn);
    const float* onLfo2 = lane (laneLfo2On);
    const float *wave1X = lane (laneOsc1Wave), *wave2X = lane (laneOsc2Wave), *typeX = lane (laneFilterType), *driveX = lane (laneDriveType);
    const float *toCutoff = lane (laneToCutoff), *toPitch = lane (laneToPitch), *toAmp = lane (laneToAmp);
    const float *toDrive = lane (laneToDrive), *toOsc2 = lane (laneToOsc2);

    const bool sustainMoves = ctx.moving[laneAmpSustain] || ctx.moving[laneFltSustain];
    updateEnvelopes (ampSus[0], fltSus[0]);
    cutoff.setTargetValue (p.cutoffHz);

    // Whole-chunk bypasses: a switched-off section costs nothing.
    const bool filterOff = ! ctx.moving[laneFilterOn] && onFilter[0] <= 0.0f;
    const bool driveOff = ! ctx.moving[laneDriveOn] && onDrive[0] <= 0.0f;
    if (filterOff)
        filterIdle = true;

    const double hzPerSemitoneBase = 440.0 / sr; // A4 = 69
    const bool fineMoves1 = ctx.moving[laneOsc1Fine], fineMoves2 = ctx.moving[laneOsc2Fine];
    double ratio1 = semisToRatio (p.osc1Steps + fine1[0]), ratio2 = semisToRatio (p.osc2Steps + fine2[0]);
    const float bendSemis = ctx.bend * p.bendRange;

    const int numChannels = out.getNumChannels();
    const float* lfo = ctx.lfo + off;
    const float* lfo2 = ctx.lfo2 != nullptr ? ctx.lfo2 + off : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        if (pitch != target)
            pitch = pitch < target ? juce::jmin (target, pitch + glideStep) : juce::jmax (target, pitch - glideStep);

        if (sustainMoves && (ampSus[i] != ampSustainSet || fltSus[i] != fltSustainSet))
            updateEnvelopes (ampSus[i], fltSus[i]);

        const float lfoValue = lfo[i];
        const float lfo2Value = lfo2 != nullptr ? lfo2[i] : 0.0f;
        const float mod2 = lfo2 != nullptr ? amount2[i] : 0.0f;
        const float fEnv = filterEnv.getNextSample() * onFltEnv[i];

        // Unison place, live: detune and spread follow their lanes.
        const float place = unisonPos.getNextValue();
        const float detuneSemis = place * 0.5f * detune[i] * param_unisonDetuneRange;
        pan = juce::jlimit (-1.0f, 1.0f, place * spread[i]);

        const float pitchMod = lfoValue * (lfoPitch[i] + ctx.modWheel * modVib[i]) + fEnv * pitchEnv[i]
                             + lfo2Value * mod2 * toPitch[i] * mod2 * 12.0f; // amount^2 * 12 st
        const double base = hzPerSemitoneBase * semisToRatio (pitch + (detuneSemis + bendSemis - 69.0f) + pitchMod);
        if (fineMoves1) ratio1 = semisToRatio (p.osc1Steps + fine1[i]);
        if (fineMoves2) ratio2 = semisToRatio (p.osc2Steps + fine2[i]);
        const double d1 = base * ratio1, d2 = base * ratio2, dSub = base * 0.5;

        const float osc1Level = osc1L[i] * on1[i];
        const float osc2Level = osc2L[i] * on2[i] * juce::jmax (0.0f, 1.0f + lfo2Value * mod2 * toOsc2[i]);
        const float subLevel = subL[i] * onSub[i], noiseLevel = noiseL[i] * onSub[i];

        auto wave = [] (int now, int prev, float x, double t, double dt)
        {
            const float v = oscillator (now, t, dt);
            return x >= 1.0f ? v : v * x + oscillator (prev, t, dt) * (1.0f - x);
        };

        float s = 0.0f;
        if (osc1Level > 0.0f)  s += osc1Level * wave (p.osc1Wave, p.prevOsc1Wave, wave1X[i], phase1, d1);
        if (osc2Level > 0.0f)  s += osc2Level * wave (p.osc2Wave, p.prevOsc2Wave, wave2X[i], phase2, d2);
        if (subLevel > 0.0f)   s += subLevel * oscillator (0, phaseSub, dSub);
        if (noiseLevel > 0.0f) s += noiseLevel * (noise.nextFloat() * 2.0f - 1.0f);

        phase1 = advance (phase1, d1);
        phase2 = advance (phase2, d2);
        phaseSub = advance (phaseSub, dSub);

        const float baseCutoff = cutoff.getNextValue();
        float y = s;
        if (! filterOff)
        {
            const float wet = onFilter[i];
            if (wet > 0.0f)
            {
                if (filterIdle)
                {
                    filter.reset(); // fading back in from a clean state
                    filterIdle = false;
                    filterCountdown = 0;
                }
                if (--filterCountdown <= 0)
                {
                    const float q = 0.5f + reso[i] * reso[i] * 14.5f;
                    const float semis = envAmt[i] * fEnv * envCutoffRange + keyTrack[i] * (pitch - 60.0f) + velCut[i] * (velocity - 1.0f)
                                      + lfoValue * lfoCutoff[i] * lfoCutoffRange + lfo2Value * mod2 * toCutoff[i] * lfoCutoffRange;
                    const float hz = juce::jlimit (20.0f, nyquistSafe, baseCutoff * static_cast<float> (semisToRatio (semis)));
                    filter.setup (hz, q, sr);
                    filterCountdown = filterUpdateInterval;
                }
                const float filtered = filter.process (s, p.filterType, p.prevFilterType, typeX[i]);
                y = wet >= 1.0f ? filtered : filtered * wet + s * (1.0f - wet);
            }
            else
            {
                filterIdle = true;
            }
        }

        if (! driveOff)
        {
            const float wet = onDrive[i];
            const float drive = juce::jlimit (0.0f, 1.0f, driveL[i] + lfoValue * lfoDrive[i] * 0.5f
                                                          + lfo2Value * mod2 * toDrive[i] * 0.5f);
            if (wet > 0.0f && drive > 0.0f)
            {
                const float x = driveX[i];
                float shaped = shape (p.driveType, y, drive);
                if (x < 1.0f)
                    shaped = shaped * x + shape (p.prevDriveType, y, drive) * (1.0f - x);
                y = wet >= 1.0f ? shaped : shaped * wet + y * (1.0f - wet);
            }
        }

        const float tremolo = 1.0f - 0.5f * mod2 * (toAmp[i] * onLfo2[i]) * (1.0f - lfo2Value);
        y *= ampEnv.getNextSample() * velGain * voiceGain * unisonGain.getNextValue() * tremolo;

        // Stereo: centre = 1/1 (same as mono), hard side = 0/2, so a mono sum never changes.
        const int at = startSample + i;
        if (numChannels > 1)
        {
            out.addSample (0, at, y * (1.0f - pan));
            out.addSample (1, at, y * (1.0f + pan));
        }
        else if (numChannels > 0)
        {
            out.addSample (0, at, y);
        }
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
