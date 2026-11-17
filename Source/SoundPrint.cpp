#include "SoundPrint.h"
#include <juce_dsp/juce_dsp.h>

namespace cash::analysis
{
namespace
{
    constexpr float floorDb = -60.0f;
    constexpr float lowestBandHz = 40.0f, highestBandHz = 16000.0f;
    constexpr int fftOrder = 11, fftSize = 1 << fftOrder;            // spectrum segments
    constexpr int harmOrder = 12, harmSize = 1 << harmOrder;         // harmonic analysis
    constexpr int windowsPerSegment = 3, windowHop = 512;

    float toNorm (float db) { return 1.0f + juce::jlimit (floorDb, 0.0f, db) / -floorDb; }
    float gainToDb (float g) { return 20.0f * std::log10 (juce::jmax (g, 1.0e-9f)); }
    float powerToDb (float p) { return 10.0f * std::log10 (juce::jmax (p, 1.0e-18f)); }

    // Power spectrum (fftSize/2 + 1 bins) of a Hann-windowed slice centred on `centre`.
    void powerSpectrum (const float* x, int n, int centre, int order, std::vector<float>& power)
    {
        const int size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> buf (static_cast<size_t> (size * 2), 0.0f);
        const int start = centre - size / 2;
        for (int i = 0; i < size; ++i)
        {
            const int k = start + i;
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (size - 1));
            buf[static_cast<size_t> (i)] = (k >= 0 && k < n) ? x[k] * w : 0.0f;
        }
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        power.resize (static_cast<size_t> (size / 2 + 1));
        for (size_t i = 0; i < power.size(); ++i)
            power[i] = buf[i] * buf[i];
    }

    const std::array<float, SoundPrint::numBands + 1>& bandEdges()
    {
        static const auto edges = []
        {
            std::array<float, SoundPrint::numBands + 1> e {};
            for (size_t i = 0; i < e.size(); ++i)
                e[i] = lowestBandHz * std::pow (highestBandHz / lowestBandHz, static_cast<float> (i) / SoundPrint::numBands);
            return e;
        }();
        return edges;
    }

    // Mean difference weighted by loudness: what's loud in either sound counts
    // most; the fizz 50 dB down between harmonics (which wobbles with oscillator
    // phase from one render to the next) counts least.
    float meanAbsDiff (const float* a, const float* b, size_t n)
    {
        float s = 0.0f, w = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            const float loud = juce::jmax (a[i], b[i]);
            const float weight = 0.1f + loud * loud;
            s += weight * std::abs (a[i] - b[i]);
            w += weight;
        }
        return w > 0.0f ? s / w : 0.0f;
    }
}

float noteToHz (int midiNote)
{
    return 440.0f * std::exp2 (static_cast<float> (midiNote - 69) / 12.0f);
}

juce::String noteName (int midiNote)
{
    return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 3);
}

float detectPitch (const float* x, int n, double sr)
{
    // YIN (de Cheveigne & Kawahara) on a few windows after the attack; median of the pitched ones.
    constexpr int window = 2048;
    const int tauMin = static_cast<int> (sr / 1500.0), tauMax = static_cast<int> (sr / 40.0);
    if (n < window + tauMax)
        return 0.0f;

    std::vector<float> found;
    std::vector<float> d (static_cast<size_t> (tauMax + 1));
    for (const float where : { 0.15f, 0.3f, 0.45f })
    {
        const int start = juce::jlimit (0, n - window - tauMax, static_cast<int> (where * static_cast<float> (n)));
        const float* s = x + start;

        double energy = 0.0;
        for (int i = 0; i < window; ++i)
            energy += s[i] * s[i];
        if (energy < 1.0e-6)
            continue;

        d[0] = 1.0f;
        double running = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double sum = 0.0;
            for (int i = 0; i < window; ++i)
            {
                const double diff = s[i] - s[i + tau];
                sum += diff * diff;
            }
            running += sum;
            d[static_cast<size_t> (tau)] = running > 0.0 ? static_cast<float> (sum * tau / running) : 1.0f;
        }

        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (d[static_cast<size_t> (tau)] < 0.2f)
            {
                while (tau + 1 < tauMax && d[static_cast<size_t> (tau + 1)] < d[static_cast<size_t> (tau)])
                    ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
            continue;

        // Parabolic interpolation around the dip.
        const float a = d[static_cast<size_t> (best - 1)], b = d[static_cast<size_t> (best)], c = d[static_cast<size_t> (best + 1)];
        const float denom = a - 2.0f * b + c;
        const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
        found.push_back (static_cast<float> (sr) / (static_cast<float> (best) + juce::jlimit (-1.0f, 1.0f, shift)));
    }

    if (found.size() < 2)
        return 0.0f;
    std::sort (found.begin(), found.end());
    return found[found.size() / 2];
}

SoundPrint analyse (const float* x, int n, float knownF0)
{
    SoundPrint sp;
    const double sr = SoundPrint::sampleRate;
    if (x == nullptr || n < fftSize)
        return sp;

    sp.seconds = n / sr;

    // ---- Level envelope (RMS per frame, relative to the loudest frame).
    std::array<float, SoundPrint::envFrames> rms {};
    const int frame = n / SoundPrint::envFrames;
    float peak = 0.0f;
    int peakFrame = 0;
    for (int f = 0; f < SoundPrint::envFrames; ++f)
    {
        double sum = 0.0;
        for (int i = f * frame; i < (f + 1) * frame; ++i)
            sum += x[i] * x[i];
        rms[static_cast<size_t> (f)] = static_cast<float> (std::sqrt (sum / juce::jmax (1, frame)));
        if (rms[static_cast<size_t> (f)] > peak)
        {
            peak = rms[static_cast<size_t> (f)];
            peakFrame = f;
        }
    }
    if (peak < 1.0e-5f)
        return sp; // silence

    for (size_t f = 0; f < rms.size(); ++f)
        sp.envelope[f] = toNorm (gainToDb (rms[f] / peak));

    // ---- Key-down time: the release is the falling run at the end.
    const auto frameSeconds = static_cast<double> (frame) / sr;
    if (sp.envelope.back() > 0.6f)
    {
        sp.holdSeconds = sp.seconds;
    }
    else
    {
        int f = SoundPrint::envFrames - 1;
        while (f > peakFrame && sp.envelope[static_cast<size_t> (f - 1)] >= sp.envelope[static_cast<size_t> (f)] - 0.02f)
            --f;
        sp.holdSeconds = juce::jmax (0.1, (f + 0.5) * frameSeconds);
    }

    // ---- Pitch.
    sp.f0 = knownF0 > 0.0f ? knownF0 : detectPitch (x, n, sr);
    sp.pitched = sp.f0 > 0.0f;
    if (sp.pitched)
        sp.midiNote = juce::jlimit (24, 96, juce::roundToInt (69.0f + 12.0f * std::log2 (sp.f0 / 440.0f)));

    // ---- Spectrum over time.
    const auto& edges = bandEdges();
    const float binHz = static_cast<float> (sr / fftSize);
    std::vector<float> power, total (static_cast<size_t> (fftSize / 2 + 1), 0.0f);
    float weightSum = 0.0f;
    for (int s = 0; s < SoundPrint::numSegments; ++s)
    {
        const int centre = static_cast<int> ((s + 0.5) / SoundPrint::numSegments * n);
        std::vector<float> avg (static_cast<size_t> (fftSize / 2 + 1), 0.0f);
        for (int w = 0; w < windowsPerSegment; ++w)
        {
            powerSpectrum (x, n, centre + (w - windowsPerSegment / 2) * windowHop, fftOrder, power);
            for (size_t i = 0; i < avg.size(); ++i)
                avg[i] += power[i] / windowsPerSegment;
        }

        std::array<float, SoundPrint::numBands> bandDb {};
        float maxDb = -1000.0f;
        for (int b = 0; b < SoundPrint::numBands; ++b)
        {
            const int lo = juce::jlimit (1, fftSize / 2, static_cast<int> (edges[static_cast<size_t> (b)] / binHz));
            const int hi = juce::jlimit (lo + 1, fftSize / 2 + 1, static_cast<int> (edges[static_cast<size_t> (b + 1)] / binHz) + 1);
            double sum = 0.0;
            for (int i = lo; i < hi; ++i)
                sum += avg[static_cast<size_t> (i)];
            bandDb[static_cast<size_t> (b)] = powerToDb (static_cast<float> (sum / (hi - lo)));
            maxDb = juce::jmax (maxDb, bandDb[static_cast<size_t> (b)]);
        }
        for (int b = 0; b < SoundPrint::numBands; ++b)
            sp.spectrum[static_cast<size_t> (s)][static_cast<size_t> (b)] = toNorm (bandDb[static_cast<size_t> (b)] - maxDb);

        const int envIndex = juce::jlimit (0, SoundPrint::envFrames - 1, centre / juce::jmax (1, frame));
        const float level = sp.envelope[static_cast<size_t> (envIndex)];
        sp.segmentLevel[static_cast<size_t> (s)] = level;

        // Loudness-weighted average spectrum for flatness and centroid.
        float maxPower = 1.0e-18f;
        for (auto p : avg)
            maxPower = juce::jmax (maxPower, p);
        for (size_t i = 0; i < avg.size(); ++i)
            total[i] += level * avg[i] / maxPower;
        weightSum += level;
    }

    if (weightSum > 0.0f)
    {
        const int lo = static_cast<int> (100.0f / binHz), hi = static_cast<int> (10000.0f / binHz);
        double logSum = 0.0, linSum = 0.0, centroidNum = 0.0;
        for (int i = lo; i < hi; ++i)
        {
            const double p = juce::jmax (1.0e-12, static_cast<double> (total[static_cast<size_t> (i)] / weightSum));
            logSum += std::log (p);
            linSum += p;
            centroidNum += p * i * binHz;
        }
        const double count = hi - lo;
        sp.flatness = static_cast<float> (std::exp (logSum / count) / (linSum / count));
        sp.centroidHz = static_cast<float> (centroidNum / juce::jmax (1.0e-12, linSum));
    }

    // ---- Harmonic balance, a quarter of the way in (past most attacks).
    if (sp.pitched)
    {
        powerSpectrum (x, n, juce::jmax (harmSize / 2, n / 4), harmOrder, power);
        const float hBin = static_cast<float> (sr / harmSize);
        std::array<float, SoundPrint::numHarmonics> db {};
        float maxDb = -1000.0f;
        for (int h = 0; h < SoundPrint::numHarmonics; ++h)
        {
            const float f = sp.f0 * static_cast<float> (h + 1);
            if (f > 18000.0f)
            {
                db[static_cast<size_t> (h)] = -1000.0f;
                continue;
            }
            const int lo = juce::jmax (1, static_cast<int> (f * 0.97f / hBin)), hi = juce::jmin (harmSize / 2, static_cast<int> (f * 1.03f / hBin) + 1);
            float best = 0.0f;
            for (int i = lo; i <= hi; ++i)
                best = juce::jmax (best, power[static_cast<size_t> (i)]);
            db[static_cast<size_t> (h)] = powerToDb (best);
            maxDb = juce::jmax (maxDb, db[static_cast<size_t> (h)]);
        }
        for (size_t h = 0; h < db.size(); ++h)
            sp.harmonics[h] = toNorm (db[h] - maxDb);
    }

    sp.valid = true;
    return sp;
}

Parts distanceParts (const SoundPrint& t, const SoundPrint& c)
{
    Parts parts { meanAbsDiff (t.envelope.data(), c.envelope.data(), t.envelope.size()), 0.0f, 0.0f, std::abs (t.flatness - c.flatness) };

    float wsum = 0.0f;
    for (size_t s = 0; s < t.spectrum.size(); ++s)
    {
        const float w = 0.05f + t.segmentLevel[s];
        parts.spectrum += w * meanAbsDiff (t.spectrum[s].data(), c.spectrum[s].data(), t.spectrum[s].size());
        wsum += w;
    }
    parts.spectrum /= wsum;
    parts.harmonics = t.pitched ? meanAbsDiff (t.harmonics.data(), c.harmonics.data(), t.harmonics.size()) : 0.0f;
    return parts;
}

float distance (const SoundPrint& t, const SoundPrint& c)
{
    if (! t.valid || ! c.valid)
        return 1.0f;
    const auto p = distanceParts (t, c);
    return t.pitched ? 0.30f * p.envelope + 0.40f * p.spectrum + 0.20f * p.harmonics + 0.10f * p.flatness
                     : 0.35f * p.envelope + 0.50f * p.spectrum + 0.15f * p.flatness;
}

int matchPercent (float d)
{
    // Calibrated so two renders of the same patch read ~95-100 % and
    // unrelated sounds land well under 50 %.
    return juce::jlimit (0, 100, juce::roundToInt (100.0f * std::exp (-d * d * 40.0f)));
}
}
