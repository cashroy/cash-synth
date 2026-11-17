#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <set>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "ParameterMap.h"
#include "CashCode.h"
#include "Mutator.h"
#include "Ledger.h"

using cash::ParameterMap;
using cash::Patch;

namespace
{
struct Plugin
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;

    float real (const char* id)
    {
        auto* param = p.getState().getParameter (id);
        REQUIRE (param != nullptr);
        return param->convertFrom0to1 (param->getValue());
    }

    // Holds one note for `seconds` and returns the left channel.
    std::vector<float> playNote (double seconds, int note = 48)
    {
        constexpr double sr = 48000.0;
        constexpr int block = 512;
        p.prepareToPlay (sr, block);
        std::vector<float> out;
        juce::AudioBuffer<float> buffer (2, block);
        for (int pos = 0; pos < static_cast<int> (seconds * sr); pos += block)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
            p.processBlock (buffer, midi);
            for (int i = 0; i < block; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        p.releaseResources();
        return out;
    }
};

Patch randomPatch (juce::Random& rng)
{
    Patch p {};
    for (int i = 0; i < ParameterMap::numParams; ++i)
        p[static_cast<size_t> (i)] = ParameterMap::snap (ParameterMap::spec (i), rng.nextFloat());
    return p;
}

size_t idx (const char* id)
{
    const int i = ParameterMap::indexOf (id);
    REQUIRE (i >= 0);
    return static_cast<size_t> (i);
}

// v3 (M7). The v1 and v2 codes are checked for back-compat in MatchTests / LeadTests.
const juce::String goldenCode = "$74S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-0000-T310";
constexpr float continuousTolerance = 0.5f / 255.0f + 1.0e-6f;
}

// ------------------------------------------------------------ ParameterMap

TEST_CASE ("ParameterMap covers every APVTS parameter exactly once", "[map]")
{
    Plugin pl;
    const auto& params = pl.p.getParameters();
    REQUIRE (params.size() == ParameterMap::numParams);

    std::set<juce::String> ids;
    for (const auto& s : ParameterMap::specs())
    {
        INFO (s.id);
        CHECK (ids.insert (s.id).second);
        auto* param = pl.p.getState().getParameter (s.id);
        REQUIRE (param != nullptr);
        CHECK (s.weight >= 0.0f);
        CHECK (s.weight <= 1.0f);

        if (s.steps > 1)
            CHECK (param->getNumSteps() == s.steps);
        else
            CHECK (dynamic_cast<juce::AudioParameterFloat*> (param) != nullptr);
    }

    for (auto* p : params)
        CHECK (ParameterMap::indexOf (dynamic_cast<juce::RangedAudioParameter*> (p)->getParameterID()) >= 0);
    CHECK (ParameterMap::indexOf ("notAParam") == -1);
}

TEST_CASE ("Performance settings are never mutated", "[map]")
{
    CHECK (ParameterMap::spec (static_cast<int> (idx (cash::param::masterGain))).weight == 0.0f);
    CHECK (ParameterMap::spec (static_cast<int> (idx (cash::param::velocitySens))).weight == 0.0f);
}

TEST_CASE ("Patches round-trip through the APVTS", "[map]")
{
    Plugin pl;
    juce::Random rng (7);
    for (int round = 0; round < 20; ++round)
    {
        const auto patch = randomPatch (rng);
        cash::applyPatch (pl.p.getState(), patch);
        const auto back = pl.p.getPatch();
        for (int i = 0; i < ParameterMap::numParams; ++i)
            // Skewed ranges lose float precision right at the bottom (0.0025 of a 1 ms..10 s
            // knob is ~1 ms either way), so allow a little more there.
            if (std::abs (back[static_cast<size_t> (i)] - patch[static_cast<size_t> (i)]) > (patch[static_cast<size_t> (i)] < 0.01f ? 3.0e-3f : 1.0e-4f))
                FAIL_CHECK (ParameterMap::spec (i).id << ": set " << patch[static_cast<size_t> (i)] << ", got " << back[static_cast<size_t> (i)]);
    }

    // Stepped values land on real steps.
    cash::applyPatch (pl.p.getState(), cash::defaultPatch (pl.p.getState()));
    CHECK (pl.real (cash::param::osc1Wave) == static_cast<float> (cash::param::saw));
    CHECK (pl.real (cash::param::osc2Octave) == 0.0f);
    CHECK (cash::samePatch (pl.p.getPatch(), cash::defaultPatch (pl.p.getState())));
}

// ---------------------------------------------------------------- CashCode

TEST_CASE ("CashCode round-trips patches", "[cashcode]")
{
    Plugin pl;
    juce::Random rng (99);
    std::vector<Patch> patches { cash::defaultPatch (pl.p.getState()) };
    patches.push_back ({});                      // all zero
    patches.push_back ({});
    patches.back().fill (1.0f);                  // all max
    for (int i = 0; i < 200; ++i)
        patches.push_back (randomPatch (rng));

    for (const auto& patch : patches)
    {
        const auto code = cash::CashCode::encode (patch);
        INFO (code);
        const auto back = cash::CashCode::decode (code);
        REQUIRE (back.has_value());

        for (int i = 0; i < ParameterMap::numParams; ++i)
        {
            const auto k = static_cast<size_t> (i);
            if (ParameterMap::spec (i).steps > 1)
                CHECK ((*back)[k] == patch[k]);
            else
                CHECK (std::abs ((*back)[k] - patch[k]) <= continuousTolerance);
        }
        CHECK (cash::CashCode::encode (*back) == code); // stable once quantised
    }
}

TEST_CASE ("CashCode format is pinned", "[cashcode]")
{
    // If this fails you changed the bit layout: bump CashCode::version instead,
    // or every shared code out there breaks.
    Plugin pl;
    const auto code = cash::CashCode::encode (cash::defaultPatch (pl.p.getState()));
    CHECK (code.startsWithChar ('$'));
    CHECK (code.removeCharacters ("$-").length() == 68);
    CHECK (code == goldenCode);
}

TEST_CASE ("CashCode tolerates hand typing", "[cashcode]")
{
    Plugin pl;
    const auto patch = cash::defaultPatch (pl.p.getState());
    const auto code = cash::CashCode::encode (patch);
    const auto bare = code.removeCharacters ("$-");

    CHECK (cash::CashCode::decode (bare).has_value());
    CHECK (cash::CashCode::decode (code.toLowerCase()).has_value());
    CHECK (cash::CashCode::decode ("  " + code.replace ("-", " ") + "\n").has_value());

    // Crockford look-alikes: O reads as 0, I and L read as 1.
    const auto lookAlike = bare.replaceCharacter ('0', 'O').replaceCharacter ('1', 'l');
    CHECK (cash::CashCode::decode (lookAlike) == cash::CashCode::decode (code));
}

TEST_CASE ("CashCode rejects damaged codes", "[cashcode]")
{
    Plugin pl;
    const auto bare = cash::CashCode::encode (cash::defaultPatch (pl.p.getState())).removeCharacters ("$-");

    CHECK_FALSE (cash::CashCode::decode ("").has_value());
    CHECK_FALSE (cash::CashCode::decode ("$").has_value());
    CHECK_FALSE (cash::CashCode::decode ("hello").has_value());
    CHECK_FALSE (cash::CashCode::decode (bare.dropLastCharacters (1)).has_value());
    CHECK_FALSE (cash::CashCode::decode (bare + "0").has_value());
    CHECK_FALSE (cash::CashCode::decode (bare.replaceSection (5, 1, "U")).has_value()); // U isn't Crockford
    CHECK_FALSE (cash::CashCode::decode ("$$" + bare).has_value());

    // Every single-character typo is caught (CRC-16 catches all bursts <= 16 bits).
    const juce::String alphabet ("0123456789ABCDEFGHJKMNPQRSTVWXYZ");
    int caught = 0, tried = 0;
    for (int pos = 0; pos < bare.length(); ++pos)
        for (int a = 0; a < alphabet.length(); ++a)
        {
            if (alphabet[a] == bare[pos])
                continue;
            ++tried;
            caught += cash::CashCode::decode (bare.replaceSection (pos, 1, juce::String::charToString (alphabet[a]))).has_value() ? 0 : 1;
        }
    CHECK (caught == tried);

    // Wrong version: first 4 bits are the version, so change the first char's top bits.
    const auto v0 = bare.replaceSection (0, 1, "0");
    CHECK_FALSE (cash::CashCode::decode (v0).has_value());
}

TEST_CASE ("Serial numbers look like banknote serials", "[cashcode]")
{
    Plugin pl;
    juce::Random rng (3);
    std::set<juce::String> seen;
    for (int i = 0; i < 100; ++i)
    {
        const auto patch = randomPatch (rng);
        const auto serial = cash::CashCode::serialNumber (patch);
        INFO (serial);
        REQUIRE (serial.length() == 13);
        CHECK (juce::String ("ABCDEFGHJKLMNPQRSTUVWXYZ").containsChar (serial[0]));
        CHECK (juce::String ("ABCDEFGHJKLMNPQRSTUVWXYZ").containsChar (serial[1]));
        CHECK (serial[2] == ' ');
        CHECK (serial.substring (3, 11).containsOnly ("0123456789"));
        CHECK (serial[11] == ' ');
        CHECK (cash::CashCode::serialNumber (patch) == serial);
        seen.insert (serial);
    }
    CHECK (seen.size() == 100);
}

// ----------------------------------------------------------------- Mutator

TEST_CASE ("Rng is deterministic and well spread", "[mutator]")
{
    cash::Rng a (42), b (42), c (43);
    CHECK (a.next() == b.next());
    CHECK (a.next() != c.next());

    cash::Rng r (1);
    double sum = 0.0, sumSq = 0.0;
    constexpr int n = 20000;
    for (int i = 0; i < n; ++i)
    {
        const float u = r.uniform();
        REQUIRE (u >= 0.0f);
        REQUIRE (u < 1.0f);
        const int k = r.below (7);
        REQUIRE (k >= 0);
        REQUIRE (k < 7);
        const double g = r.gaussian();
        sum += g;
        sumSq += g * g;
    }
    CHECK (sum / n == Catch::Approx (0.0).margin (0.03));
    CHECK (std::sqrt (sumSq / n) == Catch::Approx (1.0).margin (0.03));
}

TEST_CASE ("Investing is deterministic and scales with amount", "[mutator]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());

    CHECK (cash::Mutator::invest (base, { 0.5f, 0.5f, 11 }) == cash::Mutator::invest (base, { 0.5f, 0.5f, 11 }));
    CHECK (cash::Mutator::invest (base, { 0.5f, 0.5f, 11 }) != cash::Mutator::invest (base, { 0.5f, 0.5f, 12 }));
    CHECK (cash::Mutator::invest (base, { 0.0f, 1.0f, 11 }) == base);

    auto meanDistance = [&] (float amount, float risk)
    {
        float total = 0.0f;
        for (uint64_t seed = 1; seed <= 200; ++seed)
            total += cash::Mutator::distance (base, cash::Mutator::invest (base, { amount, risk, seed }));
        return total / 200.0f;
    };

    const float small = meanDistance (0.1f, 0.5f);
    const float medium = meanDistance (0.5f, 0.5f);
    const float big = meanDistance (1.0f, 0.5f);
    CHECK (small > 0.0f);
    CHECK (small < medium);
    CHECK (medium < big);
    CHECK (meanDistance (0.5f, 0.0f) < meanDistance (0.5f, 1.0f));
}

TEST_CASE ("Risk decides whether waves and filter types change", "[mutator]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());
    const auto wave = idx (cash::param::osc1Wave);

    int safeSwaps = 0, riskySwaps = 0;
    for (uint64_t seed = 1; seed <= 300; ++seed)
    {
        safeSwaps += cash::Mutator::invest (base, { 1.0f, 0.0f, seed })[wave] != base[wave] ? 1 : 0;
        riskySwaps += cash::Mutator::invest (base, { 1.0f, 1.0f, seed })[wave] != base[wave] ? 1 : 0;
    }
    CHECK (safeSwaps == 0);
    CHECK (riskySwaps > 150);
}

TEST_CASE ("Locked and performance parameters never move", "[mutator]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());
    cash::LockMask locks;
    locks.set (idx (cash::param::filterCutoff));
    locks.set (idx (cash::param::osc2Wave));

    for (uint64_t seed = 1; seed <= 100; ++seed)
    {
        for (const auto& out : { cash::Mutator::invest (base, { 1.0f, 1.0f, seed }, locks), cash::Mutator::jackpot (base, seed, locks) })
        {
            CHECK (out[idx (cash::param::filterCutoff)] == base[idx (cash::param::filterCutoff)]);
            CHECK (out[idx (cash::param::osc2Wave)] == base[idx (cash::param::osc2Wave)]);
            CHECK (out[idx (cash::param::masterGain)] == base[idx (cash::param::masterGain)]);
            CHECK (out[idx (cash::param::velocitySens)] == base[idx (cash::param::velocitySens)]);
        }
    }
}

TEST_CASE ("Locking one knob doesn't reshuffle the others", "[mutator]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());
    const auto lfoRate = idx (cash::param::lfoRate);
    cash::LockMask locks;
    locks.set (lfoRate);

    for (uint64_t seed = 1; seed <= 50; ++seed)
    {
        const auto free = cash::Mutator::invest (base, { 0.7f, 0.7f, seed });
        const auto locked = cash::Mutator::invest (base, { 0.7f, 0.7f, seed }, locks);
        for (size_t i = 0; i < base.size(); ++i)
            if (i != lfoRate)
                CHECK (free[i] == locked[i]);
    }
}

TEST_CASE ("Jackpot re-rolls the patch", "[mutator]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());
    float total = 0.0f;
    for (uint64_t seed = 1; seed <= 100; ++seed)
    {
        const auto out = cash::Mutator::jackpot (base, seed);
        CHECK (out == cash::Mutator::jackpot (base, seed));
        total += cash::Mutator::distance (base, out);
    }
    CHECK (total / 100.0f > 0.2f);
}

TEST_CASE ("Mutations stay playable in real units", "[mutator]")
{
    using namespace cash::param;
    Plugin pl;
    juce::Random rng (5);

    for (uint64_t seed = 1; seed <= 150; ++seed)
    {
        const auto start = randomPatch (rng);
        const auto out = seed % 2 == 0 ? cash::Mutator::jackpot (start, seed) : cash::Mutator::invest (start, { 1.0f, 1.0f, seed });
        cash::applyPatch (pl.p.getState(), out);
        INFO ("seed " << seed << " " << cash::CashCode::encode (out));

        CHECK (juce::jmax (pl.real (osc1Level), pl.real (osc2Level), pl.real (subLevel)) >= 0.35f);
        const int mode = juce::roundToInt (pl.real (filterType));
        const float cutoff = pl.real (filterCutoff);
        if (mode == lowPass)  CHECK (cutoff >= 300.0f);
        if (mode == bandPass) CHECK ((cutoff >= 200.0f && cutoff <= 8000.0f));
        if (mode == highPass) CHECK (cutoff <= 4500.0f);
        CHECK (pl.real (filterReso) <= 0.85f + 1.0e-4f);
        CHECK (pl.real (ampAttack) <= 1.6f);
        CHECK (pl.real (ampRelease) <= 4.0f);
    }
}

TEST_CASE ("Every jackpot actually makes sound", "[mutator][audio]")
{
    Plugin pl;
    const auto base = cash::defaultPatch (pl.p.getState());
    for (uint64_t seed = 1; seed <= 30; ++seed)
    {
        const auto out = seed % 2 == 0 ? cash::Mutator::jackpot (base, seed) : cash::Mutator::invest (base, { 1.0f, 1.0f, seed });
        cash::applyPatch (pl.p.getState(), out);
        const auto audio = pl.playNote (1.5);
        INFO ("seed " << seed << " " << cash::CashCode::encode (out));

        float peak = 0.0f;
        for (auto s : audio)
        {
            REQUIRE (std::isfinite (s));
            peak = juce::jmax (peak, std::abs (s));
        }
        CHECK (peak > 0.01f);
        CHECK (peak < 8.0f);
    }
}

// ------------------------------------------------------------------ Ledger

TEST_CASE ("Ledger undo and redo", "[ledger]")
{
    Patch a {}, b {}, c {};
    b.fill (0.5f);
    c.fill (1.0f);

    cash::Ledger ledger;
    CHECK_FALSE (ledger.canUndo());
    CHECK_FALSE (ledger.undo().has_value());
    CHECK (ledger.current() == nullptr);

    CHECK (ledger.record (a, "a", 1));
    CHECK_FALSE (ledger.record (a, "a again", 2)); // no-op transactions are dropped
    CHECK (ledger.record (b, "b", 3));
    CHECK (ledger.record (c, "c", 4));
    CHECK (ledger.size() == 3);
    CHECK (ledger.current()->memo == "c");

    CHECK (ledger.undo() == b);
    CHECK (ledger.undo() == a);
    CHECK_FALSE (ledger.undo().has_value());
    CHECK (ledger.redo() == b);
    CHECK (ledger.canRedo());

    // A new transaction after undo drops the redo tail.
    Patch d {};
    d.fill (0.25f);
    CHECK (ledger.record (d, "d", 5));
    CHECK_FALSE (ledger.canRedo());
    CHECK (ledger.size() == 3);
    CHECK (ledger.undo() == b);

    ledger.clear();
    CHECK (ledger.size() == 0);
    CHECK_FALSE (ledger.canRedo());
}

TEST_CASE ("Ledger forgets the oldest entries past capacity", "[ledger]")
{
    cash::Ledger ledger (5);
    for (int i = 0; i < 12; ++i)
    {
        Patch p {};
        p.fill (static_cast<float> (i) / 20.0f);
        ledger.record (p, juce::String (i));
    }
    CHECK (ledger.size() == 5);
    CHECK (ledger.at (0).memo == "7");
    CHECK (ledger.current()->memo == "11");

    int undos = 0;
    while (ledger.undo())
        ++undos;
    CHECK (undos == 4);
}

TEST_CASE ("Processor investments are undoable", "[ledger][processor]")
{
    Plugin pl;
    const auto opening = pl.p.getPatch();

    pl.p.invest ({ 0.6f, 0.6f, 21 });
    const auto invested = pl.p.getPatch();
    CHECK_FALSE (cash::samePatch (invested, opening));
    REQUIRE (pl.p.getLedger().size() == 2);
    CHECK (pl.p.getLedger().at (0).memo == "Opening balance");
    CHECK (pl.p.getLedger().at (1).memo == "Invested 60% @ 60% risk");

    // A hand tweak becomes its own transaction before the next move.
    pl.p.getState().getParameter (cash::param::filterReso)->setValueNotifyingHost (0.9f);
    const auto tweaked = pl.p.getPatch();
    pl.p.hitJackpot (77);
    REQUIRE (pl.p.getLedger().size() == 4);
    CHECK (pl.p.getLedger().at (2).memo == "Hand tweak");
    CHECK (pl.p.getLedger().at (3).memo == "Jackpot!");

    CHECK (pl.p.undo());
    CHECK (cash::samePatch (pl.p.getPatch(), tweaked));
    CHECK (pl.p.undo());
    CHECK (cash::samePatch (pl.p.getPatch(), invested));
    CHECK (pl.p.undo());
    CHECK (cash::samePatch (pl.p.getPatch(), opening));
    CHECK_FALSE (pl.p.undo());

    CHECK (pl.p.redo());
    CHECK (cash::samePatch (pl.p.getPatch(), invested));

    // Tweaking after an undo: the tweak wins and the redo tail is gone.
    pl.p.getState().getParameter (cash::param::lfoRate)->setValueNotifyingHost (0.1f);
    CHECK_FALSE (pl.p.redo());
    CHECK (pl.p.getLedger().current()->memo == "Hand tweak");
    CHECK (pl.p.undo());
    CHECK (cash::samePatch (pl.p.getPatch(), invested));
}
