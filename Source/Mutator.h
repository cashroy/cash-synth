#pragma once

#include <bitset>
#include <cstdint>
#include "ParameterMap.h"

namespace cash
{
// Small seeded RNG (splitmix64) with hand-rolled distributions, so the same
// seed gives the same patch on every platform and compiler (std:: distributions don't).
class Rng
{
public:
    explicit Rng (uint64_t seed) : state (seed) {}

    uint64_t next();
    float uniform();           // [0, 1)
    int below (int n);         // [0, n)
    float gaussian();          // mean 0, sd 1 (Irwin-Hall approximation, |x| <= 3.46)
    bool chance (float p) { return uniform() < p; }

private:
    uint64_t state;
};

// How much you put in, and how much you're willing to lose.
struct Investment
{
    float amount = 0.25f; // 0..1: how many parameters move, and how far. 0 = no change.
    float risk = 0.5f;    // 0..1: how wild the moves are (big jumps, wave/filter-type swaps)
    uint64_t seed = 1;
};

using LockMask = std::bitset<ParameterMap::numParams>; // set = never touched

class Mutator
{
public:
    // Returns a nudged copy of base. Same inputs, same result.
    static Patch invest (const Patch& base, const Investment&, const LockMask& locks = {});

    // Re-rolls every mutable, unlocked parameter from scratch (still playable).
    static Patch jackpot (const Patch& base, uint64_t seed, const LockMask& locks = {});

    // Safety rails so a mutation never produces silence or a useless sound:
    // something tonal is audible, cutoff is sane for the filter type, resonance
    // isn't screaming, the amp envelope isn't absurdly slow. Skips locked params.
    static void makePlayable (Patch&, const LockMask& locks = {});

    // Mean absolute difference over mutable params (0 = same, 1 = opposite).
    static float distance (const Patch& a, const Patch& b);
};
}
