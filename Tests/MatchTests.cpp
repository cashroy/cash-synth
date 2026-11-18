#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Parameters.h"
#include "CashCode.h"
#include "CashMatch.h"
#include "SoundPrint.h"

namespace
{
constexpr double sr = cash::SoundPrint::sampleRate;

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

std::vector<float> sine (float hz, double seconds, float level = 0.5f)
{
    std::vector<float> x (static_cast<size_t> (seconds * sr));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = level * std::sin (juce::MathConstants<float>::twoPi * hz * static_cast<float> (i) / static_cast<float> (sr));
    return x;
}

float rms (const juce::AudioBuffer<float>& b, int start, int n)
{
    return b.getRMSLevel (0, start, juce::jmin (n, b.getNumSamples() - start));
}

// A plucky square bass: easy to tell apart from the default saw pad.
cash::Patch pluck (const juce::AudioProcessorValueTreeState& s)
{
    using namespace cash::param;
    auto p = cash::defaultPatch (s);
    p[idx (osc1Wave)] = cash::ParameterMap::fromStep (cash::ParameterMap::spec (static_cast<int> (idx (osc1Wave))), square);
    setReal (s, p, osc2Level, 0.0f);
    setReal (s, p, filterCutoff, 900.0f);
    setReal (s, p, filterEnvAmt, 0.6f);
    setReal (s, p, fltDecay, 0.15f);
    setReal (s, p, ampAttack, 0.002f);
    setReal (s, p, ampDecay, 0.35f);
    setReal (s, p, ampSustain, 0.0f);
    setReal (s, p, ampRelease, 0.2f);
    return p;
}
}

// ------------------------------------------------------------ new engine controls

TEST_CASE ("Drive shapes are bounded and off at zero", "[engine][drive]")
{
    for (int type = 0; type < 3; ++type)
    {
        for (float x = -1.0f; x <= 1.0f; x += 0.01f)
        {
            CHECK (cash::CashVoice::shape (type, x, 0.0f) == x);
            for (const float d : { 0.1f, 0.5f, 1.0f })
                CHECK (std::abs (cash::CashVoice::shape (type, x, d)) <= 1.7f);
        }
    }
    // Soft clip squashes: a loud input gains less than a quiet one.
    CHECK (cash::CashVoice::shape (cash::param::softClip, 1.0f, 1.0f) / cash::CashVoice::shape (cash::param::softClip, 0.1f, 1.0f) < 5.0f);
}

TEST_CASE ("Drive, LFO 2 and pitch env change the sound", "[engine][drive]")
{
    Plugin pl;
    cash::PatchRenderer r (pl.p.getState());
    const auto& s = pl.p.getState();
    using namespace cash::param;

    auto base = cash::defaultPatch (s);
    base[idx (osc1Wave)] = 0.0f; // sine
    setReal (s, base, osc2Level, 0.0f);
    setReal (s, base, filterCutoff, 20000.0f);
    setReal (s, base, filterEnvAmt, 0.0f);
    setReal (s, base, ampSustain, 1.0f);

    juce::AudioBuffer<float> clean, dirty;
    r.render (base, 57, 1.0, 1.0, clean);
    auto driven = base;
    setReal (s, driven, driveAmount, 0.8f);
    r.render (driven, 57, 1.0, 1.0, dirty);
    const auto a = cash::analysis::analyse (clean.getReadPointer (0), clean.getNumSamples(), 220.0f);
    const auto b = cash::analysis::analyse (dirty.getReadPointer (0), dirty.getNumSamples(), 220.0f);
    CHECK (b.harmonics[2] > a.harmonics[2] + 0.2f); // a driven sine grows a 3rd harmonic

    // LFO 2 on amp: tremolo.
    auto trem = base;
    setReal (s, trem, lfo2Amount, 1.0f);
    setReal (s, trem, lfo2Rate, 4.0f);
    trem[idx (lfo2Target)] = cash::ParameterMap::fromStep (cash::ParameterMap::spec (static_cast<int> (idx (lfo2Target))), toAmp);
    juce::AudioBuffer<float> tremolo;
    r.render (trem, 57, 1.0, 1.0, tremolo);
    float lo = 10.0f, hi = 0.0f;
    for (int w = static_cast<int> (0.1 * sr); w < static_cast<int> (0.9 * sr); w += 1000)
    {
        lo = juce::jmin (lo, rms (tremolo, w, 1000));
        hi = juce::jmax (hi, rms (tremolo, w, 1000));
    }
    CHECK (lo < hi * 0.3f);

    // Pitch env: starts high, settles on the note.
    auto zap = base;
    setReal (s, zap, pitchEnvAmt, 0.5f); // +12 st at the top of the env
    setReal (s, zap, fltAttack, 0.001f);
    setReal (s, zap, fltDecay, 0.3f);
    setReal (s, zap, fltSustain, 0.0f);
    juce::AudioBuffer<float> sweep;
    r.render (zap, 57, 1.0, 1.0, sweep);
    const float late = cash::analysis::detectPitch (sweep.getReadPointer (0, static_cast<int> (0.6 * sr)), static_cast<int> (0.4 * sr), sr);
    CHECK (late == Catch::Approx (220.0f).epsilon (0.02));
    int earlyCrossings = 0, lateCrossings = 0;
    const int win = static_cast<int> (0.02 * sr);
    for (int i = 1; i < win; ++i)
    {
        earlyCrossings += (sweep.getSample (0, i - 1) < 0.0f) != (sweep.getSample (0, i) < 0.0f);
        lateCrossings += (sweep.getSample (0, static_cast<int> (0.7 * sr) + i - 1) < 0.0f) != (sweep.getSample (0, static_cast<int> (0.7 * sr) + i) < 0.0f);
    }
    CHECK (earlyCrossings > lateCrossings * 1.4f);
}

TEST_CASE ("CashCode v1 codes still decode after v2 added params", "[cashcode]")
{
    Plugin pl;
    const auto defaults = cash::defaultPatch (pl.p.getState());
    // The pinned default-patch code from before M6 (CashCode v1).
    const auto old = cash::CashCode::decode ("$34S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HGF-35R", defaults);
    REQUIRE (old.has_value());
    for (int i = 0; i < cash::ParameterMap::numParams; ++i)
        CHECK (std::abs ((*old)[static_cast<size_t> (i)] - defaults[static_cast<size_t> (i)]) <= 0.5f / 255.0f + 1.0e-6f);

    const auto now = cash::CashCode::encode (defaults);
    CHECK (now.length() > 60); // v2 is longer (more params)
    CHECK (cash::CashCode::decode (now).has_value());
}

// ------------------------------------------------------------ analysis

TEST_CASE ("Analysis finds pitch, noise and wave shape", "[match][analysis]")
{
    const auto a = sine (220.0f, 1.0);
    CHECK (cash::analysis::detectPitch (a.data(), static_cast<int> (a.size()), sr) == Catch::Approx (220.0f).epsilon (0.005));
    const auto low = sine (55.0f, 1.0);
    CHECK (cash::analysis::detectPitch (low.data(), static_cast<int> (low.size()), sr) == Catch::Approx (55.0f).epsilon (0.01));

    const auto print = cash::analysis::analyse (a.data(), static_cast<int> (a.size()));
    REQUIRE (print.valid);
    CHECK (print.pitched);
    CHECK (print.midiNote == 57);
    CHECK (print.flatness < 0.1f);
    CHECK (cash::analysis::noteName (57) == "A2"); // Logic-style names: middle C (60) = C3

    juce::Random rng (7);
    std::vector<float> noise (static_cast<size_t> (sr));
    for (auto& v : noise)
        v = rng.nextFloat() - 0.5f;
    const auto np = cash::analysis::analyse (noise.data(), static_cast<int> (noise.size()));
    CHECK_FALSE (np.pitched);
    CHECK (np.flatness > 0.5f);

    std::vector<float> silence (static_cast<size_t> (sr), 0.0f);
    CHECK_FALSE (cash::analysis::analyse (silence.data(), static_cast<int> (silence.size())).valid);
}

TEST_CASE ("Same patch scores high, different patch scores lower", "[match][analysis]")
{
    Plugin pl;
    const auto& s = pl.p.getState();
    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> a, b, c;

    const auto saw = cash::defaultPatch (s);
    const auto sq = pluck (s);
    r.render (saw, 48, 0.8, 1.2, a);
    r.render (saw, 48, 0.8, 1.2, b);
    r.render (sq, 48, 0.8, 1.2, c);

    const auto pa = cash::analysis::analyse (a.getReadPointer (0), a.getNumSamples());
    const auto pb = cash::analysis::analyse (b.getReadPointer (0), b.getNumSamples(), cash::analysis::noteToHz (48));
    const auto pc = cash::analysis::analyse (c.getReadPointer (0), c.getNumSamples(), cash::analysis::noteToHz (48));
    REQUIRE (pa.pitched);
    CHECK (pa.midiNote == 48);

    const auto same = cash::analysis::matchPercent (cash::analysis::distance (pa, pb));
    const auto diff = cash::analysis::matchPercent (cash::analysis::distance (pa, pc));
    const auto parts = cash::analysis::distanceParts (pa, pb);
    juce::String partText;
    for (int k = 0; k < cash::analysis::numParts; ++k)
        partText << cash::analysis::partName (k) << " " << parts[static_cast<size_t> (k)] << "  ";
    INFO ("same " << same << "%, different " << diff << "%; same parts " << partText);
    CHECK (same >= 97);
    CHECK (diff < 60);
    for (int k = 0; k < cash::analysis::numParts; ++k)
        CHECK (cash::analysis::matchPercent (parts[static_cast<size_t> (k)]) >= 97);
}

// ------------------------------------------------------------ matcher

TEST_CASE ("Cash Match finds a patch close to a synth-made target", "[match]")
{
    Plugin pl;
    const auto& s = pl.p.getState();
    auto& m = pl.p.getMatcher();

    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> target;
    r.render (pluck (s), 45, 0.5, 1.0, target);
    REQUIRE (m.setTarget (target, sr, "Pluck").isEmpty());
    CHECK (m.getTargetPrint().midiNote == 45);

    const auto defaults = cash::defaultPatch (s);
    const int before = cash::analysis::matchPercent (m.score (defaults));
    const int guessed = cash::analysis::matchPercent (m.score (m.guess (defaults)));

    m.runBlocking (defaults, defaults, 42, { 16, 5, 8, 1 });
    const auto res = m.getResult();
    INFO ("default " << before << "%, guess " << guessed << "%, matched " << res.percent << "% after " << res.tried);
    REQUIRE (res.found);
    CHECK (res.progress == 1.0f);
    CHECK (res.percent > before);
    CHECK (res.percent >= 75);
    CHECK (m.getBestRender().getNumSamples() == target.getNumSamples());

    // Background run can be stopped part way.
    m.start (defaults, defaults, 7);
    m.stop();
    CHECK_FALSE (m.isSearching());
}

TEST_CASE ("Cash Match improves on a sound the synth didn't make", "[match]")
{
    // Karplus-Strong plucked string at 110 Hz: inharmonic-ish, no ADSR, not one of our waves.
    Plugin pl;
    const auto& s = pl.p.getState();
    auto& m = pl.p.getMatcher();

    const int n = static_cast<int> (1.0 * sr), period = static_cast<int> (sr / 110.0);
    juce::AudioBuffer<float> target (1, n);
    juce::Random rng (11);
    std::vector<float> line (static_cast<size_t> (period));
    for (auto& v : line)
        v = rng.nextFloat() * 2.0f - 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto k = static_cast<size_t> (i % period), k1 = static_cast<size_t> ((i + 1) % period);
        const float out = line[k];
        line[k] = 0.996f * 0.5f * (line[k] + line[k1]);
        target.setSample (0, i, out * 0.5f);
    }
    REQUIRE (m.setTarget (target, sr, "String").isEmpty());
    CHECK (std::abs (m.getTargetPrint().midiNote - 45) <= 0); // 110 Hz = A (45)

    const auto defaults = cash::defaultPatch (s);
    const int before = cash::analysis::matchPercent (m.score (defaults));
    m.runBlocking (defaults, defaults, 5, { 24, 8, 10, 1 });
    const auto res = m.getResult();
    INFO ("string: default " << before << "%, matched " << res.percent << "% after " << res.tried);
    CHECK (res.percent > before + 15);
    CHECK (res.percent >= 60);
}

TEST_CASE ("Cash Match loads audio files and rejects bad ones", "[match]")
{
    Plugin pl;
    auto& m = pl.p.getMatcher();
    const auto dir = juce::File::createTempFile ("cashmatch");
    dir.createDirectory();

    // 48 kHz stereo with leading silence: resampled, mixed and trimmed.
    const auto wav = dir.getChildFile ("Tone.wav");
    {
        juce::AudioBuffer<float> buf (2, 48000);
        buf.clear();
        for (int i = 9600; i < 48000; ++i)
            for (int ch = 0; ch < 2; ++ch)
                buf.setSample (ch, i, 0.4f * std::sin (juce::MathConstants<float>::twoPi * 330.0f * static_cast<float> (i) / 48000.0f));
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> out (wav.createOutputStream().release());
        REQUIRE (out != nullptr);
        auto writer = format.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
    CHECK (m.loadTarget (wav).isEmpty());
    CHECK (m.getTargetName() == "Tone");
    CHECK (m.getTargetPrint().f0 == Catch::Approx (330.0f).epsilon (0.01));
    CHECK (m.getTargetPrint().seconds == Catch::Approx (0.8).margin (0.01)); // 0.2 s of silence trimmed

    const auto text = dir.getChildFile ("notes.wav");
    text.replaceWithText ("not audio");
    CHECK (m.loadTarget (text).isNotEmpty());

    juce::AudioBuffer<float> quiet (1, 44100);
    quiet.clear();
    CHECK (m.setTarget (quiet, sr, "Quiet").isNotEmpty());
    dir.deleteRecursively();
}

TEST_CASE ("Depositing a match is an undoable ledger entry", "[match]")
{
    Plugin pl;
    auto& p = pl.p;
    const auto& s = p.getState();
    cash::PatchRenderer r (s);
    juce::AudioBuffer<float> target;
    r.render (pluck (s), 50, 0.4, 0.8, target);
    REQUIRE (p.getMatcher().setTarget (target, sr, "Bass").isEmpty());

    CHECK_FALSE (p.depositMatch()); // nothing found yet
    const auto before = p.getPatch();
    p.getMatcher().runBlocking (before, cash::defaultPatch (s), 3, { 6, 2, 4, 0 });
    REQUIRE (p.depositMatch());
    CHECK (p.getLedger().current()->memo.startsWith ("Cash Match: Bass "));
    CHECK (cash::samePatch (p.getPatch(), p.getMatcher().getResult().patch, 1.0e-3f));
    REQUIRE (p.undo());
    CHECK (cash::samePatch (p.getPatch(), before));

    // Previews play over the synth output.
    p.prepareToPlay (48000.0, 512);
    p.previewTarget();
    juce::AudioBuffer<float> out (2, 512);
    juce::MidiBuffer midi;
    p.processBlock (out, midi);
    CHECK (out.getMagnitude (0, 512) > 0.01f);
    p.stopPreview();
    p.processBlock (out, midi);
    CHECK (out.getMagnitude (0, 512) < 1.0e-6f);
}

// ------------------------------------------------------------ UI

TEST_CASE ("Cash Match cheque opens from the stash bar", "[match][ui]")
{
    Plugin pl;
    std::unique_ptr<CashSynthAudioProcessorEditor> ed (dynamic_cast<CashSynthAudioProcessorEditor*> (pl.p.createEditor()));
    REQUIRE (ed != nullptr);

    CHECK_FALSE (ed->isShowingMatch());
    ed->getStashBar().getMatchButton().onClick();
    CHECK (ed->isShowingMatch());
    CHECK_FALSE (ed->getFront().isVisible());
    CHECK_FALSE (ed->getMatchPanel().getMatchButton().isEnabled()); // nothing to match yet
    CHECK_FALSE (ed->getMatchPanel().getDepositButton().isEnabled());

    CHECK (cash::MatchPanel::isAudioFile ("/x/kick.WAV"));
    CHECK (cash::MatchPanel::isAudioFile ("a.aiff"));
    CHECK_FALSE (cash::MatchPanel::isAudioFile ("a.txt"));
    CHECK (ed->isInterestedInFileDrag ({ "/x/y.flac" }));

    // Painting with and without a target doesn't blow up.
    juce::Image img (juce::Image::ARGB, CashSynthAudioProcessorEditor::width, CashSynthAudioProcessorEditor::height, true);
    {
        juce::Graphics g (img);
        ed->paintEntireComponent (g, true);
    }
    const auto tone = sine (440.0f, 0.5);
    juce::AudioBuffer<float> buf (1, static_cast<int> (tone.size()));
    buf.copyFrom (0, 0, tone.data(), static_cast<int> (tone.size()));
    REQUIRE (pl.p.getMatcher().setTarget (buf, sr, "Beep").isEmpty());
    ed->getMatchPanel().sync();
    CHECK (ed->getMatchPanel().getMatchButton().isEnabled());
    {
        juce::Graphics g (img);
        ed->paintEntireComponent (g, true);
    }

    // Snapshot of the cheque for the docs (hidden test writes it when asked).
    if (const auto path = juce::SystemStats::getEnvironmentVariable ("CASH_SNAPSHOT", {}); path.isNotEmpty())
    {
        pl.p.getMatcher().runBlocking (pl.p.getPatch(), cash::defaultPatch (pl.p.getState()), 1, { 8, 2, 6, 0 });
        ed->getMatchPanel().sync();
        juce::Image hi (juce::Image::ARGB, CashSynthAudioProcessorEditor::width * 2, CashSynthAudioProcessorEditor::height * 2, true);
        juce::Graphics g (hi);
        g.addTransform (juce::AffineTransform::scale (2.0f));
        ed->paintEntireComponent (g, true);
        auto file = juce::File (path).getSiblingFile ("editor-match.png");
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (hi, os);
    }

    ed->getMatchPanel().getCloseButton().onClick();
    CHECK_FALSE (ed->isShowingMatch());
    CHECK (ed->getFront().isVisible());
}
