#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Parameters.h"

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

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

    // Renders `seconds` of audio into one mono vector (left channel),
    // placing each MIDI message at its absolute sample position.
    std::vector<float> render (double seconds, std::vector<std::pair<int, juce::MidiMessage>> events = {})
    {
        const int total = static_cast<int> (seconds * sampleRate);
        std::vector<float> out;
        out.reserve (static_cast<size_t> (total));

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
                out.push_back (buffer.getSample (0, i));
        }
        return out;
    }
};

float peak (const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX)
{
    float m = 0.0f;
    for (size_t i = from; i < juce::jmin (to, v.size()); ++i)
        m = juce::jmax (m, std::abs (v[i]));
    return m;
}

double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += static_cast<double> (v[i]) * v[i];
    return std::sqrt (sum / static_cast<double> (to - from));
}

bool allFinite (const std::vector<float>& v)
{
    return std::all_of (v.begin(), v.end(), [] (float x) { return std::isfinite (x); });
}

juce::MidiMessage on (int note, float vel = 1.0f) { return juce::MidiMessage::noteOn (1, note, vel); }
juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }

// Pure sine through an open filter: makes pitch measurable.
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

TEST_CASE ("Parameter layout is complete and defaults are in range", "[params]")
{
    Rig r;
    const auto& params = r.p.getParameters();
    CHECK (params.size() == 31);

    for (auto* p : params)
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        REQUIRE (ranged != nullptr);
        CHECK (ranged->getDefaultValue() >= 0.0f);
        CHECK (ranged->getDefaultValue() <= 1.0f);
        CHECK (ranged->getName (64).isNotEmpty());
        CHECK (r.p.getState().getRawParameterValue (ranged->getParameterID()) != nullptr);
    }
}

TEST_CASE ("A note makes sound and stops after release", "[engine]")
{
    Rig r;
    r.set (cash::param::ampRelease, 0.05f);

    const int offAt = static_cast<int> (0.3 * sampleRate);
    auto out = r.render (0.8, { { 0, on (60) }, { offAt, off (60) } });

    CHECK (allFinite (out));
    CHECK (peak (out, 0, static_cast<size_t> (offAt)) > 0.05f);
    CHECK (peak (out) < 1.0f);
    // 50 ms release: well before the end the voice must be silent and freed.
    CHECK (peak (out, static_cast<size_t> (0.6 * sampleRate)) < 1.0e-4f);
    CHECK (r.p.getEngine().getNumActiveVoices() == 0);
}

TEST_CASE ("Oscillator pitch follows the MIDI note", "[engine]")
{
    Rig r;
    pureSine (r);

    auto measure = [&r] (int note)
    {
        auto out = r.render (1.2, { { 0, on (note) }, { static_cast<int> (1.1 * sampleRate), off (note) } });
        r.render (0.6); // let the release finish before the next measurement
        // Count rising zero crossings across exactly one second, skipping the attack.
        int crossings = 0;
        const size_t from = static_cast<size_t> (0.05 * sampleRate);
        for (size_t i = from + 1; i < from + static_cast<size_t> (sampleRate); ++i)
            if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                ++crossings;
        return crossings;
    };

    CHECK (measure (69) == Catch::Approx (440).margin (2));
    CHECK (measure (57) == Catch::Approx (220).margin (2));

    r.set (cash::param::osc1Octave, 1.0f);
    CHECK (measure (69) == Catch::Approx (880).margin (2));
}

TEST_CASE ("Low-pass cutoff removes content above it", "[engine]")
{
    Rig r;
    using namespace cash::param;
    r.set (filterEnvAmt, 0.0f);
    r.set (filterReso, 0.0f);
    r.set (osc2Level, 0.0f);

    // C5 saw (523 Hz fundamental) through a 100 Hz low-pass: everything sits
    // well above cutoff, so the level should collapse (12 dB/oct slope).
    auto level = [&r] (float cutoff)
    {
        r.set (filterCutoff, cutoff);
        auto out = r.render (0.5, { { 0, on (72) }, { static_cast<int> (0.45 * sampleRate), off (72) } });
        r.render (0.6);
        return rms (out, static_cast<size_t> (0.1 * sampleRate), static_cast<size_t> (0.4 * sampleRate));
    };

    const double open = level (18000.0f);
    const double closed = level (100.0f);
    CHECK (closed < open * 0.1);
}

TEST_CASE ("Master gain scales the output", "[engine]")
{
    Rig r;
    r.set (cash::param::osc2Level, 0.0f); // osc 2 detune beats would skew the RMS windows

    auto level = [&r] (float db)
    {
        r.set (cash::param::masterGain, db);
        auto out = r.render (0.4, { { 0, on (60) }, { static_cast<int> (0.35 * sampleRate), off (60) } });
        r.render (0.6);
        return rms (out, static_cast<size_t> (0.1 * sampleRate), static_cast<size_t> (0.3 * sampleRate));
    };

    const double loud = level (0.0f);
    const double quiet = level (-20.0f);
    CHECK (quiet / loud == Catch::Approx (0.1).margin (0.005));
}

TEST_CASE ("Polyphony plays 16 voices at once", "[engine]")
{
    Rig r;
    std::vector<std::pair<int, juce::MidiMessage>> events;
    for (int i = 0; i < 20; ++i)
        events.push_back ({ i * 10, on (40 + i * 2, 0.8f) });

    auto out = r.render (0.3, events);
    CHECK (allFinite (out));
    CHECK (r.p.getEngine().getNumActiveVoices() == cash::SynthEngine::numVoices);
}

TEST_CASE ("Random parameter settings never produce NaN or blow up", "[engine][fuzz]")
{
    Rig r;
    juce::Random rng (1234);

    for (int round = 0; round < 40; ++round)
    {
        for (auto* p : r.p.getParameters())
            p->setValueNotifyingHost (rng.nextFloat());

        std::vector<std::pair<int, juce::MidiMessage>> events;
        for (int i = 0; i < 4; ++i)
            events.push_back ({ rng.nextInt (2000), on (rng.nextInt ({ 24, 108 }), rng.nextFloat()) });
        events.push_back ({ 3000, juce::MidiMessage::pitchWheel (1, rng.nextInt (16384)) });

        auto out = r.render (0.1, events);
        INFO ("round " << round);
        REQUIRE (allFinite (out));
        REQUIRE (peak (out) < 8.0f);

        r.p.releaseResources(); // all notes off between rounds
    }
}

TEST_CASE ("Parameter values display with sensible precision", "[params]")
{
    Rig r;
    auto text = [&r] (const char* id, float value)
    {
        auto* p = r.p.getState().getParameter (id);
        return p->getText (p->convertTo0to1 (value), 32);
    };

    CHECK (text (cash::param::osc2Fine, 7.0f) == "7 ct");
    CHECK (text (cash::param::filterCutoff, 440.0f) == "440 Hz");
    CHECK (text (cash::param::filterCutoff, 4000.0f) == "4.00 kHz");
    CHECK (text (cash::param::ampAttack, 0.005f) == "5 ms");
}
