#include "CashMatch.h"
#include "Parameters.h"

namespace cash
{
//============================================================================== PatchRenderer
PatchRenderer::PatchRenderer (const juce::AudioProcessorValueTreeState& ranges)
    : engine ([this] (const char* id) -> std::atomic<float>*
              {
                  const int i = ParameterMap::indexOf (id);
                  return i >= 0 ? &raw[static_cast<size_t> (i)] : nullptr;
              })
{
    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        auto* p = ranges.getParameter (ParameterMap::spec (i).id);
        jassert (p != nullptr);
        params[static_cast<size_t> (i)] = p;
        raw[static_cast<size_t> (i)].store (p != nullptr ? p->convertFrom0to1 (p->getDefaultValue()) : 0.0f);
    }
}

void PatchRenderer::render (const Patch& patch, int midiNote, double holdSeconds, double totalSeconds, juce::AudioBuffer<float>& out)
{
    constexpr int block = 512;
    const double sr = SoundPrint::sampleRate;

    for (int i = 0; i < ParameterMap::numParams; ++i)
        if (auto* p = params[static_cast<size_t> (i)])
            raw[static_cast<size_t> (i)].store (p->convertFrom0to1 (ParameterMap::snap (ParameterMap::spec (i), patch[static_cast<size_t> (i)])));

    engine.prepare (sr, block); // also resets voices and LFO phases

    const int total = juce::jmax (1, static_cast<int> (totalSeconds * sr));
    const int hold = juce::jlimit (1, total, static_cast<int> (holdSeconds * sr));
    out.setSize (1, total, false, false, true);
    out.clear();

    juce::AudioBuffer<float> chunk (1, block);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        chunk.setSize (1, n, false, false, true);
        chunk.clear();

        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, 1.0f), 0);
        if (hold >= pos && hold < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, midiNote), hold - pos);

        engine.render (chunk, midi);
        out.copyFrom (0, pos, chunk, 0, 0, n);
    }
}

//============================================================================== CashMatcher
CashMatcher::CashMatcher (const juce::AudioProcessorValueTreeState& r)
    : juce::Thread ("Cash Match"), ranges (r), renderer (r)
{
}

CashMatcher::~CashMatcher()
{
    stop();
}

float CashMatcher::normalised (const char* id, float realValue) const
{
    auto* p = ranges.getParameter (id);
    jassert (p != nullptr);
    return p != nullptr ? p->convertTo0to1 (p->getNormalisableRange().snapToLegalValue (realValue)) : 0.0f;
}

juce::String CashMatcher::loadTarget (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return "Can't read that file (try WAV, AIFF, FLAC, MP3 or OGG).";
    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return "That file is empty.";

    // Read a bit more than we keep, so leading silence can be trimmed.
    const auto wanted = juce::jmin (reader->lengthInSamples, static_cast<juce::int64> ((maxSeconds + 2.0) * reader->sampleRate));
    juce::AudioBuffer<float> audio (static_cast<int> (juce::jmax (1u, reader->numChannels)), static_cast<int> (wanted));
    reader->read (&audio, 0, static_cast<int> (wanted), 0, true, true);
    return setTarget (audio, reader->sampleRate, file.getFileNameWithoutExtension());
}

juce::String CashMatcher::setTarget (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name)
{
    stop();

    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() <= 0 || sampleRate <= 0.0)
        return "That file is empty.";

    // Mix to mono.
    std::vector<float> mono (static_cast<size_t> (n), 0.0f);
    const float scale = 1.0f / static_cast<float> (audio.getNumChannels());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i)
            mono[static_cast<size_t> (i)] += audio.getSample (ch, i) * scale;

    // Resample to the analysis rate.
    const double ratio = sampleRate / SoundPrint::sampleRate;
    const int outN = juce::jmax (1, static_cast<int> (n / ratio));
    std::vector<float> resampled (static_cast<size_t> (outN), 0.0f);
    if (std::abs (ratio - 1.0) < 1.0e-9)
        resampled = mono;
    else
    {
        juce::LagrangeInterpolator interp;
        mono.resize (mono.size() + 8, 0.0f); // interpolator reads a few samples ahead
        interp.process (ratio, mono.data(), resampled.data(), outN);
    }

    // Trim leading silence (anything under -40 dB of the peak), keep 2 ms of run-up.
    float peak = 0.0f;
    for (auto s : resampled)
        peak = juce::jmax (peak, std::abs (s));
    if (peak < 1.0e-4f)
        return "That file is silent.";

    int startAt = 0;
    while (startAt < outN && std::abs (resampled[static_cast<size_t> (startAt)]) < peak * 0.01f)
        ++startAt;
    startAt = juce::jmax (0, startAt - static_cast<int> (0.002 * SoundPrint::sampleRate));

    const int keep = juce::jmin (outN - startAt, static_cast<int> (maxSeconds * SoundPrint::sampleRate));
    if (keep < static_cast<int> (0.1 * SoundPrint::sampleRate))
        return "Too short: give it at least 0.1 s of sound.";

    Target t;
    t.name = name.isNotEmpty() ? name : "Untitled";
    t.audio.setSize (1, keep);
    t.audio.copyFrom (0, 0, resampled.data() + startAt, keep);
    const int fade = juce::jmin (keep / 4, static_cast<int> (0.01 * SoundPrint::sampleRate));
    t.audio.applyGainRamp (0, keep - fade, fade, 1.0f, 0.0f);
    t.print = analysis::analyse (t.audio.getReadPointer (0), keep);
    if (! t.print.valid)
        return "Couldn't hear anything in that file.";

    target = std::move (t);
    const juce::ScopedLock sl (resultLock);
    result = {};
    bestRender.setSize (0, 0);
    return {};
}

void CashMatcher::clearTarget()
{
    stop();
    target = {};
    const juce::ScopedLock sl (resultLock);
    result = {};
    bestRender.setSize (0, 0);
}

void CashMatcher::start (const Patch& from, const Patch& defaults, uint64_t seed, MatchBudget b)
{
    stop();
    if (! hasTarget())
        return;
    start0 = from;
    defaults0 = defaults;
    seed0 = seed;
    budget = b;
    {
        const juce::ScopedLock sl (resultLock);
        result = {};
    }
    startThread (juce::Thread::Priority::low);
}

void CashMatcher::runBlocking (const Patch& from, const Patch& defaults, uint64_t seed, MatchBudget b)
{
    stop();
    if (! hasTarget())
        return;
    start0 = from;
    defaults0 = defaults;
    seed0 = seed;
    budget = b;
    {
        const juce::ScopedLock sl (resultLock);
        result = {};
    }
    search();
}

void CashMatcher::stop()
{
    cancel = true;
    stopThread (10000);
    cancel = false;
}

void CashMatcher::run()
{
    search();
}

CashMatcher::Result CashMatcher::getResult() const
{
    const juce::ScopedLock sl (resultLock);
    return result;
}

juce::AudioBuffer<float> CashMatcher::getBestRender() const
{
    const juce::ScopedLock sl (resultLock);
    return bestRender;
}

float CashMatcher::evaluate (const Patch& p, juce::AudioBuffer<float>* keep)
{
    const auto& tp = target.print;
    renderer.render (p, tp.midiNote, tp.holdSeconds, tp.seconds, scratch);
    const auto print = analysis::analyse (scratch.getReadPointer (0), scratch.getNumSamples(), analysis::noteToHz (tp.midiNote));
    if (keep != nullptr)
        *keep = scratch;
    return analysis::distance (tp, print);
}

float CashMatcher::score (const Patch& p)
{
    jassert (! isSearching());
    return hasTarget() ? evaluate (p) : 1.0f;
}

Patch CashMatcher::guess (const Patch& defaults) const
{
    using namespace param;
    Patch p = defaults;
    if (! hasTarget())
        return p;

    const auto& tp = target.print;
    const auto& env = tp.envelope;
    const double frameSeconds = tp.seconds / SoundPrint::envFrames;
    auto set = [&p] (const char* id, float norm) { p[static_cast<size_t> (ParameterMap::indexOf (id))] = juce::jlimit (0.0f, 1.0f, norm); };
    auto step = [] (const char* id, int s) { return ParameterMap::fromStep (ParameterMap::spec (ParameterMap::indexOf (id)), s); };
    auto toGain = [] (float norm) { return juce::Decibels::decibelsToGain ((norm - 1.0f) * 60.0f); };

    // Amp envelope from the level envelope.
    int peakFrame = 0;
    for (int f = 0; f < SoundPrint::envFrames; ++f)
        if (env[static_cast<size_t> (f)] > env[static_cast<size_t> (peakFrame)])
            peakFrame = f;
    int attackFrame = 0;
    while (attackFrame < peakFrame && env[static_cast<size_t> (attackFrame)] < 0.95f)
        ++attackFrame;

    const int holdFrame = juce::jlimit (peakFrame, SoundPrint::envFrames - 1, static_cast<int> (tp.holdSeconds / frameSeconds * 0.9));
    const float sustainNorm = env[static_cast<size_t> (holdFrame)];
    const float sustain = toGain (sustainNorm);

    int decayFrame = peakFrame;
    const float decayTarget = sustainNorm + (1.0f - sustainNorm) * 0.3f;
    while (decayFrame < holdFrame && env[static_cast<size_t> (decayFrame)] > decayTarget)
        ++decayFrame;

    int releaseFrame = static_cast<int> (tp.holdSeconds / frameSeconds);
    while (releaseFrame < SoundPrint::envFrames - 1 && env[static_cast<size_t> (releaseFrame)] > 0.33f)
        ++releaseFrame;

    set (ampAttack, normalised (ampAttack, static_cast<float> (juce::jmax (0.002, (attackFrame + 0.5) * frameSeconds))));
    set (ampDecay, normalised (ampDecay, static_cast<float> (juce::jmax (0.01, (decayFrame - peakFrame + 0.5) * frameSeconds))));
    set (ampSustain, sustain);
    set (ampRelease, tp.holdSeconds < tp.seconds
                         ? normalised (ampRelease, static_cast<float> (juce::jmax (0.02, (releaseFrame + 0.5) * frameSeconds - tp.holdSeconds)))
                         : normalised (ampRelease, 0.3f));

    // Tone: neutral filter a little above the brightness, no movement yet.
    set (filterType, step (filterType, lowPass));
    set (filterCutoff, normalised (filterCutoff, juce::jlimit (200.0f, 18000.0f, tp.centroidHz * 3.0f)));
    set (filterReso, 0.1f);
    set (filterEnvAmt, 0.5f); // 0 %
    set (driveAmount, 0.0f);
    set (lfo2Amount, 0.0f);

    // Noise vs tone.
    if (! tp.pitched || tp.flatness > 0.35f)
        set (noiseLevel, tp.pitched ? juce::jmin (1.0f, tp.flatness * 1.5f) : 0.8f);
    if (! tp.pitched)
    {
        set (osc1Level, 0.25f);
        set (osc2Level, 0.0f);
    }

    // Wave from the harmonic balance: weak evens = square, strong 2nd = saw, few overtones = sine.
    if (tp.pitched)
    {
        const auto& h = tp.harmonics;
        const float evens = (h[1] + h[3] + h[5]) / 3.0f, odds = (h[2] + h[4] + h[6]) / 3.0f;
        int wave = triangle;
        if (odds < 0.35f && evens < 0.35f)  wave = sine;
        else if (evens < odds - 0.2f)       wave = (h[2] > 0.75f ? square : triangle);
        else                                wave = saw;
        set (osc1Wave, step (osc1Wave, wave));
        set (osc2Wave, step (osc2Wave, wave));
    }
    return p;
}

void CashMatcher::search()
{
    struct Cand
    {
        Patch patch;
        float d;
    };
    std::vector<Cand> beam; // best few, best first
    constexpr size_t beamSize = 4;

    const int polishPerRound = 90;
    expectedEvaluations = 3 + budget.explore + budget.generations * budget.children + budget.polishRounds * polishPerRound;
    int tried = 0;

    auto publish = [this, &tried] (const Patch* better, float d, juce::AudioBuffer<float>* render)
    {
        const juce::ScopedLock sl (resultLock);
        result.tried = ++tried;
        result.progress = juce::jmin (0.99f, static_cast<float> (tried) / static_cast<float> (expectedEvaluations));
        if (better != nullptr)
        {
            result.patch = *better;
            result.distance = d;
            result.percent = analysis::matchPercent (d);
            result.found = true;
            bestRender = std::move (*render);
        }
    };

    juce::AudioBuffer<float> render;
    auto consider = [&] (const Patch& p) -> bool
    {
        if (cancelled())
            return false;
        const float d = evaluate (p, &render);
        const bool best = beam.empty() || d < beam.front().d;
        publish (best ? &p : nullptr, d, &render);

        const auto at = std::find_if (beam.begin(), beam.end(), [d] (const Cand& c) { return d < c.d; });
        if (beam.size() < beamSize || at != beam.end())
        {
            beam.insert (at, { p, d });
            if (beam.size() > beamSize)
                beam.pop_back();
        }
        return true;
    };

    const Patch first = guess (defaults0);
    if (! consider (start0) || ! consider (defaults0) || ! consider (first))
        return;

    // 1) Explore: jackpots around the guess. Half keep the guessed amp envelope.
    LockMask envLocks;
    for (auto* id : { param::ampAttack, param::ampDecay, param::ampSustain, param::ampRelease })
        envLocks.set (static_cast<size_t> (ParameterMap::indexOf (id)));

    Rng rng (seed0);
    for (int i = 0; i < budget.explore; ++i)
        if (! consider (Mutator::jackpot (first, rng.next(), i % 2 == 0 ? envLocks : LockMask {})))
            return;

    // 2) Invest around the best few, smaller and safer each round.
    for (int g = 0; g < budget.generations; ++g)
    {
        const float t = budget.generations > 1 ? static_cast<float> (g) / static_cast<float> (budget.generations - 1) : 1.0f;
        const float amount = 0.6f - 0.5f * t, risk = 0.6f * (1.0f - t) + 0.05f;
        for (int c = 0; c < budget.children; ++c)
        {
            const auto parent = beam[static_cast<size_t> (c) % beam.size()].patch;
            if (! consider (Mutator::invest (parent, { amount, risk, rng.next() })))
                return;
        }
    }

    // 3) Polish: knob by knob around the best.
    for (int round = 0; round < budget.polishRounds; ++round)
    {
        const float delta = round == 0 ? 0.08f : 0.03f;
        for (int i = 0; i < ParameterMap::numParams; ++i)
        {
            const auto& s = ParameterMap::spec (i);
            // Skip what a single full-velocity note can't hear (glide, velocity -> cutoff).
            if (s.weight <= 0.0f || juce::StringRef (s.id) == juce::StringRef (param::glideTime)
                || juce::StringRef (s.id) == juce::StringRef (param::velToCutoff))
                continue;
            const auto idx = static_cast<size_t> (i);

            if (s.steps > 1)
            {
                const int now = ParameterMap::toStep (s, beam.front().patch[idx]);
                for (int k = 0; k < s.steps; ++k)
                {
                    if (k == now || (s.ordered && std::abs (k - now) > 1))
                        continue;
                    auto p = beam.front().patch;
                    p[idx] = ParameterMap::fromStep (s, k);
                    if (! consider (p))
                        return;
                }
            }
            else
            {
                for (const float dir : { 1.0f, -1.0f })
                {
                    // Keep walking while it helps.
                    for (int walk = 0; walk < 3; ++walk)
                    {
                        auto p = beam.front().patch;
                        const float before = p[idx];
                        p[idx] = juce::jlimit (0.0f, 1.0f, before + dir * delta);
                        if (p[idx] == before)
                            break;
                        const float bestBefore = beam.front().d;
                        if (! consider (p))
                            return;
                        if (beam.front().d >= bestBefore)
                            break;
                    }
                }
            }
        }
    }

    const juce::ScopedLock sl (resultLock);
    result.progress = 1.0f;
}
}
