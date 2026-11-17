#include "Layers.h"
#include "Mint.h"

namespace cash::layers
{
namespace
{
    const char* const toneNames[numToneFields] = {
        "Level", "Pan", "Octave", "Semi", "Fine", "Attack", "Decay", "Sustain", "Release",
        "FltType", "Cutoff", "Reso", "EnvAmt", "FltAttack", "FltDecay", "FltSustain", "FltRelease",
        "Lfo1Pitch", "Lfo1Level", "Lfo1Cutoff", "Lfo2Pitch", "Lfo2Level", "Lfo2Cutoff", "Curve"
    };
    const char* const toneLabels[numToneFields] = {
        "Level", "Pan", "Octave", "Semi", "Fine", "Attack", "Decay", "Sustain", "Release",
        "Filter Type", "Cutoff", "Resonance", "Env Amount", "Filter Attack", "Filter Decay", "Filter Sustain", "Filter Release",
        "LFO 1 Pitch", "LFO 1 Level", "LFO 1 Cutoff", "LFO 2 Pitch", "LFO 2 Level", "LFO 2 Cutoff", "Env Curve"
    };
    const char* const noiseNames[numNoiseFields] = { "Level", "Centre", "Width", "Attack", "Decay", "Sustain", "Release", "Pan", "Slope" };

    // ids() order (= ParameterMap order, append-only): the shared four, every v9 tone
    // field, every v9 noise field, then (v10) each tone layer's Curve and each noise layer's Slope.
    struct IdStore
    {
        struct Slot { bool noise; int layer, field; };
        std::vector<std::string> strings;
        std::vector<const char*> all, tone, noise;
        std::vector<Slot> slots;              // per ids() index (from 4)
        std::vector<int> toneAt, noiseAt;     // [layer * fields + field] -> ids() index
        IdStore()
        {
            for (int l = 0; l < numTone; ++l)
                for (int f = 0; f < numToneFields; ++f)
                    if (f != curve) slots.push_back ({ false, l, f });
            for (int l = 0; l < numNoise; ++l)
                for (int f = 0; f < numNoiseFields; ++f)
                    if (f != nSlope) slots.push_back ({ true, l, f });
            for (int l = 0; l < numTone; ++l) slots.push_back ({ false, l, curve });
            for (int l = 0; l < numNoise; ++l) slots.push_back ({ true, l, nSlope });

            strings.reserve (slots.size());
            for (const auto& s : slots)
                strings.push_back ((s.noise ? "lnz" : "lyr") + std::to_string (s.layer + 1) + (s.noise ? noiseNames[s.field] : toneNames[s.field]));
            all = { layersOn, detail, pitchAmt, pitchTime };
            tone.assign (static_cast<size_t> (numTone * numToneFields), nullptr);
            noise.assign (static_cast<size_t> (numNoise * numNoiseFields), nullptr);
            toneAt.assign (tone.size(), -1);
            noiseAt.assign (noise.size(), -1);
            for (size_t i = 0; i < slots.size(); ++i)
            {
                const auto& s = slots[i];
                const auto k = static_cast<size_t> (s.layer * (s.noise ? static_cast<int> (numNoiseFields) : static_cast<int> (numToneFields)) + s.field);
                (s.noise ? noise : tone)[k] = strings[i].c_str();
                (s.noise ? noiseAt : toneAt)[k] = static_cast<int> (all.size());
                all.push_back (strings[i].c_str());
            }
        }
    };
    const IdStore& store() { static const IdStore s; return s; }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }
    juce::NormalisableRange<float> timeRange (bool attack) { return attack ? skewed (0.001f, 10.0f, 0.3f) : skewed (0.005f, 30.0f, 1.0f); }
    juce::NormalisableRange<float> hzRange() { return skewed (20.0f, 20000.0f, 1000.0f); }

    juce::AudioParameterFloatAttributes pct()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })
                                                    .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue() / 100.0f; });
    }
    juce::AudioParameterFloatAttributes secs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
               { return v < 0.01f ? juce::String (v * 1000.0f, 2) + " ms" : v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; })
                                                    .withValueFromStringFunction ([] (const juce::String& s)
               { return s.containsIgnoreCase ("ms") ? s.getFloatValue() / 1000.0f : s.getFloatValue(); });
    }
    juce::AudioParameterFloatAttributes units (const char* u, int decimals)
    {
        const juce::String suffix (u);
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([suffix, decimals] (float v, int)
               { return (decimals == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals)) + " " + suffix; });
    }

    float fastExp2 (float x) { return std::exp2 (x); }

    // n cascaded 2nd-order Butterworth stages are -3 dB where x^4 = 2^(1/n) - 1 (x = f / corner).
    float stageCorner (int n) { return std::pow (std::exp2 (1.0f / static_cast<float> (n)) - 1.0f, 0.25f); }

    // ---- fitting helpers
    float toDb (float a) { return a > 1.0e-6f ? 20.0f * std::log10 (a) : -120.0f; }

    struct Adsr { float a = 0.001f, d = 1.0f, s = 1.0f, r = 0.3f, gain = 1.0f, curve = 0.0f; }; // curve: the Decay Curve param (-1..1)

    // Fits the exponential ADSR (Env) to a curve (peak ~1) with the key held for `hold` frames.
    // dB = true compares levels in dB (amplitude) and also fits an overall gain (`gain`,
    // the envelope's peak relative to the curve's); false compares linearly (filter envelopes).
    // Attack, decay and sustain are searched together, so a late wobble peak can't pass for the attack.
    Adsr fitAdsr (const std::vector<float>& y, int hold, double dt, bool dB, bool allowCurve = false)
    {
        Adsr out;
        const int T = static_cast<int> (y.size());
        if (T < 2)
            return out;
        hold = juce::jlimit (1, T, hold);
        int peak = 0;
        for (int t = 0; t < hold; ++t)
            if (y[static_cast<size_t> (t)] > y[static_cast<size_t> (peak)])
                peak = t;

        auto pick = [] (int from, int to, int most)
        {
            std::vector<int> idx;
            const int step = juce::jmax (1, (to - from) / most);
            for (int t = from; t < to; t += step)
                idx.push_back (t);
            return idx;
        };
        // Denser sampling at the start (attacks are short), then even.
        std::vector<int> holdIdx = pick (0, juce::jmin (hold, juce::jmax (1, static_cast<int> (0.1 / dt))), 40);
        for (int t : pick (juce::jmin (hold, juce::jmax (1, static_cast<int> (0.1 / dt))), hold, 160))
            holdIdx.push_back (t);
        std::vector<float> target (holdIdx.size()), weight (holdIdx.size());
        for (size_t j = 0; j < holdIdx.size(); ++j)
        {
            const float v = y[static_cast<size_t> (holdIdx[j])];
            target[j] = dB ? juce::jmax (-60.0f, toDb (v)) : v;
            weight[j] = dB ? (target[j] + 60.0f) / 60.0f + 0.15f : 1.0f;
        }

        static const std::vector<float> times = []
        {
            std::vector<float> v;
            for (int i = 0; i <= 36; ++i)
                v.push_back (0.005f * std::pow (30.0f / 0.005f, i / 36.0f));
            return v;
        }();
        std::vector<float> sustains;
        if (dB)
        {
            sustains.push_back (0.0f);
            for (int i = 0; i <= 30; ++i)
                sustains.push_back (std::pow (10.0f, (-60.0f + 2.0f * static_cast<float> (i)) / 20.0f));
        }
        else
        {
            for (int i = 0; i <= 20; ++i)
                sustains.push_back (static_cast<float> (i) / 20.0f);
        }
        std::vector<float> attacks;
        const float tPeak = static_cast<float> ((peak + 1) * dt);
        for (int i = 0; i < 10; ++i)
        {
            const float a = 0.001f * std::pow (juce::jmax (1.5f, tPeak * 1.2f / 0.001f), i / 9.0f);
            attacks.push_back (a);
        }

        auto model = [dt] (int t, float a, float d, float s)
        {
            const double since = t * dt - a;
            return since <= 0.0 ? static_cast<float> (juce::jmax (0.0, t * dt) / a)
                                : static_cast<float> (s + (1.0 - s) * std::exp (-6.9078 * since / d));
        };
        float best = std::numeric_limits<float>::max();
        float bestOffset = 0.0f;
        for (float a : attacks)
            for (float s : sustains)
                for (float d : times)
                {
                    double sw = 0.0, se = 0.0, se2 = 0.0;
                    for (size_t j = 0; j < holdIdx.size(); ++j)
                    {
                        const float mv = model (holdIdx[j], a, d, s);
                        const float e = target[j] - (dB ? juce::jmax (-60.0f, toDb (mv)) : mv);
                        sw += weight[j];
                        se += weight[j] * e;
                        se2 += weight[j] * e * e;
                    }
                    // dB: the best overall gain (within +/- 6 dB) is free.
                    const double off = dB && sw > 0.0 ? juce::jlimit (-6.0, 6.0, se / sw) : 0.0;
                    const auto err = static_cast<float> (se2 - 2.0 * off * se + off * off * sw);
                    if (err < best) { best = err; out.a = a; out.d = d; out.s = s; bestOffset = static_cast<float> (off); }
                }
        // The model's level at key-up, then the release from there (decay and release
        // share the curve exponent p: dB falls as -60 (t / time)^p).
        auto levelAt = [dt] (int t, float a, float d, float s, float p)
        {
            const double since = t * dt - a;
            return since <= 0.0 ? static_cast<float> (juce::jmax (0.0, t * dt) / a)
                                : static_cast<float> (s + (1.0 - s) * std::exp (-6.9078 * std::pow (since / d, static_cast<double> (p))));
        };
        const auto relIdx = pick (hold, T, 200);
        auto fitRelease = [&] (float atHold, float p, float& rOut)
        {
            float bestR = std::numeric_limits<float>::max();
            rOut = 0.05f;
            if (relIdx.size() < 2)
                return 0.0f;
            for (float r : times)
            {
                float e = 0.0f;
                for (int t : relIdx)
                {
                    const float tv = y[static_cast<size_t> (t)];
                    const float mv = atHold * static_cast<float> (std::exp (-6.9078 * std::pow ((t - hold) * dt / r, static_cast<double> (p))));
                    if (dB)
                    {
                        const float a = juce::jmax (-60.0f, toDb (tv)), b = juce::jmax (-60.0f, toDb (mv));
                        e += ((a + 60.0f) / 60.0f + 0.15f) * (a - b) * (a - b);
                    }
                    else
                    {
                        e += (tv - mv) * (tv - mv);
                    }
                }
                if (e < bestR) { bestR = e; rOut = r; }
            }
            return bestR;
        };

        out.gain = dB ? std::pow (10.0f, bestOffset / 20.0f) : 1.0f;
        const float plainTotal = best + fitRelease (levelAt (hold, out.a, out.d, out.s, 1.0f) * out.gain, 1.0f, out.r);

        // dB only (tone layers): a curved decay + release if it fits clearly better over the
        // whole note, e.g. a kick's hold-then-drop (whose drop is often after the key-up
        // point the print picks). Same attack; decay, sustain, release and curve searched.
        if (dB && allowCurve)
        {
            float bestTotal = plainTotal;
            Adsr pick1 = out;
            for (float c : { -0.5f, -0.25f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const float p = curveExponent (c);
                float hb = std::numeric_limits<float>::max(), hd = out.d, hs = out.s, hOff = 0.0f;
                for (float s : sustains)
                    for (float d : times)
                    {
                        double sw = 0.0, se = 0.0, se2 = 0.0;
                        for (size_t j = 0; j < holdIdx.size(); ++j)
                        {
                            const float e = target[j] - juce::jmax (-60.0f, toDb (levelAt (holdIdx[j], out.a, d, s, p)));
                            sw += weight[j];
                            se += weight[j] * e;
                            se2 += weight[j] * e * e;
                        }
                        const double off = sw > 0.0 ? juce::jlimit (-6.0, 6.0, se / sw) : 0.0;
                        const auto err = static_cast<float> (se2 - 2.0 * off * se + off * off * sw);
                        if (err < hb) { hb = err; hd = d; hs = s; hOff = static_cast<float> (off); }
                    }
                const float g = std::pow (10.0f, hOff / 20.0f);
                float r = 0.05f;
                const float total = hb + fitRelease (levelAt (hold, out.a, hd, hs, p) * g, p, r);
                if (total < bestTotal)
                {
                    bestTotal = total;
                    pick1 = out;
                    pick1.d = hd; pick1.s = hs; pick1.r = r; pick1.gain = g; pick1.curve = c;
                }
            }
            if (bestTotal < 0.85f * plainTotal)
                out = pick1;
        }
        return out;
    }

    float lpGain (float h, float fcHarm) // 12 dB/oct, Q 0.707, at harmonic h with cutoff at harmonic fcHarm
    {
        const float x = h / fcHarm;
        return 1.0f / std::sqrt (1.0f + x * x * x * x);
    }
}

float noiseResponse (float f, float centre, float octaves, int slope, double rate)
{
    auto W = [rate] (float hz) { return static_cast<float> (std::tan (juce::MathConstants<double>::pi * juce::jlimit (1.0, 0.49 * rate, static_cast<double> (hz)) / rate)); };
    const float span = std::exp2 (juce::jlimit (0.1f, 8.0f, octaves));
    const float wf = W (f);
    if (slope <= 0)
    {
        const float q = std::sqrt (span) / (span - 1.0f);
        const float x = wf / W (centre);
        const float b = x / q;
        return b * b / ((1.0f - x * x) * (1.0f - x * x) + b * b);
    }
    const int n = 1 << (juce::jmin (slope, numSlopes - 1) - 1);
    const float xc = stageCorner (n);
    const float y = wf / W (centre * std::sqrt (span) / xc), z = wf / W (centre / std::sqrt (span) * xc);
    const float lpP = 1.0f / (1.0f + y * y * y * y), hpP = z * z * z * z / (1.0f + z * z * z * z);
    return std::pow (lpP * hpP, static_cast<float> (n));
}

const char* toneId (int layer, int field) { return store().tone[static_cast<size_t> (layer * numToneFields + field)]; }
const char* noiseId (int layer, int field) { return store().noise[static_cast<size_t> (layer * numNoiseFields + field)]; }
int toneIndex (int layer, int field) { return store().toneAt[static_cast<size_t> (layer * numToneFields + field)]; }
int noiseIndex (int layer, int field) { return store().noiseAt[static_cast<size_t> (layer * numNoiseFields + field)]; }
const std::vector<const char*>& ids() { return store().all; }

std::vector<ParamSpec> specs()
{
    std::vector<ParamSpec> v;
    v.push_back ({ layersOn, 2, false, 0.0f });
    v.push_back ({ detail, numTone, true, 0.0f });
    v.push_back ({ pitchAmt, 0, false, 0.0f });
    v.push_back ({ pitchTime, 0, false, 0.0f });
    const auto& st = store();
    for (size_t i = 0; i < st.slots.size(); ++i)
    {
        const auto& s = st.slots[i];
        const int f = s.field;
        const int steps = s.noise ? (f == nSlope ? numSlopes : 0) : (f == octave ? 7 : f == semi ? 25 : f == fltType ? 3 : 0);
        v.push_back ({ st.strings[i].c_str(), steps, ! s.noise && (f == octave || f == semi), 0.0f });
    }
    return v;
}

void addParams (juce::AudioProcessorValueTreeState::ParameterLayout& l)
{
    auto pid = [] (const char* id) { return juce::ParameterID { id, 1 }; };
    auto addF = [&] (const char* id, const juce::String& name, juce::NormalisableRange<float> r, float def, juce::AudioParameterFloatAttributes a)
    {
        l.add (std::make_unique<juce::AudioParameterFloat> (pid (id), name, r, def, a));
    };
    l.add (std::make_unique<juce::AudioParameterBool> (pid (layersOn), "Layers On", false));
    l.add (std::make_unique<juce::AudioParameterInt> (pid (detail), "Layers Detail", 1, numTone, 1));
    addF (pitchAmt, "Layers Pitch Env", { -48.0f, 48.0f }, 0.0f, units ("st", 1));
    addF (pitchTime, "Layers Pitch Time", skewed (0.001f, 2.0f, 0.1f), 0.05f, secs());

    for (int n = 0; n < numTone; ++n)
    {
        const juce::String pre = "Layer " + juce::String (n + 1) + " ";
        for (int f = 0; f < numToneFields; ++f)
        {
            if (f == curve)
                continue; // v10, added below
            const char* id = toneId (n, f);
            const auto name = pre + toneLabels[f];
            switch (f)
            {
                case level:   addF (id, name, { 0.0f, 1.0f }, n == 0 ? 0.7f : 0.5f, pct()); break;
                case pan:     addF (id, name, { -1.0f, 1.0f }, 0.0f, pct()); break;
                case octave:  l.add (std::make_unique<juce::AudioParameterInt> (pid (id), name, -3, 3, 0)); break;
                case semi:    l.add (std::make_unique<juce::AudioParameterInt> (pid (id), name, -12, 12, 0)); break;
                case fine:    addF (id, name, { -100.0f, 100.0f }, 0.0f, units ("ct", 0)); break;
                case attack: case fAttack: addF (id, name, timeRange (true), 0.005f, secs()); break;
                case decay: case fDecay:   addF (id, name, timeRange (false), 0.5f, secs()); break;
                case sustain: addF (id, name, { 0.0f, 1.0f }, 0.8f, pct()); break;
                case fSustain: addF (id, name, { 0.0f, 1.0f }, 0.0f, pct()); break;
                case release: case fRelease: addF (id, name, timeRange (false), 0.3f, secs()); break;
                case fltType: l.add (std::make_unique<juce::AudioParameterChoice> (pid (id), name, juce::StringArray { "Low-pass", "Band-pass", "High-pass" }, 0)); break;
                case cutoff:  addF (id, name, hzRange(), 20000.0f, units ("Hz", 0)); break;
                default:      addF (id, name, { f == reso ? 0.0f : -1.0f, 1.0f }, 0.0f, pct()); break; // reso, env amount, LFO depths
            }
        }
    }
    for (int n = 0; n < numNoise; ++n)
    {
        const juce::String pre = "Noise Layer " + juce::String (n + 1) + " ";
        addF (noiseId (n, nLevel), pre + "Level", { 0.0f, 1.0f }, 0.0f, pct());
        addF (noiseId (n, nCentre), pre + "Centre", hzRange(), 3000.0f, units ("Hz", 0));
        addF (noiseId (n, nWidth), pre + "Width", skewed (0.1f, 8.0f, 2.0f), 2.0f, units ("oct", 2));
        addF (noiseId (n, nAttack), pre + "Attack", timeRange (true), 0.005f, secs());
        addF (noiseId (n, nDecay), pre + "Decay", timeRange (false), 0.5f, secs());
        addF (noiseId (n, nSustain), pre + "Sustain", { 0.0f, 1.0f }, 0.5f, pct());
        addF (noiseId (n, nRelease), pre + "Release", timeRange (false), 0.3f, secs());
        addF (noiseId (n, nPan), pre + "Pan", { -1.0f, 1.0f }, 0.0f, pct());
    }
    // M14 part 2 (CashCode v10): envelope curve (decay + release) per tone layer, edge slope per noise layer.
    for (int n = 0; n < numTone; ++n)
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { toneId (n, curve), 2 }, "Layer " + juce::String (n + 1) + " Env Curve",
                                                            juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f, pct()));
    for (int n = 0; n < numNoise; ++n)
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { noiseId (n, nSlope), 2 }, "Noise Layer " + juce::String (n + 1) + " Slope",
                                                             juce::StringArray { "6 dB", "12 dB", "24 dB", "48 dB" }, 0));
}

//============================================================================== wavetables
void buildTables (const std::vector<float>& harmonics, const std::vector<float>& phases,
                  const std::vector<float>& spreads, std::vector<float>& waves)
{
    constexpr int N = tableSize;
    waves.assign (static_cast<size_t> (2 * numTone * numMips * (N + 1)), 0.0f);
    static const std::pair<std::vector<float>, std::vector<float>> sc = []
    {
        std::vector<float> s (N), c (N);
        for (int i = 0; i < N; ++i)
        {
            s[static_cast<size_t> (i)] = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * i / N));
            c[static_cast<size_t> (i)] = static_cast<float> (std::cos (juce::MathConstants<double>::twoPi * i / N));
        }
        return std::pair { s, c };
    }();
    for (int ch = 0; ch < 2; ++ch)
    for (int l = 0; l < numTone; ++l)
    {
        for (int m = 0; m < numMips; ++m)
        {
            float* t = waves.data() + static_cast<size_t> (((ch * numTone + l) * numMips + m) * (N + 1));
            const int maxH = numHarmonics >> m;
            for (int h = 1; h <= maxH; ++h)
            {
                const auto k = static_cast<size_t> (l * numHarmonics + h - 1);
                const float w = k < harmonics.size() ? harmonics[k] : 0.0f;
                if (w == 0.0f)
                    continue;
                const float ph = (k < phases.size() ? phases[k] : 0.0f) + (ch == 1 && k < spreads.size() ? spreads[k] : 0.0f);
                const float ws = w * std::cos (ph), wc = w * std::sin (ph); // w sin(x + ph)
                for (int i = 0; i < N; ++i)
                {
                    const auto at = static_cast<size_t> ((h * i) % N);
                    t[i] += ws * sc.first[at] + wc * sc.second[at];
                }
            }
            t[N] = t[0];
        }
    }
}

const float* tablesFor (const MintModel* m)
{
    if (m != nullptr && ! m->layerWaves.empty())
        return m->layerWaves.data();
    static const std::vector<float> saw = []
    {
        std::vector<float> h (static_cast<size_t> (numTone * numHarmonics)), ph (h.size(), 0.0f), w;
        for (int l = 0; l < numTone; ++l)
            for (int k = 1; k <= numHarmonics; ++k)
                h[static_cast<size_t> (l * numHarmonics + k - 1)] = 0.6f / static_cast<float> (k);
        buildTables (h, ph, ph, w);
        return w;
    }();
    return saw.data();
}

//============================================================================== voice
void Env::set (float attackS, float decayS, float sustainLevel, float releaseS, double sampleRate, float curveP)
{
    if (attackS == a && decayS == d && releaseS == r && sustainLevel == sus && curveP == p)
        return;
    if (curveP != p && (stage == dec || stage == rel))
    {
        // Pick the new curve up where the level is now (no jump).
        const bool falling = stage == rel;
        const float span = falling ? relStart : 1.0f - sustainLevel;
        const float from = falling ? 0.0f : sustainLevel;
        g = span > 1.0e-6f ? juce::jlimit (1.0e-12f, 1.0f, (value - from) / span) : 1.0e-12f;
        lg = std::log2 (g);
        tFall = juce::jmax (1.0e-6f, falling ? releaseS : decayS) * std::pow (-lg / 9.965784f, 1.0f / curveP);
        seg = 0;
    }
    a = attackS; d = decayS; r = releaseS; sus = sustainLevel; p = curveP;
    aStep = static_cast<float> (1.0 / juce::jmax (1.0, attackS * sampleRate));
    dMul = static_cast<float> (std::exp (-6.9078 / juce::jmax (1.0, decayS * sampleRate)));
    rMul = static_cast<float> (std::exp (-6.9078 / juce::jmax (1.0, releaseS * sampleRate)));
    segS = static_cast<float> (curveSeg / sampleRate);
}

void Env::nextSegment()
{
    // dB(t) = -60 (t / time)^p, log-linear across each segment: log2 level = -log2 (1000) (t / d)^p.
    g = std::exp2 (lg); // no drift: each segment starts exactly on the curve
    tFall += segS;
    const float lgNext = juce::jmax (-40.0f, -9.965784f * std::pow (tFall / juce::jmax (1.0e-6f, stage == rel ? r : d), p));
    gMul = std::exp2 ((lgNext - lg) / static_cast<float> (curveSeg));
    lg = lgNext;
    seg = curveSeg;
}

void Bank::Svf::setup (float hz, float q, double rate)
{
    const auto g = static_cast<float> (std::tan (juce::MathConstants<double>::pi * hz / rate));
    k = 1.0f / q;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

float Bank::Svf::lp (float v0, float& band, float& high)
{
    const float v3 = v0 - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    band = v1;
    high = v0 - k * v1 - v2;
    return v2;
}

void Bank::prepare (double sampleRate)
{
    sr = sampleRate;
    smooth = static_cast<float> (1.0 - std::exp (-1.0 / (0.005 * sr)));
    for (auto& t : tones) { t.amp.a = t.flt.a = -1.0f; }
    for (auto& n : noises) n.amp.a = -1.0f;
    kill();
}

void Bank::start (const Settings& s, uint32_t seed)
{
    const bool fresh = ! isActive();
    if (fresh)
        rng = seed * 2654435761u + 1u;
    for (int i = 0; i < numTone; ++i)
    {
        auto& st = tones[static_cast<size_t> (i)];
        const auto& t = s.tone[static_cast<size_t> (i)];
        st.amp.set (t.a, t.d, t.s, t.r, sr, t.p);
        st.flt.set (t.fa, t.fd, t.fs, t.fr, sr);
        if (fresh)
        {
            st.phase = 0.0;
            st.svf.reset();
            st.svfR.reset();
            st.gain = i < s.detail ? t.gain : 0.0f;
            st.countdown = 0;
        }
        st.amp.on();
        st.flt.on();
    }
    for (int i = 0; i < numNoise; ++i)
    {
        auto& st = noises[static_cast<size_t> (i)];
        const auto& n = s.noise[static_cast<size_t> (i)];
        st.amp.set (n.a, n.d, n.s, n.r, sr);
        if (fresh)
        {
            st.svf.reset();
            for (auto& f : st.hp) f.reset();
            for (auto& f : st.lp) f.reset();
            st.gain = n.gain;
            st.countdown = 0;
        }
        st.amp.on();
    }
    sinceOn = 0.0;
}

void Bank::release()
{
    for (auto& t : tones) { t.amp.off(); t.flt.off(); }
    for (auto& n : noises) n.amp.off();
}

void Bank::kill()
{
    for (auto& t : tones) { t.amp.reset(); t.flt.reset(); t.gain = 0.0f; t.svf.reset(); t.svfR.reset(); }
    for (auto& n : noises)
    {
        n.amp.reset(); n.gain = 0.0f; n.svf.reset();
        for (auto& f : n.hp) f.reset();
        for (auto& f : n.lp) f.reset();
    }
}

bool Bank::isActive() const
{
    for (auto& t : tones) if (t.amp.stage != Env::idle) return true;
    for (auto& n : noises) if (n.amp.stage != Env::idle) return true;
    return false;
}

void Bank::next (const Settings& s, const float* waves, float pitch, float lfo1, float lfo2, float& mid, float& side)
{
    const float nyq = static_cast<float> (sr * 0.45);
    float pe = 0.0f;
    if (s.pitchAmt != 0.0f)
        pe = s.pitchAmt * static_cast<float> (std::exp (-4.6 * sinceOn / juce::jmax (0.001f, s.pitchTime)));
    sinceOn += 1.0 / sr;
    const float keyTrack = fastExp2 ((pitch - 60.0f) / 12.0f);
    const double baseHz = 440.0 * std::exp2 ((pitch - 69.0f + pe) / 12.0);

    for (int i = 0; i < numTone; ++i)
    {
        auto& st = tones[static_cast<size_t> (i)];
        const auto& t = s.tone[static_cast<size_t> (i)];
        const float target = i < s.detail ? t.gain : 0.0f;
        st.gain += (target - st.gain) * smooth;
        if (st.amp.stage == Env::idle || (target == 0.0f && st.gain < 1.0e-5f))
            continue;
        st.amp.set (t.a, t.d, t.s, t.r, sr, t.p);
        const float env = st.amp.next();

        float semis = t.semis;
        if (t.l1p != 0.0f || t.l2p != 0.0f)
            semis += t.l1p * lfo1 + t.l2p * lfo2;
        const double hz = baseHz * std::exp2 (semis / 12.0);
        const double inc = hz / sr;
        if (--st.countdown <= 0)
        {
            const double allowed = nyq / juce::jmax (1.0, hz);
            int mip = 0;
            while (mip < numMips - 1 && (numHarmonics >> mip) > allowed)
                ++mip;
            st.mip = allowed < 1.0 ? -1 : mip;
            st.countdown = 32;
        }
        float v = 0.0f, vR = 0.0f;
        if (st.mip >= 0)
        {
            const float* tb = table (waves, i, st.mip, 0);
            const float* tr = table (waves, i, st.mip, 1);
            const double pos = st.phase * tableSize;
            const int idx = static_cast<int> (pos);
            const float frac = static_cast<float> (pos - idx);
            v = tb[idx] + frac * (tb[idx + 1] - tb[idx]);
            vR = tr[idx] + frac * (tr[idx + 1] - tr[idx]);
        }
        st.phase += inc;
        if (st.phase >= 1.0)
            st.phase -= std::floor (st.phase);

        if (! t.open())
        {
            st.flt.set (t.fa, t.fd, t.fs, t.fr, sr);
            const float fenv = st.flt.next();
            if ((st.countdown & 7) == 0)
            {
                const float semisC = t.envSemis * fenv + (t.l1c * lfo1 + t.l2c * lfo2);
                const float c = juce::jlimit (20.0f, nyq, t.cutoffHz * keyTrack * fastExp2 (semisC / 12.0f));
                st.svf.setup (c, t.q, sr);
                st.svfR.setup (c, t.q, sr);
            }
            float band, high;
            const float low = st.svf.lp (v, band, high);
            v = t.type == 0 ? low : t.type == 1 ? band : high;
            const float lowR = st.svfR.lp (vR, band, high);
            vR = t.type == 0 ? lowR : t.type == 1 ? band : high;
        }

        float g = st.gain * env;
        if (t.l1l != 0.0f) g *= juce::jmax (0.0f, 1.0f + t.l1l * lfo1);
        if (t.l2l != 0.0f) g *= juce::jmax (0.0f, 1.0f + t.l2l * lfo2);
        const float m = 0.5f * (v + vR) * g;
        mid += m;
        side += 0.5f * (v - vR) * g - m * t.pan;
    }

    for (int i = 0; i < numNoise; ++i)
    {
        auto& st = noises[static_cast<size_t> (i)];
        const auto& n = s.noise[static_cast<size_t> (i)];
        st.gain += (n.gain - st.gain) * smooth;
        if (st.amp.stage == Env::idle || (n.gain == 0.0f && st.gain < 1.0e-5f))
            continue;
        st.amp.set (n.a, n.d, n.s, n.r, sr);
        const float env = st.amp.next();
        const int stages = n.slope <= 0 ? 0 : 1 << (juce::jmin (n.slope, numSlopes - 1) - 1); // 0, 1, 2, 4 pairs
        if (--st.countdown <= 0)
        {
            const float w = juce::jlimit (0.1f, 8.0f, n.octaves);
            const float span = std::exp2 (w);
            const float c = juce::jlimit (20.0f, nyq, n.centreHz * keyTrack);
            float enbw;
            if (stages == 0)
            {
                const float q = std::sqrt (span) / (span - 1.0f);
                st.svf.setup (c, q, sr);
                enbw = juce::MathConstants<float>::halfPi * c / q;
            }
            else
            {
                // Butterworth HP + LP pairs; each stage's corner moved out so the whole
                // cascade is -3 dB at the band edges (the same edges as the band-pass).
                const float x = stageCorner (stages);
                const float lo = c / std::sqrt (span), hi = c * std::sqrt (span);
                const float loC = juce::jlimit (10.0f, nyq, lo * x), hiC = juce::jlimit (20.0f, nyq, hi / x);
                for (int k = 0; k < stages; ++k)
                {
                    st.hp[static_cast<size_t> (k)].setup (loC, 0.70710678f, sr);
                    st.lp[static_cast<size_t> (k)].setup (hiC, 0.70710678f, sr);
                }
                // Noise bandwidth of the cascade: hiC * I_n - loC * J_n (Butterworth integrals).
                const float I = stages == 1 ? 1.1107f : stages == 2 ? 0.8330f : 0.6682f;
                const float J = stages == 1 ? 1.1107f : stages == 2 ? 1.3884f : 1.6921f;
                enbw = juce::jmax (0.5f * (juce::jmin (hi, nyq) - lo), juce::jmin (hiC * I, nyq) - loC * J);
            }
            enbw = juce::jmin (static_cast<float> (sr * 0.5), enbw);
            st.norm = juce::jmin (60.0f, std::sqrt (3.0f * static_cast<float> (sr * 0.5) / enbw));
            st.countdown = 32;
        }
        float shaped;
        if (stages == 0)
        {
            float band, high;
            st.svf.lp (nextNoise(), band, high);
            shaped = band * st.svf.k;
        }
        else
        {
            shaped = nextNoise();
            for (int k = 0; k < stages; ++k)
                shaped = st.lp[static_cast<size_t> (k)].lpOnly (st.hp[static_cast<size_t> (k)].hpOnly (shaped));
        }
        const float out = shaped * st.norm * st.gain * env;
        mid += out;
        side -= out * n.pan;
    }
}

//============================================================================== fitting
Fit fit (const MintModel& m, int key, int wantLayers, const Wobbles& wob)
{
    Fit out;
    const int T = m.numFrames, K = m.numPartials;
    const double dt = MintModel::hop / MintModel::sampleRate;
    const int hold = juce::jlimit (1, juce::jmax (1, T), m.holdFrame);
    wantLayers = juce::jlimit (1, numTone, wantLayers);
    out.harmonics.assign (static_cast<size_t> (numTone * numHarmonics), 0.0f);
    out.phases.assign (out.harmonics.size(), 0.0f);
    out.spreads.assign (out.harmonics.size(), 0.0f);
    auto set = [&out] (const char* id, float v) { out.values.emplace_back (id, v); };

    // ---- partial level curves
    std::vector<std::vector<float>> A (static_cast<size_t> (K));
    std::vector<float> E (static_cast<size_t> (K), 0.0f), peakA (static_cast<size_t> (K), 0.0f), ratio (static_cast<size_t> (K), 1.0f);
    float eMax = 0.0f;
    for (int k = 0; k < K; ++k)
    {
        auto& a = A[static_cast<size_t> (k)];
        a.resize (static_cast<size_t> (T));
        for (int t = 0; t < T; ++t)
        {
            const auto i = static_cast<size_t> (k * T + t);
            const float l = m.ampL[i], r = m.ampR[i];
            a[static_cast<size_t> (t)] = std::sqrt (0.5f * (l * l + r * r));
            E[static_cast<size_t> (k)] += a[static_cast<size_t> (t)] * a[static_cast<size_t> (t)];
            peakA[static_cast<size_t> (k)] = juce::jmax (peakA[static_cast<size_t> (k)], a[static_cast<size_t> (t)]);
        }
        eMax = juce::jmax (eMax, E[static_cast<size_t> (k)]);
        // Settled frequency: median over the second half of the frames it's alive in.
        std::vector<float> alive;
        for (int t = 0; t < T; ++t)
            if (a[static_cast<size_t> (t)] >= 0.05f * peakA[static_cast<size_t> (k)])
                alive.push_back (m.freq[static_cast<size_t> (k * T + t)]);
        if (! alive.empty())
        {
            std::vector<float> late (alive.begin() + static_cast<long> (alive.size() / 2), alive.end());
            std::nth_element (late.begin(), late.begin() + static_cast<long> (late.size() / 2), late.end());
            ratio[static_cast<size_t> (k)] = late[late.size() / 2];
        }
    }
    std::vector<int> keep;
    for (int k = 0; k < K; ++k)
        if (eMax > 0.0f && E[static_cast<size_t> (k)] >= 1.0e-6f * eMax && ratio[static_cast<size_t> (k)] > 0.0f)
            keep.push_back (k);

    // ---- pitch envelope (all layers): the strongest partials' drift from where they settle
    float pAmt = 0.0f, pTime = 0.05f;
    if (! keep.empty())
    {
        std::vector<int> strong = keep;
        std::sort (strong.begin(), strong.end(), [&E] (int x, int y) { return E[static_cast<size_t> (x)] > E[static_cast<size_t> (y)]; });
        strong.resize (juce::jmin<size_t> (8, strong.size()));
        std::vector<float> off (static_cast<size_t> (hold), 0.0f), w (static_cast<size_t> (hold), 0.0f);
        for (int t = 0; t < hold; ++t)
        {
            double s = 0.0, sw = 0.0;
            for (int k : strong)
            {
                const float a = A[static_cast<size_t> (k)][static_cast<size_t> (t)];
                const float f = m.freq[static_cast<size_t> (k * T + t)];
                if (a <= 0.0f || f <= 0.0f)
                    continue;
                s += a * a * 12.0 * std::log2 (f / ratio[static_cast<size_t> (k)]);
                sw += a * a;
            }
            off[static_cast<size_t> (t)] = sw > 0.0 ? static_cast<float> (s / sw) : 0.0f;
            w[static_cast<size_t> (t)] = static_cast<float> (sw);
        }
        float bestE = 0.0f;
        for (int t = 0; t < hold; ++t)
            bestE += w[static_cast<size_t> (t)] * off[static_cast<size_t> (t)] * off[static_cast<size_t> (t)];
        for (int i = 0; i <= 30; ++i)
        {
            const float time = 0.002f * std::pow (1000.0f, i / 30.0f);
            double num = 0.0, den = 0.0;
            for (int t = 0; t < hold; ++t)
            {
                const auto g = std::exp (-4.6 * t * dt / time);
                num += w[static_cast<size_t> (t)] * off[static_cast<size_t> (t)] * g;
                den += w[static_cast<size_t> (t)] * g * g;
            }
            if (den <= 0.0)
                continue;
            const auto amt = static_cast<float> (num / den);
            float e = 0.0f;
            for (int t = 0; t < hold; ++t)
            {
                const float d = off[static_cast<size_t> (t)] - amt * static_cast<float> (std::exp (-4.6 * t * dt / time));
                e += w[static_cast<size_t> (t)] * d * d;
            }
            if (e < bestE * 0.98f) { bestE = e; pAmt = amt; pTime = time; }
        }
        if (std::abs (pAmt) < 0.25f)
            pAmt = 0.0f;
        pAmt = juce::jlimit (-48.0f, 48.0f, pAmt);
    }
    set (pitchAmt, pAmt);
    set (pitchTime, juce::jlimit (0.001f, 2.0f, pTime));

    // ---- cluster the partials by envelope shape (k-means on normalised dB curves)
    constexpr int D = 40;
    const int nClusters = juce::jmin (wantLayers, static_cast<int> (keep.size()));
    std::vector<std::array<float, D>> feat (keep.size());
    for (size_t j = 0; j < keep.size(); ++j)
    {
        const auto& a = A[static_cast<size_t> (keep[j])];
        const float pk = peakA[static_cast<size_t> (keep[j])];
        for (int d = 0; d < D; ++d)
        {
            const int t = static_cast<int> ((T - 1) * std::pow (d / static_cast<float> (D - 1), 2.0f));
            float s = 0.0f;
            int n = 0;
            for (int u = juce::jmax (0, t - 2); u <= juce::jmin (T - 1, t + 2); ++u, ++n)
                s += a[static_cast<size_t> (u)];
            const float db = juce::jmax (-60.0f, toDb (s / juce::jmax (1, n) / pk));
            feat[j][static_cast<size_t> (d)] = db / 60.0f + 1.0f;
        }
    }
    auto dist = [] (const std::array<float, D>& x, const std::array<float, D>& y)
    {
        float s = 0.0f;
        for (int d = 0; d < D; ++d)
            s += (x[static_cast<size_t> (d)] - y[static_cast<size_t> (d)]) * (x[static_cast<size_t> (d)] - y[static_cast<size_t> (d)]);
        return s;
    };
    std::vector<float> wk (keep.size());
    for (size_t j = 0; j < keep.size(); ++j)
        wk[j] = std::sqrt (E[static_cast<size_t> (keep[j])]);
    std::vector<std::array<float, D>> centres;
    if (nClusters > 0)
    {
        size_t first = 0;
        for (size_t j = 1; j < keep.size(); ++j)
            if (wk[j] > wk[first]) first = j;
        centres.push_back (feat[first]);
        while (static_cast<int> (centres.size()) < nClusters)
        {
            size_t far = 0;
            float farD = -1.0f;
            for (size_t j = 0; j < keep.size(); ++j)
            {
                float dmin = std::numeric_limits<float>::max();
                for (auto& c : centres)
                    dmin = juce::jmin (dmin, dist (feat[j], c));
                const float score = wk[j] * dmin;
                if (score > farD) { farD = score; far = j; }
            }
            if (farD <= 0.0f)
                break;
            centres.push_back (feat[far]);
        }
    }
    std::vector<int> assign (keep.size(), 0);
    for (int iter = 0; iter < 30 && ! centres.empty(); ++iter)
    {
        bool changed = false;
        for (size_t j = 0; j < keep.size(); ++j)
        {
            int best = 0;
            float bd = std::numeric_limits<float>::max();
            for (size_t c = 0; c < centres.size(); ++c)
                if (const float dd = dist (feat[j], centres[c]); dd < bd) { bd = dd; best = static_cast<int> (c); }
            changed |= assign[j] != best;
            assign[j] = best;
        }
        for (size_t c = 0; c < centres.size(); ++c)
        {
            std::array<float, D> sum {};
            float sw = 0.0f;
            for (size_t j = 0; j < keep.size(); ++j)
                if (assign[j] == static_cast<int> (c))
                {
                    for (int d = 0; d < D; ++d) sum[static_cast<size_t> (d)] += wk[j] * feat[j][static_cast<size_t> (d)];
                    sw += wk[j];
                }
            if (sw > 0.0f)
                for (int d = 0; d < D; ++d) centres[c][static_cast<size_t> (d)] = sum[static_cast<size_t> (d)] / sw;
        }
        if (! changed && iter > 0)
            break;
    }

    // Order layers loudest first (so Detail 1 keeps the main body).
    std::vector<std::pair<float, int>> order;
    for (size_t c = 0; c < centres.size(); ++c)
    {
        float e = 0.0f;
        for (size_t j = 0; j < keep.size(); ++j)
            if (assign[j] == static_cast<int> (c)) e += E[static_cast<size_t> (keep[j])];
        if (e > 0.0f)
            order.emplace_back (e, static_cast<int> (c));
    }
    std::sort (order.begin(), order.end(), std::greater<>());

    // ---- LFOs: vibrato on LFO 1, tremolo on LFO 2; a wah rides with the tremolo
    // when they move together (unison beating), else takes LFO 1 if it's free.
    const bool wahOn = wob.wahDepth > 0.0f;
    int wahLfo = 0;
    if (wahOn)
    {
        if (wob.tremDepthDb > 0.0f && std::abs (wob.tremRate - wob.wahRate) <= 0.2f * wob.wahRate)
            wahLfo = 2;
        else if (wob.vibDepth <= 0.0f)
            wahLfo = 1;
        else if (wob.tremDepthDb <= 0.0f)
            wahLfo = 2;
    }
    out.lfo1Hz = wob.vibDepth > 0.0f ? wob.vibRate : (wahLfo == 1 ? wob.wahRate : 0.0f);
    out.lfo2Hz = wob.tremDepthDb > 0.0f ? wob.tremRate : (wahLfo == 2 ? wob.wahRate : 0.0f);

    // ---- each layer: a wavetable, an ADSR, maybe a filter env
    int layer = 0;
    for (auto [energy, c] : order)
    {
        juce::ignoreUnused (energy);
        std::vector<int> members;
        for (size_t j = 0; j < keep.size(); ++j)
            if (assign[j] == c) members.push_back (keep[j]);
        int strongest = members.front();
        for (int k : members)
            if (E[static_cast<size_t> (k)] > E[static_cast<size_t> (strongest)]) strongest = k;

        // Fundamental: the strongest partial's frequency / n, whichever fits the members best.
        float base = ratio[static_cast<size_t> (strongest)];
        {
            float bestCost = std::numeric_limits<float>::max();
            for (int n = 1; n <= 35; ++n)
            {
                // The strongest partial / n, then the sample's own root (and octaves under it).
                const float b = n <= 32 ? ratio[static_cast<size_t> (strongest)] / static_cast<float> (n) : std::exp2 (static_cast<float> (33 - n));
                float cost = 0.0f;
                for (int k : members)
                {
                    const float r = ratio[static_cast<size_t> (k)] / b;
                    const int h = juce::roundToInt (r);
                    const float cents = h >= 1 && h <= numHarmonics ? 12.0f * std::log2 (r / static_cast<float> (h)) : 2.0f;
                    cost += E[static_cast<size_t> (k)] * cents * cents;
                }
                if (cost < bestCost * 0.9f) { bestCost = cost; base = b; }
            }
        }
        // Harmonic power over time, per harmonic.
        std::vector<std::vector<float>> P (static_cast<size_t> (numHarmonics));
        std::vector<float> phase (static_cast<size_t> (numHarmonics), 0.0f), phaseE (static_cast<size_t> (numHarmonics), 0.0f);
        // Stereo: mid and side energy per harmonic (from each partial's L/R level and R phase lead).
        std::vector<double> midE (static_cast<size_t> (numHarmonics), 0.0), sideE (midE.size(), 0.0), leadS (midE.size(), 0.0), leadC (midE.size(), 0.0);
        double eL = 0.0, eR = 0.0;
        for (int k : members)
        {
            const int h = juce::roundToInt (ratio[static_cast<size_t> (k)] / base);
            if (h < 1 || h > numHarmonics)
                continue;
            auto& p = P[static_cast<size_t> (h - 1)];
            if (p.empty()) p.assign (static_cast<size_t> (T), 0.0f);
            for (int t = 0; t < T; ++t)
            {
                const float a = A[static_cast<size_t> (k)][static_cast<size_t> (t)];
                p[static_cast<size_t> (t)] += a * a;
                const auto i = static_cast<size_t> (k * T + t);
                eL += m.ampL[i] * m.ampL[i];
                eR += m.ampR[i] * m.ampR[i];
                const double cl = m.ampL[i], cr = m.ampR[i], d = i < m.spread.size() ? m.spread[i] : 0.0;
                const double cross = 2.0 * cl * cr * std::cos (d);
                midE[static_cast<size_t> (h - 1)] += cl * cl + cr * cr + cross;
                sideE[static_cast<size_t> (h - 1)] += cl * cl + cr * cr - cross;
                leadS[static_cast<size_t> (h - 1)] += cl * cr * std::sin (d);
                leadC[static_cast<size_t> (h - 1)] += cl * cr * std::cos (d);
            }
            if (E[static_cast<size_t> (k)] > phaseE[static_cast<size_t> (h - 1)] && static_cast<size_t> (k) < m.startPhase.size())
            {
                phaseE[static_cast<size_t> (h - 1)] = E[static_cast<size_t> (k)];
                phase[static_cast<size_t> (h - 1)] = m.startPhase[static_cast<size_t> (k)];
            }
        }
        std::vector<float> S (static_cast<size_t> (T), 0.0f);
        for (auto& p : P)
            if (! p.empty())
                for (int t = 0; t < T; ++t) S[static_cast<size_t> (t)] += p[static_cast<size_t> (t)];
        float sPeak = 0.0f;
        for (auto& s : S) { s = std::sqrt (s); sPeak = juce::jmax (sPeak, s); }
        if (sPeak <= 0.0f)
            continue;

        // Mean shape, and brightness (log2 centroid) over the frames that matter.
        std::vector<float> W (static_cast<size_t> (numHarmonics), 0.0f), C (static_cast<size_t> (T), 0.0f);
        std::vector<int> valid;
        for (int t = 0; t < T; ++t)
        {
            if (S[static_cast<size_t> (t)] < 0.03f * sPeak)
                continue;
            double num = 0.0, den = 0.0;
            for (int h = 1; h <= numHarmonics; ++h)
                if (! P[static_cast<size_t> (h - 1)].empty())
                {
                    num += std::log2 (static_cast<double> (h)) * P[static_cast<size_t> (h - 1)][static_cast<size_t> (t)];
                    den += P[static_cast<size_t> (h - 1)][static_cast<size_t> (t)];
                }
            C[static_cast<size_t> (t)] = den > 0.0 ? static_cast<float> (num / den) : 0.0f;
            valid.push_back (t);
        }
        int harmonicsUsed = 0;
        {
            double tot = 0.0;
            for (int h = 1; h <= numHarmonics; ++h)
                if (! P[static_cast<size_t> (h - 1)].empty())
                    for (float v : P[static_cast<size_t> (h - 1)]) { W[static_cast<size_t> (h - 1)] += v; tot += v; }
            for (auto& w : W)
                harmonicsUsed += tot > 0.0 && w >= 0.01 * tot ? 1 : 0;
        }
        // Smoothed brightness range.
        std::vector<float> Cs (C.size(), 0.0f);
        float cMin = 1.0e9f, cMax = -1.0e9f;
        int brightest = valid.empty() ? 0 : valid.front();
        for (size_t v = 0; v < valid.size(); ++v)
        {
            float s = 0.0f;
            int n = 0;
            for (size_t u = v >= 3 ? v - 3 : 0; u < juce::jmin (valid.size(), v + 4); ++u, ++n)
                s += C[static_cast<size_t> (valid[u])];
            const float c2 = s / static_cast<float> (n);
            Cs[static_cast<size_t> (valid[v])] = c2;
            if (valid[v] <= hold)
            {
                if (c2 > cMax) { cMax = c2; brightest = valid[v]; }
                cMin = juce::jmin (cMin, c2);
            }
        }
        const bool useFilter = harmonicsUsed >= 2 && cMax - cMin >= 0.35f;

        std::vector<float> envT (S); // level target (filter loss compensated)
        float fltBase = 0.0f, fltTop = 0.0f;
        Adsr fltEnv;
        if (useFilter)
        {
            // Unfiltered shape = the brightest moment.
            std::fill (W.begin(), W.end(), 0.0f);
            for (int t = juce::jmax (0, brightest - 4); t <= juce::jmin (T - 1, brightest + 4); ++t)
                for (int h = 1; h <= numHarmonics; ++h)
                    if (! P[static_cast<size_t> (h - 1)].empty())
                        W[static_cast<size_t> (h - 1)] += P[static_cast<size_t> (h - 1)][static_cast<size_t> (t)];
            for (auto& w : W) w = std::sqrt (w);
            float wn = 0.0f;
            for (auto w : W) wn += w * w;
            wn = std::sqrt (wn);
            if (wn > 0.0f) for (auto& w : W) w /= wn;

            // Cutoff per frame (semitones above the layer's fundamental).
            std::vector<float> fc (static_cast<size_t> (T), 96.0f), loss (static_cast<size_t> (T), 1.0f);
            const int step = juce::jmax (1, static_cast<int> (valid.size()) / 200);
            std::vector<int> pts;
            for (size_t v = 0; v < valid.size(); v += static_cast<size_t> (step)) pts.push_back (valid[v]);
            for (int t : pts)
            {
                double tot = 0.0;
                for (int h = 1; h <= numHarmonics; ++h)
                    if (! P[static_cast<size_t> (h - 1)].empty()) tot += P[static_cast<size_t> (h - 1)][static_cast<size_t> (t)];
                if (tot <= 0.0) continue;
                float bestE = std::numeric_limits<float>::max(), bestFc = 96.0f, bestLoss = 1.0f;
                for (float semis = 0.0f; semis <= 96.0f; semis += 1.5f)
                {
                    const float fh = std::exp2 (semis / 12.0f);
                    float mn = 0.0f;
                    for (int h = 1; h <= numHarmonics; ++h)
                    {
                        const float x = W[static_cast<size_t> (h - 1)] * lpGain (static_cast<float> (h), fh);
                        mn += x * x;
                    }
                    mn = std::sqrt (mn);
                    if (mn <= 0.0f) continue;
                    float e = 0.0f;
                    for (int h = 1; h <= numHarmonics; ++h)
                    {
                        const float mh = W[static_cast<size_t> (h - 1)] * lpGain (static_cast<float> (h), fh) / mn;
                        const float oh = P[static_cast<size_t> (h - 1)].empty() ? 0.0f
                                       : static_cast<float> (std::sqrt (P[static_cast<size_t> (h - 1)][static_cast<size_t> (t)] / tot));
                        const float dd = std::sqrt (oh) - std::sqrt (mh);
                        e += dd * dd;
                    }
                    if (e < bestE) { bestE = e; bestFc = semis; bestLoss = mn; }
                }
                fc[static_cast<size_t> (t)] = bestFc;
                loss[static_cast<size_t> (t)] = bestLoss;
            }
            // Fill the frames between sample points.
            for (size_t v = 0; v < pts.size(); ++v)
            {
                const int a = pts[v], b = v + 1 < pts.size() ? pts[v + 1] : T;
                for (int t = a + 1; t < b; ++t) { fc[static_cast<size_t> (t)] = fc[static_cast<size_t> (a)]; loss[static_cast<size_t> (t)] = loss[static_cast<size_t> (a)]; }
            }
            for (int t = 0; t < (pts.empty() ? 0 : pts.front()); ++t) { fc[static_cast<size_t> (t)] = fc[static_cast<size_t> (pts.front())]; loss[static_cast<size_t> (t)] = loss[static_cast<size_t> (pts.front())]; }
            std::vector<float> held;
            for (int t : pts) if (t <= hold) held.push_back (fc[static_cast<size_t> (t)]);
            if (held.size() >= 3)
            {
                std::sort (held.begin(), held.end());
                fltBase = held[held.size() / 10];
                fltTop = held[held.size() * 9 / 10];
            }
            if (fltTop - fltBase >= 3.0f)
            {
                std::vector<float> n (static_cast<size_t> (T));
                for (int t = 0; t < T; ++t)
                    n[static_cast<size_t> (t)] = juce::jlimit (0.0f, 1.0f, (fc[static_cast<size_t> (t)] - fltBase) / (fltTop - fltBase));
                fltEnv = fitAdsr (n, hold, dt, false);
                for (int t = 0; t < T; ++t)
                    envT[static_cast<size_t> (t)] = S[static_cast<size_t> (t)] / juce::jmax (0.05f, loss[static_cast<size_t> (t)]);
            }
            else
            {
                fltTop = fltBase = 0.0f; // not worth a filter after all: back to the mean shape
                std::fill (W.begin(), W.end(), 0.0f);
                for (int h = 1; h <= numHarmonics; ++h)
                    if (! P[static_cast<size_t> (h - 1)].empty())
                        for (float v : P[static_cast<size_t> (h - 1)]) W[static_cast<size_t> (h - 1)] += v;
                for (auto& w : W) w = std::sqrt (w);
            }
        }
        else
        {
            for (auto& w : W) w = std::sqrt (w);
        }
        float wn = 0.0f;
        for (auto w : W) wn += w * w;
        wn = std::sqrt (wn);
        if (wn <= 0.0f)
            continue;
        for (auto& w : W) w /= wn;

        float ePeak = 0.0f;
        for (auto v : envT) ePeak = juce::jmax (ePeak, v);
        std::vector<float> envN (envT);
        for (auto& v : envN) v /= ePeak;
        const Adsr amp = fitAdsr (envN, hold, dt, true, true);
        ePeak *= amp.gain;
        const float lvl = juce::jmin (1.0f, ePeak);
        for (int h = 1; h <= numHarmonics; ++h)
        {
            out.harmonics[static_cast<size_t> (layer * numHarmonics + h - 1)] = W[static_cast<size_t> (h - 1)] * ePeak / lvl;
            out.phases[static_cast<size_t> (layer * numHarmonics + h - 1)] = phase[static_cast<size_t> (h - 1)];
            // A fixed R phase lead with the same side/mid energy: tan (lead / 2) = side / mid.
            const auto hh = static_cast<size_t> (h - 1);
            if (midE[hh] > 0.0)
            {
                const double w = std::sqrt (sideE[hh] / midE[hh]);
                const double lead = 2.0 * std::atan (w);
                out.spreads[static_cast<size_t> (layer * numHarmonics + h - 1)] = static_cast<float> (leadS[hh] < 0.0 ? -lead : lead);
            }
        }

        // Tuning: the layer's fundamental at the match's key.
        const float tune = (m.rootNote - static_cast<float> (key)) + 12.0f * std::log2 (base);
        const int oct = juce::jlimit (-3, 3, static_cast<int> (std::floor ((tune + 6.0f) / 12.0f)));
        const float rem = tune - 12.0f * static_cast<float> (oct);
        const int st = juce::jlimit (-12, 12, juce::roundToInt (rem));
        const float fineCt = juce::jlimit (-100.0f, 100.0f, (rem - static_cast<float> (st)) * 100.0f);

        const size_t levelAt = out.values.size();
        set (toneId (layer, level), lvl);
        const double sl = std::sqrt (eL), sr = std::sqrt (eR);
        set (toneId (layer, pan), sl + sr > 0.0 ? juce::jlimit (-1.0f, 1.0f, static_cast<float> ((sr - sl) / (sr + sl))) : 0.0f);
        set (toneId (layer, octave), static_cast<float> (oct));
        set (toneId (layer, semi), static_cast<float> (st));
        set (toneId (layer, fine), fineCt);
        set (toneId (layer, attack), juce::jlimit (0.001f, 10.0f, amp.a));
        set (toneId (layer, decay), juce::jlimit (0.005f, 30.0f, amp.d));
        set (toneId (layer, sustain), juce::jlimit (0.0f, 1.0f, amp.s));
        set (toneId (layer, release), juce::jlimit (0.005f, 30.0f, amp.r));
        set (toneId (layer, curve), amp.curve);
        set (toneId (layer, fltType), 0.0f);
        set (toneId (layer, reso), 0.0f);
        if (fltTop > fltBase)
        {
            const float layerHz = base * m.rootHz;
            const float hzAtKey = layerHz * std::exp2 (fltBase / 12.0f);
            set (toneId (layer, cutoff), juce::jlimit (20.0f, 20000.0f, hzAtKey / std::exp2 ((static_cast<float> (key) - 60.0f) / 12.0f)));
            set (toneId (layer, envAmt), juce::jlimit (0.0f, 1.0f, (fltTop - fltBase) / envCutoffRange));
            set (toneId (layer, fAttack), juce::jlimit (0.001f, 10.0f, fltEnv.a));
            set (toneId (layer, fDecay), juce::jlimit (0.005f, 30.0f, fltEnv.d));
            set (toneId (layer, fSustain), juce::jlimit (0.0f, 1.0f, fltEnv.s));
            set (toneId (layer, fRelease), juce::jlimit (0.005f, 30.0f, fltEnv.r));
        }
        else if (wahOn && harmonicsUsed >= 2)
        {
            // A wah needs a filter to move: a gentle low-pass a little above the layer's brightness.
            double c = 0.0;
            for (int h = 1; h <= numHarmonics; ++h)
                c += std::log2 (static_cast<double> (h)) * W[static_cast<size_t> (h - 1)] * W[static_cast<size_t> (h - 1)];
            const float fcHarm = 3.0f * std::exp2 (static_cast<float> (c));
            float kept = 0.0f;
            for (int h = 1; h <= numHarmonics; ++h)
                kept += juce::square (W[static_cast<size_t> (h - 1)] * lpGain (static_cast<float> (h), fcHarm));
            const float hzAtKey = base * m.rootHz * fcHarm;
            set (toneId (layer, cutoff), juce::jlimit (20.0f, 20000.0f, hzAtKey / std::exp2 ((static_cast<float> (key) - 60.0f) / 12.0f)));
            set (toneId (layer, envAmt), 0.0f);
            out.values[levelAt].second = juce::jmin (1.0f, out.values[levelAt].second / juce::jmax (0.3f, std::sqrt (kept)));
        }
        else
        {
            set (toneId (layer, cutoff), 20000.0f);
            set (toneId (layer, envAmt), 0.0f);
        }
        const float vib = wob.vibDepth > 0.0f ? juce::jlimit (0.0f, 1.0f, wob.vibDepth / lfoPitchRange) : 0.0f;
        // Tremolo depth is the peak deviation in dB either way: gain 1 +/- d with (1 + d) / (1 - d) = 10^(2 depth / 20).
        const float trem = wob.tremDepthDb > 0.0f ? juce::jlimit (0.0f, 0.9f, std::tanh (wob.tremDepthDb * std::log (10.0f) / 20.0f)) : 0.0f;
        const float wahDepth = wahOn ? juce::jlimit (0.0f, 1.0f, wob.wahDepth * 12.0f / lfoCutoffRange * 1.5f) : 0.0f;
        const bool wahFilter = wahOn && (fltTop > fltBase || harmonicsUsed >= 2);
        set (toneId (layer, l1Pitch), vib);
        set (toneId (layer, l1Level), 0.0f);
        set (toneId (layer, l1Cutoff), wahFilter && wahLfo == 1 ? wahDepth : 0.0f);
        set (toneId (layer, l2Pitch), 0.0f);
        set (toneId (layer, l2Level), trem);
        set (toneId (layer, l2Cutoff), wahFilter && wahLfo == 2 ? wahDepth : 0.0f);
        if (++layer >= numTone)
            break;
    }
    out.used = layer;
    for (int l = layer; l < numTone; ++l)
        set (toneId (l, level), 0.0f);
    set (layersOn, 1.0f);
    set (detail, static_cast<float> (juce::jmax (1, layer)));

    // ---- noise layers: the Mint's noise bands split in two by frequency
    const int nb = m.numBands();
    std::vector<float> bw (static_cast<size_t> (nb), 0.0f), Eb (static_cast<size_t> (nb), 0.0f);
    for (int b = 0; b < nb; ++b)
    {
        const float f = m.bandHz[static_cast<size_t> (b)];
        const float lo = b > 0 ? std::sqrt (f * m.bandHz[static_cast<size_t> (b - 1)]) : f / 1.2f;
        const float hi = b + 1 < nb ? std::sqrt (f * m.bandHz[static_cast<size_t> (b + 1)]) : f * 1.2f;
        bw[static_cast<size_t> (b)] = juce::jmax (1.0f, hi - lo);
    }
    auto bandPower = [&] (int b, int t, float& l, float& r)
    {
        const auto i = static_cast<size_t> (b * T + t);
        l = m.noiseL[i] * m.noiseL[i] * bw[static_cast<size_t> (b)];
        r = m.noiseR[i] * m.noiseR[i] * bw[static_cast<size_t> (b)];
        return 0.5f * (l + r);
    };
    double eTot = 0.0;
    for (int b = 0; b < nb; ++b)
    {
        for (int t = 0; t < T; ++t) { float l, r; Eb[static_cast<size_t> (b)] += bandPower (b, t, l, r); }
        eTot += Eb[static_cast<size_t> (b)];
    }
    std::vector<std::pair<int, int>> groups; // [from, to)
    if (eTot > 0.0 && nb > 0)
    {
        int split = -1;
        double bestVar = std::numeric_limits<double>::max();
        auto stats = [&] (int from, int to, double& w, double& mean, double& var)
        {
            w = mean = var = 0.0;
            for (int b = from; b < to; ++b) { w += Eb[static_cast<size_t> (b)]; mean += Eb[static_cast<size_t> (b)] * std::log2 (m.bandHz[static_cast<size_t> (b)]); }
            if (w <= 0.0) return;
            mean /= w;
            for (int b = from; b < to; ++b) { const double d = std::log2 (m.bandHz[static_cast<size_t> (b)]) - mean; var += Eb[static_cast<size_t> (b)] * d * d; }
            var /= w;
        };
        double w0, m0, v0;
        stats (0, nb, w0, m0, v0);
        for (int s = 1; s < nb; ++s)
        {
            double wa, ma, va, wb, mb, vb;
            stats (0, s, wa, ma, va);
            stats (s, nb, wb, mb, vb);
            if (wa < 0.03 * eTot || wb < 0.03 * eTot)
                continue;
            const double within = wa * va + wb * vb;
            if (within < bestVar) { bestVar = within; split = s; }
        }
        if (split > 0 && bestVar < 0.6 * w0 * v0)
        {
            groups.emplace_back (0, split);
            groups.emplace_back (split, nb);
            if (Eb.size() > 0)
            {
                double wa = 0, wb = 0;
                for (int b = 0; b < split; ++b) wa += Eb[static_cast<size_t> (b)];
                for (int b = split; b < nb; ++b) wb += Eb[static_cast<size_t> (b)];
                if (wb > wa) std::swap (groups[0], groups[1]); // louder first
            }
        }
        else
        {
            groups.emplace_back (0, nb);
        }
    }

    // Per group: envelope, stereo, a first centre/width from the band statistics.
    struct NoiseFit { float pk = 0.0f, pan = 0.0f, logC = 10.0f, width = 2.0f, power = 0.0f; int slope = 0; Adsr adsr; };
    std::vector<NoiseFit> nf;
    for (int g = 0; g < numNoise && g < static_cast<int> (groups.size()); ++g)
    {
        const auto [from, to] = groups[static_cast<size_t> (g)];
        double w = 0.0, mean = 0.0, var = 0.0, eL = 0.0, eR = 0.0;
        std::vector<float> env (static_cast<size_t> (T), 0.0f);
        for (int b = from; b < to; ++b)
        {
            w += Eb[static_cast<size_t> (b)];
            mean += Eb[static_cast<size_t> (b)] * std::log2 (m.bandHz[static_cast<size_t> (b)]);
            for (int t = 0; t < T; ++t)
            {
                float l, r;
                env[static_cast<size_t> (t)] += bandPower (b, t, l, r);
                eL += l; eR += r;
            }
        }
        mean /= w;
        for (int b = from; b < to; ++b) { const double d = std::log2 (m.bandHz[static_cast<size_t> (b)]) - mean; var += Eb[static_cast<size_t> (b)] * d * d; }
        var /= w;
        NoiseFit f;
        for (auto& v : env) { v = std::sqrt (v); f.pk = juce::jmax (f.pk, v); }
        for (auto& v : env) v /= juce::jmax (1.0e-20f, f.pk);
        f.adsr = fitAdsr (env, hold, dt, true);
        f.pk *= f.adsr.gain;
        f.logC = static_cast<float> (mean);
        f.width = juce::jlimit (0.3f, 8.0f, static_cast<float> (3.46 * std::sqrt (var)) + 0.3f);
        f.power = static_cast<float> (w);
        const double sl = std::sqrt (eL), sr = std::sqrt (eR);
        f.pan = sl + sr > 0.0 ? juce::jlimit (-1.0f, 1.0f, static_cast<float> ((sr - sl) / (sr + sl))) : 0.0f;
        nf.push_back (f);
    }

    // Spectral shape: centre, width and edge slope of each noise layer, fitted so the
    // layers' summed spectrum matches the Mint's noise bands (in dB, 60 dB range).
    if (! nf.empty())
    {
        std::vector<float> measDb (static_cast<size_t> (nb)), wt (static_cast<size_t> (nb));
        float top = -300.0f;
        for (int b = 0; b < nb; ++b)
        {
            measDb[static_cast<size_t> (b)] = 10.0f * std::log10 (juce::jmax (1.0e-30f, Eb[static_cast<size_t> (b)]));
            top = juce::jmax (top, measDb[static_cast<size_t> (b)]);
        }
        for (int b = 0; b < nb; ++b)
        {
            auto& v = measDb[static_cast<size_t> (b)];
            v = juce::jmax (top - 60.0f, v);
            wt[static_cast<size_t> (b)] = (v - top + 60.0f) / 60.0f + 0.15f;
        }
        // Each layer's band energies (shape * its group's power), cached per layer.
        std::vector<std::vector<float>> layerE (nf.size(), std::vector<float> (static_cast<size_t> (nb), 0.0f));
        auto shape = [&] (const NoiseFit& f, std::vector<float>& e)
        {
            double sum = 0.0;
            for (int b = 0; b < nb; ++b)
            {
                e[static_cast<size_t> (b)] = noiseResponse (m.bandHz[static_cast<size_t> (b)], std::exp2 (f.logC), f.width, f.slope, MintModel::sampleRate) * bw[static_cast<size_t> (b)];
                sum += e[static_cast<size_t> (b)];
            }
            for (auto& v : e) v = sum > 0.0 ? static_cast<float> (v * f.power / sum) : 0.0f;
        };
        auto error = [&]
        {
            float err = 0.0f;
            for (int b = 0; b < nb; ++b)
            {
                float e = 0.0f;
                for (auto& le : layerE) e += le[static_cast<size_t> (b)];
                const float d = measDb[static_cast<size_t> (b)] - juce::jmax (top - 60.0f, 10.0f * std::log10 (juce::jmax (1.0e-30f, e)));
                err += wt[static_cast<size_t> (b)] * d * d;
            }
            return err;
        };
        for (size_t g = 0; g < nf.size(); ++g)
            shape (nf[g], layerE[g]);
        float bestErr = error();
        static constexpr float widths[] = { 0.3f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f };
        for (int round = 0; round < 3; ++round)
            for (size_t g = 0; g < nf.size(); ++g)
            {
                const NoiseFit start = nf[g];
                NoiseFit best = start;
                for (int slope = 0; slope < numSlopes; ++slope)
                    for (int c = -8; c <= 8; ++c)
                        for (float w : widths)
                        {
                            NoiseFit f = start;
                            f.slope = slope;
                            f.logC = start.logC + static_cast<float> (c) / 8.0f;
                            f.width = w;
                            shape (f, layerE[g]);
                            if (const float e = error(); e < bestErr * 0.999f) { bestErr = e; best = f; }
                        }
                nf[g] = best;
                shape (best, layerE[g]);
            }
    }

    Settings probe;
    probe.detail = 0;
    std::vector<float> peaks;
    for (int g = 0; g < numNoise; ++g)
    {
        if (g >= static_cast<int> (nf.size()))
        {
            set (noiseId (g, nLevel), 0.0f);
            continue;
        }
        const auto& f = nf[static_cast<size_t> (g)];
        const float centre = std::exp2 (f.logC) / std::exp2 ((static_cast<float> (key) - 60.0f) / 12.0f);
        const auto& a = f.adsr;
        set (noiseId (g, nCentre), juce::jlimit (20.0f, 20000.0f, centre));
        set (noiseId (g, nWidth), f.width);
        set (noiseId (g, nAttack), juce::jlimit (0.001f, 10.0f, a.a));
        set (noiseId (g, nDecay), juce::jlimit (0.005f, 30.0f, a.d));
        set (noiseId (g, nSustain), juce::jlimit (0.0f, 1.0f, a.s));
        set (noiseId (g, nRelease), juce::jlimit (0.005f, 30.0f, a.r));
        set (noiseId (g, nPan), f.pan);
        set (noiseId (g, nSlope), static_cast<float> (f.slope));
        auto& n = probe.noise[static_cast<size_t> (g)];
        n = { f.pk, juce::jlimit (20.0f, 20000.0f, centre), f.width, f.pan, juce::jlimit (0.001f, 10.0f, a.a), juce::jlimit (0.005f, 30.0f, a.d),
              juce::jlimit (0.0f, 1.0f, a.s), juce::jlimit (0.005f, 30.0f, a.r), f.slope };
        peaks.push_back (f.pk);
    }

    // Noise levels: measured through the same code. The Mint's noise alone vs these
    // layers at relative level, both played the same way (key up at the hold point).
    if (! peaks.empty())
    {
        const int len = juce::jmin (static_cast<int> (MintModel::sampleRate * 10.0), T * MintModel::hop);
        const int holdAt = hold * MintModel::hop;
        double eMint = 0.0, eBank = 0.0;
        {
            MintPlayer player;
            player.prepare (MintModel::sampleRate);
            player.start (&m, 7u);
            MintPlayer::Settings ms;
            ms.partials = 0.0f;
            float l[MintPlayer::blockSize], r[MintPlayer::blockSize];
            for (int pos = 0; pos < len; pos += MintPlayer::blockSize)
            {
                const int n = juce::jmin (static_cast<int> (MintPlayer::blockSize), len - pos);
                if (pos <= holdAt && pos + n > holdAt) player.release();
                std::fill (l, l + n, 0.0f);
                std::fill (r, r + n, 0.0f);
                player.render (l, r, n, ms);
                for (int i = 0; i < n; ++i) eMint += 0.5 * (l[i] * l[i] + r[i] * r[i]);
            }
        }
        {
            Bank bank;
            bank.prepare (MintModel::sampleRate);
            bank.start (probe, 7u);
            const float* waves = tablesFor (nullptr);
            for (int i = 0; i < len; ++i)
            {
                if (i == holdAt) bank.release();
                float mid = 0.0f, side = 0.0f;
                bank.next (probe, waves, static_cast<float> (key), 0.0f, 0.0f, mid, side);
                eBank += mid * mid;
            }
        }
        const float scale = eBank > 0.0 ? static_cast<float> (std::sqrt (eMint / eBank)) : 0.0f;
        for (size_t g = 0; g < peaks.size(); ++g)
            set (noiseId (static_cast<int> (g), nLevel), juce::jlimit (0.0f, 1.0f, peaks[g] * scale));
    }
    return out;
}
}
