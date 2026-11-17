#include "ParameterMap.h"
#include "Parameters.h"

namespace cash
{
const std::array<ParamSpec, ParameterMap::numParams>& ParameterMap::specs()
{
    using namespace param;
    // id, steps, ordered, weight
    static const std::array<ParamSpec, numParams> table { {
        { osc1Wave,   4,  false, 1.0f },
        { osc1Octave, 5,  true,  0.4f },
        { osc1Semi,   25, true,  0.3f },
        { osc1Fine,   0,  false, 0.6f },
        { osc1Level,  0,  false, 1.0f },

        { osc2Wave,   4,  false, 1.0f },
        { osc2Octave, 5,  true,  0.4f },
        { osc2Semi,   25, true,  0.3f },
        { osc2Fine,   0,  false, 0.6f },
        { osc2Level,  0,  false, 1.0f },

        { subLevel,   0,  false, 1.0f },
        { noiseLevel, 0,  false, 0.6f },

        { filterType,     3, false, 0.5f },
        { filterCutoff,   0, false, 1.0f },
        { filterReso,     0, false, 1.0f },
        { filterEnvAmt,   0, false, 1.0f },
        { filterKeyTrack, 0, false, 0.6f },

        { fltAttack,  0, false, 1.0f },
        { fltDecay,   0, false, 1.0f },
        { fltSustain, 0, false, 1.0f },
        { fltRelease, 0, false, 1.0f },

        { ampAttack,  0, false, 1.0f },
        { ampDecay,   0, false, 1.0f },
        { ampSustain, 0, false, 1.0f },
        { ampRelease, 0, false, 1.0f },

        { lfoWave,     4, false, 0.6f },
        { lfoRate,     0, false, 1.0f },
        { lfoToPitch,  0, false, 0.4f },
        { lfoToCutoff, 0, false, 1.0f },

        // Performance settings: the player's, not the market's.
        { velocitySens, 0, false, 0.0f },
        { masterGain,   0, false, 0.0f },

        // M6 (CashCode v2): drive, LFO 2, pitch env.
        { driveType,   3, false, 0.5f },
        { driveAmount, 0, false, 0.8f },
        { lfoToDrive,  0, false, 0.4f },
        { lfo2Wave,    4, false, 0.4f },
        { lfo2Rate,    0, false, 0.8f },
        { lfo2Amount,  0, false, 0.6f },
        { lfo2Target,  5, false, 0.4f },
        { pitchEnvAmt, 0, false, 0.3f },
    } };
    return table;
}

bool isPerformanceParam (int index)
{
    return ParameterMap::spec (index).weight <= 0.0f;
}

int ParameterMap::indexOf (juce::StringRef id)
{
    for (int i = 0; i < numParams; ++i)
        if (id == juce::StringRef (spec (i).id))
            return i;
    return -1;
}

int ParameterMap::toStep (const ParamSpec& s, float normalised)
{
    jassert (s.steps > 1);
    return juce::jlimit (0, s.steps - 1, juce::roundToInt (juce::jlimit (0.0f, 1.0f, normalised) * static_cast<float> (s.steps - 1)));
}

float ParameterMap::fromStep (const ParamSpec& s, int step)
{
    jassert (s.steps > 1);
    return static_cast<float> (juce::jlimit (0, s.steps - 1, step)) / static_cast<float> (s.steps - 1);
}

float ParameterMap::snap (const ParamSpec& s, float normalised)
{
    const float v = std::isfinite (normalised) ? juce::jlimit (0.0f, 1.0f, normalised) : 0.0f;
    return s.steps > 1 ? fromStep (s, toStep (s, v)) : v;
}

namespace
{
    template <typename Fn>
    Patch readPatch (const juce::AudioProcessorValueTreeState& state, Fn&& read)
    {
        Patch p {};
        for (int i = 0; i < ParameterMap::numParams; ++i)
        {
            const auto& s = ParameterMap::spec (i);
            auto* param = state.getParameter (s.id);
            jassert (param != nullptr);
            p[static_cast<size_t> (i)] = param != nullptr ? ParameterMap::snap (s, read (*param)) : 0.0f;
        }
        return p;
    }
}

Patch capturePatch (const juce::AudioProcessorValueTreeState& state)
{
    return readPatch (state, [] (const juce::RangedAudioParameter& p) { return p.getValue(); });
}

Patch defaultPatch (const juce::AudioProcessorValueTreeState& state)
{
    return readPatch (state, [] (const juce::RangedAudioParameter& p) { return p.getDefaultValue(); });
}

void applyPatch (juce::AudioProcessorValueTreeState& state, const Patch& patch)
{
    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        const auto& s = ParameterMap::spec (i);
        auto* param = state.getParameter (s.id);
        jassert (param != nullptr);
        if (param == nullptr)
            continue;

        const float v = ParameterMap::snap (s, patch[static_cast<size_t> (i)]);
        if (std::abs (param->getValue() - v) < 1.0e-6f)
            continue;

        param->beginChangeGesture();
        param->setValueNotifyingHost (v);
        param->endChangeGesture();
    }
}

bool samePatch (const Patch& a, const Patch& b, float tolerance)
{
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs (a[i] - b[i]) > tolerance)
            return false;
    return true;
}

juce::ValueTree patchToTree (const Patch& patch, const juce::Identifier& type)
{
    juce::ValueTree tree (type);
    for (int i = 0; i < ParameterMap::numParams; ++i)
        tree.setProperty (ParameterMap::spec (i).id, patch[static_cast<size_t> (i)], nullptr);
    return tree;
}

Patch patchFromTree (const juce::ValueTree& tree, const Patch& fallback)
{
    Patch patch = fallback;
    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        const auto& s = ParameterMap::spec (i);
        if (const auto* v = tree.getPropertyPointer (s.id); v != nullptr && (v->isDouble() || v->isInt() || v->isString()))
        {
            const auto f = static_cast<float> (static_cast<double> (*v));
            if (std::isfinite (f))
                patch[static_cast<size_t> (i)] = ParameterMap::snap (s, f);
        }
    }
    return patch;
}
}
