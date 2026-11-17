#include "Effects.h"
#include "Parameters.h"

namespace cash
{
namespace
{
    constexpr int subBlock = 32;            // reverb gains are recomputed this often
    constexpr float apfGain = 0.6f;         // input diffusers
    constexpr float reverbIn = 0.35f;       // into each FDN line
    constexpr float reverbOut = 0.5f;       // wet level (calibrated: mix 50 % ~ as loud as dry on a held note)

    // FDN line lengths in ms at size 100 %, all coprime-ish. Size 0 = 20 % of these.
    constexpr std::array<float, 8> fdnMs { 37.1f, 41.9f, 47.3f, 53.9f, 59.3f, 67.7f, 73.1f, 83.3f };
    constexpr float minSizeScale = 0.2f;
    constexpr std::array<std::array<float, 2>, 2> diffuserMs { { { 5.3f, 7.9f }, { 5.9f, 8.7f } } };

    float dryGain (float gate, float mix) { return 1.0f - gate * (1.0f - juce::jmin (1.0f, 2.0f * (1.0f - mix))); }
    float wetGain (float gate, float mix) { return gate * juce::jmin (1.0f, 2.0f * mix); }

    float onePoleA (double hz, double sr)
    {
        return hz >= 0.45 * sr ? 1.0f : static_cast<float> (1.0 - std::exp (-juce::MathConstants<double>::twoPi * hz / sr));
    }

    void hadamard8 (std::array<float, 8>& v)
    {
        for (int h = 1; h < 8; h <<= 1)
            for (int i = 0; i < 8; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[static_cast<size_t> (j)], b = v[static_cast<size_t> (j + h)];
                    v[static_cast<size_t> (j)] = a + b;
                    v[static_cast<size_t> (j + h)] = a - b;
                }
        for (auto& x : v)
            x *= 0.35355339f; // 1 / sqrt (8): lossless
    }

    double noteSeconds (int note, const tempo::Info& t) { return tempo::noteQuarters (note, t) * 60.0 / t.bpm; }
}

//============================================================================== Line
void Effects::Line::allocate (int minLength)
{
    int n = 1;
    while (n < minLength + 4)
        n <<= 1;
    data.assign (static_cast<size_t> (n), 0.0f);
    mask = n - 1;
    write = 0;
}

void Effects::Line::clear()
{
    std::fill (data.begin(), data.end(), 0.0f);
    write = 0;
}

float Effects::Line::read (float d) const
{
    const int i = static_cast<int> (d);
    const float t = d - static_cast<float> (i);
    const float y0 = readInt (i), y1 = readInt (i + 1);
    if (i < 1)
        return y0 + t * (y1 - y0);
    // 4-point Hermite: smooth for moving (time / size) reads.
    const float ym = readInt (i - 1), y2 = readInt (i + 2);
    const float c1 = 0.5f * (y1 - ym);
    const float c2 = ym - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + y0;
}

//============================================================================== Effects
Effects::Effects (const Lookup& lookup)
{
    auto bind = [&lookup] (Raw& r, const char* id)
    {
        r.p = lookup (id);
        jassert (r.p != nullptr);
    };
    bind (delayOn, param::delayOn);
    bind (delayTime, param::delayTime);
    bind (delaySync, param::delaySync);
    bind (delayNote, param::delayNote);
    bind (delayFeedback, param::delayFeedback);
    bind (delayMix, param::delayMix);
    bind (delayPingPong, param::delayPingPong);
    bind (delayTone, param::delayTone);
    bind (reverbOn, param::reverbOn);
    bind (reverbSize, param::reverbSize);
    bind (reverbDecay, param::reverbDecay);
    bind (reverbDamp, param::reverbDamp);
    bind (reverbPreDelay, param::reverbPreDelay);
    bind (reverbPreSync, param::reverbPreSync);
    bind (reverbPreNote, param::reverbPreNote);
    bind (reverbWidth, param::reverbWidth);
    bind (reverbMix, param::reverbMix);
}

void Effects::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    const auto sr = sampleRate;
    auto ms = [sr] (double v) { return static_cast<int> (std::ceil (v * 0.001 * sr)); };

    for (auto& l : delay.line)
        l.allocate (static_cast<int> (maxDelaySeconds * sr) + 2);
    for (auto& l : reverb.pre)
        l.allocate (static_cast<int> (maxPreDelaySeconds * sr) + 2);
    for (size_t c = 0; c < 2; ++c)
        for (size_t s = 0; s < 2; ++s)
            reverb.diffuser[c][s].allocate (ms (diffuserMs[c][s]) + 1);
    for (size_t i = 0; i < numLines; ++i)
        reverb.fdn[i].allocate (ms (fdnMs[i]) + 4);

    for (auto* f : { &delay.fade, &reverb.fade })
    {
        f->gate.reset (sr, 0.01);
        f->mix.reset (sr, 0.015);
    }
    delay.time.reset (sr, 0.12);
    delay.feedback.reset (sr, 0.015);
    delay.pingPong.reset (sr, 0.02);
    reverb.size.reset (sr, 0.2);
    reverb.decay.reset (sr / subBlock, 0.1); // stepped once per sub-block
    reverb.preDelay.reset (sr, 0.12);
    reverb.width.reset (sr, 0.015);
    reset();
}

void Effects::reset()
{
    for (auto& l : delay.line) l.clear();
    for (auto& l : reverb.pre) l.clear();
    for (auto& ch : reverb.diffuser) for (auto& l : ch) l.clear();
    for (auto& l : reverb.fdn) l.clear();
    delay.lp.fill (0.0f);
    delay.hp.fill (0.0f);
    reverb.damp.fill (0.0f);
    delay.running = reverb.running = false;
    fresh = true;
}

double Effects::delaySeconds (const tempo::Info& t) const
{
    const double s = delaySync.asInt() == param::syncMode ? noteSeconds (delayNote.asInt(), t) : delayTime.get() * 0.001;
    return juce::jlimit (0.001, maxDelaySeconds, s);
}

double Effects::preDelaySeconds (const tempo::Info& t) const
{
    const double s = reverbPreSync.asInt() == param::syncMode ? noteSeconds (reverbPreNote.asInt(), t) : reverbPreDelay.get() * 0.001;
    return juce::jlimit (0.0, maxPreDelaySeconds, s);
}

double Effects::tailSeconds() const
{
    const tempo::Info t;
    double tail = 0.0;
    if (delayOn.get() > 0.5f)
    {
        const double fb = delayFeedback.get();
        const double repeats = fb > 1.0e-3 ? std::log (1.0e-3) / std::log (fb) : 1.0; // until -60 dB
        tail = delaySeconds (t) * (1.0 + repeats);
    }
    if (reverbOn.get() > 0.5f)
        tail = tail + preDelaySeconds (t) + reverbDecay.get();
    return juce::jmin (60.0, tail);
}

void Effects::readTargets (const tempo::Info& t)
{
    const bool dOn = delayOn.get() > 0.5f, rOn = reverbOn.get() > 0.5f;
    delay.fade.gate.setTargetValue (dOn ? 1.0f : 0.0f);
    delay.fade.mix.setTargetValue (juce::jlimit (0.0f, 1.0f, delayMix.get()));
    // At least one sample of delay (the line is read before it is written).
    delay.time.setTargetValue (juce::jmax (1.0f, static_cast<float> (delaySeconds (t) * sampleRate)));
    delay.feedback.setTargetValue (juce::jlimit (0.0f, 0.95f, delayFeedback.get()));
    delay.pingPong.setTargetValue (delayPingPong.asInt() == param::pingPong ? 1.0f : 0.0f);

    // Tone: below 0 a low-pass closes from 20 kHz to ~220 Hz; above 0 a high-pass opens to ~1.8 kHz. 0 = flat.
    const double tone = juce::jlimit (-1.0f, 1.0f, delayTone.get());
    delay.lpA = tone < 0.0 ? onePoleA (20000.0 * std::exp2 (6.5 * tone), sampleRate) : 1.0f;
    delay.hpA = tone > 0.0 ? onePoleA (20.0 * std::exp2 (6.5 * tone), sampleRate) : 0.0f;

    reverb.fade.gate.setTargetValue (rOn ? 1.0f : 0.0f);
    reverb.fade.mix.setTargetValue (juce::jlimit (0.0f, 1.0f, reverbMix.get()));
    reverb.size.setTargetValue (juce::jlimit (0.0f, 1.0f, reverbSize.get()));
    reverb.decay.setTargetValue (juce::jlimit (0.1f, 30.0f, reverbDecay.get()));
    reverb.preDelay.setTargetValue (static_cast<float> (preDelaySeconds (t) * sampleRate));
    reverb.width.setTargetValue (juce::jlimit (0.0f, 1.0f, reverbWidth.get()));
    reverb.dampA = onePoleA (18000.0 * std::pow (0.06, juce::jlimit (0.0, 1.0, static_cast<double> (reverbDamp.get()))), sampleRate);
}

void Effects::snapAll()
{
    for (auto* s : { &delay.fade.gate, &delay.fade.mix, &delay.time, &delay.feedback, &delay.pingPong,
                     &reverb.fade.gate, &reverb.fade.mix, &reverb.size, &reverb.decay, &reverb.preDelay, &reverb.width })
        s->setCurrentAndTargetValue (s->getTargetValue());
}

void Effects::process (juce::AudioBuffer<float>& buffer, const tempo::Info& t)
{
    juce::ScopedNoDenormals noDenormals;
    readTargets (t);
    if (fresh)
    {
        snapAll(); // the host's first block decides the tempo: echoes land on time from the start
        fresh = false;
    }

    // Switched on from idle: start from a clean tail, at the current settings.
    auto wake = [] (bool& running, Fade& f, auto&& clearState, std::initializer_list<juce::SmoothedValue<float>*> snaps)
    {
        if (running || (f.gate.getCurrentValue() <= 0.0f && f.gate.getTargetValue() <= 0.0f))
            return;
        clearState();
        for (auto* s : snaps)
            s->setCurrentAndTargetValue (s->getTargetValue());
        f.mix.setCurrentAndTargetValue (f.mix.getTargetValue());
        running = true;
    };
    wake (delay.running, delay.fade, [this]
          {
              for (auto& l : delay.line) l.clear();
              delay.lp.fill (0.0f);
              delay.hp.fill (0.0f);
          }, { &delay.time, &delay.feedback, &delay.pingPong });
    wake (reverb.running, reverb.fade, [this]
          {
              for (auto& l : reverb.pre) l.clear();
              for (auto& ch : reverb.diffuser) for (auto& l : ch) l.clear();
              for (auto& l : reverb.fdn) l.clear();
              reverb.damp.fill (0.0f);
          }, { &reverb.size, &reverb.decay, &reverb.preDelay, &reverb.width });

    if (! delay.running && ! reverb.running)
        return; // both off: not a single sample touched

    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2)
    {
        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        if (delay.running)  processDelay (l, r, n);
        if (reverb.running) processReverb (l, r, n);
    }
    else if (buffer.getNumChannels() == 1)
    {
        // Mono out: run in stereo on a copy and fold down.
        std::array<float, 256> right {};
        auto* l = buffer.getWritePointer (0);
        for (int pos = 0; pos < n; pos += static_cast<int> (right.size()))
        {
            const int k = juce::jmin (static_cast<int> (right.size()), n - pos);
            std::copy (l + pos, l + pos + k, right.begin());
            if (delay.running)  processDelay (l + pos, right.data(), k);
            if (reverb.running) processReverb (l + pos, right.data(), k);
            for (int i = 0; i < k; ++i)
                l[pos + i] = 0.5f * (l[pos + i] + right[static_cast<size_t> (i)]);
        }
    }

    // Faded out completely: idle until switched on again.
    for (auto [running, fade] : { std::pair { &delay.running, &delay.fade }, std::pair { &reverb.running, &reverb.fade } })
        if (fade->gate.getCurrentValue() <= 0.0f && fade->gate.getTargetValue() <= 0.0f)
            *running = false;
}

void Effects::processDelay (float* l, float* r, int n)
{
    auto& d = delay;
    for (int i = 0; i < n; ++i)
    {
        const float gate = d.fade.gate.getNextValue(), mix = d.fade.mix.getNextValue();
        const float time = d.time.getNextValue() - 1.0f; // read before write
        const float fb = d.feedback.getNextValue(), pp = d.pingPong.getNextValue();

        const float xl = l[i], xr = r[i];
        const float yl = d.line[0].read (time), yr = d.line[1].read (time);

        // Stereo: each side repeats itself. Ping-pong: the input (summed) goes in on the
        // left and the repeats cross sides, so echoes alternate L, R, L, ...
        float wl = xl + fb * yl, wr = xr + fb * yr;
        if (pp > 0.0f)
        {
            wl += pp * ((0.5f * (xl + xr) + fb * yr) - wl);
            wr += pp * (fb * yl - wr);
        }

        float w[2] = { wl, wr };
        for (size_t c = 0; c < 2; ++c)
        {
            d.lp[c] += d.lpA * (w[c] - d.lp[c]);
            w[c] = d.lp[c];
            d.hp[c] += d.hpA * (w[c] - d.hp[c]);
            w[c] -= d.hp[c];
            d.line[c].push (w[c]);
        }

        const float dry = dryGain (gate, mix), wet = wetGain (gate, mix);
        l[i] = xl * dry + yl * wet;
        r[i] = xr * dry + yr * wet;
    }
}

void Effects::updateReverbGains()
{
    // Each line loses 60 dB per decay time, whatever its length.
    const float scale = minSizeScale + (1.0f - minSizeScale) * reverb.size.getCurrentValue();
    const double rt = static_cast<double> (reverb.decay.getNextValue()) * sampleRate;
    for (size_t i = 0; i < numLines; ++i)
    {
        const double len = fdnMs[i] * 0.001 * sampleRate * scale;
        reverb.gain[i] = static_cast<float> (std::pow (10.0, -3.0 * len / rt));
    }
}

void Effects::processReverb (float* l, float* r, int n)
{
    auto& v = reverb;
    const float msToSamples = static_cast<float> (0.001 * sampleRate);
    std::array<int, 2> apLen[2];
    for (size_t c = 0; c < 2; ++c)
        for (size_t s = 0; s < 2; ++s)
            apLen[c][s] = juce::jmax (1, static_cast<int> (diffuserMs[c][s] * msToSamples));

    for (int i = 0; i < n; ++i)
    {
        if (i % subBlock == 0)
            updateReverbGains();

        const float gate = v.fade.gate.getNextValue(), mix = v.fade.mix.getNextValue();
        const float size = v.size.getNextValue(), width = v.width.getNextValue();
        const float pre = v.preDelay.getNextValue();
        const float scale = minSizeScale + (1.0f - minSizeScale) * size;

        const float xl = l[i], xr = r[i];
        v.pre[0].push (xl);
        v.pre[1].push (xr);
        float in[2] = { v.pre[0].read (pre), v.pre[1].read (pre) };

        // Input diffusion: two Schroeder all-passes per side smear the attack.
        for (size_t c = 0; c < 2; ++c)
            for (size_t s = 0; s < 2; ++s)
            {
                auto& ap = v.diffuser[c][s];
                const float delayed = ap.readInt (apLen[c][s] - 1);
                const float w = in[c] - apfGain * delayed;
                ap.push (w);
                in[c] = delayed + apfGain * w;
            }

        // 8-line feedback delay network, Hadamard mixed, damped in the loop.
        std::array<float, numLines> y {}, s {};
        for (size_t k = 0; k < numLines; ++k)
        {
            y[k] = v.fdn[k].read (fdnMs[k] * msToSamples * scale - 1.0f);
            v.damp[k] += v.dampA * (y[k] - v.damp[k]);
            s[k] = v.gain[k] * v.damp[k];
        }
        hadamard8 (s);
        for (size_t k = 0; k < numLines; ++k)
        {
            const float sign = (k & 2) != 0 ? -1.0f : 1.0f;
            v.fdn[k].push (s[k] + sign * reverbIn * in[k & 1]);
        }

        float wl = (y[0] - y[2] + y[4] - y[6]) * reverbOut;
        float wr = (y[1] - y[3] + y[5] - y[7]) * reverbOut;
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
        wl = mid + side;
        wr = mid - side;

        const float dry = dryGain (gate, mix), wet = wetGain (gate, mix);
        l[i] = xl * dry + wl * wet;
        r[i] = xr * dry + wr * wet;
    }
}
}
