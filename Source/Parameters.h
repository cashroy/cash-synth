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

    // M7: lead mode. Appended (CashCode v3).
    inline constexpr const char* voiceMode     = "voiceMode";     // Poly / Mono / Legato
    inline constexpr const char* glideTime     = "glideTime";     // 0..2 s
    inline constexpr const char* glideMode     = "glideMode";     // Always / Legato only
    inline constexpr const char* glideType     = "glideType";     // constant time / constant rate (time per octave)
    inline constexpr const char* notePriority  = "notePriority";  // Last / Low / High (mono)
    inline constexpr const char* unisonVoices  = "unisonVoices";  // 1..8
    inline constexpr const char* unisonDetune  = "unisonDetune";  // 0..1 = 0..100 ct total spread
    inline constexpr const char* unisonSpread  = "unisonSpread";  // stereo width 0..1
    inline constexpr const char* bendRange     = "bendRange";     // 1..24 st
    inline constexpr const char* modVibrato    = "modVibrato";    // mod wheel -> LFO 1 pitch depth
    inline constexpr const char* velToCutoff   = "velToCutoff";   // velocity -> cutoff amount

    // M8: section on/off switches. Appended (CashCode v4); default on, so older
    // codes, projects, stashes and vault notes sound the same.
    inline constexpr const char* osc1On      = "osc1On";
    inline constexpr const char* osc2On      = "osc2On";
    inline constexpr const char* subNoiseOn  = "subNoiseOn";
    inline constexpr const char* filterOn    = "filterOn";
    inline constexpr const char* filterEnvOn = "filterEnvOn"; // filter env incl. pitch env
    inline constexpr const char* driveOn     = "driveOn";
    inline constexpr const char* lfo1On      = "lfo1On";
    inline constexpr const char* lfo2On      = "lfo2On";
    inline constexpr const char* unisonOn    = "unisonOn";
    inline constexpr const char* glideOn     = "glideOn";

    // M10: tempo sync for the LFO rates. Appended (CashCode v5); default TIME, so
    // older codes, projects, stashes and vault notes play unsynced (in Hz) as before.
    inline constexpr const char* lfo1Sync = "lfo1Sync"; // Time / Sync
    inline constexpr const char* lfo1Note = "lfo1Note"; // note value (cash::tempo::noteNames)
    inline constexpr const char* lfo2Sync = "lfo2Sync";
    inline constexpr const char* lfo2Note = "lfo2Note";

    // M11: stereo delay + reverb after the voices. Appended (CashCode v6); both
    // default OFF, so older codes, projects, stashes and vault notes sound as before.
    inline constexpr const char* delayOn       = "delayOn";
    inline constexpr const char* delayTime     = "delayTime";     // 1..2000 ms (TIME)
    inline constexpr const char* delaySync     = "delaySync";     // Time / Sync
    inline constexpr const char* delayNote     = "delayNote";     // note value (SYNC)
    inline constexpr const char* delayFeedback = "delayFeedback"; // 0..95 %
    inline constexpr const char* delayMix      = "delayMix";
    inline constexpr const char* delayPingPong = "delayPingPong"; // Stereo / Ping-Pong
    inline constexpr const char* delayTone     = "delayTone";     // -1 dark .. 0 flat .. +1 thin
    inline constexpr const char* reverbOn       = "reverbOn";
    inline constexpr const char* reverbSize     = "reverbSize";
    inline constexpr const char* reverbDecay    = "reverbDecay";    // RT60, 0.2..20 s
    inline constexpr const char* reverbDamp     = "reverbDamp";
    inline constexpr const char* reverbPreDelay = "reverbPreDelay"; // 0..250 ms (TIME)
    inline constexpr const char* reverbPreSync  = "reverbPreSync";  // Time / Sync
    inline constexpr const char* reverbPreNote  = "reverbPreNote";  // note value (SYNC)
    inline constexpr const char* reverbWidth    = "reverbWidth";
    inline constexpr const char* reverbMix      = "reverbMix";

    enum Wave { sine = 0, triangle, saw, square };
    enum FilterMode { lowPass = 0, bandPass, highPass };
    enum DriveType { softClip = 0, hardClip, fold };
    enum LfoTarget { toCutoff = 0, toPitch, toAmp, toDrive, toOsc2 };
    enum VoiceMode { poly = 0, mono, legato };
    enum GlideMode { glideAlways = 0, glideLegato };
    enum GlideType { constantTime = 0, constantRate };
    enum NotePriority { lastNote = 0, lowNote, highNote };
    enum RateMode { timeMode = 0, syncMode };
    enum DelayMode { stereoDelay = 0, pingPong };

    inline constexpr float pitchEnvRange = 24.0f;      // semitones at 100 %
    inline constexpr float unisonDetuneRange = 1.0f;   // semitones between the outer unison voices at 100 %
    inline constexpr float modVibratoRange = 1.0f;     // semitones of LFO 1 vibrato at full wheel, 100 %
    inline constexpr float velCutoffRange = 48.0f;     // semitones darker for the softest note at 100 %
    inline constexpr int maxUnison = 8;

    juce::StringArray waveNames();
    juce::StringArray filterNames();
    juce::StringArray driveNames();
    juce::StringArray lfoTargetNames();
    juce::StringArray voiceModeNames();
    juce::StringArray glideModeNames();
    juce::StringArray glideTypeNames();
    juce::StringArray priorityNames();
    juce::StringArray rateModeNames();
    juce::StringArray delayModeNames();

    // Ranges shared with the money moves (which work in normalised values).
    juce::NormalisableRange<float> delayFeedbackRange();
    juce::NormalisableRange<float> reverbDecayRange();

    juce::StringArray sectionSwitchIds(); // the M8 on/off params, in ParameterMap order

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
