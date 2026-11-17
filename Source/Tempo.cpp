#include "Tempo.h"

namespace cash::tempo
{
namespace
{
    struct Note
    {
        const char* name;
        double length; // in bars when `bars`, else in quarter notes
        bool bars;
    };

    constexpr double dot = 1.5, trip = 2.0 / 3.0;

    // Slowest first. In 4/4 every step is shorter than the one before.
    const std::array<Note, numNotes> notes { {
        { "4 bars D", 4.0 * dot, true },  { "4 bars", 4.0, true },        { "2 bars D", 2.0 * dot, true },
        { "4 bars T", 4.0 * trip, true }, { "2 bars", 2.0, true },        { "1 bar D", 1.0 * dot, true },
        { "2 bars T", 2.0 * trip, true }, { "1 bar", 1.0, true },         { "1/2 D", 2.0 * dot, false },
        { "1 bar T", 1.0 * trip, true },  { "1/2", 2.0, false },          { "1/4 D", 1.0 * dot, false },
        { "1/2 T", 2.0 * trip, false },   { "1/4", 1.0, false },          { "1/8 D", 0.5 * dot, false },
        { "1/4 T", 1.0 * trip, false },   { "1/8", 0.5, false },          { "1/16 D", 0.25 * dot, false },
        { "1/8 T", 0.5 * trip, false },   { "1/16", 0.25, false },        { "1/32 D", 0.125 * dot, false },
        { "1/16 T", 0.25 * trip, false }, { "1/32", 0.125, false },       { "1/32 T", 0.125 * trip, false },
    } };

    bool sane (double bpm) { return std::isfinite (bpm) && bpm >= 1.0 && bpm <= 1000.0; }
}

Info fromPosition (const juce::Optional<juce::AudioPlayHead::PositionInfo>& pos)
{
    Info t;
    if (! pos.hasValue())
        return t;

    if (const auto bpm = pos->getBpm(); bpm.hasValue() && sane (*bpm))
        t.bpm = *bpm;
    if (const auto sig = pos->getTimeSignature(); sig.hasValue() && sig->numerator >= 1 && sig->numerator <= 64
                                                  && sig->denominator >= 1 && sig->denominator <= 64)
    {
        t.sigNum = sig->numerator;
        t.sigDen = sig->denominator;
    }
    if (const auto ppq = pos->getPpqPosition(); ppq.hasValue() && std::isfinite (*ppq))
    {
        t.hasPpq = true;
        t.ppq = *ppq;
    }
    t.playing = pos->getIsPlaying();
    return t;
}

Info fromPlayHead (juce::AudioPlayHead* head)
{
    return head != nullptr ? fromPosition (head->getPosition()) : Info {};
}

juce::StringArray noteNames()
{
    juce::StringArray names;
    for (const auto& n : notes)
        names.add (n.name);
    return names;
}

double noteQuarters (int note, const Info& t)
{
    const auto& n = notes[static_cast<size_t> (juce::jlimit (0, numNotes - 1, note))];
    return n.bars ? n.length * t.barQuarters() : n.length;
}

double noteHz (int note, const Info& t)
{
    return t.bpm / 60.0 / noteQuarters (note, t);
}

int nearestNote (double hz, const Info& t)
{
    int best = defaultNote;
    double bestDist = 1.0e9;
    for (int i = 0; i < numNotes; ++i)
    {
        const double d = std::abs (std::log2 (noteHz (i, t) / juce::jmax (1.0e-6, hz)));
        if (d < bestDist - 1.0e-9)
        {
            best = i;
            bestDist = d;
        }
    }
    return best;
}
}
