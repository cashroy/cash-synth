// The Mint (M13): Cash Match measures a sample into sine partials + noise and the
// synth makes the sound from them (on any key, held as long as you like).
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "CashMatch.h"
#include "SoundPrint.h"
#include "Mint.h"

namespace
{
constexpr double sr = cash::SoundPrint::sampleRate;
using namespace cash::analysis;

struct Plugin
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
};

size_t idx (const char* id)
{
    const int i = cash::ParameterMap::indexOf (id);
    REQUIRE (i >= 0);
    return static_cast<size_t> (i);
}

// A breathy flute-ish note: 10 harmonics with vibrato, band noise, attack, held
// to 1 s, then a 0.3 s release. The right side is a hair late (a stereo image).
juce::AudioBuffer<float> fluteish (double f0 = 330.0, double hold = 1.0, double total = 1.4)
{
    const int n = static_cast<int> (total * sr);
    juce::AudioBuffer<float> out (2, n);
    juce::Random rng (99);
    double phase = 0.0;
    float lp = 0.0f, bp = 0.0f;
    std::vector<float> mono (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = t < 0.06 ? t / 0.06 : (t < hold ? 1.0 : std::exp (-(t - hold) / 0.08));
        const double vib = t > 0.25 ? 0.3 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t) : 0.0;
        phase += juce::MathConstants<double>::twoPi * f0 * std::exp2 (vib / 12.0) / sr;
        double s = 0.0;
        for (int k = 1; k <= 10; ++k)
            s += std::sin (k * phase) / (k * k * 0.7 + 0.3);
        // Breath: noise through a crude band-pass around 2-3 kHz.
        const float w = rng.nextFloat() * 2.0f - 1.0f;
        lp += 0.35f * (w - lp);
        bp += 0.5f * (lp - bp);
        const float breath = (lp - bp) * 0.25f;
        mono[static_cast<size_t> (i)] = static_cast<float> (0.3 * env * s) + static_cast<float> (env) * breath;
    }
    for (int i = 0; i < n; ++i)
    {
        out.setSample (0, i, mono[static_cast<size_t> (i)]);
        out.setSample (1, i, i >= 9 ? mono[static_cast<size_t> (i - 9)] * 0.9f : 0.0f);
    }
    return out;
}

juce::AudioBuffer<float> playModel (const cash::MintModel& m, double hold, double total, float semitones = 0.0f)
{
    cash::MintPlayer player;
    player.prepare (sr);
    player.start (&m, 1);
    const int n = static_cast<int> (total * sr), h = static_cast<int> (hold * sr);
    juce::AudioBuffer<float> out (2, n);
    out.clear();
    cash::MintPlayer::Settings s;
    s.semitones = semitones;
    for (int pos = 0; pos < n; pos += cash::MintPlayer::blockSize)
    {
        const int num = juce::jmin (cash::MintPlayer::blockSize, n - pos);
        if (h >= pos && h < pos + num)
            player.release();
        player.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), num, s);
    }
    return out;
}

int scoreAgainst (const cash::SoundPrint& target, const juce::AudioBuffer<float>& audio, Parts* partsOut = nullptr)
{
    const auto print = analyse (audio.getReadPointer (0), audio.getReadPointer (1), audio.getNumSamples(), target.f0, &target);
    const auto parts = distanceParts (target, print);
    if (partsOut != nullptr)
        *partsOut = parts;
    return matchPercent (combine (target, parts));
}

juce::String describe (const Parts& p)
{
    juce::String s;
    for (int k = 0; k < numParts; ++k)
        s << partName (k) << " " << matchPercent (p[static_cast<size_t> (k)]) << "%  ";
    return s;
}

// Plays one note through the processor (the real engine) and returns the stereo output.
juce::AudioBuffer<float> playNote (CashSynthAudioProcessor& p, int note, double hold, double total, double rate = 44100.0)
{
    constexpr int block = 256;
    p.prepareToPlay (rate, block);
    const int n = static_cast<int> (total * rate), h = static_cast<int> (hold * rate);
    juce::AudioBuffer<float> out (2, n), chunk (2, block);
    for (int pos = 0; pos < n; pos += block)
    {
        const int num = juce::jmin (block, n - pos);
        chunk.setSize (2, num, false, false, true);
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (h >= pos && h < pos + num)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), h - pos);
        p.processBlock (chunk, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, chunk, ch, 0, num);
    }
    return out;
}

float rms (const juce::AudioBuffer<float>& b, double from, double to, double rate = sr)
{
    const int a = static_cast<int> (from * rate), e = juce::jmin (b.getNumSamples(), static_cast<int> (to * rate));
    double s = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = a; i < e; ++i)
            s += b.getSample (ch, i) * b.getSample (ch, i);
    return static_cast<float> (std::sqrt (s / juce::jmax (1, (e - a) * b.getNumChannels())));
}

// Runs a Mint match on `audio` and deposits it.
cash::CashMatcher::Result matchAndDeposit (CashSynthAudioProcessor& p, const juce::AudioBuffer<float>& audio, const juce::String& name)
{
    auto& m = p.getMatcher();
    REQUIRE (m.setTarget (audio, sr, name).isEmpty());
    m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 3);
    auto r = m.getResult();
    REQUIRE (r.found);
    REQUIRE (p.depositMatch());
    return r;
}
}

TEST_CASE ("The Mint rebuilds a breathy, vibrato note from partials and noise", "[mint]")
{
    Plugin pl;
    auto& m = pl.p.getMatcher();
    const auto audio = fluteish();
    REQUIRE (m.setTarget (audio, sr, "Flute").isEmpty());
    const auto model = m.getMintModel();
    REQUIRE (model != nullptr);
    CHECK (model->pitched);
    CHECK (std::abs (model->rootHz - 330.0f) < 2.0f);
    CHECK (model->numPartials >= 10);
    CHECK (model->numBands() > 20);
    CHECK (model->hasLoop()); // a held, steady note

    // Played back at its own pitch, for as long as the sample, it is the sample.
    const auto& tp = m.getTargetPrint();
    Parts parts;
    const int pct = scoreAgainst (tp, playModel (*model, tp.holdSeconds, tp.seconds), &parts);
    INFO (describe (parts));
    CHECK (pct >= 97);
    for (int k = 0; k < numParts; ++k)
        if (partApplies (tp, k))
            CHECK (matchPercent (parts[static_cast<size_t> (k)]) >= 85);
}

TEST_CASE ("Cash Match mints a patch: Mint on, every synth section off", "[mint][match]")
{
    Plugin pl;
    using namespace cash::param;
    const auto r = matchAndDeposit (pl.p, fluteish(), "Flute");
    INFO (describe (r.parts));
    CHECK (r.percent >= 95);
    REQUIRE (r.mint != nullptr);
    CHECK (r.patch[idx (mintOn)] == 1.0f);
    for (auto* id : { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, lfo2On, unisonOn, glideOn })
        CHECK (r.patch[idx (id)] == 0.0f);
    CHECK (r.patch[idx (delayOn)] == 0.0f);
    CHECK (r.patch[idx (reverbOn)] == 0.0f);

    // Deposited: the processor plays that model, and the ledger says so.
    CHECK (pl.p.getMint() == r.mint);
    CHECK (pl.p.getLedger().current()->memo.startsWith ("Cash Match: Flute"));
    CHECK (pl.p.getLedger().current()->mint == r.mint);

    // The knob-only search (M9) is still there, and never switches the Mint on.
    auto& m = pl.p.getMatcher();
    m.runBlocking (pl.p.getPatch(), cash::defaultPatch (pl.p.getState()), 3, { 6, 1, 6, 0, false });
    const auto knobs = m.getResult();
    CHECK (knobs.mint == nullptr);
    CHECK (knobs.patch[idx (mintOn)] == 0.0f);
}

TEST_CASE ("A minted sound plays on any key, held or let go early", "[mint]")
{
    Plugin pl;
    const auto r = matchAndDeposit (pl.p, fluteish(), "Flute");
    const int root = pl.p.getMatcher().getTargetPrint().midiNote;

    SECTION ("an octave up is twice the pitch, a fifth down is 2/3")
    {
        for (auto [offset, ratio] : { std::pair { 12, 2.0 }, { -7, std::exp2 (-7.0 / 12.0) }, { 0, 1.0 } })
        {
            const auto out = playNote (pl.p, root + offset, 0.8, 1.0);
            const int from = static_cast<int> (0.3 * sr);
            const float hz = detectPitch (out.getReadPointer (0, from), static_cast<int> (0.4 * sr), sr);
            INFO ("offset " << offset << ": " << hz << " Hz");
            CHECK (std::abs (hz / (330.0 * ratio) - 1.0) < 0.01);
        }
    }

    SECTION ("a note held 3x longer than the sample keeps sounding, then releases")
    {
        const auto out = playNote (pl.p, root, 3.0, 4.6);
        const float steady = rms (out, 0.5, 0.9);
        REQUIRE (steady > 1.0e-3f);
        CHECK (rms (out, 2.5, 2.9) > steady * 0.5f);   // the loop keeps it going (within 6 dB)
        CHECK (rms (out, 2.5, 2.9) < steady * 2.0f);
        CHECK (rms (out, 4.3, 4.6) < steady * 0.003f); // and it lets go (< -50 dB)
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < out.getNumSamples(); ++i)
                REQUIRE (std::isfinite (out.getSample (ch, i)));
    }

    SECTION ("a note let go early goes straight to the sample's release")
    {
        const auto out = playNote (pl.p, root, 0.3, 1.6);
        const float held = rms (out, 0.15, 0.3);
        REQUIRE (held > 1.0e-3f);
        CHECK (rms (out, 0.3, 0.33) > held * 0.25f);  // no gap at the hand-over
        CHECK (rms (out, 1.2, 1.6) < held * 0.003f);  // gone (the release is ~0.3 s)
    }

    SECTION ("at 48 kHz it is still in tune")
    {
        const auto out = playNote (pl.p, root, 0.8, 1.0, 48000.0);
        const float hz = detectPitch (out.getReadPointer (0, 14400), 19200, 48000.0);
        CHECK (std::abs (hz / 330.0 - 1.0) < 0.01);
    }

    SECTION ("MINT FINE at 0 plays the key exactly in tune")
    {
        auto patch = pl.p.getPatch();
        auto* fine = pl.p.getState().getParameter (cash::param::mintFine);
        patch[idx (cash::param::mintFine)] = fine->convertTo0to1 (0.0f);
        pl.p.setPatch (patch, "In tune");
        const auto out = playNote (pl.p, 69, 0.8, 1.0); // A4 = 440 Hz
        const float hz = detectPitch (out.getReadPointer (0, 13230), 17640, sr);
        CHECK (std::abs (hz / 440.0 - 1.0) < 0.006);
    }
    juce::ignoreUnused (r);
}

TEST_CASE ("Unpitched sounds mint as noise", "[mint]")
{
    Plugin pl;
    // A snare-ish burst: decaying noise, a little brighter at the start.
    const int n = static_cast<int> (0.5 * sr);
    juce::AudioBuffer<float> audio (2, n);
    juce::Random rng (3);
    float lp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float> (i / sr);
        const float w = rng.nextFloat() * 2.0f - 1.0f;
        lp += (0.2f + 0.6f * std::exp (-t / 0.03f)) * (w - lp);
        const float v = lp * 0.5f * std::exp (-t / 0.12f);
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }
    auto& m = pl.p.getMatcher();
    REQUIRE (m.setTarget (audio, sr, "Snare").isEmpty());
    const auto model = m.getMintModel();
    REQUIRE (model != nullptr);
    CHECK (model->numPartials == 0);
    const auto& tp = m.getTargetPrint();
    Parts parts;
    const int pct = scoreAgainst (tp, playModel (*model, tp.holdSeconds, tp.seconds), &parts);
    INFO (describe (parts));
    CHECK (pct >= 92);
}

TEST_CASE ("Mint models save and load", "[mint][state]")
{
    Plugin pl;
    auto& m = pl.p.getMatcher();
    REQUIRE (m.setTarget (fluteish(), sr, "Flute").isEmpty());
    const auto model = m.getMintModel();
    const auto bytes = model->toMemory();
    CHECK (bytes.getSize() < 400 * 1024);
    const auto back = cash::MintModel::fromMemory (bytes.getData(), bytes.getSize());
    REQUIRE (back != nullptr);
    CHECK (back->numFrames == model->numFrames);
    CHECK (back->numPartials == model->numPartials);
    CHECK (back->loopStart == model->loopStart);
    CHECK (back->loopEnd == model->loopEnd);
    const auto& tp = m.getTargetPrint();
    CHECK (scoreAgainst (tp, playModel (*back, tp.holdSeconds, tp.seconds)) >= 97);

    // Junk is refused, never half-loaded.
    CHECK (cash::MintModel::fromMemory (nullptr, 0) == nullptr);
    const char junk[] = "definitely not a mint";
    CHECK (cash::MintModel::fromMemory (junk, sizeof (junk)) == nullptr);
    auto cut = bytes;
    cut.setSize (cut.getSize() / 2);
    CHECK (cash::MintModel::fromMemory (cut.getData(), cut.getSize()) == nullptr);
}

TEST_CASE ("The Mint follows undo, the vault, stashes and the project", "[mint][state]")
{
    Plugin pl;
    auto& p = pl.p;
    const auto before = p.getPatch();
    const auto first = matchAndDeposit (p, fluteish (330.0), "Low");
    REQUIRE (p.bankIt() == 0);
    const auto second = matchAndDeposit (p, fluteish (440.0), "High");
    REQUIRE (first.mint != second.mint);
    CHECK (p.getMint() == second.mint);

    SECTION ("undo and redo bring each match's own model back")
    {
        REQUIRE (p.undo());
        CHECK (p.getMint() == first.mint);
        REQUIRE (p.undo());
        CHECK (cash::samePatch (p.getPatch(), before));
        REQUIRE (p.redo());
        REQUIRE (p.redo());
        CHECK (p.getMint() == second.mint);
    }

    SECTION ("a banked note withdraws with its own model")
    {
        REQUIRE (p.withdraw (0));
        CHECK (p.getMint() == first.mint);
        CHECK (p.bankIt() == 0); // same patch + same model: already banked
    }

    SECTION ("projects keep the current model and the vault's")
    {
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        Plugin other;
        other.p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        REQUIRE (other.p.getMint() != nullptr);
        CHECK (other.p.getMint()->numFrames == second.mint->numFrames);
        CHECK (std::abs (other.p.getMint()->rootHz - second.mint->rootHz) < 0.01f);
        CHECK (other.p.getPatch()[idx (cash::param::mintOn)] == 1.0f);
        REQUIRE (other.p.getVault().size() == 1);
        REQUIRE (other.p.getVault().at (0).mint != nullptr);
        CHECK (std::abs (other.p.getVault().at (0).mint->rootHz - first.mint->rootHz) < 0.01f);
        // And it plays: 440 Hz on the matched key.
        const auto out = playNote (other.p, 69, 0.8, 1.0);
        const float hz = detectPitch (out.getReadPointer (0, 13230), 17640, sr);
        CHECK (std::abs (hz / 440.0 - 1.0) < 0.01);
    }

    SECTION ("stashes keep the model")
    {
        juce::TemporaryFile tmp;
        const auto dir = tmp.getFile();
        REQUIRE (dir.createDirectory());
        p.getStash().setFolder (dir);
        const auto file = p.stashIt ("Minted");
        REQUIRE (file.existsAsFile());
        CHECK_FALSE (p.isStashModified());

        Plugin other;
        other.p.getStash().setFolder (dir);
        REQUIRE (other.p.loadStash (file));
        REQUIRE (other.p.getMint() != nullptr);
        CHECK (other.p.getMint()->numFrames == second.mint->numFrames);
        CHECK_FALSE (other.p.isStashModified());
        dir.deleteRecursively();
    }
}

TEST_CASE ("Swapping the model mid-note is safe", "[mint]")
{
    Plugin pl;
    auto& p = pl.p;
    const auto r = matchAndDeposit (p, fluteish(), "Flute");
    const int root = p.getMatcher().getTargetPrint().midiNote;

    p.prepareToPlay (sr, 256);
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer on;
    on.addEvent (juce::MidiMessage::noteOn (1, root, 1.0f), 0);
    p.processBlock (buf, on);
    REQUIRE (buf.getMagnitude (0, 256) > 0.0f);

    // A new match lands while the note sounds: the old model must not be touched again.
    const auto r2 = matchAndDeposit (p, fluteish (500.0), "Other");
    juce::MidiBuffer none;
    for (int b = 0; b < 20; ++b)
    {
        p.processBlock (buf, none);
        for (int i = 0; i < 256; ++i)
            REQUIRE (std::isfinite (buf.getSample (0, i)));
    }
    CHECK (p.getMint() == r2.mint);
    CHECK_FALSE (p.getEngine().mayBeUsing (r.mint.get()));
    CHECK (p.getEngine().mayBeUsing (r2.mint.get()));
}

TEST_CASE ("Money moves keep a minted sound playable", "[mint][mutator]")
{
    Plugin pl;
    using namespace cash::param;
    matchAndDeposit (pl.p, fluteish(), "Flute");
    for (uint64_t seed = 1; seed <= 20; ++seed)
    {
        const auto out = seed % 2 == 0 ? cash::Mutator::jackpot (pl.p.getPatch(), seed) : cash::Mutator::invest (pl.p.getPatch(), { 0.5f, 0.5f, seed });
        CHECK (out[idx (mintOn)] == 1.0f); // never switched off (weight 0)
        CHECK (out[idx (mintFine)] == pl.p.getPatch()[idx (mintFine)]);
    }
}

//============================================================================== M13 polish
namespace
{
void setParam (CashSynthAudioProcessor& p, const char* id, float realValue)
{
    auto* q = p.getState().getParameter (id);
    REQUIRE (q != nullptr);
    q->setValueNotifyingHost (q->convertTo0to1 (realValue));
}

// Mid and side (L-R) RMS of a stereo buffer between two times.
std::pair<float, float> midSide (const juce::AudioBuffer<float>& b, double from, double to, double rate = 44100.0)
{
    const int a = static_cast<int> (from * rate), e = juce::jmin (b.getNumSamples(), static_cast<int> (to * rate));
    double m = 0.0, s = 0.0;
    for (int i = a; i < e; ++i)
    {
        const double l = b.getSample (0, i), r = b.getSample (1, i);
        m += 0.25 * (l + r) * (l + r);
        s += 0.25 * (l - r) * (l - r);
    }
    const double n = juce::jmax (1, e - a);
    return { static_cast<float> (std::sqrt (m / n)), static_cast<float> (std::sqrt (s / n)) };
}

bool finite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (ch, i)))
                return false;
    return true;
}
}

TEST_CASE ("The Mint's stereo goes through the filter and drive", "[mint][polish]")
{
    Plugin pl;
    using namespace cash::param;
    matchAndDeposit (pl.p, fluteish(), "Flute");

    const auto dry = playNote (pl.p, 64, 1.0, 1.2);
    const auto [dryMid, drySide] = midSide (dry, 0.3, 0.9);
    REQUIRE (drySide > 0.02f * dryMid); // the right side is late: there's a stereo image to filter

    // A dark low-pass takes the top off both mid and side alike (M13 let the side skip it).
    setParam (pl.p, filterOn, 1.0f);
    setParam (pl.p, filterType, 0.0f);
    setParam (pl.p, filterCutoff, 300.0f);
    setParam (pl.p, filterReso, 0.0f);
    setParam (pl.p, filterEnvAmt, 0.0f);
    setParam (pl.p, filterKeyTrack, 0.0f);
    const auto dark = playNote (pl.p, 64, 1.0, 1.2);
    REQUIRE (finite (dark));
    const auto [darkMid, darkSide] = midSide (dark, 0.3, 0.9);
    INFO ("mid " << dryMid << " -> " << darkMid << ", side " << drySide << " -> " << darkSide);
    CHECK (darkMid < 0.7f * dryMid);
    CHECK (darkSide < 0.5f * drySide); // the side is mostly high (a 9-sample delay): it loses more

    // Drive works on left and right: the image stays about as wide as it was (not just the mid squashed).
    setParam (pl.p, filterOn, 0.0f);
    setParam (pl.p, driveOn, 1.0f);
    setParam (pl.p, driveType, 1.0f); // hard clip
    setParam (pl.p, driveAmount, 0.8f);
    const auto hot = playNote (pl.p, 64, 1.0, 1.2);
    REQUIRE (finite (hot));
    const auto [hotMid, hotSide] = midSide (hot, 0.3, 0.9);
    INFO ("side/mid dry " << drySide / dryMid << ", driven " << hotSide / hotMid);
    CHECK (hotSide / hotMid > 0.5f * drySide / dryMid);
    CHECK (hotSide / hotMid < 2.0f * drySide / dryMid);

    // Both off again: back to the exact dry render.
    setParam (pl.p, driveOn, 0.0f);
    const auto again = playNote (pl.p, 64, 1.0, 1.2);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < again.getNumSamples(); i += 7)
            REQUIRE (again.getSample (ch, i) == dry.getSample (ch, i));
}

TEST_CASE ("Mint Start skips into the attack, Mint Loop sizes the held loop", "[mint][polish]")
{
    Plugin pl;
    using namespace cash::param;
    const auto r = matchAndDeposit (pl.p, fluteish (330.0, 1.5, 1.9), "Flute");
    REQUIRE (r.mint != nullptr);
    const auto& m = *r.mint;
    REQUIRE (m.hasLoop());

    // Cash Match leaves both at their defaults (start at the top, the loop it picked).
    const auto defaults = cash::defaultPatch (pl.p.getState());
    CHECK (r.patch[idx (mintStart)] == defaults[idx (mintStart)]);
    CHECK (r.patch[idx (mintLoop)] == defaults[idx (mintLoop)]);

    cash::MintPlayer player;
    player.prepare (sr);

    // Defaults: the measured loop, exactly, from frame 0.
    player.start (&m, 1);
    CHECK (player.getPosition() == 0.0);
    CHECK (player.getLoopStart() == m.loopStart);
    CHECK (player.getLoopEnd() == m.loopEnd);

    // Start: 0..100 % of the way to the key-up point (wrapped into the loop when past it).
    player.start (&m, 1, 0.25f, 1.0f);
    CHECK (std::abs (player.getPosition() - 0.25 * (m.holdFrame - 1)) < 1.0e-6);
    player.start (&m, 1, 1.0f, 1.0f);
    CHECK (player.getPosition() >= m.loopStart);
    CHECK (player.getPosition() < m.loopEnd);

    // Loop: shorter/longer round the same middle, never past the key-up point.
    const double mid = 0.5 * (m.loopStart + m.loopEnd), len = m.loopEnd - m.loopStart;
    player.start (&m, 1, 0.0f, 0.5f);
    CHECK (std::abs (player.getLoopEnd() - player.getLoopStart() - 0.5 * len) < 1.0e-6);
    CHECK (std::abs (0.5 * (player.getLoopStart() + player.getLoopEnd()) - mid) < 1.0e-6);
    player.start (&m, 1, 0.0f, 3.0f);
    CHECK (player.getLoopStart() >= 0.0);
    CHECK (player.getLoopEnd() <= m.holdFrame);
    CHECK (player.getLoopEnd() - player.getLoopStart() > len);

    // Through the synth: skipping the 60 ms fade-in makes the first 20 ms louder.
    const auto top = playNote (pl.p, 64, 1.0, 1.2);
    setParam (pl.p, mintStart, 0.3f);
    const auto skipped = playNote (pl.p, 64, 1.0, 1.2);
    CHECK (rms (skipped, 0.0, 0.02, 44100.0) > 2.0f * rms (top, 0.0, 0.02, 44100.0));
    setParam (pl.p, mintStart, 0.0f);

    // A long hold at a few loop sizes (and the knob moved mid-note) keeps sounding, steady, no blow-ups.
    for (float loop : { 0.1f, 0.5f, 1.0f, 3.0f })
    {
        setParam (pl.p, mintLoop, loop);
        const auto held = playNote (pl.p, 64, 4.0, 4.2);
        REQUIRE (finite (held));
        const float early = rms (held, 0.4, 0.9, 44100.0), late = rms (held, 3.0, 3.9, 44100.0);
        INFO ("loop " << loop << ": " << early << " -> " << late);
        CHECK (late > 0.5f * early);
        CHECK (late < 2.0f * early);
    }
    {
        cash::MintPlayer p2;
        p2.prepare (sr);
        p2.start (&m, 1);
        cash::MintPlayer::Settings s;
        float buf[2][cash::MintPlayer::blockSize];
        for (int b = 0; b < 2000; ++b) // ~1.4 s: the loop has wrapped
        {
            if (b == 1500)
                s.loop = 0.2f; // shorter, mid-note: jumps back into the new loop
            std::fill (buf[0], buf[0] + cash::MintPlayer::blockSize, 0.0f);
            std::fill (buf[1], buf[1] + cash::MintPlayer::blockSize, 0.0f);
            p2.render (buf[0], buf[1], cash::MintPlayer::blockSize, s);
        }
        CHECK (p2.getLoopEnd() - p2.getLoopStart() < 0.25 * len + 1.0);
        CHECK (p2.getPosition() >= p2.getLoopStart());
        CHECK (p2.getPosition() < p2.getLoopEnd());
    }
    setParam (pl.p, mintLoop, 1.0f);
}

TEST_CASE ("A project with a big Mint model saves and restores quickly and exactly", "[mint][state][polish]")
{
    // pluginval-style: get state, set it on a fresh instance, get it again. A 10 s, 128-partial
    // stereo bass is about as big as a model gets.
    Plugin pl;
    const int n = static_cast<int> (10.0 * sr);
    juce::AudioBuffer<float> bass (2, n);
    juce::Random rng (7);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = t < 0.01 ? t / 0.01 : (t < 9.0 ? 1.0 - 0.3 * t / 9.0 : 0.7 * std::exp (-(t - 9.0) / 0.2));
        ph += juce::MathConstants<double>::twoPi * 55.0 * std::exp2 (0.1 * std::sin (3.0 * t) / 12.0) / sr;
        double s = 0.0;
        for (int k = 1; k <= 140; ++k)
            s += std::sin (k * ph + 0.3 * k) / k;
        const float w = 0.01f * (rng.nextFloat() - 0.5f);
        bass.setSample (0, i, static_cast<float> (0.2 * env * s) + w);
        bass.setSample (1, i, static_cast<float> (0.2 * env * s * (1.0 + 0.1 * std::sin (0.7 * t))) - w);
    }
    auto& m = pl.p.getMatcher();
    REQUIRE (m.setTarget (bass, sr, "Big bass").isEmpty());
    const auto model = m.getMintModel();
    REQUIRE (model != nullptr);
    CHECK (model->numPartials >= 100);

    auto patch = pl.p.getPatch();
    patch[idx (cash::param::mintOn)] = 1.0f;
    pl.p.setPatch (patch, "Big bass", model);
    REQUIRE (pl.p.bankIt() == 0); // the vault holds it too (stored once)

    juce::MemoryBlock saved;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    pl.p.getStateInformation (saved);
    const double saveMs = juce::Time::getMillisecondCounterHiRes() - t0;
    INFO ("state " << saved.getSize() / 1024 << " KB, save " << saveMs << " ms");
    CHECK (saved.getSize() < 4 * 1024 * 1024);
    CHECK (saveMs < 2000.0);

    Plugin other;
    t0 = juce::Time::getMillisecondCounterHiRes();
    other.p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    const double loadMs = juce::Time::getMillisecondCounterHiRes() - t0;
    INFO ("load " << loadMs << " ms");
    CHECK (loadMs < 2000.0);
    REQUIRE (other.p.getMint() != nullptr);
    CHECK (other.p.getMint()->numPartials == model->numPartials);
    CHECK (other.p.getVault().size() == 1);

    // Saving the restored project gives the same bytes (hosts compare these).
    juce::MemoryBlock again;
    other.p.getStateInformation (again);
    CHECK (again == saved);

    // And it plays.
    const auto out = playNote (other.p, 33, 0.5, 0.7);
    CHECK (rms (out, 0.1, 0.4, 44100.0) > 0.01f);
}
