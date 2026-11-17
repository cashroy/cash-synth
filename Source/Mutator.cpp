#include "Mutator.h"
#include "Parameters.h"

namespace cash
{
uint64_t Rng::next()
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float Rng::uniform()
{
    return static_cast<float> (next() >> 40) * (1.0f / 16777216.0f);
}

int Rng::below (int n)
{
    return n <= 1 ? 0 : static_cast<int> (next() % static_cast<uint64_t> (n));
}

float Rng::gaussian()
{
    const float sum = uniform() + uniform() + uniform() + uniform();
    return (sum - 2.0f) * 1.7320508f;
}

namespace
{
    size_t at (const char* id)
    {
        const int i = ParameterMap::indexOf (id);
        jassert (i >= 0);
        return static_cast<size_t> (i);
    }

    bool isFree (const LockMask& locks, int i) { return ParameterMap::spec (i).weight > 0.0f && ! locks.test (static_cast<size_t> (i)); }

    float moveStepped (const ParamSpec& s, float value, float spread, Rng& rng)
    {
        const int current = ParameterMap::toStep (s, value);
        int next = current;
        if (s.ordered)
        {
            const int reach = juce::jmax (1, juce::roundToInt (spread * static_cast<float> (s.steps - 1)));
            const int delta = 1 + rng.below (reach);
            next = juce::jlimit (0, s.steps - 1, current + (rng.chance (0.5f) ? delta : -delta));
            if (next == current) // hit an edge: go the other way
                next = juce::jlimit (0, s.steps - 1, current + (current == 0 ? delta : -delta));
        }
        else
        {
            next = (current + 1 + rng.below (s.steps - 1)) % s.steps; // any other category
        }
        return ParameterMap::fromStep (s, next);
    }

    // Ordered pitch params re-rolled in a jackpot stay near home: mostly unison/octaves.
    float rollStepped (const ParamSpec& s, Rng& rng)
    {
        if (! s.ordered)
            return ParameterMap::fromStep (s, rng.below (s.steps));
        const int centre = (s.steps - 1) / 2;
        const int spread = s.steps > 5 ? 7 : 1; // semis within a fifth, octaves within one
        return ParameterMap::fromStep (s, rng.chance (0.5f) ? centre : centre + rng.below (2 * spread + 1) - spread);
    }
}

Patch Mutator::invest (const Patch& base, const Investment& inv, const LockMask& locks)
{
    const float amount = juce::jlimit (0.0f, 1.0f, inv.amount);
    const float risk = juce::jlimit (0.0f, 1.0f, inv.risk);
    if (amount <= 0.0f)
        return base;

    Rng rng (inv.seed);
    Patch out = base;
    const float sigma = amount * (0.04f + 0.36f * risk);

    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        // Draw the same number of randoms per param regardless of locks, so
        // locking one knob doesn't reshuffle what happens to the others.
        const float touch = rng.uniform();
        const float a = rng.uniform();
        const float g = rng.gaussian();
        Rng local (rng.next());

        if (! isFree (locks, i))
            continue;

        const auto& s = ParameterMap::spec (i);
        auto& v = out[static_cast<size_t> (i)];
        if (touch >= s.weight * (0.15f + 0.85f * amount))
            continue;

        if (s.steps > 1)
        {
            if (a < risk * (0.25f + 0.75f * amount))
                v = moveStepped (s, v, sigma, local);
        }
        else if (a < 0.2f * risk * amount)
        {
            v = local.uniform(); // the wild bet
        }
        else
        {
            v = juce::jlimit (0.0f, 1.0f, v + g * sigma);
        }
    }

    makePlayable (out, locks);
    return out;
}

Patch Mutator::jackpot (const Patch& base, uint64_t seed, const LockMask& locks)
{
    Rng rng (seed);
    Patch out = base;
    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        Rng local (rng.next());
        if (! isFree (locks, i))
            continue;
        const auto& s = ParameterMap::spec (i);
        out[static_cast<size_t> (i)] = s.steps > 1 ? rollStepped (s, local) : local.uniform();
    }

    // Keep detune musical on a full re-roll.
    for (auto* id : { param::osc1Fine, param::osc2Fine })
    {
        const auto i = at (id);
        if (! locks.test (i))
            out[i] = 0.5f + (out[i] - 0.5f) * 0.2f;
    }

    // Lead settings stay mostly off on a re-roll: a jackpot is a new sound, not
    // sluggish glide or an 8-voice stack on every note. Same draws whatever is locked.
    {
        Rng lead (seed ^ 0x4C454144ull);
        const bool glideOn = lead.chance (0.25f);
        const float glide = lead.uniform() * 0.35f;      // normalised: up to ~90 ms
        const bool stack = lead.chance (0.3f);
        const int voices = 1 + lead.below (3);            // 2..4 voices
        const auto g = at (param::glideTime), u = at (param::unisonVoices);
        if (! locks.test (g))
            out[g] = glideOn ? glide : 0.0f;
        if (! locks.test (u))
            out[u] = ParameterMap::fromStep (ParameterMap::spec (static_cast<int> (u)), stack ? voices : 0);
    }

    // M8 section switches: effect sections are mostly on after a re-roll (the old
    // jackpot sound), sometimes off. Osc 1/2, filter and filter env are never touched
    // (weight 0). Own stream, same draws whatever is locked.
    {
        Rng sw (seed ^ 0x53574954ull);
        for (int i = 0; i < ParameterMap::numParams; ++i)
        {
            if (! isSectionSwitch (i))
                continue;
            const bool on = ! sw.chance (0.15f);
            if (isFree (locks, i))
                out[static_cast<size_t> (i)] = on ? 1.0f : 0.0f;
        }
    }

    // M10: synced LFO rates now and then (the note value was rolled above). Own stream.
    {
        Rng sy (seed ^ 0x53594E43ull);
        for (auto* id : { param::lfo1Sync, param::lfo2Sync })
        {
            const bool synced = sy.chance (0.15f);
            if (isFree (locks, static_cast<int> (at (id))))
                out[at (id)] = synced ? 1.0f : 0.0f;
        }
    }

    makePlayable (out, locks);
    return out;
}

void Mutator::makePlayable (Patch& p, const LockMask& locks)
{
    using namespace param;
    auto freeId = [&locks] (const char* id) { return ! locks.test (at (id)); };
    auto raiseTo = [&] (const char* id, float floor) { if (freeId (id)) p[at (id)] = juce::jmax (p[at (id)], floor); };
    auto capAt   = [&] (const char* id, float ceil)  { if (freeId (id)) p[at (id)] = juce::jmin (p[at (id)], ceil); };

    for (int i = 0; i < ParameterMap::numParams; ++i)
        p[static_cast<size_t> (i)] = ParameterMap::snap (ParameterMap::spec (i), p[static_cast<size_t> (i)]);

    // M8: never every oscillator switched off. Osc 1 or 2 must be on (the sub
    // only as a last resort, when both of their switches are locked off).
    auto isOn = [&p] (const char* id) { return p[at (id)] > 0.5f; };
    auto turnOn = [&] (const char* id) { if (freeId (id)) p[at (id)] = 1.0f; return isOn (id); };
    if (! isOn (osc1On) && ! isOn (osc2On))
        if (! turnOn (osc1On) && ! turnOn (osc2On))
            turnOn (subNoiseOn);

    // Something tonal must be audible (only switched-on sources count).
    const bool on1 = isOn (osc1On), on2 = isOn (osc2On), onSub = isOn (subNoiseOn);
    const float tonal = juce::jmax (on1 ? p[at (osc1Level)] : 0.0f, on2 ? p[at (osc2Level)] : 0.0f, onSub ? p[at (subLevel)] : 0.0f);
    if (tonal < 0.35f)
    {
        if (on1 && freeId (osc1Level))        raiseTo (osc1Level, 0.6f);
        else if (on2 && freeId (osc2Level))   raiseTo (osc2Level, 0.6f);
        else if (onSub && freeId (subLevel))  raiseTo (subLevel, 0.6f);
        else if (freeId (osc1Level) && turnOn (osc1On)) raiseTo (osc1Level, 0.6f);
    }
    capAt (noiseLevel, juce::jmax (0.5f, p[at (osc1Level)]));

    // Cutoff (normalised, skewed: 0.35 ~ 230 Hz, 0.8 ~ 7.6 kHz) must suit the filter type.
    const int mode = ParameterMap::toStep (ParameterMap::spec (static_cast<int> (at (filterType))), p[at (filterType)]);
    if (mode == lowPass)  raiseTo (filterCutoff, 0.4f);
    if (mode == bandPass) { raiseTo (filterCutoff, 0.35f); capAt (filterCutoff, 0.8f); }
    if (mode == highPass) capAt (filterCutoff, 0.7f);
    capAt (filterReso, 0.85f);

    // Amp envelope: notes speak within ~1.5 s and don't hang forever.
    capAt (ampAttack, 0.65f);
    capAt (ampRelease, 0.8f);
    if (p[at (ampSustain)] < 0.2f)
        raiseTo (ampDecay, 0.45f); // ~0.3 s

    capAt (lfoToPitch, 0.6f);

    // M6: pitch env within about +/- 14 st, LFO 2 on pitch stays a vibrato.
    raiseTo (pitchEnvAmt, 0.2f);
    capAt (pitchEnvAmt, 0.8f);
    if (ParameterMap::toStep (ParameterMap::spec (static_cast<int> (at (lfo2Target))), p[at (lfo2Target)]) == toPitch)
        capAt (lfo2Amount, 0.5f);
}

float Mutator::distance (const Patch& a, const Patch& b)
{
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        if (ParameterMap::spec (i).weight <= 0.0f)
            continue;
        sum += std::abs (a[static_cast<size_t> (i)] - b[static_cast<size_t> (i)]);
        ++n;
    }
    return n > 0 ? sum / static_cast<float> (n) : 0.0f;
}
}
