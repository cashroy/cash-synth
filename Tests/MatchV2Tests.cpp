// Cash Match v2 (M9): frame track, per-part scores, whole stereo samples,
// section switches, parallel search.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Parameters.h"
#include "CashMatch.h"
#include "SoundPrint.h"

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

void setReal (const juce::AudioProcessorValueTreeState& s, cash::Patch& patch, const char* id, float real)
{
    auto* p = s.getParameter (id);
    REQUIRE (p != nullptr);
    patch[idx (id)] = cash::ParameterMap::snap (cash::ParameterMap::spec (static_cast<int> (idx (id))), p->convertTo0to1 (real));
}

// A tone whose pitch (semitones off 220 Hz) and gain follow functions of time.
std::vector<float> tone (double seconds, const std::function<double (double)>& semis, const std::function<double (double)>& gain)
{
    std::vector<float> x (static_cast<size_t> (seconds * sr));
    double phase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double> (i) / sr;
        x[i] = static_cast<float> (0.5 * gain (t) * std::sin (phase));
        phase += juce::MathConstants<double>::twoPi * 220.0 * std::exp2 (semis (t) / 12.0) / sr;
    }
    return x;
}

cash::SoundPrint print (const std::vector<float>& x) { return analyse (x.data(), static_cast<int> (x.size())); }

juce::String describe (const Parts& p)
{
    juce::String s;
    for (int k = 0; k < numParts; ++k)
        s << partName (k) << " " << matchPercent (p[static_cast<size_t> (k)]) << "%  ";
    return s;
}

// A plain saw lead: no movement, mono.
cash::Patch plainLead (const juce::AudioProcessorValueTreeState& s)
{
    using namespace cash::param;
    auto p = cash::defaultPatch (s);
    setReal (s, p, osc2Level, 0.0f);
    setReal (s, p, filterCutoff, 3000.0f);
    setReal (s, p, filterEnvAmt, 0.0f);
    setReal (s, p, ampAttack, 0.005f);
    setReal (s, p, ampSustain, 0.8f);
    setReal (s, p, ampRelease, 0.15f);
    return p;
}
}

// ------------------------------------------------------------ frame track

TEST_CASE ("Frame track follows a pitch glide", "[match][analysis][v2]")
{
    const auto x = tone (1.0, [] (double t) { return 7.0 * t; }, [] (double) { return 1.0; });
    const auto sp = print (x);
    REQUIRE (sp.valid);
    CHECK (sp.hopSeconds == Catch::Approx (0.01).margin (0.0005));
    for (const double t : { 0.2, 0.5, 0.8 })
    {
        const auto f = static_cast<size_t> (t / sp.hopSeconds);
        INFO ("t " << t << " pitch " << sp.pitch[f]);
        CHECK (sp.pitch[f] == Catch::Approx (57.0 + 7.0 * (static_cast<double> (f) + 0.5) * sp.hopSeconds).margin (0.15));
    }
}

TEST_CASE ("Frame track hears vibrato, tremolo, a pitch drop and width", "[match][analysis][v2]")
{
    const auto plain = print (tone (2.0, [] (double) { return 0.0; }, [] (double) { return 1.0; }));
    CHECK_FALSE (plain.vibrato.present());
    CHECK_FALSE (plain.tremolo.present());
    CHECK_FALSE (plain.wah.present());
    CHECK (plain.width == 0.0f);
    CHECK (std::abs (plain.pitchStart) < 0.3f);
    CHECK (plain.f0 == Catch::Approx (220.0f).epsilon (0.003));

    const auto vib = print (tone (2.0, [] (double t) { return 0.5 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t); }, [] (double) { return 1.0; }));
    INFO ("vibrato " << vib.vibrato.rate << " Hz, " << vib.vibrato.depth << " st; tremolo " << vib.tremolo.depth);
    REQUIRE (vib.vibrato.present());
    CHECK (vib.vibrato.rate == Catch::Approx (5.5f).margin (0.3f));
    CHECK (vib.vibrato.depth == Catch::Approx (0.5f).margin (0.1f));
    CHECK_FALSE (vib.tremolo.present());
    CHECK (vib.f0 == Catch::Approx (220.0f).epsilon (0.01)); // vibrato doesn't move the note

    // Gain 1 .. 0.5 at 4 Hz: 6 dB peak to peak = 3 dB depth.
    const auto trem = print (tone (2.0, [] (double) { return 0.0; },
                                   [] (double t) { return 0.75 + 0.25 * std::sin (juce::MathConstants<double>::twoPi * 4.0 * t); }));
    INFO ("tremolo " << trem.tremolo.rate << " Hz, " << trem.tremolo.depth << " dB; vibrato " << trem.vibrato.depth);
    REQUIRE (trem.tremolo.present());
    CHECK (trem.tremolo.rate == Catch::Approx (4.0f).margin (0.3f));
    CHECK (trem.tremolo.depth == Catch::Approx (3.0f).margin (0.8f));
    CHECK_FALSE (trem.vibrato.present());

    // Starts 7 st high, falls onto the note in ~80 ms.
    const auto drop = print (tone (1.0, [] (double t) { return 7.0 * std::exp (-t / 0.03); }, [] (double) { return 1.0; }));
    INFO ("pitch start " << drop.pitchStart << " settles in " << drop.pitchSettleSeconds);
    CHECK (drop.pitchStart > 3.0f);
    CHECK (drop.pitchSettleSeconds < 0.15);
    CHECK (drop.midiNote == 57);

    // Stereo: independent noise on each side reads wide; the same on both reads mono.
    juce::Random rng (3);
    std::vector<float> l (static_cast<size_t> (sr)), r (l.size());
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = rng.nextFloat() - 0.5f;
        r[i] = rng.nextFloat() - 0.5f;
    }
    CHECK (analyse (l.data(), r.data(), static_cast<int> (l.size())).width > 0.8f);
    CHECK (analyse (l.data(), l.data(), static_cast<int> (l.size())).width == 0.0f);
}

TEST_CASE ("Long samples keep a bounded frame count", "[match][analysis][v2]")
{
    const auto x = tone (9.0, [] (double) { return 0.0; }, [] (double t) { return std::exp (-t / 3.0); });
    const auto sp = print (x);
    REQUIRE (sp.valid);
    CHECK (sp.seconds == Catch::Approx (9.0).margin (0.01));
    CHECK (sp.level.size() <= static_cast<size_t> (cash::SoundPrint::maxFrames));
    CHECK (sp.level.size() >= static_cast<size_t> (cash::SoundPrint::maxFrames - 2));
}

// ------------------------------------------------------------ parts

TEST_CASE ("Parts single out what differs", "[match][analysis][v2]")
{
    Plugin pl;
    const auto& s = pl.p.getState();
    cash::PatchRenderer r (s);
    using namespace cash::param;

    const auto base = plainLead (s);
    auto vibrato = base;
    setReal (s, vibrato, lfoRate, 5.0f);
    setReal (s, vibrato, lfoToPitch, 0.6f);
    auto wide = base;
    setReal (s, wide, unisonVoices, 4.0f);
    setReal (s, wide, unisonSpread, 1.0f);
    setReal (s, wide, unisonDetune, 0.3f);

    juce::AudioBuffer<float> a, b, c;
    r.render (base, 57, 1.5, 1.8, a);
    r.render (vibrato, 57, 1.5, 1.8, b);
    r.render (wide, 57, 1.5, 1.8, c);
    const auto pa = analyse (a.getReadPointer (0), a.getReadPointer (1), a.getNumSamples());
    const auto pb = analyse (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples(), noteToHz (57), &pa);
    const auto pc = analyse (c.getReadPointer (0), c.getReadPointer (1), c.getNumSamples(), noteToHz (57), &pa);

    const auto vibParts = distanceParts (pa, pb), wideParts = distanceParts (pa, pc);
    INFO ("vibrato: " << describe (vibParts) << "\nwide: " << describe (wideParts) << "\nwidth " << pc.width
                      << ", vibrato found " << pb.vibrato.rate << " Hz " << pb.vibrato.depth << " st");
    CHECK (pb.vibrato.present());
    CHECK (pb.vibrato.rate == Catch::Approx (5.0f).margin (0.3f));
    CHECK (matchPercent (vibParts[movementPart]) < 60);
    CHECK (matchPercent (vibParts[envelopePart]) >= 90);
    CHECK (matchPercent (vibParts[widthPart]) >= 95);

    CHECK (pc.width > 0.3f);
    CHECK (matchPercent (wideParts[widthPart]) < 80);
    CHECK (matchPercent (wideParts[envelopePart]) >= 60); // unison beats slowly: a real (small) level wobble
    CHECK (matchPercent (wideParts[pitchPart]) >= 85);
}

// ------------------------------------------------------------ matching

TEST_CASE ("Cash Match v2 matches movement, pitch drop and width", "[match][v2]")
{
    Plugin pl;
    const auto& s = pl.p.getState();
    auto& m = pl.p.getMatcher();
    using namespace cash::param;
    CHECK (m.getNumWorkers() >= 1);

    auto target = plainLead (s);
    setReal (s, target, lfoRate, 5.5f);
    setReal (s, target, lfoToPitch, 0.5f);
    setReal (s, target, unisonVoices, 3.0f);
    setReal (s, target, unisonSpread, 0.8f);
    setReal (s, target, pitchEnvAmt, 0.5f); // +12 st
    setReal (s, target, fltAttack, 0.001f);
    setReal (s, target, fltDecay, 0.2f);
    setReal (s, target, fltSustain, 0.0f);

    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> audio;
    r.render (target, 52, 1.6, 1.9, audio);
    REQUIRE (m.setTarget (audio, sr, "Lead").isEmpty());
    const auto& tp = m.getTargetPrint();
    INFO ("heard: vibrato " << tp.vibrato.rate << " Hz " << tp.vibrato.depth << " st, width " << tp.width << ", pitch start " << tp.pitchStart);
    CHECK (tp.midiNote == 52);
    CHECK (tp.vibrato.present());
    CHECK (tp.width > 0.2f);
    CHECK (tp.pitchStart > 4.0f);

    // The guess alone already switches on what it heard, and leaves the rest off.
    const auto defaults = cash::defaultPatch (s);
    const auto g = m.guess (defaults);
    CHECK (g[idx (lfo1On)] == 1.0f);
    CHECK (g[idx (unisonOn)] == 1.0f);
    CHECK (g[idx (filterEnvOn)] == 1.0f);
    CHECK (g[idx (lfo2On)] == 0.0f);
    CHECK (g[idx (driveOn)] == 0.0f);

    const auto before = m.scoreParts (defaults);
    const auto guessed = m.scoreParts (g);
    m.runBlocking (defaults, defaults, 9); // the real (default) budget: this is the headline claim
    const auto res = m.getResult();
    INFO ("default " << describe (before) << "\nguess   " << describe (guessed) << "\nmatch   " << describe (res.parts)
                     << "\noverall " << res.percent << "% after " << res.tried << ", sections off " << res.sectionsOff);
    REQUIRE (res.found);
    CHECK (res.progress == 1.0f);
    CHECK (res.percent >= 85);
    for (int k = 0; k < numParts; ++k)
        CHECK (matchPercent (res.parts[static_cast<size_t> (k)]) >= 70);
    CHECK (matchPercent (res.parts[pitchPart]) > matchPercent (before[pitchPart]));
    CHECK (m.getBestRender().getNumChannels() == 2);
}

TEST_CASE ("Cash Match switches off sections it doesn't need", "[match][v2]")
{
    Plugin pl;
    const auto& s = pl.p.getState();
    auto& m = pl.p.getMatcher();
    using namespace cash::param;

    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> audio;
    r.render (plainLead (s), 45, 0.6, 0.9, audio);
    REQUIRE (m.setTarget (audio, sr, "Plain").isEmpty());
    const auto defaults = cash::defaultPatch (s);
    const auto g = m.guess (defaults);
    for (auto* id : { lfo1On, lfo2On, unisonOn, driveOn, glideOn })
        CHECK (g[idx (id)] == 0.0f);
    CHECK (g[idx (osc1On)] == 1.0f);

    m.runBlocking (defaults, defaults, 4, { 12, 3, 8, 1, false }); // the synth-knob search (M9)
    const auto res = m.getResult();
    INFO (res.percent << "%, sections off " << res.sectionsOff);
    CHECK (res.percent >= 80);
    CHECK (res.sectionsOff >= 3);
    CHECK ((res.patch[idx (osc1On)] >= 0.5f || res.patch[idx (osc2On)] >= 0.5f)); // still makes a sound
}

TEST_CASE ("Cash Match keeps the whole sample, in stereo, and is repeatable", "[match][v2]")
{
    Plugin pl;
    auto& m = pl.p.getMatcher();

    // 6 s stereo: kept whole (M6 cut at 2.5 s), and wide.
    const int n = static_cast<int> (6.0 * sr);
    juce::AudioBuffer<float> wide (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (sr);
        const float env = std::exp (-t / 2.0f);
        wide.setSample (0, i, 0.4f * env * std::sin (juce::MathConstants<float>::twoPi * 220.0f * t));
        wide.setSample (1, i, 0.4f * env * std::sin (juce::MathConstants<float>::twoPi * 221.5f * t + 1.0f));
    }
    REQUIRE (m.setTarget (wide, sr, "Wide").isEmpty());
    CHECK (m.getTargetPrint().seconds == Catch::Approx (6.0).margin (0.02));
    CHECK (m.getTargetAudio().getNumChannels() == 2);
    CHECK (m.getTargetPrint().width > 0.2f);

    // Longer than the cap: the first 10 s.
    juce::AudioBuffer<float> longOne (1, static_cast<int> (12.0 * sr));
    for (int i = 0; i < longOne.getNumSamples(); ++i)
        longOne.setSample (0, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 110.0f * static_cast<float> (i) / static_cast<float> (sr)));
    REQUIRE (m.setTarget (longOne, sr, "Drone").isEmpty());
    CHECK (m.getTargetPrint().seconds == Catch::Approx (cash::CashMatcher::maxSeconds).margin (0.01));

    // Same seed, same answer, however the threads interleave.
    const auto& s = pl.p.getState();
    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> audio;
    r.render (plainLead (s), 50, 0.4, 0.6, audio);
    REQUIRE (m.setTarget (audio, sr, "Twice").isEmpty());
    const auto defaults = cash::defaultPatch (s);
    m.runBlocking (defaults, defaults, 21, { 8, 2, 8, 1 });
    const auto one = m.getResult();
    m.runBlocking (defaults, defaults, 21, { 8, 2, 8, 1 });
    const auto two = m.getResult();
    CHECK (one.percent == two.percent);
    CHECK (one.tried == two.tried);
    CHECK (cash::samePatch (one.patch, two.patch, 1.0e-6f));
}

TEST_CASE ("The cheque shows a score per part", "[match][ui][v2]")
{
    Plugin pl;
    std::unique_ptr<CashSynthAudioProcessorEditor> ed (dynamic_cast<CashSynthAudioProcessorEditor*> (pl.p.createEditor()));
    REQUIRE (ed != nullptr);
    ed->getStashBar().getMatchButton().onClick();
    auto& panel = ed->getMatchPanel();
    for (int k = 0; k < numParts; ++k)
        CHECK (panel.getPartPercent (k) == -1); // nothing yet

    // Unpitched noise: pitch isn't scored.
    juce::Random rng (5);
    juce::AudioBuffer<float> noise (1, static_cast<int> (0.5 * sr));
    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample (0, i, (rng.nextFloat() - 0.5f) * (1.0f - static_cast<float> (i) / static_cast<float> (noise.getNumSamples())));
    REQUIRE (pl.p.getMatcher().setTarget (noise, sr, "Hiss").isEmpty());
    pl.p.getMatcher().runBlocking (pl.p.getPatch(), cash::defaultPatch (pl.p.getState()), 2, { 6, 1, 6, 0 });
    panel.sync();
    CHECK (panel.getPartPercent (pitchPart) == -1);
    for (const int k : { envelopePart, tonePart, brightnessPart, movementPart, widthPart })
    {
        CHECK (panel.getPartPercent (k) >= 0);
        CHECK (panel.getPartPercent (k) <= 100);
    }

    juce::Image img (juce::Image::ARGB, CashSynthAudioProcessorEditor::width, CashSynthAudioProcessorEditor::height, true);
    juce::Graphics g (img);
    ed->paintEntireComponent (g, true);
}

