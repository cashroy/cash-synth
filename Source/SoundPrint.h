#pragma once

#include <array>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// Cash Match's "fingerprint" of a sound: level envelope, spectrum over time,
// harmonic balance and noisiness. Level-independent (everything is relative to
// the sound's own peak), so a quiet sample can still match a loud patch.
//
// M9 adds a frame-by-frame track (~10 ms frames): level, pitch and brightness
// over the whole sound, the wobbles in them (vibrato, tremolo, wah) and the
// stereo width. Each "part" of a match is scored on its own.
namespace cash
{
// A periodic wobble found in a curve: rate in Hz, peak depth in the curve's unit.
struct Wobble
{
    float rate = 0.0f, depth = 0.0f;
    bool present() const { return depth > 0.0f; }
};

struct SoundPrint
{
    static constexpr double sampleRate = 44100.0; // everything is analysed at this rate
    static constexpr int envFrames = 48, numBands = 28, numSegments = 6, numHarmonics = 12;
    static constexpr int maxFrames = 400;          // frame track: hop grows past 4 s so it stays cheap

    bool valid = false;
    double seconds = 0.0;      // analysed length
    double holdSeconds = 0.0;  // estimated key-down time (before the release tail)
    bool pitched = false;
    float f0 = 0.0f;           // Hz (detected, or the note we rendered)
    int midiNote = 60;

    std::array<float, envFrames> envelope {};                              // 0 = -60 dB .. 1 = peak
    std::array<std::array<float, numBands>, numSegments> spectrum {};      // per segment, 0..1 rel. loudest band
    std::array<float, numSegments> segmentLevel {};                        // envelope at each segment (0..1)
    std::array<float, numHarmonics> harmonics {};                          // 0..1 rel. loudest harmonic
    float flatness = 0.0f;   // 0 = pure tone .. 1 = white noise
    float centroidHz = 0.0f; // spectral centre of mass

    // ---- Frame track (M9)
    double hopSeconds = 0.01;
    int regionStart = 0, regionEnd = 0;  // the steady part (after the attack, before the release) wobbles are looked for in
    std::vector<float> level;            // per frame, 0 = -60 dB .. 1 = peak
    std::vector<float> pitch;            // per frame, MIDI note (fractional); 0 = no clear pitch
    std::vector<float> brightness;       // per frame, log2 of the spectral centroid in Hz
    Wobble vibrato;                      // pitch, semitones
    Wobble tremolo;                      // level, dB
    Wobble wah;                          // brightness, octaves
    float width = 0.0f;                  // side / mid level: 0 = mono .. 1 = very wide
    float pitchStart = 0.0f;             // semitones the attack starts above (+) / below the settled pitch
    double pitchSettleSeconds = 0.0;     // time to get there
    float brightStart = 0.0f, brightSettled = 0.0f; // log2 centroid at the attack / in the steady part
    double brightSettleSeconds = 0.0;
};

namespace analysis
{
    // Parts of a match, each scored on its own (0 = identical).
    enum Part { envelopePart = 0, tonePart, pitchPart, brightnessPart, movementPart, widthPart, numParts };
    using Parts = std::array<float, numParts>;
    const char* partName (int part);                          // "ENVELOPE", "TONE", ...
    bool partApplies (const SoundPrint& target, int part);    // pitch needs a pitched target

    // Audio must be at SoundPrint::sampleRate and start at the sound's onset.
    // knownF0 > 0 skips pitch detection for f0/midiNote (candidates rendered at a known note).
    // `like` (a target's print) makes a candidate use the same frames and steady region.
    SoundPrint analyse (const float* left, const float* right, int numSamples, float knownF0 = 0.0f, const SoundPrint* like = nullptr);
    SoundPrint analyse (const float* mono, int numSamples, float knownF0 = 0.0f);

    // Weighted difference, 0 = identical. Weights come from the target.
    float distance (const SoundPrint& target, const SoundPrint& candidate);
    Parts distanceParts (const SoundPrint& target, const SoundPrint& candidate);
    float combine (const SoundPrint& target, const Parts&); // parts -> overall distance
    int matchPercent (float distance); // 0..100

    float detectPitch (const float* mono, int numSamples, double sampleRate); // Hz, 0 if unpitched
    Wobble findWobble (const std::vector<float>& curve, double hopSeconds, float minDepth); // periodic part of a curve
    juce::String noteName (int midiNote); // "A3"
    float noteToHz (int midiNote);
}
}
