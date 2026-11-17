#include "SynthEngine.h"
#include "Parameters.h"

namespace cash
{
SynthEngine::SynthEngine (juce::AudioProcessorValueTreeState& state)
    : SynthEngine (RawLookup ([&state] (const char* id) { return state.getRawParameterValue (id); }))
{
}

SynthEngine::SynthEngine (const RawLookup& lookup)
    : fx (lookup)
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
    bind (voiceMode, param::voiceMode);
    bind (glideTime, param::glideTime);
    bind (glideMode, param::glideMode);
    bind (glideType, param::glideType);
    bind (notePriority, param::notePriority);
    bind (unisonVoices, param::unisonVoices);
    bind (unisonDetune, param::unisonDetune);
    bind (unisonSpread, param::unisonSpread);
    bind (bendRange, param::bendRange);
    bind (modVibrato, param::modVibrato);
    bind (velToCutoff, param::velToCutoff);
    bind (osc1On, param::osc1On);
    bind (osc2On, param::osc2On);
    bind (subNoiseOn, param::subNoiseOn);
    bind (filterOn, param::filterOn);
    bind (filterEnvOn, param::filterEnvOn);
    bind (driveOn, param::driveOn);
    bind (lfo1On, param::lfo1On);
    bind (lfo2On, param::lfo2On);
    bind (unisonOn, param::unisonOn);
    bind (glideOn, param::glideOn);
    bind (lfo1Sync, param::lfo1Sync);
    bind (lfo1Note, param::lfo1Note);
    bind (lfo2Sync, param::lfo2Sync);
    bind (lfo2Note, param::lfo2Note);
    bind (mintOn, param::mintOn);
    bind (mintLevel, param::mintLevel);
    bind (mintNoise, param::mintNoise);
    bind (mintTone, param::mintTone);
    bind (mintSpeed, param::mintSpeed);
    bind (mintFine, param::mintFine);
    bind (mintStart, param::mintStart);
    bind (mintLoop, param::mintLoop);
    for (size_t i = 0; i < layerRaw.size(); ++i)
        bind (layerRaw[i], layers::ids()[i]);

    for (int i = 0; i < numVoices; ++i)
        voices.push_back (std::make_unique<CashVoice> (ctx));
    held.reserve (128);

    readParameters();
    snapSmoothers();
}

void SynthEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    for (auto& v : voices)
        v->setSampleRate (newSampleRate);
    lfoBuffer.assign (static_cast<size_t> (juce::jmax (maxBlockSize, 32)), 0.0f);
    lfo2Buffer.assign (lfoBuffer.size(), 0.0f);
    laneData.assign (lfoBuffer.size() * static_cast<size_t> (numLanes), 0.0f);
    for (int k = 0; k < numLanes; ++k)
        ctx.lane[k] = laneData.data() + static_cast<size_t> (k) * lfoBuffer.size();
    gain.reset (newSampleRate, 0.02);
    readParameters();
    snapSmoothers();
    gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGain.get()));
    fx.prepare (newSampleRate, maxBlockSize);
    reset();
}

void SynthEngine::reset()
{
    killAll();
    held.clear();
    sustainDown = false;
    lastPitch = -1.0f;
    ctx.bend = ctx.modWheel = 0.0f;
    lfo1.phase = lfo2.phase = 0.0;
    wasLocked = false;
    fx.reset();
    // Oscillators free-run between notes; a reset makes renders repeatable (Cash Match relies on it).
    // Voice 0 starts at phase 0 as before; the others are staggered so unison voices don't start in phase.
    for (size_t i = 0; i < voices.size(); ++i)
        voices[i]->resetPhases (static_cast<double> (i) * 0.6180339887);
}

int SynthEngine::getNumActiveVoices() const
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(), [] (const auto& v) { return v->isActive(); }));
}

void SynthEngine::snapSmoothers()
{
    // Jump straight to the current values (prepare / construction): no ramp from stale ones.
    for (int k = 0; k < numLanes; ++k)
    {
        const bool isSwitch = k >= laneOsc1On; // gates, crossfades and LFO 2 routing
        smooth[static_cast<size_t> (k)].reset (sampleRate, isSwitch ? 0.01 : 0.015);
    }
    auto& p = ctx.params;
    p.prevOsc1Wave = p.osc1Wave;
    p.prevOsc2Wave = p.osc2Wave;
    p.prevFilterType = p.filterType;
    p.prevDriveType = p.driveType;
    for (auto* l : { &lfo1, &lfo2 })
    {
        l->prevWave = l->wave;
        l->rate.reset (sampleRate, 0.015);
        l->gate.reset (sampleRate, 0.01);
        l->fade.reset (sampleRate, 0.01);
        l->fade.setCurrentAndTargetValue (1.0f);
    }
    for (int k = laneOsc1Wave; k <= laneDriveType; ++k)
        smooth[static_cast<size_t> (k)].setCurrentAndTargetValue (1.0f);
}

void SynthEngine::setChoice (int newValue, int& now, int& prev, Lane fade)
{
    if (newValue == now)
        return;
    // Crossfade from what was sounding. A change mid-fade restarts from the old choice.
    prev = now;
    now = newValue;
    auto& f = smooth[static_cast<size_t> (fade)];
    f.setCurrentAndTargetValue (0.0f);
    f.setTargetValue (1.0f);
}

void SynthEngine::readParameters()
{
    auto& p = ctx.params;
    auto set = [this] (Lane k, float v) { smooth[static_cast<size_t> (k)].setTargetValue (v); };
    auto gate = [] (const Raw& r) { return r.get() > 0.5f ? 1.0f : 0.0f; };

    setChoice (osc1Wave.asInt(), p.osc1Wave, p.prevOsc1Wave, laneOsc1Wave);
    setChoice (osc2Wave.asInt(), p.osc2Wave, p.prevOsc2Wave, laneOsc2Wave);
    p.osc1Steps = static_cast<float> (osc1Octave.asInt() * 12 + osc1Semi.asInt());
    p.osc2Steps = static_cast<float> (osc2Octave.asInt() * 12 + osc2Semi.asInt());
    set (laneOsc1Fine, osc1Fine.get() / 100.0f);
    set (laneOsc2Fine, osc2Fine.get() / 100.0f);
    set (laneOsc1Level, osc1Level.get());
    set (laneOsc2Level, osc2Level.get());
    set (laneSubLevel, subLevel.get());
    set (laneNoiseLevel, noiseLevel.get());

    setChoice (filterType.asInt(), p.filterType, p.prevFilterType, laneFilterType);
    p.cutoffHz = filterCutoff.get(); // smoothed per voice (multiplicative)
    set (laneReso, filterReso.get());
    set (laneEnvAmt, filterEnvAmt.get());
    set (laneKeyTrack, filterKeyTrack.get());

    p.filterEnv = { fltA.get(), fltD.get(), fltS.get(), fltR.get() };
    p.ampEnv = { ampA.get(), ampD.get(), ampS.get(), ampR.get() };
    set (laneFltSustain, fltS.get());
    set (laneAmpSustain, ampS.get());

    set (laneLfoPitch, lfoToPitch.get());
    set (laneLfoCutoff, lfoToCutoff.get());
    p.velocitySens = velocitySens.get();

    setChoice (driveType.asInt(), p.driveType, p.prevDriveType, laneDriveType);
    set (laneDrive, driveAmount.get());
    set (laneLfoDrive, lfoToDrive.get());
    set (laneLfo2Amount, lfo2Amount.get());
    const int target2 = lfo2Target.asInt();
    for (int t = 0; t < 5; ++t)
        set (static_cast<Lane> (laneToCutoff + t), t == target2 ? 1.0f : 0.0f);
    set (lanePitchEnv, pitchEnvAmt.get() * param::pitchEnvRange);

    p.bendRange = bendRange.get();
    set (laneModVib, modVibrato.get() * param::modVibratoRange);
    set (laneVelCutoff, velToCutoff.get() * param::velCutoffRange);
    set (laneDetune, unisonDetune.get());
    set (laneSpread, unisonSpread.get());

    set (laneOsc1On, gate (osc1On));
    set (laneOsc2On, gate (osc2On));
    set (laneSubNoiseOn, gate (subNoiseOn));
    set (laneFilterOn, gate (filterOn));
    set (laneFilterEnvOn, gate (filterEnvOn));
    set (laneDriveOn, gate (driveOn));
    set (laneLfo2On, gate (lfo2On));
    set (laneMintOn, gate (mintOn));
    set (laneMintLevel, mintLevel.get());
    set (laneMintNoise, mintNoise.get());
    p.mintTone = mintTone.get();
    p.mintSpeed = mintSpeed.get();
    p.mintFine = mintFine.get() / 100.0f;
    p.mintStart = mintStart.get();
    p.mintLoop = mintLoop.get();

    // M14 LAYERS: per-block settings (each layer smooths its own level).
    set (laneLayersOn, gate (layerRaw[0]));
    {
        auto& L = p.layers;
        auto v = [this] (int i) { return layerRaw[static_cast<size_t> (i)].get(); };
        L.detail = juce::jlimit (1, layers::numTone, layerRaw[1].asInt());
        L.pitchAmt = v (2);
        L.pitchTime = v (3);
        for (int l = 0; l < layers::numTone; ++l)
        {
            auto& t = L.tone[static_cast<size_t> (l)];
            using namespace layers;
            auto f = [&] (int field) { return v (toneIndex (l, field)); };
            t.gain = f (level);
            t.pan = f (pan);
            t.semis = static_cast<float> (juce::roundToInt (f (octave)) * 12 + juce::roundToInt (f (semi))) + f (fine) / 100.0f;
            t.a = f (attack); t.d = f (decay); t.s = f (sustain); t.r = f (release);
            t.p = curveExponent (f (curve));
            t.type = juce::roundToInt (f (fltType));
            t.cutoffHz = f (cutoff);
            const float r = f (reso);
            t.q = 0.707f + r * r * 14.0f;
            t.envSemis = f (envAmt) * envCutoffRange;
            t.fa = f (fAttack); t.fd = f (fDecay); t.fs = f (fSustain); t.fr = f (fRelease);
            t.l1p = f (l1Pitch) * lfoPitchRange;
            t.l1l = f (l1Level);
            t.l1c = f (l1Cutoff) * lfoCutoffRange;
            t.l2p = f (l2Pitch) * lfoPitchRange;
            t.l2l = f (l2Level);
            t.l2c = f (l2Cutoff) * lfoCutoffRange;
        }
        for (int n = 0; n < layers::numNoise; ++n)
        {
            auto& z = L.noise[static_cast<size_t> (n)];
            using namespace layers;
            auto f = [&] (int field) { return v (noiseIndex (n, field)); };
            z.gain = f (nLevel); z.centreHz = f (nCentre); z.octaves = f (nWidth); z.pan = f (nPan);
            z.a = f (nAttack); z.d = f (nDecay); z.s = f (nSustain); z.r = f (nRelease);
            z.slope = juce::roundToInt (f (nSlope));
        }
    }

    priority = notePriority.asInt();
    glideSeconds = glideOn.get() > 0.5f ? glideTime.get() : 0.0f;
    glideModeNow = glideMode.asInt();
    glideByRate = glideType.asInt() == param::constantRate;

    // Mode or unison changes keep the held notes sounding (handed over, not released).
    const int newUnison = unisonOn.get() > 0.5f ? juce::jlimit (1, param::maxUnison, unisonVoices.asInt()) : 1;
    changeVoicing (voiceMode.asInt(), newUnison);

    for (auto [lfo, waveParam, rateParam, onParam, syncParam, noteParam] :
             { std::tuple { &lfo1, &lfoWave, &lfoRate, &lfo1On, &lfo1Sync, &lfo1Note },
               std::tuple { &lfo2, &lfo2Wave, &lfo2Rate, &lfo2On, &lfo2Sync, &lfo2Note } })
    {
        if (const int w = waveParam->asInt(); w != lfo->wave)
        {
            lfo->prevWave = lfo->wave;
            lfo->wave = w;
            lfo->fade.setCurrentAndTargetValue (0.0f);
            lfo->fade.setTargetValue (1.0f);
        }
        // TIME: Hz as set. SYNC: the note value at the host tempo (the rate ramps, so a
        // BPM, note or mode change never jumps the speed).
        lfo->synced = syncParam->asInt() == param::syncMode;
        lfo->lengthQn = tempo::noteQuarters (noteParam->asInt(), hostTempo);
        lfo->rate.setTargetValue (lfo->synced ? static_cast<float> (hostTempo.bpm / 60.0 / lfo->lengthQn) : rateParam->get());
        lfo->gate.setTargetValue (gate (*onParam));
    }
}

float SynthEngine::Lfo::shape (int wave, double t)
{
    switch (wave)
    {
        case param::sine:     return static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * t));
        case param::triangle: return static_cast<float> (1.0 - 4.0 * std::abs (t - 0.5));
        case param::saw:      return static_cast<float> (2.0 * t - 1.0);
        default:              return t < 0.5 ? 1.0f : -1.0f;
    }
}

void SynthEngine::fillLfo (Lfo& l, std::vector<float>& out, int n, int startSample)
{
    // Synced while the host plays: the phase follows the song position, so the LFO
    // sits on the beat (phase 0 on every multiple of its length from the song start).
    const bool locked = l.synced && hostTempo.locked();
    const double qnPerSample = hostTempo.bpm / 60.0 / sampleRate;
    const double qn0 = hostTempo.ppq + static_cast<double> (startSample) * qnPerSample;
    auto songPhase = [&l, qn0, qnPerSample] (int i)
    {
        const double x = (qn0 + static_cast<double> (i) * qnPerSample) / l.lengthQn;
        return x - std::floor (x);
    };

    if (! l.gate.isSmoothing() && l.gate.getTargetValue() <= 0.0f)
    {
        // Switched off: no LFO work at all (its phase waits where it stopped, or
        // keeps its place in the song when locked).
        juce::FloatVectorOperations::clear (out.data(), n);
        l.rate.skip (n);
        l.fade.skip (n);
        if (locked)
            l.phase = songPhase (n);
        return;
    }

    // A locked LFO that is off the song position (note value just changed, mode just
    // switched to SYNC) catches up over ~20 ms instead of jumping; on time, it stays exact.
    const double pull = 1.0 - std::exp (-1.0 / (0.02 * sampleRate));

    for (int i = 0; i < n; ++i)
    {
        const double t = l.phase;
        if (locked)
        {
            const double target = songPhase (i + 1);
            double ahead = l.phase + l.rate.getNextValue() / sampleRate;
            double err = target - ahead;
            err -= std::floor (err + 0.5); // shortest way round
            ahead += std::abs (err) < 1.0e-4 ? err : err * pull;
            l.phase = ahead - std::floor (ahead);
        }
        else
        {
            l.phase += static_cast<float> (l.rate.getNextValue() / sampleRate);
            if (l.phase >= 1.0)
                l.phase -= 1.0;
        }

        float v = Lfo::shape (l.wave, t);
        if (l.fade.isSmoothing())
        {
            const float x = l.fade.getNextValue();
            v = v * x + Lfo::shape (l.prevWave, t) * (1.0f - x);
        }
        out[static_cast<size_t> (i)] = v * l.gate.getNextValue();
    }
}

void SynthEngine::fillLanes (int n)
{
    for (int k = 0; k < numLanes; ++k)
    {
        auto& sm = smooth[static_cast<size_t> (k)];
        auto* d = laneData.data() + static_cast<size_t> (k) * lfoBuffer.size();
        ctx.moving[k] = sm.isSmoothing();
        if (ctx.moving[k])
            for (int i = 0; i < n; ++i)
                d[i] = sm.getNextValue();
        else
            juce::FloatVectorOperations::fill (d, sm.getTargetValue(), n);
    }
}

void SynthEngine::render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const tempo::Info& tempo)
{
    hostTempo = tempo;
    // Pick up the Mint model for this whole block (see mayBeUsing).
    usedMint.store (busy());
    ctx.mint = wantedMint.load();
    usedMint.store (ctx.mint);
    readParameters();

    // Transport just started, or the host seeked / looped: synced LFOs jump straight
    // to the song position (the song jumped too). Otherwise they glide onto it.
    const bool locked = hostTempo.locked();
    if (locked && (! wasLocked || std::abs (hostTempo.ppq - expectedPpq) > 1.0e-3))
    {
        for (auto* l : { &lfo1, &lfo2 })
            if (l->synced)
            {
                const double x = hostTempo.ppq / l->lengthQn;
                l->phase = x - std::floor (x);
            }
    }
    wasLocked = locked;
    expectedPpq = hostTempo.ppq + hostTempo.bpm / 60.0 / sampleRate * buffer.getNumSamples();

    gain.setTargetValue (juce::Decibels::decibelsToGain (masterGain.get(), -48.0f));

    const int total = buffer.getNumSamples();
    const int chunk = static_cast<int> (lfoBuffer.size());
    auto event = midi.findNextSamplePosition (0);

    // Render in chunks so the LFO buffers never need reallocating on the audio thread,
    // and split each chunk at MIDI events so notes start on their exact sample.
    for (int start = 0; start < total; start += chunk)
    {
        const int n = juce::jmin (chunk, total - start);
        fillLanes (n);
        fillLfo (lfo1, lfoBuffer, n, start);
        fillLfo (lfo2, lfo2Buffer, n, start);
        ctx.lfo = lfoBuffer.data();
        ctx.lfo2 = lfo2Buffer.data();
        ctx.lfoBase = start;

        int pos = start;
        const int end = start + n;
        while (pos < end)
        {
            while (event != midi.cend() && (*event).samplePosition <= pos)
            {
                handleMidi ((*event).getMessage());
                ++event;
            }
            const int next = event != midi.cend() ? juce::jmin (end, juce::jmax (pos + 1, (*event).samplePosition)) : end;
            renderVoices (buffer, pos, next - pos);
            pos = next;
        }
    }

    // Events stamped past the end of the buffer (host bugs) still count.
    for (; event != midi.cend(); ++event)
        handleMidi ((*event).getMessage());

    fx.process (buffer, hostTempo); // off = untouched
    gain.applyGain (buffer, total);
}

void SynthEngine::renderVoices (juce::AudioBuffer<float>& buffer, int start, int num)
{
    for (auto& v : voices)
        v->render (buffer, start, num);
}

//============================================================================== MIDI
void SynthEngine::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        noteOff (m.getNoteNumber());
    else if (m.isPitchWheel())
        ctx.bend = juce::jlimit (-1.0f, 1.0f, (static_cast<float> (m.getPitchWheelValue()) - 8192.0f) / 8192.0f);
    else if (m.isAllSoundOff())
        killAll();
    else if (m.isAllNotesOff())
    {
        held.clear();
        releaseAll();
    }
    else if (m.isSustainPedalOn())
        setSustain (true);
    else if (m.isSustainPedalOff())
        setSustain (false);
    else if (m.isControllerOfType (1))
        ctx.modWheel = static_cast<float> (m.getControllerValue()) / 127.0f;
}

CashVoice::Glide SynthEngine::glide() const
{
    return { glideSeconds, glideByRate };
}

int SynthEngine::unisonCount() const
{
    return juce::jlimit (1, numVoices, unison);
}

void SynthEngine::placeUnison (CashVoice& v, int index, int count) const
{
    // Spread evenly from -1 to +1: detune low-to-high, panned left-to-right.
    const float t = count > 1 ? static_cast<float> (index) / static_cast<float> (count - 1) * 2.0f - 1.0f : 0.0f;
    v.setUnison (t, 1.0f / std::sqrt (static_cast<float> (count)));
}

void SynthEngine::noteOn (int note, float velocity)
{
    const bool othersHeld = std::any_of (held.begin(), held.end(), [note] (const Held& h) { return h.note != note; });
    held.erase (std::remove_if (held.begin(), held.end(), [note] (const Held& h) { return h.note == note; }), held.end());
    if (held.size() < held.capacity())
        held.push_back ({ note, velocity });

    if (mode != param::poly)
    {
        const auto* chosen = priorityNote();
        if (chosen != nullptr && ! (monoGate && chosen->note == monoNote))
            monoPlay (*chosen, othersHeld);
        return;
    }

    // Poly. Re-hitting a ringing note (e.g. under the pedal) lets the old one go first.
    for (auto& v : voices)
        if (v->isActive() && v->getNote() == note && (v->keyDown || v->sustained))
            v->release();

    const bool glides = lastPitch >= 0.0f && (glideModeNow == param::glideAlways || othersHeld);
    startPolyNote (note, velocity, glides ? lastPitch : -1.0f);
    lastPitch = static_cast<float> (note);
}

void SynthEngine::startPolyNote (int note, float velocity, float fromPitch)
{
    const int count = unisonCount();
    const uint64_t stamp = clock + 1;

    for (int k = 0; k < count; ++k)
    {
        auto* v = allocate (stamp);
        if (v == nullptr)
            break;
        placeUnison (*v, k, count);
        v->play (note, velocity, true, fromPitch, glide());
        v->keyDown = true;
        v->sustained = false;
        v->age = ++clock;
    }
}

void SynthEngine::noteOff (int note)
{
    held.erase (std::remove_if (held.begin(), held.end(), [note] (const Held& h) { return h.note == note; }), held.end());

    if (mode != param::poly)
    {
        if (! monoGate || note != monoNote)
            return; // a key that wasn't sounding: nothing to do
        if (const auto* fallback = priorityNote())
            monoPlay (*fallback, true); // fall back to a still-held key
        else if (! sustainDown)
            releaseMono();
        return;
    }

    for (auto& v : voices)
    {
        if (v->isActive() && v->keyDown && v->getNote() == note)
        {
            v->keyDown = false;
            if (sustainDown)
                v->sustained = true;
            else
                v->release();
        }
    }
}

void SynthEngine::setSustain (bool down)
{
    sustainDown = down;
    if (down)
        return;

    if (mode != param::poly)
    {
        if (monoGate && held.empty())
            releaseMono();
        return;
    }

    for (auto& v : voices)
        if (v->isActive() && v->sustained && ! v->keyDown)
            v->release();
}

void SynthEngine::releaseAll()
{
    for (auto& v : voices)
        if (v->isActive())
            v->release();
    monoGate = false;
    monoNote = -1;
}

void SynthEngine::killAll()
{
    for (auto& v : voices)
        v->kill();
    monoGate = false;
    monoNote = -1;
}

CashVoice* SynthEngine::allocate (uint64_t notBefore)
{
    // A free voice first; otherwise steal the oldest, preferring ones whose key is up.
    // Never steal a voice started for this same note (stamp >= notBefore).
    CashVoice* best = nullptr;
    for (auto& v : voices)
        if (! v->isActive())
            return v.get();

    for (int pass = 0; pass < 2 && best == nullptr; ++pass)
        for (auto& v : voices)
        {
            if (v->age >= notBefore || (pass == 0 && v->keyDown))
                continue;
            if (best == nullptr || v->age < best->age)
                best = v.get();
        }
    return best;
}

//============================================================================== Voicing changes
// Voice mode or unison count changed (automation, a click): hand the held keys
// over to the new voicing instead of releasing everything.
void SynthEngine::changeVoicing (int newMode, int newUnison)
{
    const int oldMode = mode, oldCount = juce::jlimit (1, numVoices, unison);
    if (newMode == oldMode && newUnison == unison)
        return;
    mode = newMode;
    unison = newUnison;
    const int count = unisonCount();
    const bool wasPoly = oldMode == param::poly, isPoly = newMode == param::poly;

    if (wasPoly && isPoly)
    {
        for (const auto& h : held)
            regroup (h.note, count);
        return;
    }

    if (! wasPoly && ! isPoly)
    {
        // Mono <-> Legato needs nothing; a new unison count reshapes the stack in place.
        if (! monoGate || count == oldCount)
            return;
        for (int k = count; k < oldCount; ++k)
            if (voices[static_cast<size_t> (k)]->isActive())
                voices[static_cast<size_t> (k)]->release();
        const float vel = voices[0]->getVelocity();
        for (int k = 0; k < count; ++k)
        {
            auto& v = *voices[static_cast<size_t> (k)];
            placeUnison (v, k, count);
            if (k >= oldCount || ! v.isActive())
                v.play (monoNote, vel, true, -1.0f, {});
            v.keyDown = true;
            v.sustained = false;
            v.age = ++clock;
        }
        return;
    }

    if (wasPoly)
    {
        // Poly -> Mono/Legato: the priority key keeps its voices (moved to the
        // mono slots, no retrigger); every other note rings out.
        const auto* chosen = priorityNote();
        int kept = 0;
        if (chosen != nullptr)
            for (size_t j = 0; j < voices.size() && kept < count; ++j)
                if (voices[j]->isActive() && voices[j]->keyDown && voices[j]->getNote() == chosen->note)
                    std::swap (voices[static_cast<size_t> (kept++)], voices[j]);

        for (size_t j = static_cast<size_t> (kept); j < voices.size(); ++j)
            if (voices[j]->isActive() && (voices[j]->keyDown || voices[j]->sustained))
                voices[j]->release();

        monoGate = false;
        monoNote = -1;
        if (chosen == nullptr)
            return;

        for (int k = 0; k < count; ++k)
        {
            auto& v = *voices[static_cast<size_t> (k)];
            placeUnison (v, k, count);
            if (k >= kept)
                v.play (chosen->note, chosen->velocity, true, -1.0f, {});
            v.keyDown = true;
            v.sustained = false;
            v.age = ++clock;
        }
        monoNote = chosen->note;
        monoGate = true;
        lastPitch = static_cast<float> (chosen->note);
        return;
    }

    // Mono/Legato -> Poly: the sounding note becomes a poly note (pedal-held
    // stays pedal-held), and every other held key starts sounding too.
    const int sounding = monoGate ? monoNote : -1;
    if (sounding >= 0)
    {
        const bool keyHeld = std::any_of (held.begin(), held.end(), [sounding] (const Held& h) { return h.note == sounding; });
        for (int k = 0; k < oldCount; ++k)
        {
            auto& v = *voices[static_cast<size_t> (k)];
            if (! v.isActive() || keyHeld)
                continue;
            v.keyDown = false;
            if (sustainDown)
                v.sustained = true;
            else
                v.release();
        }
        if (keyHeld)
            regroup (sounding, count);
    }
    monoGate = false;
    monoNote = -1;

    for (const auto& h : held)
        if (h.note != sounding)
            startPolyNote (h.note, h.velocity, -1.0f);
}

void SynthEngine::regroup (int note, int count)
{
    // The voices holding this key, oldest first (= unison order they were dealt in).
    std::array<CashVoice*, numVoices> group {};
    int n = 0;
    for (auto& v : voices)
        if (v->isActive() && v->keyDown && v->getNote() == note)
            group[static_cast<size_t> (n++)] = v.get();
    std::sort (group.begin(), group.begin() + n, [] (const CashVoice* a, const CashVoice* b) { return a->age < b->age; });

    for (int k = count; k < n; ++k)
        group[static_cast<size_t> (k)]->release();
    n = juce::jmin (n, count);
    if (n == 0)
        return; // its voices were stolen: leave it

    // Fresh stamps so allocate() never steals this note's own voices.
    for (int k = 0; k < n; ++k)
        group[static_cast<size_t> (k)]->age = ++clock;
    const uint64_t stamp = group[0]->age;
    const float vel = group[0]->getVelocity();

    for (int k = 0; k < n; ++k)
        placeUnison (*group[static_cast<size_t> (k)], k, count);
    while (n < count)
    {
        auto* v = allocate (stamp);
        if (v == nullptr)
            break;
        placeUnison (*v, n, count);
        v->play (note, vel, true, -1.0f, {});
        v->keyDown = true;
        v->sustained = false;
        v->age = ++clock;
        group[static_cast<size_t> (n++)] = v;
    }
    if (n < count) // ran out of voices: spread the ones it got
        for (int k = 0; k < n; ++k)
            placeUnison (*group[static_cast<size_t> (k)], k, n);
}

//============================================================================== Mono / Legato
const SynthEngine::Held* SynthEngine::priorityNote() const
{
    if (held.empty())
        return nullptr;
    switch (priority)
    {
        case param::lowNote:
            return &*std::min_element (held.begin(), held.end(), [] (const Held& a, const Held& b) { return a.note < b.note; });
        case param::highNote:
            return &*std::max_element (held.begin(), held.end(), [] (const Held& a, const Held& b) { return a.note < b.note; });
        default:
            return &held.back();
    }
}

void SynthEngine::monoPlay (const Held& h, bool legatoContext)
{
    // Mono retriggers the envelopes on every new note; Legato only when no key was down.
    const bool retrigger = mode == param::mono || ! legatoContext || ! monoGate;
    const bool glides = lastPitch >= 0.0f && (glideModeNow == param::glideAlways || legatoContext);
    const int count = unisonCount();

    for (int k = 0; k < count; ++k)
    {
        auto& v = *voices[static_cast<size_t> (k)];
        // Glide from where the voice actually is (it may be mid-glide).
        const float from = ! glides ? -1.0f : (v.isActive() ? v.getPitch() : lastPitch);
        placeUnison (v, k, count);
        v.play (h.note, h.velocity, retrigger, from, glide());
        v.keyDown = true;
        v.sustained = false;
        v.age = ++clock;
    }

    monoNote = h.note;
    monoGate = true;
    lastPitch = static_cast<float> (h.note);
}

void SynthEngine::releaseMono()
{
    for (int k = 0; k < unisonCount(); ++k)
        if (voices[static_cast<size_t> (k)]->isActive())
            voices[static_cast<size_t> (k)]->release();
    monoGate = false;
}
}
