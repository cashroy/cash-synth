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
    out.setSize (2, total, false, false, true);
    out.clear();

    juce::AudioBuffer<float> chunk (2, block);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        chunk.setSize (2, n, false, false, true);
        chunk.clear();

        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, 1.0f), 0);
        if (hold >= pos && hold < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, midiNote), hold - pos);

        engine.render (chunk, midi);
        out.copyFrom (0, pos, chunk, 0, 0, n);
        out.copyFrom (1, pos, chunk, 1, 0, n);
    }
}


//============================================================================== CashMatcher
namespace
{
    int workerCount()
    {
        // Leave a core for the audio thread and the UI.
        return juce::jlimit (1, 8, juce::SystemStats::getNumCpus() - 1);
    }

    size_t at (const char* id)
    {
        const int i = ParameterMap::indexOf (id);
        jassert (i >= 0);
        return static_cast<size_t> (juce::jmax (0, i));
    }

    // Polish order: one part of the sound at a time, then the rest.
    std::vector<std::vector<int>> polishGroups()
    {
        using namespace param;
        const std::vector<std::vector<const char*>> named {
            { ampAttack, ampDecay, ampSustain, ampRelease },                                       // envelope
            { pitchEnvAmt, fltAttack, fltDecay, fltSustain, fltRelease, osc1Fine, osc2Fine, lfoToPitch, lfoRate }, // pitch
            { filterCutoff, filterReso, filterEnvAmt, filterKeyTrack, filterType },                // brightness
            { lfoWave, lfoToCutoff, lfo2Wave, lfo2Rate, lfo2Amount, lfo2Target, lfoToDrive },     // movement
            { unisonVoices, unisonDetune, unisonSpread },                                          // width
            { osc1Wave, osc1Level, osc2Wave, osc2Level, osc1Octave, osc1Semi, osc2Octave, osc2Semi,
              subLevel, noiseLevel, driveType, driveAmount },                                      // tone
        };

        auto polishable = [] (int i)
        {
            const auto& s = ParameterMap::spec (i);
            const juce::StringRef id (s.id);
            // A single full-velocity note can't hear glide or velocity -> cutoff.
            if (id == juce::StringRef (glideTime) || id == juce::StringRef (velToCutoff) || id == juce::StringRef (glideOn))
                return false;
            // M10: matched in Hz, so the sync mode and note values are left alone.
            for (auto* syncId : { lfo1Sync, lfo1Note, lfo2Sync, lfo2Note })
                if (id == juce::StringRef (syncId))
                    return false;
            // M11: delay and reverb are left off (and alone).
            if (isEffectParam (i))
                return false;
            return s.weight > 0.0f || isSectionSwitch (i);
        };

        std::vector<std::vector<int>> groups;
        std::vector<bool> used (static_cast<size_t> (ParameterMap::numParams), false);
        for (const auto& g : named)
        {
            groups.emplace_back();
            for (auto* id : g)
            {
                const int i = ParameterMap::indexOf (id);
                if (i >= 0 && polishable (i))
                {
                    groups.back().push_back (i);
                    used[static_cast<size_t> (i)] = true;
                }
            }
        }
        groups.emplace_back(); // section switches
        groups.emplace_back(); // anything else
        for (int i = 0; i < ParameterMap::numParams; ++i)
            if (! used[static_cast<size_t> (i)] && polishable (i))
                (isSectionSwitch (i) ? groups[groups.size() - 2] : groups.back()).push_back (i);
        return groups;
    }

    // Nearby values of one knob to try.
    std::vector<float> variants (int i, float now, float delta)
    {
        const auto& s = ParameterMap::spec (i);
        std::vector<float> v;
        if (s.steps > 1)
        {
            const int step = ParameterMap::toStep (s, now);
            for (int k = 0; k < s.steps; ++k)
                if (k != step && (! s.ordered || std::abs (k - step) <= 1))
                    v.push_back (ParameterMap::fromStep (s, k));
        }
        else
        {
            for (const float m : { 1.0f, -1.0f, 2.5f, -2.5f })
            {
                const float x = juce::jlimit (0.0f, 1.0f, now + m * delta);
                if (x != now && std::find (v.begin(), v.end(), x) == v.end())
                    v.push_back (x);
            }
        }
        return v;
    }

    int countOff (const Patch& p)
    {
        int n = 0;
        for (int i = ParameterMap::numParamsV3; i < ParameterMap::numParamsV4; ++i)
            n += p[static_cast<size_t> (i)] < 0.5f ? 1 : 0;
        return n;
    }
}

CashMatcher::CashMatcher (const juce::AudioProcessorValueTreeState& r)
    : juce::Thread ("Cash Match"), ranges (r), numWorkers (workerCount())
{
}

CashMatcher::~CashMatcher()
{
    stop();
    pool.reset(); // helpers finish before their workers go
}

void CashMatcher::ensureWorkers()
{
    while (static_cast<int> (workers.size()) < numWorkers)
        workers.push_back (std::make_unique<Worker> (ranges));
    if (pool == nullptr && numWorkers > 1)
        pool = std::make_unique<juce::ThreadPool> (numWorkers - 1, 0, juce::Thread::Priority::low);
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
    juce::AudioBuffer<float> audio (static_cast<int> (juce::jlimit (1u, 2u, reader->numChannels)), static_cast<int> (wanted));
    reader->read (&audio, 0, static_cast<int> (wanted), 0, true, true);
    return setTarget (audio, reader->sampleRate, file.getFileNameWithoutExtension());
}

juce::String CashMatcher::setTarget (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name)
{
    stop();

    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() <= 0 || sampleRate <= 0.0)
        return "That file is empty.";

    // Keep stereo (left/right; more channels fold into them, mono doubles up).
    std::array<std::vector<float>, 2> lr;
    for (auto& ch : lr)
        ch.assign (static_cast<size_t> (n), 0.0f);
    const int numIn = audio.getNumChannels();
    for (int ch = 0; ch < numIn; ++ch)
    {
        const bool both = numIn == 1;
        const float g = numIn <= 2 ? 1.0f : 2.0f / static_cast<float> (numIn);
        for (int side = 0; side < 2; ++side)
            if (both || ch % 2 == side)
                for (int i = 0; i < n; ++i)
                    lr[static_cast<size_t> (side)][static_cast<size_t> (i)] += audio.getSample (ch, i) * g;
    }

    // Resample to the analysis rate.
    const double ratio = sampleRate / SoundPrint::sampleRate;
    const int outN = juce::jmax (1, static_cast<int> (n / ratio));
    std::array<std::vector<float>, 2> res;
    for (size_t side = 0; side < 2; ++side)
    {
        if (std::abs (ratio - 1.0) < 1.0e-9)
        {
            res[side] = lr[side];
            continue;
        }
        res[side].assign (static_cast<size_t> (outN), 0.0f);
        juce::LagrangeInterpolator interp;
        lr[side].resize (lr[side].size() + 8, 0.0f); // interpolator reads a few samples ahead
        interp.process (ratio, lr[side].data(), res[side].data(), outN);
    }

    // Trim leading silence (anything under -40 dB of the peak), keep 2 ms of run-up.
    float peak = 0.0f;
    for (const auto& ch : res)
        for (auto s : ch)
            peak = juce::jmax (peak, std::abs (s));
    if (peak < 1.0e-4f)
        return "That file is silent.";

    int startAt = 0;
    while (startAt < outN && juce::jmax (std::abs (res[0][static_cast<size_t> (startAt)]), std::abs (res[1][static_cast<size_t> (startAt)])) < peak * 0.01f)
        ++startAt;
    startAt = juce::jmax (0, startAt - static_cast<int> (0.002 * SoundPrint::sampleRate));

    const int keep = juce::jmin (outN - startAt, static_cast<int> (maxSeconds * SoundPrint::sampleRate));
    if (keep < static_cast<int> (0.1 * SoundPrint::sampleRate))
        return "Too short: give it at least 0.1 s of sound.";

    Target t;
    t.name = name.isNotEmpty() ? name : "Untitled";
    t.audio.setSize (2, keep);
    for (int side = 0; side < 2; ++side)
        t.audio.copyFrom (side, 0, res[static_cast<size_t> (side)].data() + startAt, keep);
    const int fade = juce::jmin (keep / 4, static_cast<int> (0.01 * SoundPrint::sampleRate));
    t.audio.applyGainRamp (0, keep - fade, fade, 1.0f, 0.0f);
    t.print = analysis::analyse (t.audio.getReadPointer (0), t.audio.getReadPointer (1), keep);
    if (! t.print.valid)
        return "Couldn't hear anything in that file.";

    target = std::move (t);
    targetMint.reset();
    layerTables.reset();
    const juce::ScopedLock sl (resultLock);
    result = {};
    bestRender.setSize (0, 0);
    return {};
}

void CashMatcher::clearTarget()
{
    stop();
    target = {};
    targetMint.reset();
    layerTables.reset();
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
    tried = 0;
    {
        const juce::ScopedLock sl (resultLock);
        if (b.refit && b.mint && result.found)
            result.progress = 0.0f; // M14: re-fit one patch, keep the other
        else
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
    tried = 0;
    {
        const juce::ScopedLock sl (resultLock);
        if (b.refit && b.mint && result.found)
            result.progress = 0.0f; // M14: re-fit one patch, keep the other
        else
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

void CashMatcher::useMint (const MintModel* m)
{
    ensureWorkers();
    for (auto& w : workers)
        w->renderer.setMint (m);
}

std::shared_ptr<const MintModel> CashMatcher::getMintModel()
{
    if (targetMint == nullptr && hasTarget())
        targetMint = mint::build (target.audio.getReadPointer (0), target.audio.getReadPointer (1), target.audio.getNumSamples(), target.print);
    return targetMint;
}

Patch CashMatcher::mintPatch (const Patch& defaults, const MintModel& m) const
{
    using namespace param;
    Patch p = defaults;
    auto set = [&p] (const char* id, float norm) { p[at (id)] = juce::jlimit (0.0f, 1.0f, norm); };
    // Only the Mint sounds: every oscillator, the filter, its envelope (and pitch env), drive, LFOs, unison and glide off.
    for (auto* id : { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn })
        set (id, 0.0f);
    set (mintOn, 1.0f);
    // The amp envelope just opens: the Mint carries the sample's own attack, decay and release.
    set (ampAttack, normalised (ampAttack, 0.001f));
    set (ampSustain, 1.0f);
    const double tail = juce::jmax (0.0, m.seconds() - m.holdSeconds());
    set (ampRelease, normalised (ampRelease, static_cast<float> (juce::jlimit (0.05, 10.0, 3.0 * tail + 0.2))));
    // Played on the nearest key, tuned to the sample's own pitch (zero MINT FINE to play in tune).
    set (mintFine, normalised (mintFine, juce::jlimit (-100.0f, 100.0f, (m.rootNote - static_cast<float> (target.print.midiNote)) * 100.0f)));
    return p;
}

void CashMatcher::searchMint()
{
    using Mode = MatchBudget::Mode;
    const bool wantExact = budget.mode != Mode::layers, wantLayers = budget.mode != Mode::exact;
    expectedEvaluations = (wantExact ? 24 : 0) + (wantLayers ? 110 : 0);
    const auto model = getMintModel();
    if (model == nullptr || cancelled())
        return;
    if (wantExact)
    {
        useMint (model.get());

        Patch best = mintPatch (defaults0, *model);
        unsync (best);
        effectsOff (best);
        Eval bestE = evaluateBatch ({ best }).front();

        auto publish = [&]
        {
            juce::AudioBuffer<float> render;
            evaluate (*workers[0], best, &render);
            const juce::ScopedLock sl (resultLock);
            result.patch = best;
            result.distance = bestE.d;
            result.parts = bestE.parts;
            result.percent = analysis::matchPercent (bestE.d);
            result.sectionsOff = countOff (best);
            result.mint = model;
            result.found = true;
            result.exact = { best, result.percent, bestE.d, bestE.parts, model, 0, true };
            bestRender = std::move (render);
        };
        publish();

        // Light polish: the Mint's noise and tone, and the amp release over its tail.
        using namespace param;
        for (const float delta : { 0.08f, 0.03f })
        {
            std::vector<Patch> batch;
            for (auto* id : { mintNoise, mintTone, ampRelease })
            {
                const int i = ParameterMap::indexOf (id);
                for (float v : variants (i, best[static_cast<size_t> (i)], delta))
                {
                    batch.push_back (best);
                    batch.back()[static_cast<size_t> (i)] = v;
                }
            }
            if (cancelled())
                return;
            const auto evals = evaluateBatch (batch);
            bool better = false;
            for (size_t k = 0; k < batch.size(); ++k)
                if (evals[k].d < bestE.d - 1.0e-4f)
                {
                    best = batch[k];
                    bestE = evals[k];
                    better = true;
                }
            if (better)
                publish();
        }
    }
    if (wantLayers && ! cancelled())
        searchLayers (*model);
}

const MintModel* CashMatcher::modelFor (const Patch& p) const
{
    const bool lay = p[at (layers::layersOn)] >= 0.5f, minted = p[at (param::mintOn)] >= 0.5f;
    return lay && ! minted && layerTables != nullptr ? layerTables.get() : targetMint.get();
}

Patch CashMatcher::layersPatch (const Patch& defaults, const MintModel& m, const layers::Fit& f) const
{
    using namespace param;
    Patch p = defaults;
    auto set = [&p] (const char* id, float norm) { p[at (id)] = juce::jlimit (0.0f, 1.0f, norm); };
    for (auto* id : { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn })
        set (id, 0.0f);
    set (mintOn, 0.0f);
    // The amp envelope just opens: each layer has its own.
    set (ampAttack, normalised (ampAttack, 0.001f));
    set (ampSustain, 1.0f);
    const double tail = juce::jmax (0.0, m.seconds() - m.holdSeconds());
    set (ampRelease, normalised (ampRelease, static_cast<float> (juce::jlimit (0.05, 10.0, 3.0 * tail + 0.2))));
    for (const auto& [id, real] : f.values)
        set (id, normalised (id, real));
    // Vibrato, tremolo, wah: LFO 1 / 2, routed by each layer's own depths (the global routings stay at zero).
    if (f.lfo1Hz > 0.0f)
    {
        set (lfo1On, 1.0f);
        set (lfoWave, 0.0f);
        set (lfoRate, normalised (lfoRate, f.lfo1Hz));
        set (lfoToPitch, normalised (lfoToPitch, 0.0f));
        set (lfoToCutoff, normalised (lfoToCutoff, 0.0f));
        set (lfoToDrive, normalised (lfoToDrive, 0.0f));
    }
    if (f.lfo2Hz > 0.0f)
    {
        set (lfo2On, 1.0f);
        set (lfo2Wave, 0.0f);
        set (lfo2Rate, normalised (lfo2Rate, f.lfo2Hz));
        set (lfo2Amount, 0.0f);
    }
    return p;
}

void CashMatcher::searchLayers (const MintModel& model)
{
    const auto& tp = target.print;
    layers::Wobbles wob;
    if (tp.vibrato.present()) { wob.vibRate = tp.vibrato.rate; wob.vibDepth = tp.vibrato.depth; }
    if (tp.tremolo.present()) { wob.tremRate = tp.tremolo.rate; wob.tremDepthDb = tp.tremolo.depth; }
    if (tp.wah.present()) { wob.wahRate = tp.wah.rate; wob.wahDepth = tp.wah.depth; }
    const auto fitted = layers::fit (model, tp.midiNote, budget.detail, wob);
    auto tables = mint::tablesOnly (fitted.harmonics, fitted.phases, fitted.spreads);
    layerTables = tables;
    useMint (tables.get());

    Patch best = layersPatch (defaults0, model, fitted);
    unsync (best);
    effectsOff (best);
    if (cancelled())
        return;
    Eval bestE = evaluateBatch ({ best }).front();

    auto publish = [&]
    {
        juce::AudioBuffer<float> render;
        evaluate (*workers[0], best, &render);
        const juce::ScopedLock sl (resultLock);
        const int pc = analysis::matchPercent (bestE.d);
        result.layered = { best, pc, bestE.d, bestE.parts, tables, fitted.used, true };
        if (! result.exact.found)
        {
            result.patch = best;
            result.distance = bestE.d;
            result.parts = bestE.parts;
            result.percent = pc;
            result.sectionsOff = countOff (best);
            result.mint = tables;
            result.found = true;
        }
        layersRender = std::move (render);
    };
    publish();

    // Short polish (the knob-polish moves): each used layer's level, the loudest
    // layers' envelope, the noise levels and decays, the pitch envelope. Times move
    // by ratios (x0.5 / x2, then x0.75 / x1.33), the rest by steps.
    struct Knob { int index; bool time; };
    std::vector<Knob> knobs;
    auto add = [&knobs] (const char* id, bool time) { knobs.push_back ({ ParameterMap::indexOf (id), time }); };
    for (int l = 0; l < fitted.used; ++l)
    {
        add (layers::toneId (l, layers::level), false);
        if (l < 3)
        {
            add (layers::toneId (l, layers::attack), true);
            add (layers::toneId (l, layers::decay), true);
            add (layers::toneId (l, layers::sustain), false);
            add (layers::toneId (l, layers::release), true);
            add (layers::toneId (l, layers::curve), false); // M14 part 2: decay shape
        }
    }
    for (int n = 0; n < layers::numNoise; ++n)
        if (best[at (layers::noiseId (n, layers::nLevel))] > 0.0f)
        {
            add (layers::noiseId (n, layers::nLevel), false);
            add (layers::noiseId (n, layers::nDecay), true);
        }
    if (best[at (layers::pitchAmt)] != normalised (layers::pitchAmt, 0.0f))
    {
        add (layers::pitchAmt, false);
        add (layers::pitchTime, true);
    }
    auto moved = [this] (const Knob& k, float now, float step, float ratio)
    {
        if (! k.time)
            return juce::jlimit (0.0f, 1.0f, now + step);
        auto* param = ranges.getParameter (ParameterMap::spec (k.index).id);
        const float real = param->convertFrom0to1 (now) * ratio;
        return param->convertTo0to1 (param->getNormalisableRange().snapToLegalValue (real));
    };

    for (const auto [delta, ratio] : { std::pair { 0.05f, 2.0f }, std::pair { 0.02f, 1.33f } })
    {
        std::vector<Patch> batch;
        std::vector<int> knob;
        for (const auto& k : knobs)
            for (const float m : { 1.0f, -1.0f })
            {
                const int i = k.index;
                const float v = moved (k, best[static_cast<size_t> (i)], m * delta, m > 0.0f ? ratio : 1.0f / ratio);
                if (std::abs (v - best[static_cast<size_t> (i)]) < 1.0e-5f)
                    continue;
                batch.push_back (best);
                batch.back()[static_cast<size_t> (i)] = v;
                knob.push_back (i);
            }
        if (batch.empty() || cancelled())
            return;
        const auto evals = evaluateBatch (batch);
        // Each knob's best move, then all of them together (kept only if better still).
        Patch combined = best;
        std::vector<float> bestFor (static_cast<size_t> (ParameterMap::numParams), bestE.d);
        size_t single = batch.size();
        for (size_t k = 0; k < batch.size(); ++k)
        {
            const auto i = static_cast<size_t> (knob[k]);
            if (evals[k].d < bestFor[i] - 1.0e-4f)
            {
                bestFor[i] = evals[k].d;
                combined[i] = batch[k][i];
                if (single == batch.size() || evals[k].d < evals[single].d)
                    single = k;
            }
        }
        if (single == batch.size())
            continue;
        if (cancelled())
            return;
        const auto combinedE = evaluateBatch ({ combined }).front();
        if (combinedE.d < evals[single].d)
        {
            best = combined;
            bestE = combinedE;
        }
        else
        {
            best = batch[single];
            bestE = evals[single];
        }
        publish();
    }
}

CashMatcher::Result CashMatcher::getResult() const
{
    const juce::ScopedLock sl (resultLock);
    auto r = result;
    r.tried = tried.load();
    if (r.progress < 1.0f)
        r.progress = juce::jmin (0.99f, static_cast<float> (r.tried) / static_cast<float> (juce::jmax (1, expectedEvaluations.load())));
    return r;
}

juce::AudioBuffer<float> CashMatcher::getBestRender() const
{
    const juce::ScopedLock sl (resultLock);
    return result.exact.found || ! result.layered.found ? bestRender : layersRender;
}

juce::AudioBuffer<float> CashMatcher::getRender (bool layers) const
{
    const juce::ScopedLock sl (resultLock);
    return layers ? layersRender : bestRender;
}

CashMatcher::Eval CashMatcher::evaluate (Worker& w, const Patch& p, juce::AudioBuffer<float>* keep)
{
    const auto& tp = target.print;
    w.renderer.render (p, tp.midiNote, tp.holdSeconds, tp.seconds, w.scratch);
    const auto print = analysis::analyse (w.scratch.getReadPointer (0), w.scratch.getReadPointer (1), w.scratch.getNumSamples(),
                                          analysis::noteToHz (tp.midiNote), &tp);
    if (keep != nullptr)
        *keep = w.scratch;
    Eval e;
    if (print.valid)
    {
        e.parts = analysis::distanceParts (tp, print);
        e.d = analysis::combine (tp, e.parts);
    }
    else
    {
        e.parts.fill (1.0f); // silent: nothing matches
    }
    return e;
}

std::vector<CashMatcher::Eval> CashMatcher::evaluateBatch (const std::vector<Patch>& patches)
{
    ensureWorkers();
    std::vector<Eval> out (patches.size());
    std::atomic<size_t> next { 0 };

    auto work = [this, &patches, &out, &next] (Worker& w)
    {
        for (size_t i = next++; i < patches.size(); i = next++)
        {
            if (cancelled())
                continue; // leave it at "no match"
            out[i] = evaluate (w, patches[i]);
            ++tried;
        }
    };

    const int helpers = pool != nullptr ? juce::jmin (numWorkers - 1, static_cast<int> (patches.size()) - 1) : 0;
    batchDone.reset();
    batchActive = helpers;
    for (int h = 1; h <= helpers; ++h)
    {
        pool->addJob (std::function<void()> ([this, work, h]
                      {
                          work (*workers[static_cast<size_t> (h)]);
                          if (--batchActive == 0)
                              batchDone.signal();
                      }));
    }
    work (*workers[0]);
    if (helpers > 0)
        batchDone.wait();
    return out;
}

float CashMatcher::score (const Patch& p)
{
    jassert (! isSearching());
    if (! hasTarget())
        return 1.0f;
    ensureWorkers();
    workers[0]->renderer.setMint (modelFor (p));
    return evaluate (*workers[0], p).d;
}

analysis::Parts CashMatcher::scoreParts (const Patch& p)
{
    jassert (! isSearching());
    analysis::Parts parts {};
    if (! hasTarget())
    {
        parts.fill (1.0f);
        return parts;
    }
    ensureWorkers();
    workers[0]->renderer.setMint (modelFor (p));
    return evaluate (*workers[0], p).parts;
}

Patch CashMatcher::overlay (const Patch& base, const Patch& guessed, const LockMask& features)
{
    Patch p = base;
    for (size_t i = 0; i < p.size(); ++i)
        if (features.test (i))
            p[i] = guessed[i];
    return p;
}

Patch CashMatcher::guess (const Patch& defaults, LockMask* features) const
{
    using namespace param;
    Patch p = defaults;
    if (features != nullptr)
        features->reset();
    if (! hasTarget())
        return p;
    auto feature = [features] (std::initializer_list<const char*> ids)
    {
        if (features != nullptr)
            for (auto* id : ids)
                features->set (at (id));
    };
    feature ({ ampAttack, ampDecay, ampSustain, ampRelease, lfo1On, lfo2On, unisonOn, driveOn, glideOn, filterEnvOn, osc1On, osc2On, filterOn });

    const auto& tp = target.print;
    const auto& env = tp.envelope;
    const double frameSeconds = tp.seconds / SoundPrint::envFrames;
    auto set = [&p] (const char* id, float norm) { p[at (id)] = juce::jlimit (0.0f, 1.0f, norm); };
    auto setReal = [this, &set] (const char* id, float real) { set (id, normalised (id, real)); };
    auto step = [] (const char* id, int s) { return ParameterMap::fromStep (ParameterMap::spec (ParameterMap::indexOf (id)), s); };
    auto toGain = [] (float norm) { return juce::Decibels::decibelsToGain ((norm - 1.0f) * 60.0f); };

    // Amp envelope from the 10 ms level track, smoothed over ~100 ms so beating
    // (unison, chorus) and tremolo don't read as the attack peak.
    const auto& lv = tp.level;
    const int frames = static_cast<int> (lv.size());
    const double hop = tp.hopSeconds;
    std::vector<float> smooth (lv.size(), 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        float sum = 0.0f;
        int count = 0;
        for (int k = juce::jmax (0, f - 5); k <= juce::jmin (frames - 1, f + 5); ++k, ++count)
            sum += lv[static_cast<size_t> (k)];
        smooth[static_cast<size_t> (f)] = sum / static_cast<float> (juce::jmax (1, count));
    }
    float peakNorm = 0.0f;
    for (auto v : smooth)
        peakNorm = juce::jmax (peakNorm, v);
    int peakFrame = 0; // first frame within ~1 dB of the top
    while (peakFrame < frames - 1 && smooth[static_cast<size_t> (peakFrame)] < peakNorm - 0.02f)
        ++peakFrame;

    int attackFrame = 0; // the (linear) attack is ~70 % done 3 dB under the peak
    while (attackFrame < peakFrame && lv[static_cast<size_t> (attackFrame)] < peakNorm - 0.05f)
        ++attackFrame;

    const int holdFrame = juce::jlimit (0, juce::jmax (0, frames - 1), static_cast<int> (tp.holdSeconds / hop));
    float sustainNorm = 0.0f; // 0 = silence
    if (tp.regionEnd > tp.regionStart + 3)
    {
        std::vector<float> steady (lv.begin() + tp.regionStart, lv.begin() + tp.regionEnd);
        std::nth_element (steady.begin(), steady.begin() + static_cast<std::ptrdiff_t> (steady.size() / 2), steady.end());
        // Still falling at the key-up = a decay, not a sustain level.
        const float atHold = smooth[static_cast<size_t> (juce::jmax (0, holdFrame - 6))];
        sustainNorm = juce::jmin (steady[steady.size() / 2], atHold + 0.05f);
    }
    const float sustain = sustainNorm > 0.0f ? juce::jmin (1.0f, toGain (1.0f + (sustainNorm - peakNorm))) : 0.0f;

    int decayFrame = peakFrame;
    const float decayTarget = sustainNorm + (peakNorm - sustainNorm) * 0.3f;
    while (decayFrame < holdFrame && smooth[static_cast<size_t> (decayFrame)] > decayTarget)
        ++decayFrame;

    int releaseFrame = holdFrame;
    while (releaseFrame < frames - 1 && lv[static_cast<size_t> (releaseFrame)] > juce::jmax (0.33f, sustainNorm - 0.33f))
        ++releaseFrame;

    setReal (ampAttack, static_cast<float> (juce::jmax (0.002, attackFrame * hop / 0.7)));
    setReal (ampDecay, static_cast<float> (juce::jmax (0.01, (decayFrame - peakFrame + 0.5) * hop)));
    set (ampSustain, sustain);
    setReal (ampRelease, tp.holdSeconds < tp.seconds - 0.02 ? static_cast<float> (juce::jmax (0.02, (releaseFrame - holdFrame + 0.5) * hop)) : 0.3f);
    juce::ignoreUnused (frameSeconds, env);

    // Section switches: only what the sample needs (M9). Each find below turns its own back on.
    for (auto* id : { subNoiseOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn, filterEnvOn })
        set (id, 0.0f);
    set (osc1On, 1.0f);
    set (osc2On, 1.0f);
    set (filterOn, 1.0f);

    // Brightness: cutoff a little above the steady brightness; a falling
    // brightness curve = filter envelope (+/-60 st at 100 %).
    const float settledHz = std::exp2 (tp.brightSettled);
    set (filterType, step (filterType, lowPass));
    setReal (filterCutoff, juce::jlimit (200.0f, 18000.0f, (tp.brightSettled > 0.0f ? settledHz : tp.centroidHz) * 2.5f));
    set (filterReso, 0.1f);
    setReal (filterEnvAmt, 0.0f);
    setReal (driveAmount, 0.0f);
    setReal (lfo2Amount, 0.0f);
    setReal (lfoToPitch, 0.0f);
    setReal (lfoToCutoff, 0.0f);

    const float brightDrop = tp.brightStart - tp.brightSettled;
    float envDecay = 0.0f;
    if (brightDrop > 0.4f)
    {
        set (filterEnvOn, 1.0f);
        setReal (filterEnvAmt, juce::jlimit (0.0f, 1.0f, brightDrop * 12.0f / 60.0f * 1.3f));
        feature ({ filterEnvAmt });
        envDecay = static_cast<float> (juce::jmax (0.02, tp.brightSettleSeconds));
    }

    // Pitch envelope: the attack starts off the note and slides onto it.
    if (std::abs (tp.pitchStart) > 0.3f)
    {
        set (filterEnvOn, 1.0f);
        setReal (pitchEnvAmt, juce::jlimit (-1.0f, 1.0f, tp.pitchStart / pitchEnvRange));
        feature ({ pitchEnvAmt });
        const auto settle = static_cast<float> (juce::jmax (0.01, tp.pitchSettleSeconds * 0.6));
        envDecay = envDecay > 0.0f ? 0.5f * (envDecay + settle) : settle;
    }
    if (envDecay > 0.0f)
    {
        setReal (fltAttack, 0.001f);
        setReal (fltDecay, envDecay);
        setReal (fltSustain, 0.0f);
        feature ({ fltAttack, fltDecay, fltSustain });
    }

    // Movement: vibrato and wah = LFO 1, tremolo = LFO 2 on amp. A wide sample's
    // slow level/brightness wobble is most likely unison beating, which unison makes itself.
    const bool beating = tp.width > 0.08f;
    const bool wah = tp.wah.present() && ! (beating && tp.wah.rate < 3.0f);
    const bool tremolo = tp.tremolo.present() && ! (beating && tp.tremolo.rate < 3.0f);
    if (tp.vibrato.present() || wah)
    {
        set (lfo1On, 1.0f);
        set (lfoWave, step (lfoWave, sine));
        setReal (lfoRate, tp.vibrato.present() ? tp.vibrato.rate : tp.wah.rate);
        if (tp.vibrato.present())
            setReal (lfoToPitch, tp.vibrato.depth);
        if (wah)
            setReal (lfoToCutoff, juce::jlimit (0.0f, 1.0f, tp.wah.depth * 12.0f / 36.0f * 1.5f));
        feature ({ lfoWave, lfoRate, lfoToPitch, lfoToCutoff });
    }
    if (tremolo)
    {
        // Tremolo gain runs 1 .. 1 - amount, so peak-to-peak dB = -20 log10 (1 - amount).
        set (lfo2On, 1.0f);
        set (lfo2Wave, step (lfo2Wave, sine));
        set (lfo2Target, step (lfo2Target, toAmp));
        setReal (lfo2Rate, tp.tremolo.rate);
        setReal (lfo2Amount, juce::jlimit (0.0f, 1.0f, 1.0f - juce::Decibels::decibelsToGain (-2.0f * tp.tremolo.depth)));
        feature ({ lfo2Wave, lfo2Target, lfo2Rate, lfo2Amount });
    }

    // Width: unison spread.
    if (tp.width > 0.08f)
    {
        set (unisonOn, 1.0f);
        setReal (unisonVoices, 4.0f);
        setReal (unisonSpread, juce::jlimit (0.0f, 1.0f, tp.width * 1.5f));
        feature ({ unisonVoices, unisonSpread });
    }

    // Noise vs tone.
    if (! tp.pitched || tp.flatness > 0.35f)
    {
        set (subNoiseOn, 1.0f);
        set (subLevel, 0.0f);
        set (noiseLevel, tp.pitched ? juce::jmin (1.0f, tp.flatness * 1.5f) : 0.8f);
    }
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
    if (budget.mint)
    {
        searchMint();
        const juce::ScopedLock sl (resultLock);
        result.progress = 1.0f;
        return;
    }
    useMint (nullptr);

    struct Cand
    {
        Patch patch;
        Eval e;
    };
    std::vector<Cand> beam; // best few, best first
    constexpr size_t beamSize = 4;
    const auto groups = polishGroups();

    {
        int polish = 0;
        for (const auto& g : groups)
        {
            polish += 1; // the combined try
            for (int i : g)
                polish += static_cast<int> (variants (i, 0.5f, 0.05f).size());
        }
        expectedEvaluations = 7 + budget.explore + budget.generations * budget.children + budget.polishRounds * polish
                            + (ParameterMap::numParamsV4 - ParameterMap::numParamsV3) + 1;
    }

    auto publish = [this] (const Cand& best)
    {
        juce::AudioBuffer<float> render;
        evaluate (*workers[0], best.patch, &render); // keep its audio for HEAR MATCH and the cheque
        const juce::ScopedLock sl (resultLock);
        result.patch = best.patch;
        result.distance = best.e.d;
        result.parts = best.e.parts;
        result.percent = analysis::matchPercent (best.e.d);
        result.sectionsOff = countOff (best.patch);
        result.found = true;
        bestRender = std::move (render);
    };

    // Scores a batch in parallel and keeps the best few. False when cancelled.
    auto take = [&] (std::vector<Patch> patches) -> bool
    {
        if (cancelled())
            return false;
        // M10: matches are made in Hz (renders run at 120 BPM, stopped); sync is the user's call.
        // M11: and with the delay and reverb off (the voices are what gets matched).
        for (auto& p : patches)
        {
            unsync (p);
            effectsOff (p);
            p[at (param::mintOn)] = 0.0f; // synth knobs only
        }
        const auto evals = evaluateBatch (patches);
        if (cancelled())
            return false;
        const float bestBefore = beam.empty() ? 2.0f : beam.front().e.d;
        for (size_t k = 0; k < patches.size(); ++k)
        {
            const float d = evals[k].d;
            const auto pos = std::find_if (beam.begin(), beam.end(), [d] (const Cand& c) { return d < c.e.d; });
            if (beam.size() < beamSize || pos != beam.end())
            {
                beam.insert (pos, { patches[k], evals[k] });
                if (beam.size() > beamSize)
                    beam.pop_back();
            }
        }
        if (beam.front().e.d < bestBefore)
            publish (beam.front());
        return true;
    };

    // 0) Where we are, the defaults, the guess from the analysis (also with every
    //    section on), and what the analysis measured laid over the current sound and the defaults.
    LockMask features;
    const Patch first = guess (defaults0, &features);
    Patch firstAllOn = first;
    allSectionsOn (firstAllOn);
    if (! take ({ start0, defaults0, first, firstAllOn, overlay (start0, first, features), overlay (defaults0, first, features) }))
        return;

    // 1) Explore: jackpots around the guess. Some keep the guessed amp envelope,
    //    some also keep the guessed movement, pitch env and width.
    LockMask envLocks, shapeLocks;
    for (auto* id : { param::ampAttack, param::ampDecay, param::ampSustain, param::ampRelease })
        envLocks.set (at (id));
    shapeLocks = features;

    Rng rng (seed0);
    {
        std::vector<Patch> batch;
        for (int i = 0; i < budget.explore; ++i)
            batch.push_back (Mutator::jackpot (i % 2 == 0 ? first : beam.front().patch, rng.next(), i % 3 == 0 ? envLocks : (i % 3 == 1 ? shapeLocks : LockMask {})));
        if (! batch.empty() && ! take (batch))
            return;
    }

    // 2) Invest around the best few, smaller and safer each round.
    for (int g = 0; g < budget.generations; ++g)
    {
        const float t = budget.generations > 1 ? static_cast<float> (g) / static_cast<float> (budget.generations - 1) : 1.0f;
        const float amount = 0.6f - 0.5f * t, risk = 0.6f * (1.0f - t) + 0.05f;
        std::vector<Patch> batch;
        for (int c = 0; c < budget.children; ++c)
            batch.push_back (Mutator::invest (beam[static_cast<size_t> (c) % beam.size()].patch, { amount, risk, rng.next() }));
        if (! batch.empty() && ! take (batch))
            return;
    }

    // The best tone so far with the measured movement/width/envelope on top.
    if (! take ({ overlay (beam.front().patch, first, features) }))
        return;

    // 3) Staged polish: one part at a time (envelope, pitch, brightness, movement,
    //    width, tone, section switches, the rest). Every nearby value of every knob
    //    in the group is tried at once; the winners are then tried together.
    for (int round = 0; round < budget.polishRounds; ++round)
    {
        const float delta = round == 0 ? 0.06f : 0.025f;
        for (const auto& group : groups)
        {
            const Patch base = beam.front().patch;
            const float baseD = beam.front().e.d;
            std::vector<Patch> batch;
            std::vector<int> knob;
            for (int i : group)
                for (float v : variants (i, base[static_cast<size_t> (i)], delta))
                {
                    batch.push_back (base);
                    batch.back()[static_cast<size_t> (i)] = v;
                    knob.push_back (i);
                }
            if (batch.empty())
                continue;
            if (cancelled())
                return;
            const auto evals = evaluateBatch (batch);

            // Keep the batch's results, and combine each knob's best move.
            Patch combined = base;
            std::vector<float> bestFor (static_cast<size_t> (ParameterMap::numParams), baseD);
            int moved = 0;
            for (size_t k = 0; k < batch.size(); ++k)
            {
                const auto i = static_cast<size_t> (knob[k]);
                if (evals[k].d < bestFor[i])
                {
                    moved += bestFor[i] == baseD ? 1 : 0;
                    bestFor[i] = evals[k].d;
                    combined[i] = batch[k][i];
                }
            }
            // take() re-scores; cheap compared with the batch, and keeps the beam honest.
            std::vector<Patch> keepers;
            for (size_t k = 0; k < batch.size(); ++k)
                if (evals[k].d < baseD)
                    keepers.push_back (batch[k]);
            if (moved > 1)
                keepers.push_back (combined);
            if (! keepers.empty() && ! take (keepers))
                return;
        }
    }

    // 4) Tidy: switch off sections the match doesn't need (cleaner patch, less CPU),
    //    as long as it costs no more than a hair of the score.
    {
        const Patch best = beam.front().patch;
        const float bestD = beam.front().e.d, allowed = bestD + 0.003f;
        std::vector<Patch> batch;
        std::vector<int> which;
        for (int i = ParameterMap::numParamsV3; i < ParameterMap::numParamsV4; ++i)
            if (best[static_cast<size_t> (i)] >= 0.5f)
            {
                batch.push_back (best);
                batch.back()[static_cast<size_t> (i)] = 0.0f;
                Mutator::makePlayable (batch.back());
                which.push_back (i);
            }
        if (! batch.empty())
        {
            if (cancelled())
                return;
            const auto evals = evaluateBatch (batch);
            Patch tidy = best;
            int off = 0, single = -1;
            for (size_t k = 0; k < batch.size(); ++k)
                if (evals[k].d <= allowed && batch[k][static_cast<size_t> (which[k])] < 0.5f) // makePlayable may have refused
                {
                    tidy[static_cast<size_t> (which[k])] = 0.0f;
                    ++off;
                    if (single < 0 || evals[k].d < evals[static_cast<size_t> (single)].d)
                        single = static_cast<int> (k);
                }
            if (off > 0)
            {
                Mutator::makePlayable (tidy);
                auto tidyEval = off > 1 ? evaluateBatch ({ tidy }).front() : evals[static_cast<size_t> (single)];
                if (off > 1 && tidyEval.d > allowed)
                {
                    tidy = batch[static_cast<size_t> (single)];
                    tidyEval = evals[static_cast<size_t> (single)];
                }
                if (! cancelled())
                {
                    beam.insert (beam.begin(), { tidy, tidyEval });
                    publish (beam.front());
                }
            }
        }
    }

    const juce::ScopedLock sl (resultLock);
    result.progress = 1.0f;
}
}
