#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "CashCode.h"
#include "Mutator.h"
#include "Parameters.h"
#include "Stash.h"
#include "GoldenScenes.h"

// Milestone 8: section on/off switches and automation-ready parameters.
namespace
{
using namespace cash::param;

constexpr double sampleRate = 48000.0;

struct GoldenRender
{
    const char* name;
    std::vector<float> data;
};

const std::vector<GoldenRender>& goldenRenders()
{
    static const std::vector<GoldenRender> renders {
#include "GoldenRenders.inc"
    };
    return renders;
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    int block = 64; // small blocks = fast automation

    Rig() { p.prepareToPlay (sampleRate, 512); }
    ~Rig() { p.releaseResources(); }

    juce::RangedAudioParameter& param (const char* id)
    {
        auto* q = p.getState().getParameter (id);
        REQUIRE (q != nullptr);
        return *q;
    }
    void set (const char* id, float real) { param (id).setValueNotifyingHost (param (id).convertTo0to1 (real)); }
    void setNorm (const char* id, float norm) { param (id).setValueNotifyingHost (norm); }
    void reset() { p.releaseResources(); p.prepareToPlay (sampleRate, 512); }

    // Renders `seconds` (left channel + right channel summed is not needed: left is enough
    // for clicks, both are checked for NaN). `each` runs before every block with the time.
    template <typename Fn>
    std::vector<float> render (double seconds, std::vector<std::pair<double, juce::MidiMessage>> events, Fn&& each, bool* finite = nullptr)
    {
        const int total = static_cast<int> (seconds * sampleRate);
        std::vector<float> out;
        out.reserve (static_cast<size_t> (total));
        juce::AudioBuffer<float> buffer (2, block);
        bool ok = true;
        for (int pos = 0; pos < total; pos += block)
        {
            const int n = juce::jmin (block, total - pos);
            each (pos / sampleRate);
            buffer.setSize (2, n, false, false, true);
            buffer.clear();
            juce::MidiBuffer midi;
            for (auto& [when, msg] : events)
            {
                const int s = static_cast<int> (when * sampleRate);
                if (s >= pos && s < pos + n)
                    midi.addEvent (msg, s - pos);
            }
            p.processBlock (buffer, midi);
            for (int i = 0; i < n; ++i)
            {
                ok = ok && std::isfinite (buffer.getSample (0, i)) && std::isfinite (buffer.getSample (1, i));
                out.push_back (buffer.getSample (0, i));
            }
        }
        if (finite != nullptr)
            *finite = ok;
        return out;
    }

    std::vector<float> render (double seconds, std::vector<std::pair<double, juce::MidiMessage>> events = {})
    {
        return render (seconds, std::move (events), [] (double) {});
    }
};

juce::MidiMessage on (int note, float vel = 1.0f) { return juce::MidiMessage::noteOn (1, note, vel); }
juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }
size_t at (double s) { return static_cast<size_t> (s * sampleRate); }

float maxJump (const std::vector<float>& v, double from, double to)
{
    float m = 0.0f;
    for (size_t i = juce::jmax<size_t> (1, at (from)); i < juce::jmin (v.size(), at (to)); ++i)
        m = juce::jmax (m, std::abs (v[i] - v[i - 1]));
    return m;
}

double rms (const std::vector<float>& v, double from, double to)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = at (from); i < juce::jmin (v.size(), at (to)); ++i, ++n)
        sum += static_cast<double> (v[i]) * v[i];
    return n > 0 ? std::sqrt (sum / static_cast<double> (n)) : 0.0;
}

float peak (const std::vector<float>& v) { float m = 0.0f; for (auto x : v) m = juce::jmax (m, std::abs (x)); return m; }

bool same (const std::vector<float>& a, const std::vector<float>& b, float tol = 1.0e-6f)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs (a[i] - b[i]) > tol)
            return false;
    return true;
}

// A smooth, busy patch: low sines so any step stands out, every section doing something.
void smoothPatch (Rig& r)
{
    r.set (osc1Wave, sine);
    r.set (osc2Wave, triangle);
    r.set (osc2Semi, 7.0f);
    r.set (osc2Level, 0.3f);
    r.set (subLevel, 0.3f);
    r.set (filterCutoff, 2000.0f);
    r.set (filterReso, 0.3f);
    r.set (driveAmount, 0.4f);
    r.set (lfoToCutoff, 0.3f);
    r.set (lfoToPitch, 0.1f);
    r.set (lfo2Amount, 0.3f);
    r.set (lfo2Target, toAmp);
    r.set (unisonVoices, 3.0f);
    r.set (unisonDetune, 0.3f);
    r.set (glideTime, 0.05f);
    r.set (ampSustain, 0.8f);
}

const char* const switches[] = { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn };
}

//============================================================================== Old sounds
TEST_CASE ("Everything on sounds exactly like the M7 engine", "[m8][golden]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto scenes = golden::scenes();
    const auto& renders = goldenRenders();
    REQUIRE (renders.size() == scenes.size());

    for (size_t s = 0; s < scenes.size(); ++s)
    {
        INFO (scenes[s].name);
        CashSynthAudioProcessor p;
        const auto now = golden::render (p, scenes[s]);
        const auto& then = renders[s].data;
        REQUIRE (now.size() == then.size());
        float worst = 0.0f;
        for (size_t i = 0; i < now.size(); ++i)
            worst = juce::jmax (worst, std::abs (now[i] - then[i]));
        CHECK (worst < 1.0e-5f);
        CHECK (peak (now) > 0.01f);
    }
}

TEST_CASE ("Old codes, projects, stashes and vault notes load with every section on", "[m8][state][cashcode]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CashSynthAudioProcessor p;
    auto& st = p.getState();
    const auto defaults = cash::defaultPatch (st);
    for (auto* id : switches)
        CHECK (defaults[static_cast<size_t> (cash::ParameterMap::indexOf (id))] == 1.0f);

    // The pinned M7 (v3) default-patch code still decodes, to the defaults.
    const auto v3 = cash::CashCode::decode ("$74S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-0000-T310", defaults);
    REQUIRE (v3.has_value());
    CHECK (cash::samePatch (*v3, defaults, 0.5f / 255.0f + 1.0e-6f));
    CHECK (cash::CashCode::version >= 4);

    // An M7 project (no switches anywhere), loaded into an instance with everything off.
    p.bankIt();
    juce::MemoryBlock block;
    p.getStateInformation (block);
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    auto root = juce::ValueTree::fromXml (*xml);
    auto params = root.getChildWithName ("PARAMS");
    for (auto* id : switches)
    {
        params.removeChild (params.getChildWithProperty ("id", id), nullptr);
        root.getChildWithName ("Vault").getChild (0).getChildWithName ("Patch").removeProperty (id, nullptr);
    }
    for (auto* id : { cash::param::lfo1Sync, cash::param::lfo1Note, cash::param::lfo2Sync, cash::param::lfo2Note }) // M10 didn't exist either
    {
        params.removeChild (params.getChildWithProperty ("id", id), nullptr);
        root.getChildWithName ("Vault").getChild (0).getChildWithName ("Patch").removeProperty (id, nullptr);
    }
    REQUIRE (params.getNumChildren() == cash::ParameterMap::numParamsV3);

    for (auto* id : switches)
        st.getParameter (id)->setValueNotifyingHost (0.0f);

    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), old);
    p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    for (auto* id : switches)
        CHECK (st.getParameter (id)->getValue() == 1.0f);
    REQUIRE (p.getVault().size() == 1);
    for (auto* id : switches)
        CHECK (p.getVault().at (0).patch[static_cast<size_t> (cash::ParameterMap::indexOf (id))] == 1.0f);

    // A code-only stash file holding a v3 code, opened while sections are off.
    for (auto* id : switches)
        st.getParameter (id)->setValueNotifyingHost (0.0f);
    const auto dir = juce::File::createTempFile ("m8stash");
    REQUIRE (dir.createDirectory());
    const auto file = dir.getChildFile ("Old.cashstash");
    REQUIRE (file.replaceWithText ("<CashStash name=\"Old\" code=\"$74S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-0000-T310\"/>"));
    REQUIRE (p.loadStash (file));
    for (auto* id : switches)
        CHECK (st.getParameter (id)->getValue() == 1.0f);
    dir.deleteRecursively();
}

//============================================================================== Bypass
TEST_CASE ("Each switch silences or bypasses exactly its section", "[m8][switches]")
{
    Rig r;
    const std::vector<std::pair<double, juce::MidiMessage>> note { { 0.0, on (45) }, { 0.3, off (45) } };
    auto take = [&r, &note] { r.reset(); return r.render (0.45, note); };
    auto restore = [&r] { for (int i = 0; i < cash::ParameterMap::numParams; ++i) { auto& q = r.param (cash::ParameterMap::spec (i).id); q.setValueNotifyingHost (q.getDefaultValue()); } };

    SECTION ("oscillators")
    {
        struct Source { const char* level; const char* sw; };
        for (auto src : { Source { osc1Level, osc1On }, Source { osc2Level, osc2On }, Source { subLevel, subNoiseOn }, Source { noiseLevel, subNoiseOn } })
        {
            INFO (src.level);
            restore();
            for (auto* l : { osc1Level, osc2Level, subLevel, noiseLevel })
                r.set (l, l == src.level ? 0.8f : 0.0f);
            CHECK (peak (take()) > 0.02f);
            r.set (src.sw, 0.0f);
            CHECK (peak (take()) < 1.0e-6f);
        }
        // Osc 1 off leaves osc 2 exactly as it was.
        restore();
        r.set (osc1Level, 0.0f);
        const auto osc2Only = take();
        restore();
        r.set (osc1On, 0.0f);
        CHECK (same (take(), osc2Only));
    }

    // With a section off, none of its knobs change the sound; with it on, they do.
    struct Case
    {
        const char* sw;
        std::vector<std::pair<const char*, float>> knobs; // real values that change the sound a lot
    };
    const std::vector<Case> cases {
        { filterOn, { { filterType, 2.0f }, { filterCutoff, 200.0f }, { filterReso, 0.9f }, { filterKeyTrack, 1.0f } } },
        { filterEnvOn, { { filterEnvAmt, 1.0f }, { pitchEnvAmt, 0.6f }, { fltDecay, 2.0f } } },
        { driveOn, { { driveAmount, 0.9f }, { driveType, 2.0f } } },
        { lfo1On, { { lfoToPitch, 3.0f }, { lfoToCutoff, 0.8f }, { lfoToDrive, 0.5f }, { lfoRate, 7.0f } } },
        { lfo2On, { { lfo2Amount, 0.8f }, { lfo2Target, 2.0f }, { lfo2Rate, 6.0f } } },
        { unisonOn, { { unisonVoices, 5.0f }, { unisonDetune, 0.8f }, { unisonSpread, 1.0f } } },
    };
    SECTION ("effects")
    {
        for (const auto& c : cases)
        {
            INFO (c.sw);
            restore();
            r.set (filterCutoff, 1500.0f);
            r.set (driveAmount, 0.2f);
            r.set (c.sw, 0.0f);
            const auto plain = take();
            for (auto& [id, v] : c.knobs)
                r.set (id, v);
            CHECK (same (take(), plain)); // bypassed: knobs do nothing
            r.set (c.sw, 1.0f);
            CHECK_FALSE (same (take(), plain, 1.0e-3f));
        }
    }

    SECTION ("bypassed filter = the dry oscillators")
    {
        // Dry reference: an LP far above everything a 110 Hz sine makes, no env, no reso.
        restore();
        r.set (osc1Wave, sine);
        r.set (osc2Level, 0.0f);
        r.set (filterReso, 0.3f);
        r.set (filterEnvAmt, 0.0f);
        r.set (filterCutoff, 20000.0f);
        const auto open = take();
        r.set (filterOn, 0.0f);
        r.set (filterCutoff, 100.0f);
        r.set (filterType, 2.0f);
        const auto bypassed = take();
        double num = 0.0, ea = 0.0, eb = 0.0;
        for (size_t i = 0; i < open.size(); ++i)
        {
            num += static_cast<double> (open[i]) * bypassed[i];
            ea += static_cast<double> (open[i]) * open[i];
            eb += static_cast<double> (bypassed[i]) * bypassed[i];
        }
        CHECK (num / std::sqrt (ea * eb) > 0.999);
        CHECK (std::sqrt (eb / ea) == Catch::Approx (1.0).margin (0.01));
    }

    SECTION ("unison off = one voice, glide off = no glide")
    {
        restore();
        r.set (unisonVoices, 1.0f);
        const auto one = take();
        r.set (unisonVoices, 6.0f);
        r.set (unisonDetune, 0.9f);
        r.set (unisonOn, 0.0f);
        CHECK (same (take(), one));

        restore();
        const std::vector<std::pair<double, juce::MidiMessage>> two { { 0.0, on (45) }, { 0.1, on (57) } };
        r.set (glideTime, 0.0f);
        r.reset();
        const auto noGlide = r.render (0.3, two);
        r.set (glideTime, 0.5f);
        r.set (glideOn, 0.0f);
        r.reset();
        CHECK (same (r.render (0.3, two), noGlide));
    }
}

//============================================================================== Clicks
TEST_CASE ("Switching sections mid-note never clicks", "[m8][switches][clicks]")
{
    Rig r;
    smoothPatch (r);
    const std::vector<std::pair<double, juce::MidiMessage>> held { { 0.0, on (36) } };

    for (auto* sw : switches)
    {
        INFO (sw);
        // Steady references with the section on and off.
        r.set (sw, 1.0f);
        r.reset();
        const float refOn = maxJump (r.render (0.6, held), 0.1, 0.6);
        r.set (sw, 0.0f);
        r.reset();
        const float refOff = maxJump (r.render (0.6, held), 0.1, 0.6);
        r.set (sw, 1.0f);
        r.reset();

        bool finite = false;
        const auto flipping = r.render (0.8, held, [&r, sw] (double t)
                                        { if (t >= 0.1) r.setNorm (sw, std::fmod (t, 0.1) < 0.05 ? 0.0f : 1.0f); }, &finite);
        CHECK (finite);
        const float ref = juce::jmax (refOn, refOff);
        CHECK (maxJump (flipping, 0.1, 0.8) <= ref * 1.5f + 1.0e-3f);
        CHECK (rms (flipping, 0.1, 0.8) > 0.001); // osc 1 off still leaves osc 2 + sub
    }
}

TEST_CASE ("Automating every parameter fast never clicks or NaNs", "[m8][automation][clicks]")
{
    Rig r;
    const std::vector<std::pair<double, juce::MidiMessage>> held { { 0.0, on (36) } };

    for (int i = 0; i < cash::ParameterMap::numParams; ++i)
    {
        const char* id = cash::ParameterMap::spec (i).id;
        INFO (id);
        for (int k = 0; k < cash::ParameterMap::numParams; ++k)
        {
            auto& q = r.param (cash::ParameterMap::spec (k).id);
            q.setValueNotifyingHost (q.getDefaultValue());
        }
        smoothPatch (r);
        auto& q = r.param (id);
        const float home = q.getValue();

        // Steady references at the low end, middle and top of its range.
        float ref = 0.0f;
        for (const float v : { 0.0f, 0.5f, 1.0f })
        {
            q.setValueNotifyingHost (v);
            r.reset();
            bool finite = false;
            ref = juce::jmax (ref, maxJump (r.render (0.5, held, [] (double) {}, &finite), 0.15, 0.5));
            CHECK (finite);
        }

        // 1) Fast sweeps: 0 -> 1 -> 0 in 100 ms, the way a host draws a ramp (per 64-sample block).
        q.setValueNotifyingHost (home);
        r.reset();
        bool finite = false;
        const auto sweep = r.render (0.8, held, [&q] (double t)
                                     { if (t >= 0.15) { const double ph = std::fmod (t, 0.1) / 0.1; q.setValueNotifyingHost (static_cast<float> (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0)); } },
                                     &finite);
        CHECK (finite);
        INFO ("ref " << ref << " sweep " << maxJump (sweep, 0.15, 0.8));
        CHECK (maxJump (sweep, 0.15, 0.8) <= ref * 2.0f + 2.0e-3f);

        // 2) Hard jumps: end to end every 50 ms (a stepped lane, or snapping a knob).
        q.setValueNotifyingHost (home);
        r.reset();
        const auto steps = r.render (0.8, held, [&q] (double t) { if (t >= 0.15) q.setValueNotifyingHost (std::fmod (t, 0.1) < 0.05 ? 0.0f : 1.0f); },
                                     &finite);
        CHECK (finite);
        INFO ("ref " << ref << " steps " << maxJump (steps, 0.15, 0.8));
        if (juce::StringRef (id) != juce::StringRef (filterCutoff))
        {
            CHECK (maxJump (steps, 0.15, 0.8) <= ref * 2.0f + 2.0e-3f);
        }
        else
        {
            // Slamming 20 Hz <-> 20 kHz sweeps the resonant peak through ten octaves in
            // 20 ms: a "zap" (the filter ringing), not a step. Bounded, and with no
            // resonance the same slam is as clean as everything else.
            CHECK (maxJump (steps, 0.15, 0.8) <= ref * 8.0f);
            r.set (filterReso, 0.0f);
            q.setValueNotifyingHost (home);
            r.reset();
            const auto clean = r.render (0.8, held, [&q] (double t) { if (t >= 0.15) q.setValueNotifyingHost (std::fmod (t, 0.1) < 0.05 ? 0.0f : 1.0f); });
            CHECK (maxJump (clean, 0.15, 0.8) <= ref * 2.0f + 2.0e-3f);
        }
    }
}

//============================================================================== Voicing hand-over
TEST_CASE ("Voice mode and unison changes keep held keys sounding", "[m8][lead]")
{
    Rig r;
    r.block = 512;
    auto& e = r.p.getEngine();
    auto keyed = [&e]
    {
        int n = 0;
        for (int i = 0; i < cash::SynthEngine::numVoices; ++i)
            n += e.isVoiceActive (i) ? 1 : 0;
        return n;
    };

    const std::vector<std::pair<double, juce::MidiMessage>> chord { { 0.0, on (48) }, { 0.0, on (52) }, { 0.0, on (55) } };
    auto playing = r.render (0.2, chord);
    CHECK (keyed() == 3);
    const double before = rms (playing, 0.1, 0.2);

    // Poly -> Mono: the last key carries on, no gap.
    r.set (voiceMode, mono);
    auto after = r.render (0.4);
    CHECK (e.getMonoNote() == 55);
    CHECK (rms (after, 0.0, 0.05) > before * 0.3);
    CHECK (rms (after, 0.3, 0.4) > before * 0.2);
    INFO ("before " << maxJump (playing, 0.0, 0.2) << " after " << maxJump (after, 0.0, 0.4));
    CHECK (maxJump (after, 0.0, 0.4) <= maxJump (playing, 0.05, 0.2) * 1.2f); // nothing sharper than the saws themselves

    // Unison up while mono: the stack grows on the held note.
    r.set (unisonVoices, 4.0f);
    after = r.render (0.3);
    CHECK (keyed() == 4);
    CHECK (e.getMonoNote() == 55);

    // Mono -> Legato -> Poly: every held key sounds again, each with 4 voices.
    r.set (voiceMode, legato);
    after = r.render (0.1);
    CHECK (e.getMonoNote() == 55);
    r.set (voiceMode, poly);
    after = r.render (0.4);
    CHECK (keyed() == 12);
    CHECK (rms (after, 0.2, 0.4) > before * 0.5);

    // Unison down while poly: 2 voices per held key, the rest ring out.
    r.set (unisonVoices, 2.0f);
    after = r.render (1.0);
    CHECK (keyed() == 6);

    // Unison switch off = 1 voice each, still sounding.
    r.set (unisonOn, 0.0f);
    after = r.render (1.0);
    CHECK (keyed() == 3);
    CHECK (rms (after, 0.8, 1.0) > before * 0.5);

    // Releasing them still works after all that.
    after = r.render (1.0, { { 0.0, off (48) }, { 0.0, off (52) }, { 0.0, off (55) } });
    CHECK (keyed() == 0);
}

TEST_CASE ("Mono to poly under the sustain pedal keeps the pedal note", "[m8][lead]")
{
    Rig r;
    r.block = 512;
    auto& e = r.p.getEngine();
    r.set (voiceMode, mono);
    r.render (0.2, { { 0.0, on (60) }, { 0.05, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 0.1, off (60) } });
    r.set (voiceMode, poly);
    auto held = r.render (0.3);
    CHECK (rms (held, 0.2, 0.3) > 0.01);
    held = r.render (1.0, { { 0.0, juce::MidiMessage::controllerEvent (1, 64, 0) } });
    int active = 0;
    for (int i = 0; i < cash::SynthEngine::numVoices; ++i)
        active += e.isVoiceActive (i) ? 1 : 0;
    CHECK (active == 0); // pedal up released it: nothing stuck
}

//============================================================================== Money moves
TEST_CASE ("Jackpot and Invest flip effect sections lightly, never the core", "[m8][mutator]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CashSynthAudioProcessor p;
    const auto defaults = cash::defaultPatch (p.getState());
    auto idx = [] (const char* id) { return static_cast<size_t> (cash::ParameterMap::indexOf (id)); };
    auto isOn = [] (const cash::Patch& q, size_t i) { return q[i] > 0.5f; };

    int effectOff = 0, effectTotal = 0;
    for (uint64_t seed = 1; seed <= 400; ++seed)
    {
        for (const auto& q : { cash::Mutator::jackpot (defaults, seed), cash::Mutator::invest (defaults, { 1.0f, 1.0f, seed }) })
        {
            CHECK (isOn (q, idx (osc1On)));
            CHECK (isOn (q, idx (filterOn)));
            CHECK ((isOn (q, idx (osc1On)) || isOn (q, idx (osc2On)) || isOn (q, idx (subNoiseOn))));
            for (auto* id : { driveOn, lfo1On, lfo2On, unisonOn, glideOn, subNoiseOn })
            {
                effectOff += isOn (q, idx (id)) ? 0 : 1;
                ++effectTotal;
            }
        }
    }
    const double offShare = static_cast<double> (effectOff) / effectTotal;
    CHECK (offShare > 0.02);  // they do move
    CHECK (offShare < 0.2);   // lightly

    // Every oscillator off going in: makePlayable switches one back on (osc 1 first).
    auto dead = defaults;
    for (auto* id : { osc1On, osc2On, subNoiseOn })
        dead[idx (id)] = 0.0f;
    for (uint64_t seed = 1; seed <= 50; ++seed)
    {
        const auto j = cash::Mutator::jackpot (dead, seed);
        CHECK (isOn (j, idx (osc1On)));
        const float tonal = juce::jmax (j[idx (osc1Level)], isOn (j, idx (subNoiseOn)) ? j[idx (subLevel)] : 0.0f);
        CHECK (tonal >= 0.35f);
    }
    // ... osc 2 if osc 1's switch is locked.
    cash::LockMask locks;
    locks.set (idx (osc1On));
    auto q = dead;
    cash::Mutator::makePlayable (q, locks);
    CHECK_FALSE (isOn (q, idx (osc1On)));
    CHECK (isOn (q, idx (osc2On)));
    CHECK (q[idx (osc2Level)] >= 0.35f);
}

//============================================================================== UI + host view
TEST_CASE ("Every section switch has a control that drives and dims", "[m8][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CashSynthAudioProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    auto& panel = dynamic_cast<CashSynthAudioProcessorEditor&> (*editor).getAdvancedPanel();
    auto& st = p.getState();

    const std::vector<std::pair<const char*, const char*>> sectionKnob {
        { osc1On, osc1Level }, { osc2On, osc2Level }, { subNoiseOn, subLevel }, { filterOn, filterCutoff },
        { filterEnvOn, pitchEnvAmt }, { driveOn, driveAmount }, { lfo1On, lfoRate }, { lfo2On, lfo2Target },
        { unisonOn, unisonDetune }, { glideOn, glideTime } };

    for (auto [sw, knob] : sectionKnob)
    {
        INFO (sw);
        auto* s = panel.getSwitch (sw);
        REQUIRE (s != nullptr);
        CHECK (s->isVisible());
        CHECK (s->getWidth() > 0);
        CHECK (s->isOn());
        CHECK (panel.getControlAlpha (knob) == 1.0f);

        s->toggle();
        CHECK (st.getParameter (sw)->getValue() == 0.0f);
        CHECK (panel.getControlAlpha (knob) < 1.0f);
        CHECK (panel.getControlAlpha (voiceMode) == 1.0f); // other sections untouched

        st.getParameter (sw)->setValueNotifyingHost (1.0f); // host automation moves the switch too
        CHECK (s->isOn());
        CHECK (panel.getControlAlpha (knob) == 1.0f);
    }
}

TEST_CASE ("Every parameter reads well in a host automation lane", "[m8][params]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CashSynthAudioProcessor p;
    juce::StringArray names;
    for (auto* base : p.getParameters())
    {
        auto* q = dynamic_cast<juce::RangedAudioParameter*> (base);
        REQUIRE (q != nullptr);
        INFO (q->paramID);
        CHECK (q->isAutomatable());
        const auto name = q->getName (64);
        CHECK (name.isNotEmpty());
        CHECK (name.length() <= 24);
        CHECK_FALSE (names.contains (name));
        names.add (name);
        CHECK (q->getName (12).length() <= 12);

        const int steps = q->getNumSteps();
        const bool discrete = q->isDiscrete() || steps < 1000;
        for (const float v : { 0.0f, 0.13f, 0.5f, 0.77f, 1.0f })
        {
            const auto text = q->getText (v, 64);
            INFO (text);
            CHECK (text.isNotEmpty());
            CHECK_FALSE (text.containsIgnoreCase ("nan"));
            const float back = q->getValueForText (text);
            if (discrete)
                CHECK (q->convertFrom0to1 (back) == Catch::Approx (q->convertFrom0to1 (v)).margin (0.5f));
            else
                CHECK (back == Catch::Approx (v).margin (0.02f)); // text is rounded for display
        }
    }
    CHECK (names.size() == cash::ParameterMap::numParams);
}
