#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// M10: tempo sync. The host's tempo, time signature and song position, plus the
// note values a synced rate can take. The BPM is never typed in: it comes from
// the host's play head each block (120 BPM, 4/4, stopped when there is none).
namespace cash::tempo
{
inline constexpr double defaultBpm = 120.0;

struct Info
{
    double bpm = defaultBpm;
    int sigNum = 4, sigDen = 4;
    bool playing = false;
    bool hasPpq = false;
    double ppq = 0.0; // song position at the block start, in quarter notes

    double barQuarters() const { return static_cast<double> (sigNum) * 4.0 / static_cast<double> (sigDen); }
    bool locked() const { return playing && hasPpq; } // synced LFOs follow the song position
};

// Reads the play head (nullptr or missing fields fall back to the defaults).
Info fromPlayHead (juce::AudioPlayHead*);
// Same rules, for a position the host already handed over (tests use this).
Info fromPosition (const juce::Optional<juce::AudioPlayHead::PositionInfo>&);

// Note values, slowest first (as the knob turns: lengths always get shorter).
// Bars follow the time signature; 1/2 .. 1/32 are fixed note lengths.
// Dotted = x1.5, triplet = x2/3.
inline constexpr int numNotes = 24;
inline constexpr int defaultNote = 13; // 1/4

juce::StringArray noteNames();          // "4 bars D", "1 bar", "1/4", "1/8 T", ...
double noteQuarters (int note, const Info&); // length in quarter notes
double noteHz (int note, const Info&);       // cycles per second at the host tempo
int nearestNote (double hz, const Info&);    // the note value closest to a rate (in octaves)
}
