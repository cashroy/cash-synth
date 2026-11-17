#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Every synth parameter lives here. IDs are stable: hosts, presets and the
// future ParameterMap key on them, so never rename one.
namespace cash::param
{
    inline constexpr const char* osc1Wave   = "osc1Wave";
    inline constexpr const char* osc1Octave = "osc1Octave";
    inline constexpr const char* osc1Semi   = "osc1Semi";
    inline constexpr const char* osc1Fine   = "osc1Fine";
    inline constexpr const char* osc1Level  = "osc1Level";

    inline constexpr const char* osc2Wave   = "osc2Wave";
    inline constexpr const char* osc2Octave = "osc2Octave";
    inline constexpr const char* osc2Semi   = "osc2Semi";
    inline constexpr const char* osc2Fine   = "osc2Fine";
    inline constexpr const char* osc2Level  = "osc2Level";

    inline constexpr const char* subLevel   = "subLevel";
    inline constexpr const char* noiseLevel = "noiseLevel";

    inline constexpr const char* filterType     = "filterType";
    inline constexpr const char* filterCutoff   = "filterCutoff";
    inline constexpr const char* filterReso     = "filterReso";
    inline constexpr const char* filterEnvAmt   = "filterEnvAmt";
    inline constexpr const char* filterKeyTrack = "filterKeyTrack";

    inline constexpr const char* fltAttack  = "fltAttack";
    inline constexpr const char* fltDecay   = "fltDecay";
    inline constexpr const char* fltSustain = "fltSustain";
    inline constexpr const char* fltRelease = "fltRelease";

    inline constexpr const char* ampAttack  = "ampAttack";
    inline constexpr const char* ampDecay   = "ampDecay";
    inline constexpr const char* ampSustain = "ampSustain";
    inline constexpr const char* ampRelease = "ampRelease";

    inline constexpr const char* lfoWave     = "lfoWave";
    inline constexpr const char* lfoRate     = "lfoRate";
    inline constexpr const char* lfoToPitch  = "lfoToPitch";
    inline constexpr const char* lfoToCutoff = "lfoToCutoff";

    inline constexpr const char* velocitySens = "velocitySens";
    inline constexpr const char* masterGain   = "masterGain";

    // M6: drive, LFO 2 with a routable target, pitch envelope. Appended (CashCode v2).
    inline constexpr const char* driveType   = "driveType";
    inline constexpr const char* driveAmount = "driveAmount";
    inline constexpr const char* lfoToDrive  = "lfoToDrive";   // LFO 1 -> drive
    inline constexpr const char* lfo2Wave    = "lfo2Wave";
    inline constexpr const char* lfo2Rate    = "lfo2Rate";
    inline constexpr const char* lfo2Amount  = "lfo2Amount";
    inline constexpr const char* lfo2Target  = "lfo2Target";
    inline constexpr const char* pitchEnvAmt = "pitchEnvAmt";  // filter env -> pitch, +/- 24 st

    enum Wave { sine = 0, triangle, saw, square };
    enum FilterMode { lowPass = 0, bandPass, highPass };
    enum DriveType { softClip = 0, hardClip, fold };
    enum LfoTarget { toCutoff = 0, toPitch, toAmp, toDrive, toOsc2 };

    inline constexpr float pitchEnvRange = 24.0f; // semitones at 100 %

    juce::StringArray waveNames();
    juce::StringArray filterNames();
    juce::StringArray driveNames();
    juce::StringArray lfoTargetNames();

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
