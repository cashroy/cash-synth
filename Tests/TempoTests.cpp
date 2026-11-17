#include "Layers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "AdvancedPanel.h"
#include "CashCode.h"
#include "Mutator.h"
#include "Parameters.h"
#include "Tempo.h"

// Milestone 10: tempo sync for the LFO rates.
namespace
{
using namespace cash::param;
using Catch::Approx;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

// A host whose transport the test drives.
struct FakeHost final : juce::AudioPlayHead
{
    bool present = true;
    double bpm = 120.0, ppq = 0.0;
    int num = 4, den = 4;
    bool playing = false;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! present)
            return {};
        PositionInfo pos;
        pos.setBpm (bpm);
        pos.setTimeSignature (TimeSignature { num, den });
        pos.setPpqPosition (ppq);
        pos.setIsPlaying (playing);
        return pos;
    }
};

struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CashSynthAudioProcessor p;
    FakeHost host;
    juce::AudioBuffer<float> buffer { 2, blockSize };
    juce::MidiBuffer midi;

    Rig()
    {
        p.setPlayHead (&host);
        p.prepareToPlay (sampleRate, blockSize);
    }
    ~Rig() { p.releaseResources(); }

    juce::RangedAudioParameter& param (const char* id)
    {
        auto* q = p.getState().getParameter (id);
        REQUIRE (q != nullptr);
        return *q;
    }
    void set (const char* id, float real) { param (id).setValueNotifyingHost (param (id).convertTo0to1 (real)); }

    // One host block: the play head reports the block start; the song moves on while playing.
    void block (int n = blockSize)
    {
        juce::AudioBuffer<float> b (2, n);
        midi.clear();
        p.processBlock (b, midi);
        if (host.playing)
            host.ppq += host.bpm / 60.0 * n / sampleRate;
    }
    void run (double seconds)
    {
        for (int i = 0, n = static_cast<int> (seconds * sampleRate / blockSize); i < n; ++i)
            block();
    }
    double phase (int lfo = 0) const { return p.getEngine().getLfoPhase (lfo); }
};

cash::tempo::Info info (double bpm, int num, int den)
{
    cash::tempo::Info t;
    t.bpm = bpm;
    t.sigNum = num;
    t.sigDen = den;
    return t;
}

int note (const char* name)
{
    const int i = cash::tempo::noteNames().indexOf (name);
    REQUIRE (i >= 0);
    return i;
}

double wrapDiff (double a, double b)
{
    double d = a - b;
    return d - std::floor (d + 0.5);
}
}

TEST_CASE ("Note values: every length, straight, dotted and triplet", "[m10][tempo]")
{
    const auto names = cash::tempo::noteNames();
    REQUIRE (names.size() == cash::tempo::numNotes);
    CHECK (names[cash::tempo::defaultNote] == "1/4");
    for (const auto* base : { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32" })
        for (const auto* kind : { "", " D", " T" })
        {
            INFO (base << kind);
            CHECK (names.contains (juce::String (base) + kind));
        }

    // Slowest first: the knob only ever speeds up as it turns (in 4/4).
    const auto common = info (120.0, 4, 4);
    for (int i = 1; i < cash::tempo::numNotes; ++i)
        CHECK (cash::tempo::noteQuarters (i, common) < cash::tempo::noteQuarters (i - 1, common));

    struct Case { double bpm; int num, den; const char* note; double hz; };
    const Case cases[] {
        { 120.0, 4, 4, "1/4", 2.0 },            { 120.0, 4, 4, "1 bar", 0.5 },
        { 120.0, 4, 4, "1/16 D", 2.0 / 0.375 }, { 100.0, 4, 4, "1/8 T", 5.0 },
        { 90.0, 3, 4, "1 bar", 0.5 },           { 90.0, 3, 4, "1/4", 1.5 },          // bars follow the time signature,
        { 140.0, 7, 8, "1 bar", 140.0 / 60.0 / 3.5 }, { 140.0, 7, 8, "1/8", 140.0 / 60.0 * 2.0 }, // note values don't
        { 60.0, 6, 8, "4 bars D", 1.0 / 18.0 }, { 174.0, 5, 4, "2 bars T", 174.0 / 60.0 / (10.0 * 2.0 / 3.0) },
        { 75.5, 4, 4, "1/32 T", 75.5 / 60.0 / (0.125 * 2.0 / 3.0) },  { 200.0, 2, 2, "1 bar D", 200.0 / 60.0 / 6.0 },
        { 128.0, 4, 4, "1/2 D", 128.0 / 60.0 / 3.0 },                  { 128.0, 4, 4, "4 bars", 128.0 / 60.0 / 16.0 },
    };
    for (const auto& c : cases)
    {
        INFO (c.note << " @ " << c.bpm << " BPM " << c.num << "/" << c.den);
        const auto t = info (c.bpm, c.num, c.den);
        CHECK (cash::tempo::noteHz (note (c.note), t) == Approx (c.hz).epsilon (1.0e-12));
        CHECK (cash::tempo::nearestNote (c.hz, t) == note (c.note));
    }

    // The note value's host text survives the round trip (automation lanes).
    Rig r;
    auto& q = r.param (lfo1Note);
    for (int i = 0; i < cash::tempo::numNotes; ++i)
    {
        const float v = q.convertTo0to1 (static_cast<float> (i));
        CHECK (q.getValueForText (q.getText (v, 32)) == Approx (v));
    }
}

TEST_CASE ("The tempo always comes from the host, 120 BPM without one", "[m10][tempo]")
{
    const auto none = cash::tempo::fromPlayHead (nullptr);
    CHECK (none.bpm == 120.0);
    CHECK (none.sigNum == 4);
    CHECK (none.sigDen == 4);
    CHECK_FALSE (none.locked());

    FakeHost host;
    host.present = false; // a play head that knows nothing (some standalone hosts)
    CHECK (cash::tempo::fromPlayHead (&host).bpm == 120.0);

    host.present = true;
    host.bpm = 93.0;
    host.num = 7;
    host.den = 8;
    host.ppq = 12.5;
    host.playing = true;
    const auto t = cash::tempo::fromPlayHead (&host);
    CHECK (t.bpm == 93.0);
    CHECK (t.sigNum == 7);
    CHECK (t.sigDen == 8);
    CHECK (t.barQuarters() == 3.5);
    CHECK (t.locked());
    CHECK (t.ppq == 12.5);

    host.bpm = 0.0; // nonsense from a host: ignored
    CHECK (cash::tempo::fromPlayHead (&host).bpm == 120.0);

    // The processor follows it every block.
    Rig r;
    r.host.bpm = 87.0;
    r.host.num = 5;
    r.block();
    CHECK (r.p.getHostTempo().bpm == 87.0);
    CHECK (r.p.getHostTempo().sigNum == 5);
    r.p.setPlayHead (nullptr);
    r.block();
    CHECK (r.p.getHostTempo().bpm == 120.0);
}

TEST_CASE ("A synced LFO runs at the note value's rate when stopped", "[m10][tempo][engine]")
{
    struct Case { double bpm; int num, den; const char* note; };
    for (const auto& c : { Case { 120.0, 4, 4, "1/4" }, Case { 90.0, 3, 4, "1 bar" }, Case { 140.0, 7, 8, "1/8 D" },
                           Case { 60.0, 6, 8, "1/16 T" }, Case { 174.0, 4, 4, "2 bars" } })
    {
        INFO (c.note << " @ " << c.bpm << " BPM " << c.num << "/" << c.den);
        Rig r;
        r.host.bpm = c.bpm;
        r.host.num = c.num;
        r.host.den = c.den;
        r.set (lfo1Sync, syncMode);
        r.set (lfo1Note, static_cast<float> (note (c.note)));
        r.set (lfoRate, 7.3f); // ignored in SYNC
        r.run (0.1); // the rate ramps to its new value

        const double before = r.phase();
        const int blocks = 40;
        for (int i = 0; i < blocks; ++i)
            r.block();
        const double seconds = blocks * blockSize / sampleRate;
        const double hz = cash::tempo::noteHz (note (c.note), info (c.bpm, c.num, c.den));
        CHECK (std::abs (wrapDiff (r.phase(), before + hz * seconds)) < 1.0e-3);
    }

    // TIME mode ignores the host: same Hz at any tempo.
    Rig r;
    r.host.bpm = 77.0;
    r.set (lfoRate, 3.0f);
    r.run (0.1);
    const double before = r.phase();
    r.run (0.4267); // 40 blocks
    CHECK (std::abs (wrapDiff (r.phase(), before + 3.0 * 40 * blockSize / sampleRate)) < 1.0e-3);
}

TEST_CASE ("While the host plays, synced LFOs lock to the song position", "[m10][tempo][engine]")
{
    struct Case { double bpm; int num, den; const char* note; double startPpq; };
    for (const auto& c : { Case { 120.0, 4, 4, "1/4", 0.0 }, Case { 97.0, 3, 4, "1 bar", 7.25 }, Case { 140.0, 7, 8, "1 bar", 0.0 },
                           Case { 128.0, 5, 4, "1/4 D", 33.3 }, Case { 60.0, 6, 8, "2 bars T", 100.0 }, Case { 170.0, 4, 4, "1/32 T", 3.0 } })
    {
        INFO (c.note << " @ " << c.bpm << " BPM " << c.num << "/" << c.den << " from ppq " << c.startPpq);
        Rig r;
        r.host.bpm = c.bpm;
        r.host.num = c.num;
        r.host.den = c.den;
        r.set (lfo1Sync, syncMode);
        r.set (lfo1Note, static_cast<float> (note (c.note)));
        r.set (lfo2Sync, syncMode);
        r.set (lfo2Note, static_cast<float> (note ("1/8")));
        r.run (0.2); // stopped: free-running somewhere

        const double len = cash::tempo::noteQuarters (note (c.note), info (c.bpm, c.num, c.den));
        auto expected = [&r] (double l) { const double x = r.host.ppq / l; return x - std::floor (x); };

        r.host.ppq = c.startPpq;
        r.host.playing = true; // press play: locked from the first sample
        for (int i = 0; i < 60; ++i)
        {
            r.block();
            CHECK (std::abs (wrapDiff (r.phase (0), expected (len))) < 1.0e-6);
            CHECK (std::abs (wrapDiff (r.phase (1), expected (0.5))) < 1.0e-6);
        }

        // Seek / loop: straight to the new song position.
        r.host.ppq = c.startPpq + 17.37;
        r.block();
        CHECK (std::abs (wrapDiff (r.phase (0), expected (len))) < 1.0e-6);

        // Stop: free-runs on at the synced rate from where it was.
        r.host.playing = false;
        const double at = r.phase (0);
        r.block();
        const double hz = c.bpm / 60.0 / len;
        CHECK (std::abs (wrapDiff (r.phase (0), at + hz * blockSize / sampleRate)) < 1.0e-4);
    }
}

TEST_CASE ("TIME-mode LFOs ignore the song position", "[m10][tempo][engine]")
{
    Rig a, b;
    a.host.playing = true;
    a.host.ppq = 13.7;
    for (auto* r : { &a, &b })
        r->set (lfoRate, 4.4f);
    for (int i = 0; i < 30; ++i)
    {
        a.block();
        b.block();
    }
    CHECK (a.phase() == b.phase());
}

TEST_CASE ("Changing note value or tempo mid-note stays smooth", "[m10][tempo][clicks]")
{
    // LFO 1 on the cutoff (and LFO 2 on the amp), a held note, transport playing.
    auto render = [] (Rig& r, double seconds, auto&& each)
    {
        std::vector<float> out;
        for (int i = 0, n = static_cast<int> (seconds * sampleRate / 64); i < n; ++i)
        {
            each (i * 64 / sampleRate);
            juce::AudioBuffer<float> b (2, 64);
            juce::MidiBuffer m;
            if (i == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 48, 1.0f), 0);
            r.p.processBlock (b, m);
            if (r.host.playing)
                r.host.ppq += r.host.bpm / 60.0 * 64 / sampleRate;
            for (int k = 0; k < 64; ++k)
                out.push_back (b.getSample (0, k));
        }
        return out;
    };
    auto maxJump = [] (const std::vector<float>& x, double from)
    {
        float m = 0.0f;
        for (auto i = static_cast<size_t> (from * sampleRate); i + 1 < x.size(); ++i)
            m = juce::jmax (m, std::abs (x[i + 1] - x[i]));
        return m;
    };
    auto setup = [] (Rig& r)
    {
        r.set (filterCutoff, 800.0f);
        r.set (filterReso, 0.0f);
        r.set (lfoToCutoff, 0.8f);
        r.set (lfoWave, sine);
        r.set (lfo1Sync, syncMode);
        r.set (lfo1Note, static_cast<float> (note ("1/8")));
        r.set (lfo2Amount, 0.6f);
        r.set (lfo2Target, toAmp);
        r.set (lfo2Sync, syncMode);
        r.set (lfo2Note, static_cast<float> (note ("1/4 T")));
        r.host.playing = true;
        r.p.releaseResources();
        r.p.prepareToPlay (sampleRate, blockSize);
    };

    Rig steady;
    setup (steady);
    const float ref = maxJump (render (steady, 1.2, [] (double) {}), 0.1);
    REQUIRE (ref > 0.0f);

    Rig changing;
    setup (changing);
    bool switched = false;
    const auto out = render (changing, 1.2, [&] (double t)
    {
        if (t >= 0.4 && ! switched)
        {
            switched = true;
            changing.set (lfo1Note, static_cast<float> (note ("1/16 D"))); // note value change: glides onto the grid
            changing.set (lfo2Sync, timeMode);                            // mode switch: rate ramps
        }
        changing.host.bpm = t < 0.7 ? 120.0 : 120.0 + (t - 0.7) * 100.0;   // tempo ramp, as automation
    });
    CHECK (maxJump (out, 0.1) <= ref * 2.0f + 1.0e-3f);
    for (const float x : out)
        REQUIRE (std::isfinite (x));

    // And it ends up locked to the new grid.
    const double len = cash::tempo::noteQuarters (note ("1/16 D"), info (120.0, 4, 4));
    const double x = changing.host.ppq / len;
    CHECK (std::abs (wrapDiff (changing.phase (0), x - std::floor (x))) < 1.0e-6);
}

TEST_CASE ("Old codes, projects and stashes load unsynced", "[m10][state]")
{
    Rig r;
    auto& st = r.p.getState();
    const auto defaults = cash::defaultPatch (st);
    auto at = [] (const char* id) { return static_cast<size_t> (cash::ParameterMap::indexOf (id)); };

    // A v4 (M8/M9) code: everything it carried, plus TIME mode.
    const auto v4 = cash::CashCode::decode ("$94S0-6CJC-H200-001C-1KMR-02MY-ACF4-N73K-3H58-000B-5HG0-00AJ-0080-0004-100C-003Z-W182-A", defaults);
    REQUIRE (v4.has_value());
    CHECK (cash::samePatch (*v4, defaults, 0.5f / 255.0f + 1.0e-6f)); // codes are 8-bit
    CHECK ((*v4)[at (lfo1Sync)] == 0.0f);
    CHECK ((*v4)[at (lfo2Sync)] == 0.0f);

    // v5 round trip keeps a synced patch.
    auto synced = defaults;
    synced[at (lfo1Sync)] = 1.0f;
    synced[at (lfo2Note)] = cash::ParameterMap::fromStep (cash::ParameterMap::spec (static_cast<int> (at (lfo2Note))), note ("1/8 T"));
    const auto back = cash::CashCode::decode (cash::CashCode::encode (synced), defaults);
    REQUIRE (back.has_value());
    CHECK (cash::samePatch (*back, synced, 0.5f / 255.0f + 1.0e-6f));

    // An M9 project (no M10 params) loaded into an instance that is synced.
    juce::MemoryBlock block;
    r.p.getStateInformation (block);
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    auto root = juce::ValueTree::fromXml (*xml);
    auto params = root.getChildWithName ("PARAMS");
    for (auto* id : { lfo1Sync, lfo1Note, lfo2Sync, lfo2Note })
        params.removeChild (params.getChildWithProperty ("id", id), nullptr);
    for (int k = cash::ParameterMap::numParamsV5; k < cash::ParameterMap::numParams; ++k) // nor M11's effects
        params.removeChild (params.getChildWithProperty ("id", cash::ParameterMap::spec (k).id), nullptr);
    REQUIRE (params.getNumChildren() == cash::ParameterMap::numParamsV4);

    r.set (lfo1Sync, syncMode);
    r.set (lfo2Sync, syncMode);
    r.set (lfo1Note, 3.0f);
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), old);
    r.p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    CHECK (r.param (lfo1Sync).getValue() == 0.0f);
    CHECK (r.param (lfo2Sync).getValue() == 0.0f);
    CHECK (juce::roundToInt (r.param (lfo1Note).convertFrom0to1 (r.param (lfo1Note).getValue())) == cash::tempo::defaultNote);

    // Stash / vault notes without the new IDs keep the fallback (default = TIME).
    auto tree = cash::patchToTree (synced, "Patch");
    tree.removeProperty (lfo1Sync, nullptr);
    CHECK (cash::patchFromTree (tree, defaults)[at (lfo1Sync)] == 0.0f);
}

TEST_CASE ("Money moves and Cash Match with synced rates", "[m10][money]")
{
    Rig r;
    const auto base = cash::defaultPatch (r.p.getState());
    auto at = [] (const char* id) { return static_cast<size_t> (cash::ParameterMap::indexOf (id)); };

    int synced = 0;
    const int rolls = 400;
    for (int i = 0; i < rolls; ++i)
    {
        const auto p = cash::Mutator::jackpot (base, static_cast<uint64_t> (i) * 7919u + 3u);
        synced += p[at (lfo1Sync)] > 0.5f ? 1 : 0;
    }
    CHECK (synced > rolls / 20);     // now and then...
    CHECK (synced < rolls * 3 / 10); // ...not usually

    // A locked sync switch stays put.
    cash::LockMask locks;
    locks.set (at (lfo1Sync));
    auto on = base;
    on[at (lfo1Sync)] = 1.0f;
    for (int i = 0; i < 50; ++i)
        CHECK (cash::Mutator::jackpot (on, static_cast<uint64_t> (i), locks)[at (lfo1Sync)] == 1.0f);

    auto p = on;
    cash::unsync (p);
    CHECK (p[at (lfo1Sync)] == 0.0f);
}

TEST_CASE ("TIME / SYNC toggle on the back of the bill", "[m10][ui]")
{
    Rig r;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor> (r.p.createEditor());
    auto* ed = dynamic_cast<CashSynthAudioProcessorEditor*> (editor.get());
    REQUIRE (ed != nullptr);
    auto& panel = ed->getAdvancedPanel();
    CHECK (panel.getNumControls() == r.p.getParameters().size() - cash::layers::numParams); // M14: LAYERS controls come in part 2

    for (auto [syncId, rateId, noteId] : { std::tuple { lfo1Sync, lfoRate, lfo1Note }, std::tuple { lfo2Sync, lfo2Rate, lfo2Note } })
    {
        INFO (syncId);
        auto* sw = panel.getRateMode (syncId);
        auto* rate = panel.getKnob (rateId);
        auto* noteKnob = panel.getKnob (noteId);
        REQUIRE (sw != nullptr);
        REQUIRE (rate != nullptr);
        REQUIRE (noteKnob != nullptr);
        CHECK (sw->getWidth() > 0);
        CHECK (rate->getBounds() == noteKnob->getBounds()); // same place

        CHECK_FALSE (sw->isSynced());
        CHECK (rate->isVisible());
        CHECK_FALSE (noteKnob->isVisible());

        sw->toggle();
        CHECK (r.param (syncId).getValue() == 1.0f);
        CHECK_FALSE (rate->isVisible());
        CHECK (noteKnob->isVisible());
        CHECK (noteKnob->getTextFromValue (noteKnob->getValue()) == "1/4");

        noteKnob->setValue (note ("1/8 D"), juce::sendNotificationSync);
        CHECK (juce::roundToInt (r.param (noteId).convertFrom0to1 (r.param (noteId).getValue())) == note ("1/8 D"));

        r.param (syncId).setValueNotifyingHost (0.0f); // host automation flips it back
        CHECK_FALSE (sw->isSynced());
        CHECK (rate->isVisible());
        CHECK_FALSE (noteKnob->isVisible());
    }

    // Dims with its section.
    panel.getSwitch (lfo1On)->toggle();
    CHECK (panel.getRateMode (lfo1Sync)->getAlpha() < 1.0f);
}
