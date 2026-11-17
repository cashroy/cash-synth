#pragma once

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>

// One fixed, ordered table of every synth parameter. The order is the CashCode
// bit order, so it is APPEND-ONLY: never reorder, remove or change a step count
// without bumping CashCode::version.
namespace cash
{
struct ParamSpec
{
    const char* id;
    int steps;     // 0 = continuous, otherwise the number of discrete values
    bool ordered;  // stepped values have an order (octave) rather than being categories (wave)
    float weight;  // mutation weight: 0 = never mutated, 1 = normal
};

class ParameterMap
{
public:
    static constexpr int numParams = 303;
    static constexpr int numParamsV1 = 31; // CashCode v1 carried only the first 31
    static constexpr int numParamsV2 = 39; // v2 (M6) the first 39
    static constexpr int numParamsV3 = 50; // v3 (M7) the first 50; v4 (M8) adds the section switches
    static constexpr int numParamsV4 = 60; // v4 the first 60; v5 (M10) adds LFO tempo sync
    static constexpr int numParamsV5 = 64; // v5 the first 64; v6 (M11) adds delay + reverb
    static constexpr int numParamsV6 = 81; // v6 the first 81; v7 (M13) adds the Mint
    static constexpr int numParamsV7 = 87; // v7 the first 87; v8 adds Mint Start + Mint Loop
    static constexpr int numParamsV8 = 89; // v8 the first 89; v9 (M14) adds the LAYERS (204 params)
    static constexpr int numParamsV9 = 293; // v9 the first 293; v10 adds layer Decay Curve (8) + noise Slope (2)

    static const std::array<ParamSpec, numParams>& specs();
    static const ParamSpec& spec (int index) { return specs()[static_cast<size_t> (index)]; }
    static int indexOf (juce::StringRef id); // -1 if unknown

    // Stepped helpers: JUCE maps step k of n to k / (n - 1).
    static int toStep (const ParamSpec&, float normalised);
    static float fromStep (const ParamSpec&, int step);
    static float snap (const ParamSpec&, float normalised); // clamps, and rounds stepped values
};

// Every parameter's normalised (0..1) value, in ParameterMap order.
using Patch = std::array<float, ParameterMap::numParams>;

Patch capturePatch (const juce::AudioProcessorValueTreeState&);
Patch defaultPatch (const juce::AudioProcessorValueTreeState&);
void applyPatch (juce::AudioProcessorValueTreeState&, const Patch&); // message thread; notifies host

// Equal within tolerance: values that went through a parameter's real-unit
// round trip come back a hair off.
bool samePatch (const Patch&, const Patch&, float tolerance = 1.0e-4f);

// Patch <-> ValueTree with one property per parameter ID (normalised value).
// Used for project state and Stash files. Reading is forgiving: unknown IDs are
// ignored and missing ones keep `fallback`, so old files survive new parameters.
juce::ValueTree patchToTree (const Patch&, const juce::Identifier& type);
Patch patchFromTree (const juce::ValueTree&, const Patch& fallback);

// Parameters that are performance settings, not part of the sound (weight 0).
bool isPerformanceParam (int index);

// M8 section on/off switches (indices numParamsV3 .. numParamsV4 - 1).
bool isSectionSwitch (int index);
void allSectionsOn (Patch&);

// M10: LFO rates back to TIME (Hz). Cash Match works in Hz only.
void unsync (Patch&);

// M11: delay + reverb (indices numParamsV5 .. numParamsV6 - 1). Cash Match leaves them off.
bool isEffectParam (int index);
void effectsOff (Patch&);

// M13: the Mint's params (indices numParamsV6 .. numParamsV8 - 1).
bool isMintParam (int index);

// M14: the LAYERS params (indices numParamsV8 ..).
bool isLayerParam (int index);
}
