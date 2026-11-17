// M14 LAYERS: Cash Match fits an editable patch of wavetable layers, filters,
// envelopes and noise (normal synth parts) next to the Mint's EXACT one.
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "CashMatch.h"
#include "CashCode.h"
#include "MatchPanel.h"
#include "SoundPrint.h"
#include "Mint.h"
#include "Layers.h"
#include "PluginEditor.h"

namespace
{
constexpr double sr = cash::SoundPrint::sampleRate;
namespace L = cash::layers;

struct Plugin
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
};

size_t idx (const char* id)
{
    const int i = cash::ParameterMap::indexOf (id);
    REQUIRE (i >= 0);
    return static_cast<size_t> (i);
}

// Sets a real value on a patch (normalised through the parameter's own range).
void setReal (CashSynthAudioProcessor& p, cash::Patch& patch, const char* id, float real)
{
    auto* param = p.getState().getParameter (id);
    REQUIRE (param != nullptr);
    patch[idx (id)] = param->convertTo0to1 (real);
}

juce::AudioBuffer<float> playNote (CashSynthAudioProcessor& p, int note, double hold, double total, double rate = sr)
{
    constexpr int block = 256;
    p.prepareToPlay (rate, block);
    const int n = static_cast<int> (total * rate), h = static_cast<int> (hold * rate);
    juce::AudioBuffer<float> out (2, n), chunk (2, block);
    for (int pos = 0; pos < n; pos += block)
    {
        const int num = juce::jmin (block, n - pos);
        chunk.setSize (2, num, false, false, true);
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (h >= pos && h < pos + num)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), h - pos);
        p.processBlock (chunk, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, chunk, ch, 0, num);
    }
    return out;
}

float rms (const juce::AudioBuffer<float>& b, double from, double to)
{
    const int a = static_cast<int> (from * sr), e = juce::jmin (b.getNumSamples(), static_cast<int> (to * sr));
    double s = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = a; i < e; ++i)
            s += b.getSample (ch, i) * b.getSample (ch, i);
    return static_cast<float> (std::sqrt (s / juce::jmax (1, (e - a) * b.getNumChannels())));
}

// Energy above `hz` vs total (one-pole split, crude but monotonic).
float highShare (const juce::AudioBuffer<float>& b, double from, double to, float hz)
{
    const float a = std::exp (-juce::MathConstants<float>::twoPi * hz / static_cast<float> (sr));
    float lp = 0.0f;
    double hi = 0.0, all = 0.0;
    for (int i = static_cast<int> (from * sr); i < juce::jmin (b.getNumSamples(), static_cast<int> (to * sr)); ++i)
    {
        const float x = b.getSample (0, i);
        lp = x + a * (lp - x);
        hi += (x - lp) * (x - lp);
        all += x * x;
    }
    return all > 0.0 ? static_cast<float> (hi / all) : 0.0f;
}

// A layers-only patch: every other section off, layer 1 on (the default saw table).
cash::Patch layersPatch (CashSynthAudioProcessor& p)
{
    using namespace cash::param;
    auto patch = cash::defaultPatch (p.getState());
    for (auto* id : { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn })
        patch[idx (id)] = 0.0f;
    patch[idx (L::layersOn)] = 1.0f;
    setReal (p, patch, ampAttack, 0.001f);
    patch[idx (ampSustain)] = 1.0f;
    setReal (p, patch, ampRelease, 3.0f);
    setReal (p, patch, L::toneId (0, L::attack), 0.001f);
    setReal (p, patch, L::toneId (0, L::sustain), 1.0f);
    setReal (p, patch, L::toneId (0, L::release), 0.05f);
    return patch;
}

// Two parts with different envelopes: a sustained low body (harmonics 1-4) and a
// fast-decaying bright click (harmonics 9-14), plus a little breath noise.
juce::AudioBuffer<float> twoPart (double f0 = 220.0, double hold = 0.9, double total = 1.2)
{
    const int n = static_cast<int> (total * sr);
    juce::AudioBuffer<float> out (2, n);
    juce::Random rng (5);
    float lp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double body = (t < 0.03 ? t / 0.03 : 1.0) * (t < hold ? 1.0 : std::exp (-(t - hold) / 0.05));
        const double click = std::exp (-t / 0.04);
        double s = 0.0;
        for (int k = 1; k <= 4; ++k)
            s += body * 0.25 / k * std::sin (juce::MathConstants<double>::twoPi * f0 * k * t);
        for (int k = 9; k <= 14; ++k)
            s += click * 0.12 * std::sin (juce::MathConstants<double>::twoPi * f0 * k * t);
        const float w = rng.nextFloat() * 2.0f - 1.0f;
        lp += 0.3f * (w - lp);
        const auto v = static_cast<float> (s) + static_cast<float> (body) * 0.01f * lp;
        out.setSample (0, i, v);
        out.setSample (1, i, v);
    }
    return out;
}
}

TEST_CASE ("Layers off is bit-identical whatever the layer knobs say", "[layers][engine]")
{
    Plugin a, b;
    auto patch = cash::defaultPatch (b.p.getState());
    // Every layer knob moved, but LAYERS off.
    for (int i = cash::ParameterMap::numParamsV8; i < cash::ParameterMap::numParams; ++i)
        if (i != static_cast<int> (idx (L::layersOn)))
            patch[static_cast<size_t> (i)] = 0.73f;
    b.p.setPatch (patch, "knobs");
    const auto x = playNote (a.p, 60, 0.4, 0.7);
    const auto y = playNote (b.p, 60, 0.4, 0.7);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < x.getNumSamples(); ++i)
            REQUIRE (x.getSample (ch, i) == y.getSample (ch, i));
}

TEST_CASE ("Layer engine: level, detail, envelope and filter per layer", "[layers][engine]")
{
    Plugin pl;
    auto& p = pl.p;
    auto base = layersPatch (p);

    SECTION ("level scales the layer; LAYERS off is silent")
    {
        p.setPatch (base, "l");
        const float full = rms (playNote (p, 57, 0.5, 0.6), 0.1, 0.4);
        REQUIRE (full > 0.01f);
        auto half = base;
        setReal (p, half, L::toneId (0, L::level), 0.35f);
        p.setPatch (half, "half");
        const float h = rms (playNote (p, 57, 0.5, 0.6), 0.1, 0.4);
        CHECK (std::abs (h / full - 0.5f) < 0.03f);
        auto off = base;
        off[idx (L::layersOn)] = 0.0f;
        p.setPatch (off, "off");
        CHECK (rms (playNote (p, 57, 0.5, 0.6), 0.1, 0.4) < 1.0e-6f);
    }

    SECTION ("detail = how many tone layers play")
    {
        auto two = base;
        setReal (p, two, L::toneId (1, L::attack), 0.001f);
        setReal (p, two, L::toneId (1, L::sustain), 1.0f);
        setReal (p, two, L::toneId (1, L::octave), 1.0f);
        setReal (p, two, L::detail, 1.0f);
        p.setPatch (two, "d1");
        const float one = rms (playNote (p, 57, 0.5, 0.6), 0.1, 0.4);
        setReal (p, two, L::detail, 2.0f);
        p.setPatch (two, "d2");
        const float both = rms (playNote (p, 57, 0.5, 0.6), 0.1, 0.4);
        CHECK (both > one * 1.2f);
    }

    SECTION ("each layer has its own amp envelope")
    {
        auto slow = base;
        setReal (p, slow, L::toneId (0, L::attack), 0.4f);
        setReal (p, slow, L::toneId (0, L::release), 0.5f);
        p.setPatch (slow, "slow");
        const auto out = playNote (p, 57, 0.6, 1.4);
        CHECK (rms (out, 0.0, 0.05) < 0.3f * rms (out, 0.45, 0.55)); // still rising early
        CHECK (rms (out, 0.62, 0.7) > 0.3f * rms (out, 0.45, 0.55)); // release lingers ...
        CHECK (rms (out, 1.2, 1.4) < 0.05f * rms (out, 0.45, 0.55)); // ... then dies (-60 dB in 0.5 s)
    }

    SECTION ("each layer has its own filter, opened by its own envelope")
    {
        p.setPatch (base, "open");
        const float open = highShare (playNote (p, 45, 0.5, 0.6), 0.1, 0.4, 2000.0f);
        auto dark = base;
        setReal (p, dark, L::toneId (0, L::cutoff), 300.0f);
        p.setPatch (dark, "dark");
        const float shut = highShare (playNote (p, 45, 0.5, 0.6), 0.1, 0.4, 2000.0f);
        CHECK (shut < open * 0.2f);
        // The filter envelope opens it at the start, then it closes.
        setReal (p, dark, L::toneId (0, L::envAmt), 1.0f);
        setReal (p, dark, L::toneId (0, L::fAttack), 0.001f);
        setReal (p, dark, L::toneId (0, L::fDecay), 0.4f);
        setReal (p, dark, L::toneId (0, L::fSustain), 0.0f);
        p.setPatch (dark, "env");
        const auto out = playNote (p, 45, 0.5, 0.6);
        CHECK (highShare (out, 0.0, 0.05, 1000.0f) > 3.0f * highShare (out, 0.3, 0.45, 1000.0f));
    }

    SECTION ("noise layers: band-passed noise with their own envelope")
    {
        auto noise = base;
        setReal (p, noise, L::toneId (0, L::level), 0.0f);
        setReal (p, noise, L::noiseId (0, L::nLevel), 0.5f);
        setReal (p, noise, L::noiseId (0, L::nCentre), 4000.0f);
        setReal (p, noise, L::noiseId (0, L::nWidth), 1.0f);
        setReal (p, noise, L::noiseId (0, L::nAttack), 0.001f);
        setReal (p, noise, L::noiseId (0, L::nSustain), 1.0f);
        p.setPatch (noise, "noise");
        const auto out = playNote (p, 60, 0.4, 0.5);
        CHECK (rms (out, 0.1, 0.3) > 0.005f);
        CHECK (highShare (out, 0.1, 0.3, 1500.0f) > 0.7f);
    }
}

TEST_CASE ("Fitting splits a sound into layers by envelope shape", "[layers][fit]")
{
    Plugin pl;
    auto& m = pl.p.getMatcher();
    REQUIRE (m.setTarget (twoPart(), sr, "TwoPart").isEmpty());
    const auto model = m.getMintModel();
    REQUIRE (model != nullptr);
    const int key = m.getTargetPrint().midiNote;

    const auto fit = L::fit (*model, key, 2, {});
    REQUIRE (fit.used == 2);
    auto value = [&fit] (const char* id)
    {
        for (const auto& [k, v] : fit.values)
            if (juce::StringRef (k) == juce::StringRef (id))
                return v;
        FAIL ("missing " << id);
        return 0.0f;
    };
    // Loudest first: the sustained body, then the click (short decay, little sustain).
    CHECK (value (L::toneId (0, L::sustain)) > 0.5f);
    CHECK (value (L::toneId (1, L::sustain)) < 0.1f);
    CHECK (value (L::toneId (1, L::decay)) < 0.6f);
    // The body's wavetable is harmonics 1-4; the click's sits up at 9-14.
    auto harm = [&fit] (int layer, int h) { return fit.harmonics[static_cast<size_t> (layer * L::numHarmonics + h - 1)]; };
    float bodyLow = 0.0f, bodyHigh = 0.0f;
    for (int h = 1; h <= 4; ++h) bodyLow += harm (0, h) * harm (0, h);
    for (int h = 9; h <= 14; ++h) bodyHigh += harm (0, h) * harm (0, h);
    CHECK (bodyLow > 10.0f * bodyHigh);
    float clickHigh = 0.0f;
    for (int h = 5; h <= L::numHarmonics; ++h) clickHigh += harm (1, h) * harm (1, h);
    CHECK (clickHigh > 0.0f);
    // Detail 1 = everything in one layer; asking for more than there are partials is fine.
    CHECK (L::fit (*model, key, 1, {}).used == 1);
    CHECK (L::fit (*model, key, 8, {}).used >= 2);
}

TEST_CASE ("Fitting finds the pitch drop and the noise", "[layers][fit]")
{
    // A kick-like sweep (300 Hz -> 60 Hz in ~60 ms) over a noise burst.
    const int n = static_cast<int> (0.6 * sr);
    juce::AudioBuffer<float> kick (2, n);
    juce::Random rng (3);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double hz = 60.0 + 240.0 * std::exp (-t / 0.02);
        ph += juce::MathConstants<double>::twoPi * hz / sr;
        const auto v = static_cast<float> (0.8 * std::exp (-t / 0.2) * std::sin (ph)) + 0.4f * static_cast<float> (std::exp (-t / 0.1)) * (rng.nextFloat() * 2.0f - 1.0f);
        kick.setSample (0, i, v);
        kick.setSample (1, i, v);
    }
    Plugin pl;
    auto& m = pl.p.getMatcher();
    REQUIRE (m.setTarget (kick, sr, "Kick").isEmpty());
    const auto model = m.getMintModel();
    REQUIRE (model != nullptr);
    const auto fit = L::fit (*model, m.getTargetPrint().midiNote, 4, {});
    float amt = 0.0f, noise = 0.0f;
    for (const auto& [k, v] : fit.values)
    {
        if (juce::StringRef (k) == juce::StringRef (L::pitchAmt)) amt = v;
        if (juce::StringRef (k) == juce::StringRef (L::noiseId (0, L::nLevel))) noise = v;
    }
    CHECK (amt > 3.0f);   // starts well above where it settles
    CHECK (noise > 0.0f);
}

TEST_CASE ("Cash Match makes EXACT and LAYERS; the cheque picks, each deposit is undoable", "[layers][match]")
{
    Plugin pl;
    auto& p = pl.p;
    auto& m = p.getMatcher();
    REQUIRE (m.setTarget (twoPart(), sr, "TwoPart").isEmpty());
    const auto before = p.getPatch();
    m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 3);
    const auto r = m.getResult();
    REQUIRE (r.found);
    REQUIRE (r.exact.found);
    REQUIRE (r.layered.found);
    CHECK (r.exact.percent >= 90);
    CHECK (r.layered.percent >= 80);
    CHECK (r.layered.patch[idx (L::layersOn)] == 1.0f);
    CHECK (r.layered.patch[idx (cash::param::mintOn)] == 0.0f);
    CHECK (r.exact.patch[idx (L::layersOn)] == 0.0f);
    REQUIRE (r.layered.mint != nullptr);
    CHECK (r.layered.mint->numPartials == 0);            // tables only: small to save
    CHECK (! r.layered.mint->layerWaves.empty());
    CHECK (r.layered.mint->toMemory().getSize() < 16 * 1024);
    CHECK (m.getRender (true).getNumSamples() > 0);
    CHECK (m.getRender (false).getNumSamples() > 0);
    // Scoring a patch uses its own model.
    CHECK (cash::analysis::matchPercent (m.score (r.layered.patch)) >= r.layered.percent - 3);
    CHECK (cash::analysis::matchPercent (m.score (r.exact.patch)) >= r.exact.percent - 3);

    cash::MatchPanel panel (p);
    panel.setSize (900, 520);
    panel.sync();
    CHECK (panel.getExactButton().getButtonText().contains (juce::String (r.exact.percent)));
    CHECK (panel.getLayersButton().getButtonText().contains (juce::String (r.layered.percent)));
    CHECK (panel.getExactButton().getToggleState());

    panel.getLayersButton().onClick();
    CHECK (p.getMatchPicksLayers());
    CHECK (panel.getLayersButton().getToggleState());
    REQUIRE (p.depositMatch());
    CHECK (p.getPatch()[idx (L::layersOn)] == 1.0f);
    CHECK (p.getMint() == r.layered.mint);
    CHECK (p.getLedger().current()->memo.contains ("layers"));
    CHECK (rms (playNote (p, m.getTargetPrint().midiNote, 0.5, 0.8), 0.1, 0.4) > 0.005f);

    panel.pick (false);
    REQUIRE (p.depositMatch());
    CHECK (p.getPatch()[idx (cash::param::mintOn)] == 1.0f);
    CHECK (p.getPatch()[idx (L::layersOn)] == 0.0f);
    CHECK (p.getMint() == r.exact.mint);

    REQUIRE (p.undo());
    CHECK (p.getPatch()[idx (L::layersOn)] == 1.0f);
    CHECK (p.getMint() == r.layered.mint);
    REQUIRE (p.undo());
    CHECK (cash::samePatch (p.getPatch(), before));
    REQUIRE (p.redo());
    CHECK (p.getMint() == r.layered.mint);

    SECTION ("Detail re-fits the layers only, from the kept model")
    {
        cash::MatchBudget b;
        b.mode = cash::MatchBudget::Mode::layers;
        b.detail = 1;
        b.refit = true;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 4, b);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto r1 = m.getResult();
        REQUIRE (r1.layered.found);
        CHECK (r1.layered.layers == 1);
        CHECK (r1.layered.mint != r.layered.mint);
        CHECK (r1.exact.mint == r.exact.mint);       // EXACT kept as it was
        CHECK (r1.exact.percent == r.exact.percent);
        CHECK (ms < 60000.0);
    }

    SECTION ("LAYERS-only matches fill the main result too")
    {
        cash::MatchBudget b;
        b.mode = cash::MatchBudget::Mode::layers;
        m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 5, b);
        const auto r2 = m.getResult();
        CHECK (! r2.exact.found);
        REQUIRE (r2.layered.found);
        CHECK (r2.patch[idx (L::layersOn)] == 1.0f);
        p.setMatchPicksLayers (false); // falls back to the one there is
        CHECK (p.matchUsesLayers());
    }
}

TEST_CASE ("Layer wavetables ride with the sound: state, stash, vault, project", "[layers][state]")
{
    Plugin pl;
    auto& p = pl.p;
    auto& m = p.getMatcher();
    REQUIRE (m.setTarget (twoPart(), sr, "TwoPart").isEmpty());
    cash::MatchBudget b;
    b.mode = cash::MatchBudget::Mode::layers;
    b.detail = 3;
    m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 3, b);
    const auto r = m.getResult();
    REQUIRE (r.layered.found);
    REQUIRE (p.depositMatch());
    const auto tables = p.getMint();
    REQUIRE (tables == r.layered.mint);
    REQUIRE (p.bankIt() == 0);
    const auto played = playNote (p, 57, 0.4, 0.6);

    auto sameTables = [&tables] (const cash::MintRef& other)
    {
        REQUIRE (other != nullptr);
        REQUIRE (other->layerHarmonics.size() == tables->layerHarmonics.size());
        for (size_t i = 0; i < tables->layerHarmonics.size(); ++i)
        {
            REQUIRE (other->layerHarmonics[i] == tables->layerHarmonics[i]);
            REQUIRE (other->layerSpreads[i] == tables->layerSpreads[i]);
        }
        REQUIRE (other->layerWaves.size() == tables->layerWaves.size());
    };

    SECTION ("model save/load")
    {
        const auto bytes = tables->toMemory();
        sameTables (cash::MintModel::fromMemory (bytes.getData(), bytes.getSize()));
    }

    SECTION ("project: current sound and vault, and it plays the same")
    {
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        Plugin other;
        other.p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        sameTables (other.p.getMint());
        CHECK (cash::samePatch (other.p.getPatch(), p.getPatch()));
        REQUIRE (other.p.getVault().size() == 1);
        sameTables (other.p.getVault().at (0).mint);
        const auto again = playNote (other.p, 57, 0.4, 0.6);
        CHECK (std::abs (rms (again, 0.05, 0.5) - rms (played, 0.05, 0.5)) < 0.02f * rms (played, 0.05, 0.5));
    }

    SECTION ("stash")
    {
        juce::TemporaryFile tmp;
        const auto dir = tmp.getFile();
        REQUIRE (dir.createDirectory());
        p.getStash().setFolder (dir);
        const auto file = p.stashIt ("Layered");
        REQUIRE (file.existsAsFile());
        Plugin other;
        other.p.getStash().setFolder (dir);
        REQUIRE (other.p.loadStash (file));
        sameTables (other.p.getMint());
        CHECK_FALSE (other.p.isStashModified());
        dir.deleteRecursively();
    }

    SECTION ("vault withdraw and undo swap the tables back")
    {
        p.setPatch (cash::defaultPatch (p.getState()), "reset", cash::MintRef {});
        CHECK (p.getMint() == nullptr);
        REQUIRE (p.withdraw (0));
        CHECK (p.getMint() == tables);
        REQUIRE (p.undo());
        CHECK (p.getMint() == nullptr);
    }
}

TEST_CASE ("Older codes, models and projects load with LAYERS off", "[layers][state]")
{
    Plugin pl;
    const auto defaults = cash::defaultPatch (pl.p.getState());
    CHECK (defaults[idx (L::layersOn)] == 0.0f);

    // The v8 default code (before M14): decodes, layers from the fallback (off).
    const auto v8 = cash::CashCode::decode ("$H4S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-003Z-WTD8-NEBS-02S8-1036-9DDZ-SKCS-040G-2001-01W6-M", defaults);
    REQUIRE (v8.has_value());
    CHECK ((*v8)[idx (L::layersOn)] == 0.0f);
    for (int i = cash::ParameterMap::numParamsV8; i < cash::ParameterMap::numParams; ++i)
        CHECK ((*v8)[static_cast<size_t> (i)] == defaults[static_cast<size_t> (i)]);

    // A v9 code with layers on round-trips.
    auto patch = defaults;
    patch[idx (L::layersOn)] = 1.0f;
    patch[idx (L::toneId (3, L::cutoff))] = 0.4f;
    const auto back = cash::CashCode::decode (cash::CashCode::encode (patch), defaults);
    REQUIRE (back.has_value());
    CHECK ((*back)[idx (L::layersOn)] == 1.0f);
    CHECK (std::abs ((*back)[idx (L::toneId (3, L::cutoff))] - 0.4f) < 0.003f);

    // A project from before M14 (no layer properties): layers stay off.
    juce::MemoryBlock saved;
    pl.p.getStateInformation (saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    if (xml != nullptr)
    {
        // Strip every layer param from the saved state, as an M13 project would be.
        int stripped = 0;
        for (auto* e : xml->getChildIterator())
        {
            std::vector<juce::XmlElement*> drop;
            for (auto* q : e->getChildIterator())
            {
                const auto id = q->getStringAttribute ("id");
                if (q->hasTagName ("PARAM") && (id.startsWith ("lyr") || id.startsWith ("lnz") || id.startsWith ("layer")))
                    drop.push_back (q);
            }
            for (auto* q : drop)
                e->removeChildElement (q, true);
            stripped += static_cast<int> (drop.size());
        }
        CHECK (stripped == L::numParams);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        Plugin other;
        other.p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
        CHECK (other.p.getPatch()[idx (L::layersOn)] == 0.0f);
    }

    // A v1 model (M13, no tables) still loads, and plays saws if layers are switched on.
    cash::MintModel m1;
    m1.numFrames = 4;
    m1.bandHz = { 1000.0f };
    m1.noiseL = m1.noiseR = std::vector<float> (4, 0.0f);
    m1.frameLevel = std::vector<float> (4, 0.0f);
    m1.holdFrame = 2;
    // v1 bytes = today's (v2) without the trailing table count, version 1.
    juce::MemoryBlock raw;
    {
        const auto v2 = m1.toMemory();
        juce::MemoryInputStream zipped (v2, false);
        juce::GZIPDecompressorInputStream gz (zipped);
        gz.readIntoMemoryBlock (raw);
    }
    REQUIRE (raw.getSize() > 12);
    raw.setSize (raw.getSize() - 4);                 // no tables (count 0)
    static_cast<char*> (raw.getData())[4] = 1;       // version (little-endian int after the magic)
    static_cast<char*> (raw.getData())[5] = static_cast<char*> (raw.getData())[6] = static_cast<char*> (raw.getData())[7] = 0;
    juce::MemoryOutputStream rezipped;
    {
        juce::GZIPCompressorOutputStream gz (rezipped, 6);
        gz.write (raw.getData(), raw.getSize());
    }
    const auto bytes = rezipped.getMemoryBlock();
    const auto back1 = cash::MintModel::fromMemory (bytes.getData(), bytes.getSize());
    REQUIRE (back1 != nullptr);
    CHECK (back1->layerWaves.empty());
    CHECK (L::tablesFor (back1.get()) == L::tablesFor (nullptr));
}

TEST_CASE ("Layer params are automatable, named and in range", "[layers][params]")
{
    Plugin pl;
    CHECK (L::ids().size() == static_cast<size_t> (L::numParams));
    CHECK (cash::ParameterMap::numParams - cash::ParameterMap::numParamsV8 == L::numParams);
    for (auto* id : L::ids())
    {
        auto* param = pl.p.getState().getParameter (id);
        REQUIRE (param != nullptr);
        CHECK (param->isAutomatable());
        CHECK (param->getName (64).isNotEmpty());
        CHECK (cash::ParameterMap::spec (cash::ParameterMap::indexOf (id)).weight == 0.0f); // money moves leave layers alone
    }
}

//============================================================================== M14 part 2
namespace
{
// RBJ biquad (tests only): a steep reference band for the noise fit.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad make (bool high, double hz, double rate)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / rate, c = std::cos (w), al = std::sin (w) / (2.0 * 0.70710678);
        const double a0 = 1.0 + al;
        Biquad q;
        q.b0 = (high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0) / a0;
        q.b1 = (high ? -(1.0 + c) : 1.0 - c) / a0;
        q.b2 = q.b0;
        q.a1 = -2.0 * c / a0;
        q.a2 = (1.0 - al) / a0;
        return q;
    }
    double run (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

float fitValue (const L::Fit& fit, const char* id)
{
    for (const auto& [k, v] : fit.values)
        if (juce::StringRef (k) == juce::StringRef (id))
            return v;
    FAIL ("missing " << id);
    return 0.0f;
}

bool shownIn (const juce::Component* c, const juce::Component* top)
{
    for (; c != nullptr && c != top; c = c->getParentComponent())
        if (! c->isVisible())
            return false;
    return c == top;
}

float envDbAt (float curve, double seconds, float decay = 0.4f)
{
    L::Env e;
    e.set (0.0005f, decay, 0.0f, 1.0f, sr, L::curveExponent (curve));
    e.on();
    float v = 0.0f;
    const int n = static_cast<int> ((0.0005 + seconds) * sr);
    for (int i = 0; i < n; ++i)
        v = e.next();
    return 20.0f * std::log10 (juce::jmax (1.0e-9f, v));
}
}

TEST_CASE ("Env curve: hold then drop, or drop then linger; -60 dB at the decay / release time", "[layers][engine]")
{
    // dB(t) = -60 (t / decay)^p, p = 4^curve.
    CHECK (std::abs (envDbAt (0.0f, 0.2) + 30.0f) < 0.5f);   // exponential: half way = -30 dB
    CHECK (std::abs (envDbAt (1.0f, 0.2) + 3.75f) < 0.5f);   // p = 4: still loud half way (a kick's body)
    CHECK (std::abs (envDbAt (-1.0f, 0.2) + 50.45f) < 1.0f); // p = 1/4: gone fast, then a long tail
    for (float c : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
    {
        INFO (c);
        CHECK (std::abs (envDbAt (c, 0.4) + 60.0f) < 1.5f);
    }

    // The release takes the same curve: from wherever it starts, -60 dB at the release time.
    for (float c : { -1.0f, 0.0f, 1.0f })
    {
        INFO (c);
        L::Env e;
        e.set (0.0005f, 1.0f, 0.5f, 0.4f, sr, L::curveExponent (c));
        e.on();
        for (int i = 0; i < static_cast<int> (0.01 * sr); ++i) e.next();
        const float start = e.value;
        e.off();
        float half = 0.0f, full = 0.0f;
        for (int i = 1; i <= static_cast<int> (0.4 * sr); ++i)
        {
            const float v = e.next();
            if (i == static_cast<int> (0.2 * sr)) half = v;
            full = v;
        }
        const float expectHalf = -60.0f * std::pow (0.5f, L::curveExponent (c));
        CHECK (std::abs (20.0f * std::log10 (half / start) - expectHalf) < 1.0f);
        CHECK ((e.stage == L::Env::idle || std::abs (20.0f * std::log10 (full / start) + 60.0f) < 1.5f));
    }

    // Changing the curve mid-decay doesn't jump.
    L::Env e;
    e.set (0.0005f, 0.5f, 0.2f, 1.0f, sr);
    e.on();
    float v = 0.0f;
    for (int i = 0; i < static_cast<int> (0.1 * sr); ++i)
        v = e.next();
    e.set (0.0005f, 0.5f, 0.2f, 1.0f, sr, L::curveExponent (0.8f));
    const float after = e.next();
    CHECK (std::abs (after - v) < 0.01f);

    // Through the engine: a layer with a +100 % curve holds its level far longer.
    Plugin pl;
    auto patch = layersPatch (pl.p);
    setReal (pl.p, patch, L::toneId (0, L::decay), 0.4f);
    setReal (pl.p, patch, L::toneId (0, L::sustain), 0.0f);
    pl.p.setPatch (patch, "exp");
    const auto plain = playNote (pl.p, 60, 0.8, 0.8);
    setReal (pl.p, patch, L::toneId (0, L::curve), 1.0f);
    pl.p.setPatch (patch, "curved");
    const auto held = playNote (pl.p, 60, 0.8, 0.8);
    CHECK (rms (held, 0.15, 0.25) > 4.0f * rms (plain, 0.15, 0.25));
    CHECK (rms (held, 0.5, 0.8) < 0.02f); // and still gone after the decay time

}

TEST_CASE ("Noise slope: steeper band edges at the same loudness", "[layers][engine]")
{
    // The response: -3 dB at both edges whatever the slope; far outside, much lower when steep.
    for (int slope = 0; slope < L::numSlopes; ++slope)
    {
        INFO (slope);
        CHECK (std::abs (10.0f * std::log10 (L::noiseResponse (1000.0f / std::sqrt (2.0f), 1000.0f, 1.0f, slope, sr)) + 3.0f) < 0.6f);
        CHECK (std::abs (10.0f * std::log10 (L::noiseResponse (1000.0f * std::sqrt (2.0f), 1000.0f, 1.0f, slope, sr)) + 3.0f) < 0.6f);
    }
    CHECK (L::noiseResponse (5657.0f, 1000.0f, 1.0f, 3, sr) < 1.0e-4f * L::noiseResponse (5657.0f, 1000.0f, 1.0f, 0, sr) * 1000.0f);
    CHECK (L::noiseResponse (5657.0f, 1000.0f, 1.0f, 3, sr) < 1.0e-6f);

    Plugin pl;
    auto patch = layersPatch (pl.p);
    patch[idx (L::toneId (0, L::level))] = 0.0f;
    setReal (pl.p, patch, L::noiseId (0, L::nLevel), 0.5f);
    setReal (pl.p, patch, L::noiseId (0, L::nCentre), 1000.0f);
    setReal (pl.p, patch, L::noiseId (0, L::nWidth), 1.0f);
    setReal (pl.p, patch, L::noiseId (0, L::nAttack), 0.001f);
    setReal (pl.p, patch, L::noiseId (0, L::nSustain), 1.0f);
    std::array<float, L::numSlopes> level {}, high {};
    for (int slope = 0; slope < L::numSlopes; ++slope)
    {
        patch[idx (L::noiseId (0, L::nSlope))] = static_cast<float> (slope) / static_cast<float> (L::numSlopes - 1);
        pl.p.setPatch (patch, "slope");
        const auto out = playNote (pl.p, 60, 1.0, 1.0);
        level[static_cast<size_t> (slope)] = rms (out, 0.2, 0.9);
        // Energy above ~6 kHz (an 8th-order high-pass) vs all of it.
        std::array<Biquad, 4> hp;
        for (auto& q : hp) q = Biquad::make (true, 6000.0, sr);
        double hi = 0.0, all = 0.0;
        for (int i = static_cast<int> (0.2 * sr); i < static_cast<int> (0.9 * sr); ++i)
        {
            double x = out.getSample (0, i);
            all += x * x;
            for (auto& q : hp) x = q.run (x);
            hi += x * x;
        }
        high[static_cast<size_t> (slope)] = static_cast<float> (hi / all);
    }
    for (int slope = 1; slope < L::numSlopes; ++slope)
    {
        INFO (slope);
        CHECK (std::abs (20.0f * std::log10 (level[static_cast<size_t> (slope)] / level[0])) < 2.0f); // RMS-normalised
        CHECK (high[static_cast<size_t> (slope)] < high[static_cast<size_t> (slope - 1)]);
    }
    CHECK (high[3] < 0.05f * high[0]);
}

TEST_CASE ("Fitting: a hold-then-drop body gets a decay curve, a steep band a steep slope", "[layers][fit]")
{
    const int n = static_cast<int> (0.7 * sr);
    // A kick-ish body: 110 Hz (harmonics 1-3) that holds, then falls 60 dB at ~0.35 s.
    {
        juce::AudioBuffer<float> body (2, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double env = std::pow (10.0, -3.0 * std::pow (t / 0.35, 3.0)) * juce::jmin (1.0, t / 0.002);
            double s = 0.0;
            for (int h = 1; h <= 3; ++h)
                s += 0.5 / h * std::sin (juce::MathConstants<double>::twoPi * 110.0 * h * t);
            body.setSample (0, i, static_cast<float> (0.8 * env * s));
            body.setSample (1, i, static_cast<float> (0.8 * env * s));
        }
        Plugin pl;
        auto& m = pl.p.getMatcher();
        REQUIRE (m.setTarget (body, sr, "Body").isEmpty());
        const auto model = m.getMintModel();
        REQUIRE (model != nullptr);
        const auto fit = L::fit (*model, m.getTargetPrint().midiNote, 1, {});
        CHECK (fitValue (fit, L::toneId (0, L::curve)) >= 0.25f); // holds, then drops
    }
    // A noise band with brick-ish edges at 1.5 - 3 kHz (4 HP + 4 LP Butterworth pairs).
    {
        juce::AudioBuffer<float> noise (2, n);
        juce::Random rng (11);
        std::array<Biquad, 8> band;
        for (int k = 0; k < 4; ++k)
        {
            band[static_cast<size_t> (k)] = Biquad::make (true, 1500.0 * 0.66, sr);
            band[static_cast<size_t> (k + 4)] = Biquad::make (false, 3000.0 / 0.66, sr);
        }
        for (int i = 0; i < n; ++i)
        {
            double z = rng.nextFloat() * 2.0 - 1.0;
            for (auto& q : band)
                z = q.run (z);
            const auto v = static_cast<float> (2.0 * z * std::exp (-(i / sr) / 0.3) * juce::jmin (1.0, i / (0.003 * sr)));
            noise.setSample (0, i, v);
            noise.setSample (1, i, v);
        }
        Plugin pl;
        auto& m = pl.p.getMatcher();
        REQUIRE (m.setTarget (noise, sr, "Band").isEmpty());
        const auto model = m.getMintModel();
        REQUIRE (model != nullptr);
        const int key = m.getTargetPrint().midiNote;
        const auto fit = L::fit (*model, key, 1, {});
        CHECK (fitValue (fit, L::noiseId (0, L::nLevel)) > 0.0f);
        CHECK (fitValue (fit, L::noiseId (0, L::nSlope)) >= 2.0f); // 24 or 48 dB edges
        const float centre = fitValue (fit, L::noiseId (0, L::nCentre)) * std::exp2 ((static_cast<float> (key) - 60.0f) / 12.0f);
        CHECK (std::abs (std::log2 (centre / 2121.0f)) < 0.5f);
        CHECK (fitValue (fit, L::noiseId (0, L::nWidth)) < 2.0f);
    }
}

TEST_CASE ("LAYERS page on the back of the bill", "[layers][ui]")
{
    Plugin pl;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor> (pl.p.createEditor());
    auto* ed = dynamic_cast<CashSynthAudioProcessorEditor*> (editor.get());
    REQUIRE (ed != nullptr);
    auto& panel = ed->getAdvancedPanel();
    auto& tabs = ed->getBack().getPageTabs();
    ed->flip();
    ed->getBack().resized();

    // Every LAYERS param has a control, and only the picked layer's are on show.
    for (auto* id : L::ids())
    {
        INFO (id);
        CHECK (panel.hasControlFor (id));
    }
    auto* cutoff1 = panel.getKnob (L::toneId (0, L::cutoff));
    auto* cutoff4 = panel.getKnob (L::toneId (3, L::cutoff));
    auto* slope1 = panel.getMenu (L::noiseId (0, L::nSlope));
    REQUIRE (cutoff1 != nullptr);
    REQUIRE (cutoff4 != nullptr);
    REQUIRE (slope1 != nullptr);
    CHECK (panel.getMenu (L::toneId (0, L::fltType)) != nullptr);
    CHECK (panel.getKnob (L::toneId (5, L::curve)) != nullptr);
    CHECK (panel.getSwitch (L::layersOn) != nullptr);
    CHECK_FALSE (shownIn (cutoff1, ed));

    tabs.choose (cash::AdvancedPanel::layersPage);
    CHECK (panel.getPage() == cash::AdvancedPanel::layersPage);
    CHECK (shownIn (&panel.getLayerTabs(), ed));
    CHECK (shownIn (&panel.getWaveView(), ed));
    CHECK (shownIn (panel.getKnob (L::detail), ed));
    CHECK (shownIn (cutoff1, ed));
    CHECK_FALSE (shownIn (cutoff4, ed));
    CHECK_FALSE (shownIn (panel.getKnob (cash::param::filterCutoff), ed));

    // Clicking a tab picks the layer.
    const auto tab4 = panel.getLayerTabs().tabArea (3).getCentre();
    CHECK (panel.getLayerTabs().layerAt (tab4) == 3);
    CHECK (panel.getLayerTabs().layerAt ({ 1.0f, 1.0f }) == -1);
    panel.selectLayer (panel.getLayerTabs().layerAt (tab4));
    CHECK (panel.getSelectedLayer() == 3);
    CHECK (shownIn (cutoff4, ed));
    CHECK_FALSE (shownIn (cutoff1, ed));
    panel.selectLayer (L::numTone); // noise 1
    CHECK (shownIn (slope1, ed));
    CHECK_FALSE (shownIn (cutoff4, ed));

    // Off = the layer knobs dimmed (still editable); on: tone layers past Detail stay dimmed.
    CHECK (panel.getControlAlpha (L::toneId (0, L::level)) < 0.5f);
    CHECK_FALSE (tabs.isLayersLit());
    CHECK_FALSE (panel.getLayerTabs().isLit (0));
    panel.getSwitch (L::layersOn)->toggle();
    pl.p.getState().getParameter (L::detail)->setValueNotifyingHost (pl.p.getState().getParameter (L::detail)->convertTo0to1 (2.0f));
    CHECK (tabs.isLayersLit());
    CHECK (panel.getControlAlpha (L::toneId (0, L::level)) == 1.0f);
    CHECK (panel.getControlAlpha (L::toneId (1, L::level)) == 1.0f);
    CHECK (panel.getControlAlpha (L::toneId (2, L::level)) < 0.5f);
    CHECK (panel.getControlAlpha (L::noiseId (1, L::nLevel)) == 1.0f);
    CHECK (panel.getLayerTabs().isLit (0));
    CHECK_FALSE (panel.getLayerTabs().isLit (2));          // past Detail
    CHECK_FALSE (panel.getLayerTabs().isLit (L::numTone)); // noise level 0

    // The wave view follows the processor's model (none yet: the plain saw).
    panel.selectLayer (0);
    panel.getWaveView().refresh();
    CHECK (panel.getWaveView().getShownModel() == pl.p.getMint().get());
}
