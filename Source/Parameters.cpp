#include "Parameters.h"

namespace cash::param
{
juce::StringArray waveNames()   { return { "Sine", "Triangle", "Saw", "Square" }; }
juce::StringArray filterNames() { return { "Low-pass", "Band-pass", "High-pass" }; }

namespace
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
    using FloatAttrs = juce::AudioParameterFloatAttributes;

    constexpr int version = 1;

    juce::ParameterID pid (const char* id) { return { id, version }; }

    juce::String fmt (float v, int decimals, const juce::String& unit)
    {
        // juce::String (float, 0) prints full precision, so round explicitly for integers.
        const auto number = decimals == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
        return number + (unit.isEmpty() ? "" : " " + unit);
    }

    FloatAttrs percent()
    {
        return FloatAttrs().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })
                           .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue() / 100.0f; });
    }

    FloatAttrs seconds()
    {
        return FloatAttrs().withStringFromValueFunction ([] (float v, int)
                           { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : fmt (v, 2, "s"); })
                           .withValueFromStringFunction ([] (const juce::String& s)
                           { return s.containsIgnoreCase ("ms") ? s.getFloatValue() / 1000.0f : s.getFloatValue(); });
    }

    FloatAttrs unit (int decimals, const char* u)
    {
        const juce::String suffix (u);
        return FloatAttrs().withStringFromValueFunction ([decimals, suffix] (float v, int) { return fmt (v, decimals, suffix); });
    }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    void addFloat (Layout& l, const char* id, const char* name, juce::NormalisableRange<float> range, float def, FloatAttrs attrs)
    {
        l.add (std::make_unique<juce::AudioParameterFloat> (pid (id), name, range, def, attrs));
    }

    void addOsc (Layout& l, const char* wave, const char* oct, const char* semi, const char* fine, const char* level,
                 const juce::String& name, int defWave, float defFine, float defLevel)
    {
        l.add (std::make_unique<juce::AudioParameterChoice> (pid (wave), name + " Wave", waveNames(), defWave));
        l.add (std::make_unique<juce::AudioParameterInt> (pid (oct), name + " Octave", -2, 2, 0));
        l.add (std::make_unique<juce::AudioParameterInt> (pid (semi), name + " Semi", -12, 12, 0,
                   juce::AudioParameterIntAttributes().withLabel ("st")));
        addFloat (l, fine, (name + " Fine").toRawUTF8(), { -100.0f, 100.0f }, defFine, unit (0, "ct"));
        addFloat (l, level, (name + " Level").toRawUTF8(), { 0.0f, 1.0f }, defLevel, percent());
    }

    void addEnv (Layout& l, const char* a, const char* d, const char* s, const char* r, const juce::String& name,
                 float defA, float defD, float defS, float defR)
    {
        const auto time = skewed (0.001f, 10.0f, 0.5f);
        addFloat (l, a, (name + " Attack").toRawUTF8(), time, defA, seconds());
        addFloat (l, d, (name + " Decay").toRawUTF8(), time, defD, seconds());
        addFloat (l, s, (name + " Sustain").toRawUTF8(), { 0.0f, 1.0f }, defS, percent());
        addFloat (l, r, (name + " Release").toRawUTF8(), time, defR, seconds());
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    Layout l;

    addOsc (l, osc1Wave, osc1Octave, osc1Semi, osc1Fine, osc1Level, "Osc 1", saw, 0.0f, 0.8f);
    addOsc (l, osc2Wave, osc2Octave, osc2Semi, osc2Fine, osc2Level, "Osc 2", saw, 7.0f, 0.5f);

    addFloat (l, subLevel, "Sub Level", { 0.0f, 1.0f }, 0.0f, percent());
    addFloat (l, noiseLevel, "Noise Level", { 0.0f, 1.0f }, 0.0f, percent());

    l.add (std::make_unique<juce::AudioParameterChoice> (pid (filterType), "Filter Type", filterNames(), lowPass));
    addFloat (l, filterCutoff, "Filter Cutoff", skewed (20.0f, 20000.0f, 1000.0f), 4000.0f,
              FloatAttrs().withStringFromValueFunction ([] (float v, int)
              { return v >= 1000.0f ? fmt (v / 1000.0f, 2, "kHz") : fmt (v, 0, "Hz"); }));
    addFloat (l, filterReso, "Filter Resonance", { 0.0f, 1.0f }, 0.2f, percent());
    addFloat (l, filterEnvAmt, "Filter Env Amount", { -1.0f, 1.0f }, 0.3f, percent());
    addFloat (l, filterKeyTrack, "Filter Key Track", { 0.0f, 1.0f }, 0.0f, percent());

    addEnv (l, fltAttack, fltDecay, fltSustain, fltRelease, "Filter Env", 0.005f, 0.4f, 0.3f, 0.4f);
    addEnv (l, ampAttack, ampDecay, ampSustain, ampRelease, "Amp Env", 0.005f, 0.3f, 0.8f, 0.3f);

    l.add (std::make_unique<juce::AudioParameterChoice> (pid (lfoWave), "LFO Wave", waveNames(), sine));
    addFloat (l, lfoRate, "LFO Rate", skewed (0.05f, 20.0f, 2.0f), 5.0f, unit (2, "Hz"));
    addFloat (l, lfoToPitch, "LFO to Pitch", skewed (0.0f, 12.0f, 1.0f), 0.0f, unit (2, "st"));
    addFloat (l, lfoToCutoff, "LFO to Cutoff", { 0.0f, 1.0f }, 0.0f, percent());

    addFloat (l, velocitySens, "Velocity Sensitivity", { 0.0f, 1.0f }, 0.7f, percent());
    addFloat (l, masterGain, "Master Gain", { -48.0f, 6.0f }, -6.0f, unit (1, "dB"));

    return l;
}
}
