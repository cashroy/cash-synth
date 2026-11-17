#pragma once

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>

// Cash Match's "fingerprint" of a sound: level envelope, spectrum over time,
// harmonic balance and noisiness. Level-independent (everything is relative to
// the sound's own peak), so a quiet sample can still match a loud patch.
namespace cash
{
struct SoundPrint
{
    static constexpr double sampleRate = 44100.0; // everything is analysed at this rate
    static constexpr int envFrames = 48, numBands = 28, numSegments = 6, numHarmonics = 12;

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
};

namespace analysis
{
    // mono must be at SoundPrint::sampleRate and start at the sound's onset.
    // knownF0 > 0 skips pitch detection (candidates rendered at a known note).
    SoundPrint analyse (const float* mono, int numSamples, float knownF0 = 0.0f);

    // Weighted difference, 0 = identical. Weights come from the target.
    float distance (const SoundPrint& target, const SoundPrint& candidate);
    int matchPercent (float distance); // 0..100

    struct Parts { float envelope, spectrum, harmonics, flatness; }; // each 0 = identical
    Parts distanceParts (const SoundPrint& target, const SoundPrint& candidate);

    float detectPitch (const float* mono, int numSamples, double sampleRate); // Hz, 0 if unpitched
    juce::String noteName (int midiNote); // "A3"
    float noteToHz (int midiNote);
}
}
