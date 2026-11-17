#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "AdvancedPanel.h"
#include "BillComponents.h"
#include "CashCode.h"
#include "CashMatch.h"
#include "Mutator.h"
#include "Parameters.h"
#include "SoundPrint.h"
#include "Tempo.h"
#include "GoldenScenes.h"

// Milestone 11: stereo delay + reverb after the voices.
namespace
{
using namespace cash::param;
using Catch::Approx;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

struct GoldenRender
{
    const char* name;
    std::vector<float> data;
};

const std::vector<GoldenRender>& goldenRenders()
{
    static const std::vector<GoldenRender> renders {
#include "GoldenRenders.inc"
    };
    return renders;
}

struct Host final : juce::AudioPlayHead
{
    double bpm = 120.0, ppq = 0.0;
    int num = 4, den = 4;
    bool playing = false;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo pos;
        pos.setBpm (bpm);
        pos.setTimeSignature (TimeSignature { num, den });
        pos.setPpqPosition (ppq);
        pos.setIsPlaying (playing);
        return pos;
    }
};

struct Fx
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    Host host;

    Fx() { p.setPlayHead (&host); }

    juce::RangedAudioParameter& param (const char* id)
    {
        auto* q = p.getState().getParameter (id);
        REQUIRE (q != nullptr);
        return *q;
    }
    void set (const char* id, float real) { param (id).setValueNotifyingHost (param (id).convertTo0to1 (real)); }
    void setNote (const char* id, const char* name) { set (id, static_cast<float> (cash::tempo::noteNames().indexOf (name))); }

    // A ~5 ms click of a note: easy to find again in the echoes.
    void blip()
    {
        set (ampAttack, 0.001f);
        set (ampDecay, 0.004f);
        set (ampSustain, 0.0f);
        set (ampRelease, 0.003f);
        set (filterCutoff, 12000.0f);
    }

    // Prepares (so every setting is snapped), then plays `notes` from 0 to `hold` s.
    juce::AudioBuffer<float> play (double seconds, double hold, std::vector<int> notes = { 60 },
                                   const std::function<void (double)>& everyBlock = {})
    {
        p.prepareToPlay (sampleRate, blockSize);
        const int total = static_cast<int> (seconds * sampleRate);
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        const int off = static_cast<int> (hold * sampleRate);
        for (int pos = 0; pos < total; pos += blockSize)
        {
            if (everyBlock)
                everyBlock (pos / sampleRate);
            const int n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> b (2, n);
            b.clear();
            juce::MidiBuffer midi;
            for (int k : notes)
            {
                if (pos == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, k, 1.0f), 0);
                if (off >= pos && off < pos + n)
                    midi.addEvent (juce::MidiMessage::noteOff (1, k), off - pos);
            }
            p.processBlock (b, midi);
            for (int c = 0; c < 2; ++c)
                out.copyFrom (c, pos, b, c, 0, n);
            if (host.playing)
                host.ppq += host.bpm / 60.0 * n / sampleRate;
        }
        p.releaseResources();
        return out;
    }
};

int onset (const juce::AudioBuffer<float>& b, int ch, int from, float threshold)
{
    for (int i = juce::jmax (0, from); i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (ch, i)) > threshold)
            return i;
    return -1;
}

float peakIn (const juce::AudioBuffer<float>& b, int ch, double from, double to)
{
    const int a = juce::jlimit (0, b.getNumSamples(), static_cast<int> (from * sampleRate));
    const int z = juce::jlimit (a, b.getNumSamples(), static_cast<int> (to * sampleRate));
    return z > a ? b.getMagnitude (ch, a, z - a) : 0.0f;
}

float rmsDb (const juce::AudioBuffer<float>& b, double from, double length)
{
    const int a = static_cast<int> (from * sampleRate), n = static_cast<int> (length * sampleRate);
    const float r = std::sqrt (0.5f * (juce::square (b.getRMSLevel (0, a, n)) + juce::square (b.getRMSLevel (1, a, n))));
    return juce::Decibels::gainToDecibels (r, -300.0f);
}

bool finite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i)))
                return false;
    return true;
}

double noteSeconds (const char* name, double bpm, int num, int den)
{
    cash::tempo::Info t;
    t.bpm = bpm;
    t.sigNum = num;
    t.sigDen = den;
    return cash::tempo::noteQuarters (cash::tempo::noteNames().indexOf (name), t) * 60.0 / bpm;
}

// Visible all the way up to `top` (the editor is never on a desktop in tests).
bool shown (const juce::Component* c, const juce::Component* top)
{
    for (; c != nullptr && c != top; c = c->getParentComponent())
        if (! c->isVisible())
            return false;
    return c == top;
}

size_t at (const char* id) { return static_cast<size_t> (cash::ParameterMap::indexOf (id)); }

void delayOnly (Fx& r, float feedback, float mix)
{
    r.set (delayOn, 1.0f);
    r.set (delayFeedback, feedback);
    r.set (delayMix, mix);
    r.set (delayTone, 0.0f);
}
}

//============================================================================== Delay
TEST_CASE ("Echoes land on the synced note value at any tempo", "[m11][delay][tempo]")
{
    struct Case { double bpm; int num, den; const char* note; };
    for (const auto& c : { Case { 90, 4, 4, "1/4" }, Case { 120, 4, 4, "1/8 D" }, Case { 137.5, 4, 4, "1/16 T" },
                           Case { 174, 4, 4, "1/8" }, Case { 100, 3, 4, "1 bar" }, Case { 60, 6, 8, "1/4 D" },
                           Case { 128, 7, 8, "1/2 T" } })
    {
        INFO (c.bpm << " BPM " << c.num << "/" << c.den << " " << c.note);
        Fx r;
        r.blip();
        delayOnly (r, 0.0f, 0.5f); // dry and wet both at full level
        r.set (delaySync, syncMode);
        r.setNote (delayNote, c.note);
        r.host.bpm = c.bpm;
        r.host.num = c.num;
        r.host.den = c.den;

        const double t = noteSeconds (c.note, c.bpm, c.num, c.den);
        REQUIRE (t < cash::Effects::maxDelaySeconds);
        const auto out = r.play (t + 0.25, 0.01);
        REQUIRE (finite (out));

        const float thr = 0.05f * out.getMagnitude (0, 0, out.getNumSamples());
        const int dry = onset (out, 0, 0, thr);
        const int echo = onset (out, 0, static_cast<int> (0.05 * sampleRate), thr);
        REQUIRE (dry >= 0);
        REQUIRE (echo > 0);
        CHECK (std::abs ((echo - dry) - t * sampleRate) <= 2.0);
        CHECK (r.p.getEngine().getEffects().delaySeconds (cash::tempo::fromPlayHead (&r.host)) == Approx (t).epsilon (1.0e-9));
    }
}

TEST_CASE ("TIME-mode delay ignores the tempo; long synced times are clamped", "[m11][delay][tempo]")
{
    for (const double bpm : { 80.0, 150.0 })
    {
        INFO (bpm);
        Fx r;
        r.blip();
        delayOnly (r, 0.0f, 0.5f);
        r.set (delaySync, timeMode);
        r.set (delayTime, 250.0f);
        r.host.bpm = bpm;
        const auto out = r.play (0.5, 0.01);
        const float thr = 0.05f * out.getMagnitude (0, 0, out.getNumSamples());
        const int dry = onset (out, 0, 0, thr), echo = onset (out, 0, static_cast<int> (0.05 * sampleRate), thr);
        CHECK (std::abs ((echo - dry) - 0.25 * sampleRate) <= 2.0);
    }

    Fx r;
    r.set (delaySync, syncMode);
    r.setNote (delayNote, "4 bars D"); // 24 s at 60 BPM
    cash::tempo::Info slow;
    slow.bpm = 60.0;
    CHECK (r.p.getEngine().getEffects().delaySeconds (slow) == cash::Effects::maxDelaySeconds);
}

TEST_CASE ("Feedback repeats, ping-pong alternates sides, tone darkens", "[m11][delay]")
{
    const double t = 0.2;
    auto run = [t] (float fb, bool pp, float tone)
    {
        Fx r;
        r.blip();
        delayOnly (r, fb, 0.5f);
        r.set (delaySync, timeMode);
        r.set (delayTime, static_cast<float> (t * 1000.0));
        r.set (delayPingPong, pp ? 1.0f : 0.0f);
        r.set (delayTone, tone);
        return r.play (5 * t, 0.01);
    };

    // Stereo: each repeat at half the level of the one before (feedback 50 %, tone flat).
    const auto st = run (0.5f, false, 0.0f);
    const float e1 = peakIn (st, 0, t - 0.01, t + 0.03), e2 = peakIn (st, 0, 2 * t - 0.01, 2 * t + 0.03);
    REQUIRE (e1 > 0.01f);
    CHECK (e2 / e1 == Approx (0.5f).margin (0.05f));
    CHECK (peakIn (st, 1, t - 0.01, t + 0.03) == Approx (e1).margin (1.0e-5f)); // centred source, both sides

    // Ping-pong: echo 1 on the left only, echo 2 on the right only.
    const auto pp = run (0.5f, true, 0.0f);
    const float l1 = peakIn (pp, 0, t - 0.01, t + 0.03), r1 = peakIn (pp, 1, t - 0.01, t + 0.03);
    const float l2 = peakIn (pp, 0, 2 * t - 0.01, 2 * t + 0.03), r2 = peakIn (pp, 1, 2 * t - 0.01, 2 * t + 0.03);
    INFO ("L1 " << l1 << " R1 " << r1 << " L2 " << l2 << " R2 " << r2);
    CHECK (l1 > 0.01f);
    CHECK (r1 < l1 * 0.01f);
    CHECK (r2 > 0.005f);
    CHECK (l2 < r2 * 0.01f);
    CHECK (finite (pp));

    // Tone: dark repeats lose level (the click is mostly highs); the dry note is the same.
    const auto dark = run (0.5f, false, -1.0f);
    CHECK (peakIn (dark, 0, t - 0.01, t + 0.03) < e1 * 0.7f);
    CHECK (peakIn (dark, 0, 0.0, 0.02) == Approx (peakIn (st, 0, 0.0, 0.02)).margin (1.0e-6f));
}

//============================================================================== Reverb
TEST_CASE ("Reverb tail rings, decays at the set time and dies away", "[m11][reverb]")
{
    auto run = [] (float decay, float width, float pre, double seconds = 3.0)
    {
        Fx r;
        r.blip();
        r.set (reverbOn, 1.0f);
        r.set (reverbMix, 1.0f); // wet only
        r.set (reverbDecay, decay);
        r.set (reverbDamp, 0.0f);
        r.set (reverbSize, 0.5f);
        r.set (reverbWidth, width);
        r.set (reverbPreSync, timeMode);
        r.set (reverbPreDelay, pre);
        return r.play (seconds, 0.01);
    };

    const auto oneSec = run (1.0f, 1.0f, 0.0f);
    REQUIRE (finite (oneSec));
    const float a = rmsDb (oneSec, 0.2, 0.05), b = rmsDb (oneSec, 0.7, 0.05);
    INFO ("1 s decay: " << a << " dB -> " << b << " dB");
    CHECK (a > -60.0f);              // it rings after the 5 ms click
    CHECK (a - b == Approx (30.0f).margin (6.0f)); // RT60 1 s: -30 dB in 0.5 s
    CHECK (rmsDb (oneSec, 2.9, 0.05) < a - 120.0f); // and dies away

    const auto threeSec = run (3.0f, 1.0f, 0.0f);
    const float c = rmsDb (threeSec, 0.2, 0.05), d = rmsDb (threeSec, 0.7, 0.05);
    INFO ("3 s decay: " << c << " dB -> " << d << " dB");
    CHECK (c - d == Approx (10.0f).margin (4.0f));

    // Width: 0 = the same on both sides; 100 % = a wide, decorrelated tail.
    const auto mono = run (1.0f, 0.0f, 0.0f, 1.0);
    for (int i = 0; i < mono.getNumSamples(); ++i)
        REQUIRE (mono.getSample (0, i) == mono.getSample (1, i));
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = static_cast<int> (0.05 * sampleRate); i < static_cast<int> (0.5 * sampleRate); ++i)
    {
        const double l = oneSec.getSample (0, i), r = oneSec.getSample (1, i);
        lr += l * r; ll += l * l; rr += r * r;
    }
    CHECK (std::abs (lr) / std::sqrt (ll * rr) < 0.6);

    // Pre-delay holds the tail back (TIME in ms, or synced: 1/16 at 120 = 125 ms).
    const auto late = run (1.0f, 1.0f, 100.0f, 0.5);
    const float thr = 1.0e-4f;
    const int start = onset (late, 0, 0, thr);
    INFO ("pre-delay 100 ms: tail starts at " << start / sampleRate * 1000.0 << " ms");
    CHECK (start >= static_cast<int> (0.1 * sampleRate));
    CHECK (start < static_cast<int> (0.15 * sampleRate));

    // Wet level: a held note's tail is about as loud as the note itself.
    auto held = [] (bool on)
    {
        Fx h;
        h.set (reverbOn, on ? 1.0f : 0.0f);
        h.set (reverbMix, 1.0f);
        return h.play (2.0, 2.0);
    };
    const float wetDb = rmsDb (held (true), 1.0, 0.5), dryDb = rmsDb (held (false), 1.0, 0.5);
    INFO ("held note: dry " << dryDb << " dB, wet " << wetDb << " dB");
    CHECK (std::abs (wetDb - dryDb) < 6.0f);

    Fx s;
    s.set (reverbPreSync, syncMode);
    s.setNote (reverbPreNote, "1/16");
    CHECK (s.p.getEngine().getEffects().preDelaySeconds ({}) == Approx (0.125));
}

//============================================================================== Old sounds stay old
TEST_CASE ("Mix 0 is exactly dry; effects off leave the sound untouched", "[m11][golden]")
{
    auto chord = [] (const std::function<void (Fx&)>& setup)
    {
        Fx r;
        r.set (ampRelease, 0.3f);
        setup (r);
        return r.play (1.0, 0.4, { 48, 55, 60 });
    };
    const auto dry = chord ([] (Fx&) {});

    // Both effects on, every knob away from home, mixes at 0: bit for bit the dry sound.
    const auto zero = chord ([] (Fx& r)
    {
        r.set (delayOn, 1.0f);   r.set (delayFeedback, 0.8f); r.set (delayPingPong, 1.0f); r.set (delayTone, 0.5f);
        r.set (delayMix, 0.0f);
        r.set (reverbOn, 1.0f);  r.set (reverbSize, 0.9f);    r.set (reverbDecay, 8.0f);   r.set (reverbPreDelay, 60.0f);
        r.set (reverbMix, 0.0f);
    });
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < dry.getNumSamples(); ++i)
            REQUIRE (zero.getSample (c, i) == dry.getSample (c, i));

    // ... and with some mix they do something.
    const auto wet = chord ([] (Fx& r) { r.set (delayOn, 1.0f); r.set (delayMix, 0.3f); r.set (reverbOn, 1.0f); r.set (reverbMix, 0.3f); });
    CHECK (rmsDb (wet, 0.8, 0.1) > rmsDb (dry, 0.8, 0.1) + 20.0f); // the tail outlives the release
}

TEST_CASE ("Old codes, projects and stashes load with the effects off and sound the same", "[m11][state][golden]")
{
    Fx r;
    auto& st = r.p.getState();
    const auto defaults = cash::defaultPatch (st);
    CHECK (defaults[at (delayOn)] == 0.0f);
    CHECK (defaults[at (reverbOn)] == 0.0f);

    // The M10 (v5) default code.
    const auto v5 = cash::CashCode::decode ("$B4S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-003Z-WTD5-P70", defaults);
    REQUIRE (v5.has_value());
    CHECK (cash::samePatch (*v5, defaults, 0.5f / 255.0f + 1.0e-6f));
    CHECK ((*v5)[at (delayOn)] == 0.0f);
    CHECK ((*v5)[at (reverbOn)] == 0.0f);

    // A v5 code decoded on top of a fallback with effects on still gets... the fallback's
    // values for params it doesn't carry; callers pass the default patch, so: off.
    // v6 round trip keeps effects on.
    auto fx = defaults;
    fx[at (delayOn)] = 1.0f;
    fx[at (reverbOn)] = 1.0f;
    fx[at (delayPingPong)] = 1.0f;
    fx[at (reverbMix)] = 0.4f;
    const auto back = cash::CashCode::decode (cash::CashCode::encode (fx), defaults);
    REQUIRE (back.has_value());
    CHECK (cash::samePatch (*back, fx, 0.5f / 255.0f + 1.0e-6f));

    // An M10 project (no M11 params) loaded into an instance with both effects on.
    juce::MemoryBlock block;
    r.p.getStateInformation (block);
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    auto root = juce::ValueTree::fromXml (*xml);
    auto params = root.getChildWithName ("PARAMS");
    for (int k = cash::ParameterMap::numParamsV5; k < cash::ParameterMap::numParams; ++k)
        params.removeChild (params.getChildWithProperty ("id", cash::ParameterMap::spec (k).id), nullptr);
    REQUIRE (params.getNumChildren() == cash::ParameterMap::numParamsV5);

    r.set (delayOn, 1.0f);
    r.set (reverbOn, 1.0f);
    r.set (delayMix, 0.9f);
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), old);
    r.p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    CHECK (r.param (delayOn).getValue() == 0.0f);
    CHECK (r.param (reverbOn).getValue() == 0.0f);
    CHECK (r.param (delayMix).getValue() == Approx (r.param (delayMix).getDefaultValue()));

    // ... and sounds exactly like the old engine's golden render.
    const auto scene = golden::scenes().front();
    const auto now = golden::render (r.p, scene);
    const auto& then = goldenRenders().front().data;
    REQUIRE (now.size() == then.size());
    float worst = 0.0f;
    for (size_t i = 0; i < now.size(); ++i)
        worst = juce::jmax (worst, std::abs (now[i] - then[i]));
    CHECK (worst < 1.0e-5f);

    // Stash / vault notes without the new IDs keep the fallback (default = off).
    auto tree = cash::patchToTree (fx, "Patch");
    tree.removeProperty (delayOn, nullptr);
    tree.removeProperty (reverbOn, nullptr);
    const auto loaded = cash::patchFromTree (tree, defaults);
    CHECK (loaded[at (delayOn)] == 0.0f);
    CHECK (loaded[at (reverbOn)] == 0.0f);
}

//============================================================================== Robustness
TEST_CASE ("Effects never blow up, even pushed and automated", "[m11][fuzz]")
{
    // Max feedback, ping-pong, a held chord: stays bounded.
    {
        Fx r;
        r.set (delayOn, 1.0f); r.set (delayFeedback, 0.95f); r.set (delayPingPong, 1.0f); r.set (delayMix, 1.0f);
        r.set (delayTone, 0.0f); r.set (delayTime, 30.0f);
        r.set (reverbOn, 1.0f); r.set (reverbDecay, 20.0f); r.set (reverbSize, 1.0f); r.set (reverbDamp, 0.0f); r.set (reverbMix, 1.0f);
        const auto out = r.play (6.0, 5.0, { 36, 48, 55, 60, 64 });
        CHECK (finite (out));
        CHECK (out.getMagnitude (0, out.getNumSamples()) < 8.0f);
    }
    // Times, sizes, notes, the tempo and the switches all moving while it plays.
    {
        Fx r;
        r.set (delayOn, 1.0f); r.set (delayFeedback, 0.7f); r.set (delayMix, 0.5f);
        r.set (reverbOn, 1.0f); r.set (reverbMix, 0.5f);
        r.host.playing = true;
        int k = 0;
        const auto out = r.play (3.0, 2.5, { 48, 60 }, [&r, &k] (double t)
        {
            ++k;
            r.set (delayTime, static_cast<float> (5.0 + 1900.0 * (0.5 + 0.5 * std::sin (t * 9.0))));
            r.set (reverbSize, static_cast<float> (0.5 + 0.5 * std::sin (t * 13.0)));
            r.set (reverbPreDelay, static_cast<float> (125.0 + 120.0 * std::sin (t * 7.0)));
            r.host.bpm = 70.0 + 100.0 * (k % 7) / 6.0;
            if (k % 20 == 0) r.set (delaySync, (k / 20) % 2 == 0 ? 1.0f : 0.0f);
            if (k % 25 == 0) r.set (reverbOn, (k / 25) % 2 == 0 ? 0.0f : 1.0f);
            if (k % 9 == 0)  r.set (delayNote, static_cast<float> (k % cash::tempo::numNotes));
        });
        CHECK (finite (out));
        CHECK (out.getMagnitude (0, out.getNumSamples()) < 8.0f);
    }
    // The host hears about the tail.
    Fx r;
    CHECK (r.p.getTailLengthSeconds() == 0.0);
    r.set (reverbOn, 1.0f);
    r.set (reverbDecay, 3.0f);
    CHECK (r.p.getTailLengthSeconds() >= 3.0);
}

//============================================================================== Money moves + Cash Match
TEST_CASE ("Jackpot and Invest keep the effects in the background", "[m11][money]")
{
    Fx r;
    const auto base = cash::defaultPatch (r.p.getState());
    const float fbCap = delayFeedbackRange().convertTo0to1 (0.6f) + 1.0e-6f;
    const float decayCap = reverbDecayRange().convertTo0to1 (5.0f) + 1.0e-6f;

    int delays = 0, reverbs = 0;
    const int rolls = 400;
    for (int i = 0; i < rolls; ++i)
    {
        const auto p = cash::Mutator::jackpot (base, static_cast<uint64_t> (i) * 7919u + 5u);
        if (p[at (delayOn)] > 0.5f)
        {
            ++delays;
            CHECK (p[at (delayMix)] <= 0.3f);
            CHECK (p[at (delayFeedback)] <= fbCap);
        }
        if (p[at (reverbOn)] > 0.5f)
        {
            ++reverbs;
            CHECK (p[at (reverbMix)] <= 0.3f);
            CHECK (p[at (reverbDecay)] <= decayCap);
        }
    }
    CHECK (delays > rolls / 10);
    CHECK (delays < rolls * 3 / 10);
    CHECK (reverbs > rolls / 5);
    CHECK (reverbs < rolls * 2 / 5);

    // Invest: an effect it switches on or moves stays modest; one the user set and the
    // move didn't touch keeps the user's level.
    auto mine = base;
    mine[at (delayOn)] = 1.0f;
    mine[at (delayMix)] = 0.8f;
    int kept = 0;
    for (uint64_t seed = 1; seed <= 200; ++seed)
    {
        const auto p = cash::Mutator::invest (mine, { 0.5f, 0.5f, seed });
        if (p[at (delayOn)] < 0.5f)
            continue;
        if (p[at (delayMix)] == mine[at (delayMix)])
            ++kept;
        else
            CHECK (p[at (delayMix)] <= 0.3f);
    }
    CHECK (kept > 50);
    for (uint64_t seed = 1; seed <= 200; ++seed)
    {
        const auto p = cash::Mutator::invest (base, { 1.0f, 1.0f, seed });
        if (p[at (reverbOn)] > 0.5f)
            CHECK (p[at (reverbMix)] <= 0.3f);
    }

    // A locked level is never capped.
    cash::LockMask locks;
    locks.set (at (delayMix));
    auto loud = mine;
    for (int i = 0; i < 50; ++i)
        CHECK (cash::Mutator::jackpot (loud, static_cast<uint64_t> (i), locks)[at (delayMix)] == 0.8f);
}

TEST_CASE ("Cash Match leaves the effects off", "[m11][match]")
{
    Fx r;
    const auto& s = r.p.getState();
    auto& m = r.p.getMatcher();
    const double sr = cash::SoundPrint::sampleRate;

    // A target with a reverb tail, and a start patch with both effects on.
    auto start = cash::defaultPatch (s);
    start[at (delayOn)] = 1.0f;
    start[at (reverbOn)] = 1.0f;
    cash::PatchRenderer render (s);
    juce::AudioBuffer<float> target;
    render.render (start, 52, 0.4, 1.2, target);
    REQUIRE (m.setTarget (target, sr, "Wet").isEmpty());

    m.runBlocking (start, cash::defaultPatch (s), 11, { 8, 2, 6, 1 });
    const auto res = m.getResult();
    REQUIRE (res.found);
    CHECK (res.patch[at (delayOn)] == 0.0f);
    CHECK (res.patch[at (reverbOn)] == 0.0f);
    for (int i = cash::ParameterMap::numParamsV5; i < cash::ParameterMap::numParamsV6; ++i)
        if (i != static_cast<int> (at (delayOn)) && i != static_cast<int> (at (reverbOn)))
            CHECK (res.patch[static_cast<size_t> (i)] == start[static_cast<size_t> (i)]); // left alone
}

//============================================================================== Back of the bill
TEST_CASE ("Effects page on the back of the bill", "[m11][ui]")
{
    Fx r;
    r.p.prepareToPlay (sampleRate, blockSize);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor> (r.p.createEditor());
    auto* ed = dynamic_cast<CashSynthAudioProcessorEditor*> (editor.get());
    REQUIRE (ed != nullptr);
    auto& panel = ed->getAdvancedPanel();
    CHECK (panel.getNumControls() == r.p.getParameters().size());
    CHECK (panel.getPage() == cash::AdvancedPanel::synthPage);

    auto* time = panel.getKnob (delayTime);
    auto* mix = panel.getKnob (reverbMix);
    auto* cutoff = panel.getKnob (filterCutoff);
    REQUIRE (time != nullptr);
    REQUIRE (mix != nullptr);
    CHECK_FALSE (shown (time, ed));
    CHECK (shown (cutoff, ed) == ed->isFlipped());

    ed->flip();
    auto& tabs = ed->getBack().getPageTabs();
    CHECK (shown (&tabs, ed));
    tabs.choose (cash::AdvancedPanel::effectsPage);
    CHECK (panel.getPage() == cash::AdvancedPanel::effectsPage);
    CHECK (shown (panel.getKnob (delayNote), ed)); // the delay time is synced by default
    CHECK (shown (mix, ed));
    CHECK_FALSE (shown (cutoff, ed));
    CHECK (mix->getHeight() == cutoff->getHeight()); // same knob size as the synth page
    CHECK (panel.getBounds().contains (mix->getBounds()));

    // On/off switches drive the params and dim their section; off by default.
    for (auto [sw, knob] : { std::pair { delayOn, delayFeedback }, std::pair { reverbOn, reverbSize } })
    {
        INFO (sw);
        auto* s = panel.getSwitch (sw);
        REQUIRE (s != nullptr);
        CHECK (shown (s, ed));
        CHECK_FALSE (s->isOn());
        CHECK (panel.getControlAlpha (knob) < 1.0f);
        s->toggle();
        CHECK (r.param (sw).getValue() == 1.0f);
        CHECK (panel.getControlAlpha (knob) == 1.0f);
    }

    // TIME / SYNC over the delay time (synced by default) and the pre-delay.
    auto* dsync = panel.getRateMode (delaySync);
    auto* psync = panel.getRateMode (reverbPreSync);
    REQUIRE (dsync != nullptr);
    REQUIRE (psync != nullptr);
    CHECK (dsync->isSynced());
    CHECK_FALSE (psync->isSynced());
    auto* dnote = panel.getKnob (delayNote);
    CHECK (dnote->isVisible());
    CHECK_FALSE (time->isVisible());
    CHECK (dnote->getTextFromValue (dnote->getValue()) == "1/8 D");
    dsync->toggle();
    CHECK (time->isVisible());
    CHECK (time->getTextFromValue (time->getValue()) == "375 ms");

    tabs.choose (cash::AdvancedPanel::synthPage);
    CHECK (shown (cutoff, ed));
    CHECK_FALSE (shown (mix, ed));
    r.p.releaseResources();
}
