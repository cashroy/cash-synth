#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "CashCode.h"
#include "Mutator.h"
#include "Parameters.h"

// Milestone 7: lead mode (voice modes, glide, note priority, unison, controllers).
namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

using Events = std::vector<std::pair<int, juce::MidiMessage>>;

struct Stereo
{
    std::vector<float> left, right;
};

struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;

    Rig() { p.prepareToPlay (sampleRate, blockSize); }
    ~Rig() { p.releaseResources(); }

    void set (const char* id, float value)
    {
        auto* param = p.getState().getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    const cash::SynthEngine& engine() const { return p.getEngine(); }

    // Renders `seconds`; MIDI positions are relative to the start of this call.
    Stereo render (double seconds, Events events = {})
    {
        const int total = static_cast<int> (std::lround (seconds * sampleRate));
        Stereo out;
        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            buffer.setSize (2, n, false, false, true);
            juce::MidiBuffer midi;
            for (auto& [when, msg] : events)
                if (when >= pos && when < pos + n)
                    midi.addEvent (msg, when - pos);
            p.processBlock (buffer, midi);
            for (int i = 0; i < n; ++i)
            {
                out.left.push_back (buffer.getSample (0, i));
                out.right.push_back (buffer.getSample (1, i));
            }
        }
        return out;
    }

    void reset() { p.releaseResources(); p.prepareToPlay (sampleRate, blockSize); }
};

int at (double seconds) { return static_cast<int> (std::lround (seconds * sampleRate)); }
juce::MidiMessage on (int note, float vel = 1.0f) { return juce::MidiMessage::noteOn (1, note, vel); }
juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }

float peak (const std::vector<float>& v, double from, double to)
{
    float m = 0.0f;
    for (size_t i = static_cast<size_t> (at (from)); i < juce::jmin (v.size(), static_cast<size_t> (at (to))); ++i)
        m = juce::jmax (m, std::abs (v[i]));
    return m;
}

double rms (const std::vector<float>& v, double from, double to)
{
    double sum = 0.0;
    const auto a = static_cast<size_t> (at (from)), b = static_cast<size_t> (at (to));
    for (size_t i = a; i < b; ++i)
        sum += static_cast<double> (v[i]) * v[i];
    return std::sqrt (sum / static_cast<double> (b - a));
}

// Frequency from rising zero crossings over a window.
double frequency (const std::vector<float>& v, double from, double to)
{
    int crossings = 0;
    for (size_t i = static_cast<size_t> (at (from)) + 1; i < static_cast<size_t> (at (to)); ++i)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f)
            ++crossings;
    return crossings / (to - from);
}

void pureSine (Rig& r)
{
    using namespace cash::param;
    r.set (osc1Wave, sine);
    r.set (osc2Level, 0.0f);
    r.set (filterCutoff, 20000.0f);
    r.set (filterEnvAmt, 0.0f);
    r.set (filterReso, 0.0f);
}
}

TEST_CASE ("Mono and Legato play one voice (times unison); Poly plays chords", "[lead]")
{
    Rig r;
    using namespace cash::param;
    const Events chord { { 0, on (60) }, { 10, on (64) }, { 20, on (67) } };

    r.render (0.1, chord);
    CHECK (r.engine().getNumActiveVoices() == 3);
    CHECK (r.engine().getMonoNote() == -1);
    r.reset();

    for (auto m : { mono, legato })
    {
        r.set (voiceMode, static_cast<float> (m));
        r.render (0.1, chord);
        CHECK (r.engine().getNumActiveVoices() == 1);
        CHECK (r.engine().getMonoNote() == 67);
        r.reset();

        r.set (unisonVoices, 3.0f);
        r.render (0.1, chord);
        CHECK (r.engine().getNumActiveVoices() == 3);
        r.set (unisonVoices, 1.0f);
        r.reset();
    }

    // Poly with unison: every note takes `unison` voices.
    r.set (voiceMode, poly);
    r.set (unisonVoices, 4.0f);
    r.render (0.1, chord);
    CHECK (r.engine().getNumActiveVoices() == 12);
    // 16 voices / 4 per note: a fifth note steals, but never the note being started.
    r.render (0.05, { { 0, on (70) }, { 5, on (72) } });
    CHECK (r.engine().getNumActiveVoices() == cash::SynthEngine::numVoices);
}

TEST_CASE ("Mono retriggers the envelopes; Legato only when no key was down", "[lead]")
{
    Rig r;
    using namespace cash::param;
    pureSine (r);
    r.set (ampAttack, 0.003f);
    r.set (ampDecay, 0.1f);
    r.set (ampSustain, 0.25f);

    // Hold 60, then at 0.5 s play 62 over it. Ratio of the level just after to just before.
    auto jump = [&r]
    {
        auto out = r.render (0.8, { { 0, on (60) }, { at (0.5), on (62) } });
        const float before = peak (out.left, 0.40, 0.50);
        const float after = peak (out.left, 0.505, 0.55);
        r.reset();
        return after / before;
    };

    r.set (voiceMode, mono);
    CHECK (jump() > 2.5f);   // attack again up to full level
    r.set (voiceMode, legato);
    CHECK (jump() < 1.15f);  // stays on the sustain level

    // Legato, keys not overlapping: a fresh note does retrigger.
    auto out = r.render (1.0, { { 0, on (60) }, { at (0.4), off (60) }, { at (0.7), on (62) } });
    CHECK (peak (out.left, 0.705, 0.75) > 2.5f * peak (out.left, 0.30, 0.40));
}

TEST_CASE ("Glide reaches the new pitch in the set time", "[lead][glide]")
{
    Rig r;
    using namespace cash::param;
    pureSine (r);
    r.set (voiceMode, legato);
    r.set (glideTime, 0.2f);

    SECTION ("constant time: 0.2 s whatever the interval")
    {
        r.render (0.3, { { 0, on (57) } });
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (57.0f));
        r.render (0.1, { { 0, on (69) } });
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (63.0f).margin (0.1)); // halfway in semitones
        r.render (0.1);
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (69.0f).margin (0.01));
        auto settled = r.render (0.5);
        CHECK (frequency (settled.left, 0.1, 0.5) == Catch::Approx (440.0).margin (5.0));

        // A bigger jump takes the same time.
        r.render (0.1, { { 0, on (45) } });
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (57.0f).margin (0.1));
    }

    SECTION ("constant rate: the time is per octave")
    {
        r.set (glideType, constantRate);
        r.set (glideTime, 0.1f);
        r.render (0.3, { { 0, on (57) } });
        r.render (0.1, { { 0, on (81) } }); // two octaves: 0.2 s
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (69.0f).margin (0.1));
        r.render (0.1);
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (81.0f).margin (0.01));
    }

    SECTION ("Legato-only glide skips detached notes; Always glides them")
    {
        r.set (glideMode, glideLegato);
        r.render (0.3, { { 0, on (57) }, { at (0.2), off (57) } });
        r.render (0.05, { { 0, on (69) } });
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (69.0f));
        r.render (0.3, { { 0, off (69) } });

        r.set (glideMode, glideAlways);
        r.render (0.05, { { 0, on (57) } });
        CHECK (r.engine().getVoicePitch (0) < 66.0f);
    }

    SECTION ("Poly glides each new note from the last one")
    {
        r.set (voiceMode, poly);
        r.render (0.3, { { 0, on (57) } });
        r.render (0.1, { { 0, on (69) } });
        // The new note sits on its own voice, mid-glide; the old one stays put.
        float newest = -1.0f;
        for (int i = 0; i < cash::SynthEngine::numVoices; ++i)
            if (r.engine().isVoiceActive (i) && r.engine().getVoicePitch (i) != Catch::Approx (57.0f))
                newest = r.engine().getVoicePitch (i);
        CHECK (newest == Catch::Approx (63.0f).margin (0.1));
    }
}

TEST_CASE ("Note priority picks the note and falls back to held keys", "[lead][priority]")
{
    Rig r;
    using namespace cash::param;
    r.set (voiceMode, mono);
    auto note = [&r] { return r.engine().getMonoNote(); };
    auto play = [&r] (const juce::MidiMessage& m) { r.render (0.02, { { 0, m } }); };

    SECTION ("Last")
    {
        play (on (60)); play (on (64));
        CHECK (note() == 64);
        play (off (64));
        CHECK (note() == 60);
        CHECK (r.engine().getVoicePitch (0) == Catch::Approx (60.0f));
        play (on (67)); play (off (60)); // releasing a key that isn't sounding changes nothing
        CHECK (note() == 67);
        play (off (67));
        CHECK (note() == -1);
    }

    SECTION ("Low")
    {
        r.set (notePriority, lowNote);
        play (on (60)); play (on (64));
        CHECK (note() == 60);
        play (on (55));
        CHECK (note() == 55);
        play (off (55));
        CHECK (note() == 60);
        play (off (60));
        CHECK (note() == 64);
    }

    SECTION ("High")
    {
        r.set (notePriority, highNote);
        play (on (64)); play (on (60));
        CHECK (note() == 64);
        play (off (64));
        CHECK (note() == 60);
    }

    SECTION ("Sustain pedal holds the last note")
    {
        play (on (60));
        play (juce::MidiMessage::controllerEvent (1, 64, 127));
        play (off (60));
        CHECK (note() == 60);
        play (juce::MidiMessage::controllerEvent (1, 64, 0));
        CHECK (note() == -1);
    }
}

TEST_CASE ("Unison widens the stereo image; spread 0 stays mono", "[lead][unison]")
{
    Rig r;
    using namespace cash::param;
    r.set (unisonDetune, 0.4f);

    auto width = [&r] (float voices, float spread, double* level = nullptr)
    {
        r.set (unisonVoices, voices);
        r.set (unisonSpread, spread);
        auto out = r.render (0.5, { { 0, on (48) } });
        r.reset();
        std::vector<float> mid, side;
        for (size_t i = 0; i < out.left.size(); ++i)
        {
            mid.push_back (0.5f * (out.left[i] + out.right[i]));
            side.push_back (0.5f * (out.left[i] - out.right[i]));
        }
        if (level != nullptr)
            *level = rms (mid, 0.1, 0.5);
        return rms (side, 0.1, 0.5) / rms (mid, 0.1, 0.5);
    };

    double single = 0.0, stack = 0.0;
    CHECK (width (1.0f, 1.0f, &single) < 1.0e-6);
    CHECK (width (4.0f, 0.0f) < 1.0e-6);
    CHECK (width (4.0f, 1.0f, &stack) > 0.25);
    CHECK (width (8.0f, 1.0f) > 0.25);
    // Stacking doesn't blow the level up (voices are scaled by 1/sqrt(n)).
    CHECK (stack / single > 0.5);
    CHECK (stack / single < 2.0);

    // Outer voices are panned hard, mirrored.
    r.set (unisonVoices, 3.0f);
    r.set (unisonSpread, 1.0f);
    r.set (voiceMode, mono);
    r.render (0.05, { { 0, on (48) } });
    CHECK (r.engine().getVoicePan (0) == Catch::Approx (-1.0f));
    CHECK (r.engine().getVoicePan (1) == Catch::Approx (0.0f));
    CHECK (r.engine().getVoicePan (2) == Catch::Approx (1.0f));
}

TEST_CASE ("Bend range, mod wheel vibrato and velocity to cutoff", "[lead][controllers]")
{
    Rig r;
    using namespace cash::param;
    pureSine (r);

    // Full bend up with a 12 st range: an octave.
    r.set (bendRange, 12.0f);
    auto bent = r.render (0.6, { { 0, juce::MidiMessage::pitchWheel (1, 16383) }, { 0, on (57) } });
    CHECK (frequency (bent.left, 0.1, 0.6) == Catch::Approx (440.0).margin (5.0));
    r.reset();
    r.set (bendRange, 2.0f);
    bent = r.render (0.6, { { 0, juce::MidiMessage::pitchWheel (1, 16383) }, { 0, on (57) } });
    CHECK (frequency (bent.left, 0.1, 0.6) == Catch::Approx (220.0 * std::exp2 (2.0 / 12.0)).margin (4.0));
    r.reset();

    // Mod wheel adds LFO 1 vibrato only when it's up and the depth is set.
    auto take = [&r] (int wheel)
    {
        auto out = r.render (0.5, { { 0, juce::MidiMessage::controllerEvent (1, 1, wheel) }, { 0, on (69) } });
        r.reset();
        return out.left;
    };
    r.set (modVibrato, 1.0f);
    const auto still = take (0), wobble = take (127);
    r.set (modVibrato, 0.0f);
    const auto noDepth = take (127);
    CHECK (noDepth == still);
    double diff = 0.0;
    for (size_t i = 0; i < still.size(); ++i)
        diff += std::abs (still[i] - wobble[i]);
    CHECK (diff / static_cast<double> (still.size()) > 0.05);

    // Velocity -> cutoff: soft notes are darker (level isn't touched: velocitySens 0).
    r.set (osc1Wave, saw);
    r.set (filterCutoff, 4000.0f);
    r.set (velocitySens, 0.0f);
    r.set (velToCutoff, 1.0f);
    auto level = [&r] (float vel)
    {
        auto out = r.render (0.4, { { 0, on (72, vel) } });
        r.reset();
        return rms (out.left, 0.1, 0.4);
    };
    const double hard = level (1.0f), soft = level (0.2f);
    CHECK (soft < hard * 0.6);
    r.set (velToCutoff, 0.0f);
    CHECK (level (0.2f) == Catch::Approx (hard).epsilon (0.01));
}

TEST_CASE ("Old codes and projects load with lead mode off", "[lead][state][cashcode]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    const auto defaults = cash::defaultPatch (p.getState());
    auto& st = p.getState();

    // The pinned default-patch code from M6 (CashCode v2).
    const auto v2 = cash::CashCode::decode ("$54S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0GBB", defaults);
    REQUIRE (v2.has_value());
    for (int i = 0; i < cash::ParameterMap::numParams; ++i)
        CHECK (std::abs ((*v2)[static_cast<size_t> (i)] - defaults[static_cast<size_t> (i)]) <= 0.5f / 255.0f + 1.0e-6f);

    // An M6 project: no M7 params in it anywhere.
    const char* m7[] = { cash::param::voiceMode, cash::param::glideTime, cash::param::glideMode, cash::param::glideType,
                         cash::param::notePriority, cash::param::unisonVoices, cash::param::unisonDetune,
                         cash::param::unisonSpread, cash::param::bendRange, cash::param::modVibrato, cash::param::velToCutoff };
    p.bankIt();
    juce::MemoryBlock block;
    p.getStateInformation (block);
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    auto root = juce::ValueTree::fromXml (*xml);
    auto params = root.getChildWithName ("PARAMS");
    for (auto* id : m7)
    {
        params.removeChild (params.getChildWithProperty ("id", id), nullptr);
        root.getChildWithName ("Vault").getChild (0).getChildWithName ("Patch").removeProperty (id, nullptr);
    }
    REQUIRE (params.getNumChildren() == cash::ParameterMap::numParamsV2);

    // Load it into an instance that's currently in Mono with unison and glide.
    auto setParam = [&st] (const char* id, float v) { auto* q = st.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
    setParam (cash::param::voiceMode, cash::param::mono);
    setParam (cash::param::unisonVoices, 5.0f);
    setParam (cash::param::glideTime, 0.8f);
    setParam (cash::param::bendRange, 12.0f);

    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), old);
    p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));

    CHECK (cash::samePatch (p.getPatch(), defaults));
    CHECK (st.getRawParameterValue (cash::param::voiceMode)->load() == 0.0f);
    CHECK (st.getRawParameterValue (cash::param::bendRange)->load() == 2.0f);
    REQUIRE (p.getVault().size() == 1);
    CHECK (cash::samePatch (p.getVault().at (0).patch, defaults));
}

TEST_CASE ("Money moves leave how you play alone and keep lead settings tame", "[lead][mutator]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    auto patch = cash::defaultPatch (p.getState());
    const auto mode = static_cast<size_t> (cash::ParameterMap::indexOf (cash::param::voiceMode));
    const auto glide = static_cast<size_t> (cash::ParameterMap::indexOf (cash::param::glideTime));
    const auto uni = static_cast<size_t> (cash::ParameterMap::indexOf (cash::param::unisonVoices));
    patch[mode] = cash::ParameterMap::fromStep (cash::ParameterMap::spec (static_cast<int> (mode)), cash::param::legato);

    int glides = 0, stacks = 0;
    constexpr int rolls = 200;
    for (uint64_t seed = 1; seed <= rolls; ++seed)
    {
        const auto j = cash::Mutator::jackpot (patch, seed);
        CHECK (j[mode] == patch[mode]);
        glides += j[glide] > 0.0f ? 1 : 0;
        const int voices = cash::ParameterMap::toStep (cash::ParameterMap::spec (static_cast<int> (uni)), j[uni]) + 1;
        CHECK (voices <= 4);
        stacks += voices > 1 ? 1 : 0;

        const auto inv = cash::Mutator::invest (patch, { 1.0f, 1.0f, seed });
        CHECK (inv[mode] == patch[mode]);
    }
    CHECK (glides > rolls / 10);
    CHECK (glides < rolls / 2);
    CHECK (stacks > rolls / 10);
    CHECK (stacks < rolls / 2);
}

TEST_CASE ("Front voice switch and back VOICE section drive the params", "[lead][editor][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    std::unique_ptr<CashSynthAudioProcessorEditor> editor (dynamic_cast<CashSynthAudioProcessorEditor*> (p.createEditor()));
    REQUIRE (editor != nullptr);

    auto& sw = editor->getFront().getVoiceSwitch();
    CHECK (sw.isVisible());
    CHECK (sw.getMode() == cash::param::poly);
    sw.choose (cash::param::legato);
    CHECK (p.getState().getRawParameterValue (cash::param::voiceMode)->load() == static_cast<float> (cash::param::legato));

    // And the other way: the host changes it, the switch follows.
    auto* param = p.getState().getParameter (cash::param::voiceMode);
    param->setValueNotifyingHost (param->convertTo0to1 (cash::param::mono));
    CHECK (sw.getMode() == cash::param::mono);

    // The switch sits inside the bill, clear of the portrait and the serial.
    const auto bounds = sw.getBounds();
    CHECK (editor->getFront().getLocalBounds().contains (bounds));
    CHECK_FALSE (bounds.intersects (editor->getFront().getPortrait().getBounds()));
    CHECK_FALSE (bounds.intersects (editor->getFront().getSerial (0).getBounds()));
}

