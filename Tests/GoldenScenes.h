#pragma once

// Fixed render scenes used to prove the M8 engine (smoothing, section toggles)
// sounds exactly like the M7 engine when every section is on and nothing moves.
// GoldenRenders.inc was written by the M7 build ("[.golden-write]" in AutomationTests.cpp).
#include <juce_audio_processors/juce_audio_processors.h>
#include <utility>
#include <vector>

namespace golden
{
inline constexpr double sampleRate = 48000.0;
inline constexpr int blockSize = 512;
inline constexpr int stride = 31;   // keep every 31st sample
inline constexpr double seconds = 0.6;

struct Scene
{
    const char* name;
    std::vector<std::pair<const char*, float>> values; // real units, M7 IDs only
    std::vector<std::pair<double, juce::MidiMessage>> events;
};

inline std::vector<Scene> scenes()
{
    using M = juce::MidiMessage;
    return {
        { "defaults chord", {},
          { { 0.0, M::noteOn (1, 48, 0.9f) }, { 0.0, M::noteOn (1, 55, 0.7f) }, { 0.01, M::noteOn (1, 60, 1.0f) },
            { 0.35, M::noteOff (1, 48) }, { 0.35, M::noteOff (1, 55) }, { 0.4, M::noteOff (1, 60) } } },

        { "busy poly",
          { { "osc1Wave", 3.0f }, { "osc2Wave", 1.0f }, { "osc2Semi", 7.0f }, { "osc2Fine", 12.0f }, { "subLevel", 0.4f },
            { "filterType", 1.0f }, { "filterCutoff", 1200.0f }, { "filterReso", 0.6f }, { "filterEnvAmt", -0.4f },
            { "filterKeyTrack", 0.5f }, { "fltAttack", 0.02f }, { "fltDecay", 0.2f }, { "fltSustain", 0.5f },
            { "lfoRate", 6.0f }, { "lfoToPitch", 0.3f }, { "lfoToCutoff", 0.4f },
            { "driveType", 2.0f }, { "driveAmount", 0.5f }, { "lfoToDrive", 0.3f },
            { "lfo2Wave", 3.0f }, { "lfo2Rate", 3.0f }, { "lfo2Amount", 0.6f }, { "lfo2Target", 2.0f },
            { "pitchEnvAmt", 0.3f }, { "velToCutoff", 0.5f } },
          { { 0.0, M::noteOn (1, 50, 0.6f) }, { 0.1, M::noteOn (1, 57, 0.4f) }, { 0.3, M::noteOff (1, 50) }, { 0.45, M::noteOff (1, 57) } } },

        { "legato unison",
          { { "voiceMode", 2.0f }, { "glideTime", 0.1f }, { "unisonVoices", 4.0f }, { "unisonDetune", 0.6f },
            { "unisonSpread", 0.8f }, { "lfoToPitch", 0.0f }, { "lfo2Amount", 0.3f }, { "lfo2Target", 4.0f } },
          { { 0.0, M::noteOn (1, 48, 1.0f) }, { 0.05, M::controllerEvent (1, 1, 64) }, { 0.15, M::noteOn (1, 55, 0.8f) },
            { 0.2, M::pitchWheel (1, 12000) }, { 0.25, M::noteOff (1, 48) }, { 0.4, M::noteOff (1, 55) } } },

        { "mono rate glide",
          { { "voiceMode", 1.0f }, { "glideTime", 0.05f }, { "glideType", 1.0f }, { "filterType", 2.0f },
            { "filterCutoff", 300.0f }, { "driveType", 1.0f }, { "driveAmount", 0.3f },
            { "lfo2Amount", 0.3f }, { "lfo2Target", 1.0f }, { "osc1Octave", -1.0f } },
          { { 0.0, M::noteOn (1, 60, 1.0f) }, { 0.1, M::controllerEvent (1, 64, 127) }, { 0.12, M::noteOn (1, 64, 0.9f) },
            { 0.2, M::noteOff (1, 64) }, { 0.22, M::noteOff (1, 60) }, { 0.35, M::controllerEvent (1, 64, 0) } } },
    };
}

// Plays a scene on a freshly prepared processor and returns every `stride`th sample (L then R interleaved).
template <typename Processor>
std::vector<float> render (Processor& p, const Scene& scene)
{
    auto& state = p.getState();
    for (auto& [id, value] : scene.values)
        if (auto* param = state.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    p.prepareToPlay (sampleRate, blockSize);

    const int total = static_cast<int> (seconds * sampleRate);
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        buffer.setSize (2, n, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        for (auto& [when, msg] : scene.events)
        {
            const int s = static_cast<int> (when * sampleRate);
            if (s >= pos && s < pos + n)
                midi.addEvent (msg, s - pos);
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < n; ++i)
            if ((pos + i) % stride == 0)
            {
                out.push_back (buffer.getSample (0, i));
                out.push_back (buffer.getSample (1, i));
            }
    }
    p.releaseResources();
    return out;
}
}
