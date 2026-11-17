#include "SoundPrint.h"
#include <algorithm>
#include <tuple>
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

Wobble findWobble (const std::vector<float>& curve, double hop, float minDepth)
{
    // Quadratic detrend (removes the envelope's slow sweep without eating slow
    // wobbles), then the strongest autocorrelation peak between 0.7 and 15 Hz.
    const int n = static_cast<int> (curve.size());
    if (n < 8 || n * hop < 0.4)
        return {};

    double m[3][4] {};
    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * i / (n - 1) - 1.0;
        const double basis[3] { 1.0, t, t * t };
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                m[r][c] += basis[r] * basis[c];
            m[r][3] += basis[r] * curve[static_cast<size_t> (i)];
        }
    }
    for (int col = 0; col < 3; ++col) // Gauss-Jordan (well conditioned: t in [-1, 1])
    {
        for (int r = 0; r < 3; ++r)
        {
            if (r == col || std::abs (m[col][col]) < 1.0e-12)
                continue;
            const double k = m[r][col] / m[col][col];
            for (int c = 0; c < 4; ++c)
                m[r][c] -= k * m[col][c];
        }
    }
    const double a0 = m[0][3] / m[0][0], a1 = m[1][3] / m[1][1], a2 = m[2][3] / m[2][2];

    std::vector<double> res (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * i / (n - 1) - 1.0;
        res[static_cast<size_t> (i)] = curve[static_cast<size_t> (i)] - (a0 + a1 * t + a2 * t * t);
    }

    // Clip outliers (a stray pitch frame, a click) to 3 robust sigmas so a couple
    // of bad frames can't swamp a real wobble.
    {
        std::vector<double> absDev (res.size());
        for (size_t i = 0; i < res.size(); ++i)
            absDev[i] = std::abs (res[i]);
        std::nth_element (absDev.begin(), absDev.begin() + static_cast<std::ptrdiff_t> (absDev.size() / 2), absDev.end());
        const double limit = juce::jmax (1.0e-9, 3.0 * 1.4826 * absDev[absDev.size() / 2]);
        for (auto& v : res)
            v = juce::jlimit (-limit, limit, v);
    }
    double energy = 0.0;
    for (auto v : res)
        energy += v * v;
    const double rmsRes = std::sqrt (energy / n);
    if (rmsRes * std::sqrt (2.0) < minDepth || energy <= 0.0)
        return {};

    const double maxRate = juce::jmin (15.0, 0.45 / hop), minRate = juce::jmax (0.7, 1.5 / (n * hop));
    const int lagMin = juce::jmax (2, static_cast<int> (1.0 / (maxRate * hop))), lagMax = juce::jmin (n * 2 / 3, static_cast<int> (std::ceil (1.0 / (minRate * hop))));
    if (lagMax <= lagMin + 1)
        return {};

    std::vector<double> acf (static_cast<size_t> (lagMax + 2), 0.0);
    for (int lag = lagMin - 1; lag <= lagMax + 1 && lag < n; ++lag)
    {
        double s = 0.0;
        for (int i = 0; i + lag < n; ++i)
            s += res[static_cast<size_t> (i)] * res[static_cast<size_t> (i + lag)];
        acf[static_cast<size_t> (lag)] = s / energy * n / (n - lag); // unbiased
    }

    // The shortest lag whose peak is nearly as strong as the strongest (multiples
    // of the period peak too, and the unbiased estimate can lift them).
    std::vector<int> peaks;
    double strongest = 0.0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const auto a = acf[static_cast<size_t> (lag)];
        if (a > acf[static_cast<size_t> (lag - 1)] && a >= acf[static_cast<size_t> (lag + 1)] && a >= 0.4)
        {
            peaks.push_back (lag);
            strongest = juce::jmax (strongest, a);
        }
    }
    int best = -1;
    for (int lag : peaks)
        if (acf[static_cast<size_t> (lag)] >= 0.8 * strongest)
        {
            best = lag;
            break;
        }
    if (best < 0)
        return {};

    const double ya = acf[static_cast<size_t> (best - 1)], yb = acf[static_cast<size_t> (best)], yc = acf[static_cast<size_t> (best + 1)];
    const double denom = ya - 2.0 * yb + yc;
    const double shift = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (ya - yc) / denom) : 0.0;
    return { static_cast<float> (1.0 / ((best + shift) * hop)), static_cast<float> (rmsRes * std::sqrt (2.0)) };
}

namespace
{
    constexpr int trackWindow = 2048, trackOrder = 12, trackSize = 1 << trackOrder;
    constexpr float voicedLevel = 0.4f; // frames quieter than -36 dB have no pitch

    struct TrackTables
    {
        std::array<float, trackWindow> window {};
        std::array<float, trackWindow> windowAcf {};
        TrackTables()
        {
            for (int i = 0; i < trackWindow; ++i)
                window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (trackWindow - 1));
            for (int lag = 0; lag < trackWindow; ++lag)
            {
                double s = 0.0;
                for (int i = 0; i + lag < trackWindow; ++i)
                    s += window[static_cast<size_t> (i)] * window[static_cast<size_t> (i + lag)];
                windowAcf[static_cast<size_t> (lag)] = static_cast<float> (s);
            }
        }
    };

    const TrackTables& tables()
    {
        static const TrackTables t;
        return t;
    }

    // The attack's end: the first frame within ~1 dB of the loudest (a flat
    // sustain's exact maximum can be anywhere, even at the very end).
    int firstNearPeak (const std::vector<float>& level)
    {
        float top = 0.0f;
        for (auto v : level)
            top = juce::jmax (top, v);
        for (size_t f = 0; f < level.size(); ++f)
            if (level[f] >= top - 0.02f)
                return static_cast<int> (f);
        return 0;
    }

    float median (std::vector<float> v)
    {
        if (v.empty())
            return 0.0f;
        std::nth_element (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (v.size() / 2), v.end());
        return v[v.size() / 2];
    }

    // Level, pitch (YIN's cumulative-mean difference built from an FFT
    // autocorrelation) and brightness for every frame.
    void trackFrames (SoundPrint& sp, const float* x, int n, const SoundPrint* like)
    {
        const double sr = SoundPrint::sampleRate;
        const int hop = like != nullptr ? juce::jmax (1, juce::roundToInt (like->hopSeconds * sr))
                                        : juce::jmax (441, (n + SoundPrint::maxFrames - 1) / SoundPrint::maxFrames);
        const int frames = like != nullptr ? static_cast<int> (like->level.size()) : juce::jmax (1, n / hop);
        sp.hopSeconds = hop / sr;
        sp.level.assign (static_cast<size_t> (frames), 0.0f);
        sp.pitch.assign (static_cast<size_t> (frames), 0.0f);
        sp.brightness.assign (static_cast<size_t> (frames), 0.0f);

        // Level: RMS over two hops round each frame centre.
        std::vector<float> rms (static_cast<size_t> (frames), 0.0f);
        float peak = 1.0e-9f;
        for (int f = 0; f < frames; ++f)
        {
            const int a = juce::jlimit (0, n, f * hop - hop / 2), b = juce::jlimit (0, n, f * hop + hop + hop / 2);
            double s = 0.0;
            for (int i = a; i < b; ++i)
                s += x[i] * x[i];
            rms[static_cast<size_t> (f)] = static_cast<float> (std::sqrt (s / juce::jmax (1, b - a)));
            peak = juce::jmax (peak, rms[static_cast<size_t> (f)]);
        }

        const auto& tab = tables();
        juce::dsp::FFT fft (trackOrder);
        std::vector<float> buf (static_cast<size_t> (trackSize * 2)), power (static_cast<size_t> (trackSize / 2 + 1));
        std::vector<float> cmnd (static_cast<size_t> (trackWindow));
        const float binHz = static_cast<float> (sr / trackSize);
        const int tauMin = static_cast<int> (sr / 1500.0), tauMax = juce::jmin (static_cast<int> (sr / 40.0), trackWindow * 6 / 10);
        const int loBin = static_cast<int> (50.0f / binHz), hiBin = juce::jmin (trackSize / 2, static_cast<int> (16000.0f / binHz));

        for (int f = 0; f < frames; ++f)
        {
            const auto fi = static_cast<size_t> (f);
            sp.level[fi] = toNorm (gainToDb (rms[fi] / peak));

            const int start = f * hop + hop / 2 - trackWindow / 2;
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < trackWindow; ++i)
            {
                const int k = start + i;
                buf[static_cast<size_t> (i)] = (k >= 0 && k < n) ? x[k] * tab.window[static_cast<size_t> (i)] : 0.0f;
            }
            fft.performRealOnlyForwardTransform (buf.data(), true);

            double pSum = 0.0, fSum = 0.0;
            for (int i = 0; i <= trackSize / 2; ++i)
            {
                const float re = buf[static_cast<size_t> (2 * i)], im = buf[static_cast<size_t> (2 * i + 1)];
                power[static_cast<size_t> (i)] = re * re + im * im;
                if (i >= loBin && i < hiBin)
                {
                    pSum += power[static_cast<size_t> (i)];
                    fSum += power[static_cast<size_t> (i)] * i * binHz;
                }
            }
            sp.brightness[fi] = pSum > 1.0e-18 ? static_cast<float> (std::log2 (juce::jmax (20.0, fSum / pSum))) : (f > 0 ? sp.brightness[fi - 1] : 10.0f);

            if (sp.level[fi] < voicedLevel)
                continue;

            // Autocorrelation = inverse FFT of the power spectrum (full, conjugate-symmetric).
            for (int i = 0; i < trackSize; ++i)
            {
                buf[static_cast<size_t> (2 * i)] = power[static_cast<size_t> (i <= trackSize / 2 ? i : trackSize - i)];
                buf[static_cast<size_t> (2 * i + 1)] = 0.0f;
            }
            fft.performRealOnlyInverseTransform (buf.data());
            const float r0 = buf[0] / tab.windowAcf[0];
            if (r0 <= 1.0e-12f)
                continue;

            // YIN's cumulative mean normalised difference, d(tau) = 2 (r0 - r(tau)).
            double running = 0.0;
            cmnd[0] = 1.0f;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                const float r = buf[static_cast<size_t> (tau)] / tab.windowAcf[static_cast<size_t> (tau)];
                const double d = juce::jmax (0.0, 2.0 * (r0 - r));
                running += d;
                cmnd[static_cast<size_t> (tau)] = running > 0.0 ? static_cast<float> (d * tau / running) : 1.0f;
            }

            int best = -1;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                if (cmnd[static_cast<size_t> (tau)] < 0.2f)
                {
                    while (tau + 1 < tauMax && cmnd[static_cast<size_t> (tau + 1)] < cmnd[static_cast<size_t> (tau)])
                        ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 1)
                continue;
            // Octave-up guard: if 2 or 3 times the lag fits much better, the dip we took was
            // a strong harmonic, not the period (unison beating and bright waves do this).
            {
                int chosen = best;
                for (const int mult : { 2, 3 })
                {
                    const int centre = best * mult;
                    if (centre + 2 >= tauMax)
                        continue;
                    int k = centre;
                    for (int j = centre - 2; j <= centre + 2; ++j)
                        if (cmnd[static_cast<size_t> (j)] < cmnd[static_cast<size_t> (k)])
                            k = j;
                    if (cmnd[static_cast<size_t> (best)] > 0.04f && cmnd[static_cast<size_t> (k)] < 0.5f * cmnd[static_cast<size_t> (best)]
                        && cmnd[static_cast<size_t> (k)] < cmnd[static_cast<size_t> (chosen)])
                        chosen = k;
                }
                best = chosen;
            }
            const float a = cmnd[static_cast<size_t> (best - 1)], b = cmnd[static_cast<size_t> (best)], c = cmnd[static_cast<size_t> (best + 1)];
            const float denom = a - 2.0f * b + c;
            const float shift = std::abs (denom) > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (a - c) / denom) : 0.0f;
            const float hz = static_cast<float> (sr) / (static_cast<float> (best) + shift);
            sp.pitch[fi] = 69.0f + 12.0f * std::log2 (hz / 440.0f);
        }

        // One-frame slips onto a harmonic (unison beating, noise) snap back to the
        // median of their neighbours; real sweeps move smoothly and stay.
        {
            const auto raw = sp.pitch;
            for (int f = 0; f < frames; ++f)
            {
                if (raw[static_cast<size_t> (f)] <= 0.0f)
                    continue;
                std::vector<float> near;
                for (int k = juce::jmax (0, f - 3); k <= juce::jmin (frames - 1, f + 3); ++k)
                    if (k != f && raw[static_cast<size_t> (k)] > 0.0f)
                        near.push_back (raw[static_cast<size_t> (k)]);
                if (near.size() >= 3)
                {
                    const float m = median (near);
                    if (std::abs (raw[static_cast<size_t> (f)] - m) > 2.0f)
                        sp.pitch[static_cast<size_t> (f)] = m;
                }
                else if (near.empty())
                {
                    sp.pitch[static_cast<size_t> (f)] = 0.0f; // a lone pitched frame isn't a pitch
                }
            }

            // Longer slips: split the curve where it jumps (> 5 st between frames: a sweep
            // never moves that fast) and move short runs sitting a harmonic away from the
            // main run back down, keeping their shape.
            struct Run { int a, b; float med; };
            std::vector<Run> runs;
            int last = -1;
            for (int f = 0; f < frames; ++f)
            {
                const float v = sp.pitch[static_cast<size_t> (f)];
                if (v <= 0.0f)
                    continue;
                if (last < 0 || f - last > 3 || std::abs (v - sp.pitch[static_cast<size_t> (last)]) > 5.0f)
                    runs.push_back ({ f, f + 1, 0.0f });
                runs.back().b = f + 1;
                last = f;
            }
            if (runs.size() > 1)
            {
                size_t main = 0;
                for (size_t k = 0; k < runs.size(); ++k)
                {
                    runs[k].med = median (std::vector<float> (sp.pitch.begin() + runs[k].a, sp.pitch.begin() + runs[k].b));
                    if (runs[k].b - runs[k].a > runs[main].b - runs[main].a)
                        main = k;
                }
                for (size_t k = 0; k < runs.size(); ++k)
                {
                    if (k == main || (runs[k].b - runs[k].a) * sp.hopSeconds > 0.25)
                        continue;
                    const float off = runs[k].med - runs[main].med;
                    for (const float interval : { 12.0f, 19.02f, 24.0f, -12.0f })
                        if (std::abs (off - interval) < 1.5f)
                        {
                            for (int f = runs[k].a; f < runs[k].b; ++f)
                                if (sp.pitch[static_cast<size_t> (f)] > 0.0f)
                                    sp.pitch[static_cast<size_t> (f)] -= interval;
                            break;
                        }
                }
            }
        }

        // Steady region: from just after the peak to the last loud frame before the key-up.
        if (like != nullptr)
        {
            sp.regionStart = like->regionStart;
            sp.regionEnd = like->regionEnd;
            return;
        }
        const int peakFrame = firstNearPeak (sp.level);
        const int holdFrame = juce::jlimit (0, frames, static_cast<int> (sp.holdSeconds / sp.hopSeconds));
        int end = holdFrame;
        while (end > peakFrame && sp.level[static_cast<size_t> (end - 1)] < 0.5f)
            --end;
        sp.regionStart = juce::jmin (end, peakFrame + juce::roundToInt (0.05 / sp.hopSeconds));
        sp.regionEnd = end;
    }

    // Share of the loud frames that have a pitch.
    float voicedShare (const SoundPrint& sp)
    {
        int loud = 0, voiced = 0;
        for (size_t f = 0; f < sp.level.size(); ++f)
            if (sp.level[f] >= voicedLevel)
            {
                ++loud;
                voiced += sp.pitch[f] > 0.0f ? 1 : 0;
            }
        return loud > 0 ? static_cast<float> (voiced) / static_cast<float> (loud) : 0.0f;
    }

    std::vector<float> voicedIn (const SoundPrint& sp, int a, int b)
    {
        std::vector<float> v;
        for (int f = a; f < b; ++f)
            if (sp.pitch[static_cast<size_t> (f)] > 0.0f)
                v.push_back (sp.pitch[static_cast<size_t> (f)]);
        return v;
    }

    float settledPitchHz (const SoundPrint& sp)
    {
        if (voicedShare (sp) < 0.5f)
            return 0.0f;
        auto v = voicedIn (sp, sp.regionStart, sp.regionEnd);
        if (v.size() < 3)
            v = voicedIn (sp, 0, static_cast<int> (sp.pitch.size()));
        const float note = median (v);
        return note > 0.0f ? noteToHz (0) * std::exp2 (note / 12.0f) : 0.0f;
    }

    // Wobbles in the steady part, and how pitch and brightness start and settle.
    void describeTrack (SoundPrint& sp)
    {
        const int a = sp.regionStart, b = sp.regionEnd;
        const auto hop = sp.hopSeconds;

        // Tremolo: level in dB.
        std::vector<float> levelDb;
        for (int f = a; f < b; ++f)
            levelDb.push_back ((sp.level[static_cast<size_t> (f)] - 1.0f) * -floorDb);
        sp.tremolo = findWobble (levelDb, hop, 0.6f);

        // Wah: brightness, but only if the level is up (quiet frames' centroid is fizz).
        std::vector<float> bright (sp.brightness.begin() + a, sp.brightness.begin() + b);
        sp.wah = findWobble (bright, hop, 0.15f);

        // Vibrato: pitch, gaps filled in; skipped when the pitch drops out a lot.
        if (sp.pitched)
        {
            std::vector<float> p (sp.pitch.begin() + a, sp.pitch.begin() + b);
            int voiced = 0;
            for (auto v : p)
                voiced += v > 0.0f ? 1 : 0;
            if (! p.empty() && voiced >= static_cast<int> (p.size() * 7 / 10))
            {
                float last = median (voicedIn (sp, a, b));
                for (auto& v : p)
                    v = v > 0.0f ? (last = v) : last;
                sp.vibrato = findWobble (p, hop, 0.06f);
            }

            // Vibrato sweeps partials across the spectrum: brightness wobbling at the
            // vibrato's rate is its shadow, not a wah.
            if (sp.vibrato.present() && sp.wah.present() && std::abs (std::log2 (sp.wah.rate / sp.vibrato.rate)) < 0.25f)
                sp.wah = {};

            // Pitch envelope: where the attack starts vs the settled note.
            const float settled = 69.0f + 12.0f * std::log2 (sp.f0 / 440.0f);
            // The first pitched frame that the next one agrees with.
            int first = -1;
            const int count = static_cast<int> (sp.pitch.size());
            for (int f = 0; f + 1 < count && first < 0 && f * hop < 0.1; ++f)
                if (sp.pitch[static_cast<size_t> (f)] > 0.0f && sp.pitch[static_cast<size_t> (f + 1)] > 0.0f
                    && std::abs (sp.pitch[static_cast<size_t> (f)] - sp.pitch[static_cast<size_t> (f + 1)]) < 3.0f)
                    first = f;
            if (first >= 0)
            {
                const float start = sp.pitch[static_cast<size_t> (first)] - settled;
                if (std::abs (start) > 0.3f && std::abs (start) <= 24.0f)
                {
                    sp.pitchStart = start;
                    const float near = juce::jmax (0.15f, 0.3f * std::abs (start), sp.vibrato.depth + 0.1f);
                    int f = first;
                    while (f < static_cast<int> (sp.pitch.size()) && (sp.pitch[static_cast<size_t> (f)] <= 0.0f || std::abs (sp.pitch[static_cast<size_t> (f)] - settled) > near))
                        ++f;
                    sp.pitchSettleSeconds = (f - first) * hop;
                }
            }
        }

        // Brightness: at the peak vs the steady part.
        const int peakFrame = firstNearPeak (sp.level);
        sp.brightStart = sp.brightness.empty() ? 0.0f : sp.brightness[static_cast<size_t> (peakFrame)];
        sp.brightSettled = b > a ? median (bright) : sp.brightStart;
        const float drop = sp.brightStart - sp.brightSettled;
        int f = peakFrame;
        while (f < static_cast<int> (sp.brightness.size()) && sp.brightness[static_cast<size_t> (f)] - sp.brightSettled > 0.3f * drop && drop > 0.0f)
            ++f;
        sp.brightSettleSeconds = (f - peakFrame) * hop;
    }

    float stereoWidth (const float* l, const float* r, int n)
    {
        if (l == r)
            return 0.0f;
        double mid = 0.0, side = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double m = 0.5 * (l[i] + r[i]), s = 0.5 * (l[i] - r[i]);
            mid += m * m;
            side += s * s;
        }
        return mid > 1.0e-12 ? static_cast<float> (juce::jlimit (0.0, 1.0, std::sqrt (side / mid))) : (side > 0.0 ? 1.0f : 0.0f);
    }
}

SoundPrint analyse (const float* left, const float* right, int n, float knownF0, const SoundPrint* like)
{
    SoundPrint sp;
    if (left == nullptr || n < fftSize)
        return sp;
    if (right == nullptr)
        right = left;

    std::vector<float> monoMix;
    const float* x = left;
    if (right != left)
    {
        monoMix.resize (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            monoMix[static_cast<size_t> (i)] = 0.5f * (left[i] + right[i]);
        x = monoMix.data();
    }
    const double sr = SoundPrint::sampleRate;
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
        while (f > peakFrame && sp.envelope[static_cast<size_t> (f)] < 0.02f) // skip the silent tail
            --f;
        // M9: only a clearly falling run counts (M6 also walked back through a flat
        // sustain, so a held note read as a 0.1 s tap).
        while (f > peakFrame && sp.envelope[static_cast<size_t> (f - 1)] > sp.envelope[static_cast<size_t> (f)] + 0.004f)
            --f;
        sp.holdSeconds = juce::jmax (0.1, (f + 0.5) * frameSeconds);
    }

    // ---- Frame track: level, pitch and brightness every ~10 ms.
    trackFrames (sp, x, n, like);

    // ---- Pitch: the settled pitch of the frame track (a pitch env or vibrato
    // doesn't pull it), else the old few-window YIN.
    sp.f0 = knownF0 > 0.0f ? knownF0 : settledPitchHz (sp);
    if (knownF0 <= 0.0f && sp.f0 <= 0.0f && voicedShare (sp) > 0.3f)
        sp.f0 = detectPitch (x, n, sr);
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

    // ---- Movement, pitch/brightness shape, stereo width.
    describeTrack (sp);
    sp.width = stereoWidth (left, right, n);

    sp.valid = true;
    return sp;
}

SoundPrint analyse (const float* mono, int n, float knownF0)
{
    return analyse (mono, mono, n, knownF0, nullptr);
}

const char* partName (int part)
{
    static const char* names[numParts] { "ENVELOPE", "TONE", "PITCH", "BRIGHTNESS", "MOVEMENT", "WIDTH" };
    return part >= 0 && part < numParts ? names[part] : "";
}

bool partApplies (const SoundPrint& t, int part)
{
    return part != pitchPart || t.pitched;
}

namespace
{
    // Moving average over +/- `radius` frames: beating and other fast flutter
    // (scored under movement) don't count against the slow curves.
    std::vector<float> smoothed (const std::vector<float>& x, size_t n, int radius)
    {
        std::vector<float> out (n, 0.0f);
        for (size_t f = 0; f < n; ++f)
        {
            const size_t a = f >= static_cast<size_t> (radius) ? f - static_cast<size_t> (radius) : 0;
            const size_t b = juce::jmin (n - 1, f + static_cast<size_t> (radius));
            float sum = 0.0f;
            for (size_t k = a; k <= b; ++k)
                sum += x[k];
            out[f] = sum / static_cast<float> (b - a + 1);
        }
        return out;
    }
}

Parts distanceParts (const SoundPrint& t, const SoundPrint& c)
{
    Parts parts {};
    const size_t frames = juce::jmin (t.level.size(), c.level.size());
    const int levelRadius = juce::jmax (1, juce::roundToInt (0.02 / t.hopSeconds));  // ~50 ms window
    const int brightRadius = juce::jmax (1, juce::roundToInt (0.05 / t.hopSeconds)); // ~110 ms window

    // Envelope: the level over time.
    if (frames > 0)
    {
        const auto tl = smoothed (t.level, frames, levelRadius), cl = smoothed (c.level, frames, levelRadius);
        parts[envelopePart] = meanAbsDiff (tl.data(), cl.data(), frames);
    }
    else
    {
        parts[envelopePart] = meanAbsDiff (t.envelope.data(), c.envelope.data(), t.envelope.size());
    }

    // Tone: band spectrum over time, harmonic balance, noisiness.
    float spec = 0.0f, wsum = 0.0f;
    for (size_t s = 0; s < t.spectrum.size(); ++s)
    {
        const float w = 0.05f + t.segmentLevel[s];
        spec += w * meanAbsDiff (t.spectrum[s].data(), c.spectrum[s].data(), t.spectrum[s].size());
        wsum += w;
    }
    spec /= wsum;
    const float flat = std::abs (t.flatness - c.flatness);
    parts[tonePart] = t.pitched ? 0.57f * spec + 0.29f * meanAbsDiff (t.harmonics.data(), c.harmonics.data(), t.harmonics.size()) + 0.14f * flat
                                : 0.77f * spec + 0.23f * flat;

    // Pitch over time (pitch env, vibrato, detune): semitones off in each loud frame.
    if (t.pitched)
    {
        double e = 0.0, w = 0.0;
        for (size_t f = 0; f < frames; ++f)
        {
            if (t.level[f] < voicedLevel || t.pitch[f] <= 0.0f)
                continue;
            const double off = c.pitch[f] > 0.0f ? juce::jmin (12.0, static_cast<double> (std::abs (t.pitch[f] - c.pitch[f]))) : 6.0;
            e += t.level[f] * off;
            w += t.level[f];
        }
        parts[pitchPart] = w > 0.0 ? static_cast<float> (juce::jmin (1.0, 0.12 * e / w)) : 0.0f;
    }

    // Brightness over time (filter cutoff + envelope): octaves off.
    if (frames > 0)
    {
        const auto tb = smoothed (t.brightness, frames, brightRadius), cb = smoothed (c.brightness, frames, brightRadius);
        double e = 0.0, w = 0.0;
        for (size_t f = 0; f < frames; ++f)
        {
            if (t.level[f] < 0.33f)
                continue;
            const double weight = t.level[f] * t.level[f];
            e += weight * juce::jmin (3.0, static_cast<double> (std::abs (tb[f] - cb[f])));
            w += weight;
        }
        parts[brightnessPart] = w > 0.0 ? static_cast<float> (juce::jmin (1.0, 0.2 * e / w)) : 0.0f;
    }

    // Movement: vibrato (1 st), tremolo (6 dB), wah (1 oct) depth, and rate when both move.
    {
        float sum = 0.0f;
        const std::array<std::tuple<const Wobble*, const Wobble*, float>, 3> kinds {
            std::tuple { &t.vibrato, &c.vibrato, 1.0f }, std::tuple { &t.tremolo, &c.tremolo, 6.0f }, std::tuple { &t.wah, &c.wah, 1.0f } };
        for (const auto& [a, b, scale] : kinds)
        {
            const float da = juce::jmin (1.5f, a->depth / scale), db = juce::jmin (1.5f, b->depth / scale);
            sum += std::abs (da - db);
            if (a->present() && b->present())
                sum += juce::jmin (da, db) * juce::jmin (1.0f, std::abs (std::log2 (a->rate / b->rate)));
        }
        parts[movementPart] = juce::jmin (1.0f, 0.5f * sum);
    }

    // Stereo width (unison spread).
    parts[widthPart] = juce::jmin (1.0f, 0.6f * std::abs (t.width - c.width));
    return parts;
}

float combine (const SoundPrint& t, const Parts& p)
{
    //                                     env    tone   pitch  bright move   width
    static constexpr Parts pitchedW     { 0.22f, 0.30f, 0.13f, 0.15f, 0.10f, 0.10f };
    static constexpr Parts unpitchedW   { 0.25f, 0.38f, 0.00f, 0.17f, 0.10f, 0.10f };
    const auto& w = t.pitched ? pitchedW : unpitchedW;
    float d = 0.0f;
    for (int i = 0; i < numParts; ++i)
        d += w[static_cast<size_t> (i)] * p[static_cast<size_t> (i)];
    return d;
}

float distance (const SoundPrint& t, const SoundPrint& c)
{
    if (! t.valid || ! c.valid)
        return 1.0f;
    return combine (t, distanceParts (t, c));
}

int matchPercent (float d)
{
    // Calibrated so two renders of the same patch read ~95-100 % and
    // unrelated sounds land well under 50 %.
    return juce::jlimit (0, 100, juce::roundToInt (100.0f * std::exp (-d * d * 40.0f)));
}
}
