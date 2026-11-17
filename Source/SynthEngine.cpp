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

    for (int i = 0; i < numVoices; ++i)
        voices.push_back (std::make_unique<CashVoice> (ctx));
    held.reserve (128);

    readParameters();
}

void SynthEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    for (auto& v : voices)
        v->setSampleRate (newSampleRate);
    lfoBuffer.assign (static_cast<size_t> (juce::jmax (maxBlockSize, 32)), 0.0f);
    lfo2Buffer.assign (lfoBuffer.size(), 0.0f);
    gain.reset (newSampleRate, 0.02);
    readParameters();
    gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGain.get()));
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
    // Oscillators free-run between notes; a reset makes renders repeatable (Cash Match relies on it).
    // Voice 0 starts at phase 0 as before; the others are staggered so unison voices don't start in phase.
    for (size_t i = 0; i < voices.size(); ++i)
        voices[i]->resetPhases (static_cast<double> (i) * 0.6180339887);
}

int SynthEngine::getNumActiveVoices() const
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(), [] (const auto& v) { return v->isActive(); }));
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

    p.bendRange = bendRange.get();
    p.modVibratoSemis = modVibrato.get() * param::modVibratoRange;
    p.velCutoffSemis = velToCutoff.get() * param::velCutoffRange;

    const int newMode = voiceMode.asInt();
    const int newUnison = juce::jlimit (1, param::maxUnison, unisonVoices.asInt());
    // Changing how notes are dealt mid-phrase: let what's playing ring out and start clean.
    if (newMode != mode || (newMode != param::poly && newUnison != unison))
        releaseAll();
    mode = newMode;
    unison = newUnison;
    priority = notePriority.asInt();
    glideSeconds = glideTime.get();
    glideModeNow = glideMode.asInt();
    glideByRate = glideType.asInt() == param::constantRate;
    detuneAmount = unisonDetune.get();
    spreadAmount = unisonSpread.get();

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
    auto event = midi.findNextSamplePosition (0);

    // Render in chunks so the LFO buffers never need reallocating on the audio thread,
    // and split each chunk at MIDI events so notes start on their exact sample.
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
    v.setUnison (t * 0.5f * detuneAmount * param::unisonDetuneRange, t * spreadAmount,
                 1.0f / std::sqrt (static_cast<float> (count)));
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
    const float from = glides ? lastPitch : -1.0f;
    const int count = unisonCount();
    const uint64_t stamp = clock + 1;

    for (int k = 0; k < count; ++k)
    {
        auto* v = allocate (stamp);
        if (v == nullptr)
            break;
        placeUnison (*v, k, count);
        v->play (note, velocity, true, from, glide());
        v->keyDown = true;
        v->sustained = false;
        v->age = ++clock;
    }
    lastPitch = static_cast<float> (note);
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
