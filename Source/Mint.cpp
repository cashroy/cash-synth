#include "Mint.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <complex>
#include <numeric>

namespace cash
{
namespace
{
    constexpr double sr = MintModel::sampleRate;
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr int hop = MintModel::hop;
    constexpr int nSize = MintModel::noiseSize;
    using cd = std::complex<double>;

    double princarg (double x) { return x - twoPi * std::round (x / twoPi); }
    float hzToNote (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
    float noteToHz (float note) { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

    std::vector<float> hannWindow (int n)
    {
        std::vector<float> w (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            w[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (twoPi * i / n)); // periodic
        return w;
    }

    // Noise bands, as FFT bin edges of the 512-point grain: 1 bin wide at the
    // bottom, then ~12 % wide (roughly how finely the ear hears noise colour).
    std::vector<int> bandEdges()
    {
        std::vector<int> e { 1 };
        while (e.back() < nSize / 2)
            e.push_back (juce::jmin (nSize / 2, e.back() + juce::jmax (1, juce::roundToInt (e.back() * 0.12f))));
        return e;
    }

    // |X|^2 of a Hann-windowed 512-point frame centred on `centre`.
    void powerSpectrum (const float* x, int n, int centre, const std::vector<float>& win, juce::dsp::FFT& fft, std::vector<float>& buf)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        const int a = centre - nSize / 2;
        for (int i = 0; i < nSize; ++i)
            if (a + i >= 0 && a + i < n)
                buf[static_cast<size_t> (i)] = x[a + i] * win[static_cast<size_t> (i)];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= nSize / 2; ++k)
            buf[static_cast<size_t> (k)] *= buf[static_cast<size_t> (k)];
    }

    // One noise grain of magnitude `mag` per bin and random phase.
    void noiseGrain (const float* magL, const float* magR, float coherence, juce::Random& rng, juce::dsp::FFT& fft,
                     std::vector<float>& outL, std::vector<float>& outR)
    {
        std::fill (outL.begin(), outL.end(), 0.0f);
        std::fill (outR.begin(), outR.end(), 0.0f);
        const float other = std::sqrt (juce::jmax (0.0f, 1.0f - coherence * coherence));
        for (int k = 1; k < nSize / 2; ++k)
        {
            const float a = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            const float ca = std::cos (a), sa = std::sin (a);
            const auto ki = static_cast<size_t> (2 * k);
            outL[ki] = magL[k] * ca;
            outL[ki + 1] = magL[k] * sa;
            if (other > 0.0f)
            {
                const float b = rng.nextFloat() * juce::MathConstants<float>::twoPi;
                outR[ki] = magR[k] * (coherence * ca + other * std::cos (b));
                outR[ki + 1] = magR[k] * (coherence * sa + other * std::sin (b));
            }
            else
            {
                outR[ki] = magR[k] * ca;
                outR[ki + 1] = magR[k] * sa;
            }
        }
        fft.performRealOnlyInverseTransform (outL.data());
        fft.performRealOnlyInverseTransform (outR.data());
    }

    // What the analysis reads (mean |X|^2 per bin) from noise grains of magnitude 1:
    // the synthesis scale is 1 / sqrt of it. Measured once, through the same code.
    float noiseCalibration()
    {
        static const float c = []
        {
            juce::dsp::FFT fft (MintModel::noiseOrder);
            const auto win = hannWindow (nSize);
            std::vector<float> ones (nSize / 2 + 1, 1.0f), gl (nSize * 2), gr (nSize * 2), buf (nSize * 2);
            juce::Random rng (12345);
            const int grains = 400, len = grains * hop + nSize;
            std::vector<float> y (static_cast<size_t> (len), 0.0f);
            for (int g = 0; g < grains; ++g)
            {
                noiseGrain (ones.data(), ones.data(), 1.0f, rng, fft, gl, gr);
                for (int i = 0; i < nSize; ++i)
                    y[static_cast<size_t> (g * hop + i)] += gl[static_cast<size_t> (i)] * win[static_cast<size_t> (i)];
            }
            double sum = 0.0;
            int count = 0;
            for (int c0 = nSize * 2; c0 < len - nSize * 2; c0 += hop * 3)
            {
                powerSpectrum (y.data(), len, c0, win, fft, buf);
                for (int k = 8; k < nSize / 2 - 8; ++k, ++count)
                    sum += buf[static_cast<size_t> (k)];
            }
            return static_cast<float> (sum / juce::jmax (1, count));
        }();
        return c;
    }

    // The fundamental per model frame (Hz) from the SoundPrint pitch track, gaps
    // filled from the nearest pitched frame, kept within an octave of the root.
    std::vector<float> pitchTrack (const SoundPrint& p, int frames, float rootNote)
    {
        const int pf = static_cast<int> (p.pitch.size());
        std::vector<float> notes (p.pitch.begin(), p.pitch.end());
        int firstValid = -1;
        for (int f = 0; f < pf; ++f)
            if (notes[static_cast<size_t> (f)] > 0.0f) { firstValid = f; break; }
        if (firstValid < 0)
            return {};
        // Fill gaps: carry the last pitch forward, and the first one back.
        float last = notes[static_cast<size_t> (firstValid)];
        for (int f = 0; f < pf; ++f)
        {
            auto& v = notes[static_cast<size_t> (f)];
            if (v > 0.0f)
            {
                while (v - rootNote > 7.0f) v -= 12.0f;   // octave slips back to the root
                while (rootNote - v > 7.0f) v += 12.0f;
                last = v;
            }
            else
            {
                v = last;
            }
        }
        std::vector<float> hz (static_cast<size_t> (frames));
        const double hopSec = hop / sr;
        for (int t = 0; t < frames; ++t)
        {
            const double x = (t * hopSec) / p.hopSeconds - 0.5; // print frame f is centred on (f + 0.5) hops
            const int f0 = juce::jlimit (0, pf - 1, static_cast<int> (std::floor (x)));
            const int f1 = juce::jmin (pf - 1, f0 + 1);
            const float fr = static_cast<float> (juce::jlimit (0.0, 1.0, x - f0));
            hz[static_cast<size_t> (t)] = noteToHz (notes[static_cast<size_t> (f0)] * (1.0f - fr) + notes[static_cast<size_t> (f1)] * fr);
        }
        return hz;
    }

    // String stiffness: fits the partial frequencies of the steady part to
    // f_k = k f0 sqrt(1 + B k^2). Returns B (0 for plain harmonic sounds) and a
    // corrected f0 scale.
    std::pair<float, float> measureStretch (const float* x, int n, int from, float f0)
    {
        int order = 16;
        while (order > 12 && (1 << order) > n - from)
            --order;
        const int size = 1 << order;
        if (n - from < size / 2 || f0 <= 0.0f)
            return { 0.0f, 1.0f };
        juce::dsp::FFT fft (order);
        std::vector<float> buf (static_cast<size_t> (size * 2), 0.0f);
        const auto win = hannWindow (size);
        for (int i = 0; i < size && from + i < n; ++i)
            buf[static_cast<size_t> (i)] = x[from + i] * win[static_cast<size_t> (i)];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        const double binHz = sr / size;
        float top = 0.0f;
        for (int k = 1; k < size / 2; ++k)
            top = juce::jmax (top, buf[static_cast<size_t> (k)]);

        double B = 0.0;
        double scale = 1.0;
        for (int pass = 0; pass < 2; ++pass)
        {
            // Weighted least squares of r^2 = a + b k^2, r = f_k / (k f0).
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            int used = 0;
            for (int k = 1; k <= 40; ++k)
            {
                const double expect = k * f0 * scale * std::sqrt (1.0 + B * k * k);
                if (expect > 12000.0)
                    break;
                const int lo = juce::jmax (1, static_cast<int> ((expect - 0.3 * f0) / binHz));
                const int hi = juce::jmin (size / 2 - 2, static_cast<int> ((expect + 0.3 * f0) / binHz));
                int best = -1;
                for (int b = lo; b <= hi; ++b)
                    if (best < 0 || buf[static_cast<size_t> (b)] > buf[static_cast<size_t> (best)])
                        best = b;
                if (best <= 1 || buf[static_cast<size_t> (best)] < top * 0.003f) // -50 dB
                    continue;
                const double l = std::log (buf[static_cast<size_t> (best - 1)] + 1e-12), c = std::log (buf[static_cast<size_t> (best)] + 1e-12),
                             r = std::log (buf[static_cast<size_t> (best + 1)] + 1e-12);
                const double d = 0.5 * (l - r) / (l - 2 * c + r);
                const double fk = (best + juce::jlimit (-0.5, 0.5, d)) * binHz;
                const double ratio = fk / (k * f0);
                const double w = buf[static_cast<size_t> (best)] / top;
                sw += w; sx += w * k * k; sy += w * ratio * ratio; sxx += w * k * k * k * k; sxy += w * k * k * ratio * ratio;
                ++used;
            }
            if (used < 6)
                return { 0.0f, 1.0f };
            const double den = sw * sxx - sx * sx;
            if (std::abs (den) < 1e-12)
                return { 0.0f, 1.0f };
            const double b = (sw * sxy - sx * sy) / den, a = (sy - b * sx) / sw;
            if (a <= 0.0)
                return { 0.0f, 1.0f };
            B = juce::jlimit (0.0, 0.02, b / a);
            scale = std::sqrt (a);
        }
        if (B < 2.0e-5)
            B = 0.0;
        return { static_cast<float> (B), static_cast<float> (scale) };
    }

    // Fast sweeps (kicks, toms, zaps) outrun the print's 10 ms pitch track. A second
    // guess per frame: the lowest strong spectral peak. Each frame keeps whichever
    // guess its first harmonics explain better (energy captured by 6 harmonics).
    void choosePitch (const float* x, int n, std::vector<float>& frameHz)
    {
        constexpr int order = 12, size = 1 << order, win = 1024;
        const int T = static_cast<int> (frameHz.size());
        juce::dsp::FFT fft (order);
        std::vector<float> buf (size * 2);
        const auto w = hannWindow (win);
        const double binHz = sr / size;
        auto captured = [&] (int c, float f0)
        {
            if (f0 <= 0.0f)
                return 0.0;
            const int W = juce::jlimit (64, 8192, juce::roundToInt (3.0 * sr / f0));
            double e = 0.0, wsum = 0.0;
            for (int k = 1; k <= 6; ++k)
            {
                if (k * f0 > sr * 0.45)
                    break;
                cd acc {};
                wsum = 0.0;
                for (int i = 0; i < W; ++i)
                {
                    const int s = c - W / 2 + i;
                    const double h = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / W);
                    wsum += h;
                    if (s >= 0 && s < n)
                        acc += h * x[s] * std::polar (1.0, -twoPi * k * f0 * (s - c) / sr);
                }
                e += std::norm (acc * (2.0 / wsum));
            }
            return e;
        };

        std::vector<float> peakHz (static_cast<size_t> (T), 0.0f);
        for (int t = 0; t < T; ++t)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            const int a = t * hop - win / 2;
            for (int i = 0; i < win; ++i)
                if (a + i >= 0 && a + i < n)
                    buf[static_cast<size_t> (i)] = x[a + i] * w[static_cast<size_t> (i)];
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            const int lo = static_cast<int> (25.0 / binHz), hi = static_cast<int> (2500.0 / binHz);
            float top = 0.0f;
            for (int k = lo; k <= hi; ++k)
                top = juce::jmax (top, buf[static_cast<size_t> (k)]);
            if (top <= 1.0e-7f)
                continue;
            for (int k = lo + 1; k < hi; ++k)
            {
                const float v = buf[static_cast<size_t> (k)];
                if (v >= top * 0.3f && v >= buf[static_cast<size_t> (k - 1)] && v >= buf[static_cast<size_t> (k + 1)])
                {
                    const double l = std::log (buf[static_cast<size_t> (k - 1)] + 1e-12), c = std::log (v + 1e-12), r = std::log (buf[static_cast<size_t> (k + 1)] + 1e-12);
                    const double den = l - 2 * c + r;
                    const double d = den < 0.0 ? 0.5 * (l - r) / den : 0.0;
                    peakHz[static_cast<size_t> (t)] = static_cast<float> ((k + juce::jlimit (-0.5, 0.5, d)) * binHz);
                    break;
                }
            }
        }

        std::vector<float> chosen (frameHz);
        for (int t = 0; t < T; ++t)
        {
            const float a = frameHz[static_cast<size_t> (t)], b = peakHz[static_cast<size_t> (t)];
            if (b <= 0.0f || std::abs (std::log2 (b / a)) < 0.04f) // within ~half a semitone: same guess
                continue;
            if (captured (t * hop, b) > 1.5 * captured (t * hop, a))
                chosen[static_cast<size_t> (t)] = b;
        }
        // Median of 5 so a lone frame can't flip.
        for (int t = 0; t < T; ++t)
        {
            float v[5];
            int c = 0;
            for (int u = t - 2; u <= t + 2; ++u)
                v[c++] = chosen[static_cast<size_t> (juce::jlimit (0, T - 1, u))];
            std::sort (v, v + 5);
            frameHz[static_cast<size_t> (t)] = v[2];
        }
    }

    double stretch (int k, double B) { return k * std::sqrt (1.0 + B * k * k); }

    // Complex amplitude of every partial, per analysis frame, for both channels.
    // c_k = 2/sum(w) * sum w x e^{-i ks phi}: the partial is Re(c_k e^{i ks phi}).
    struct Demod
    {
        int frames = 0, K = 0;
        std::vector<int> centre;                 // sample each frame is centred on (rising)
        std::vector<std::complex<float>> cL, cR; // [j * K + (k - 1)]
    };

    // Analysis frames: about one period apart (4-period windows overlap 4x), never closer than `hop`.
    std::vector<int> frameCentres (const std::vector<float>& f0s, int n)
    {
        std::vector<int> c;
        for (int at = 0;;)
        {
            c.push_back (at);
            if (at >= n - 1)
                break;
            const float f0 = f0s[static_cast<size_t> (at)];
            const int step = juce::jlimit (hop, 2048, (juce::roundToInt (sr / juce::jmax (20.0f, f0)) / hop) * hop);
            at = juce::jmin (n - 1, at + step);
        }
        return c;
    }

    Demod demodulate (const float* L, const float* R, int n, const std::vector<double>& phi, const std::vector<float>& f0s,
                      int K, double B)
    {
        Demod d;
        d.K = K;
        d.centre = frameCentres (f0s, n);
        d.frames = static_cast<int> (d.centre.size());
        d.cL.assign (static_cast<size_t> (d.frames * K), {});
        d.cR.assign (static_cast<size_t> (d.frames * K), {});
        std::vector<cd> accL (static_cast<size_t> (K)), accR (static_cast<size_t> (K)), st (static_cast<size_t> (K)), step (static_cast<size_t> (K));
        std::vector<double> eps (static_cast<size_t> (K));
        for (int k = 1; k <= K; ++k)
            eps[static_cast<size_t> (k - 1)] = stretch (k, B) - k;
        const bool stretched = B > 0.0;

        for (int j = 0; j < d.frames; ++j)
        {
            const int c = d.centre[static_cast<size_t> (j)];
            const float f0 = f0s[static_cast<size_t> (c)];
            const int W = juce::jlimit (128, 8192, 2 * juce::roundToInt (2.0 * sr / juce::jmax (20.0f, f0)));
            const int a = c - W / 2;
            std::fill (accL.begin(), accL.end(), cd {});
            std::fill (accR.begin(), accR.end(), cd {});
            const double omega = c + 1 < n ? phi[static_cast<size_t> (c + 1)] - phi[static_cast<size_t> (c)] : 0.0;
            if (stretched)
            {
                const int n0 = juce::jmax (0, a);
                for (size_t k = 0; k < static_cast<size_t> (K); ++k)
                {
                    st[k] = std::polar (1.0, -eps[k] * (phi[static_cast<size_t> (c)] + omega * (n0 - c)));
                    step[k] = std::polar (1.0, -eps[k] * omega);
                }
            }
            double wsum = 0.0;
            for (int i = 0; i < W; ++i)
            {
                const int s = a + i;
                const double w = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / W);
                wsum += w;
                if (s < 0 || s >= n)
                    continue;
                const double xl = w * L[s], xr = w * R[s];
                const cd z = std::polar (1.0, -phi[static_cast<size_t> (s)]);
                cd zk (1.0, 0.0);
                for (size_t k = 0; k < static_cast<size_t> (K); ++k)
                {
                    zk *= z;
                    cd term = zk;
                    if (stretched)
                    {
                        term *= st[k];
                        st[k] *= step[k];
                    }
                    accL[k] += xl * term;
                    accR[k] += xr * term;
                }
            }
            const double g = wsum > 0.0 ? 2.0 / wsum : 0.0;
            for (size_t k = 0; k < static_cast<size_t> (K); ++k)
            {
                d.cL[static_cast<size_t> (j * K) + k] = std::complex<float> (accL[k] * g);
                d.cR[static_cast<size_t> (j * K) + k] = std::complex<float> (accR[k] * g);
            }
        }
        return d;
    }

    std::vector<double> integrate (const std::vector<float>& f0s)
    {
        std::vector<double> phi (f0s.size());
        double p = 0.0;
        for (size_t i = 0; i < f0s.size(); ++i)
        {
            phi[i] = p;
            p += twoPi * f0s[i] / sr;
        }
        return phi;
    }

    // Per-sample f0 from per-frame values.
    std::vector<float> perSample (const std::vector<float>& frameHz, int n, int frameHop)
    {
        std::vector<float> out (static_cast<size_t> (n));
        const int frames = static_cast<int> (frameHz.size());
        for (int i = 0; i < n; ++i)
        {
            const double x = static_cast<double> (i) / frameHop;
            const int f0 = juce::jlimit (0, frames - 1, static_cast<int> (x));
            const int f1 = juce::jmin (frames - 1, f0 + 1);
            const float fr = static_cast<float> (x - f0);
            out[static_cast<size_t> (i)] = frameHz[static_cast<size_t> (f0)] * (1.0f - fr) + frameHz[static_cast<size_t> (f1)] * fr;
        }
        return out;
    }

    std::complex<float> lerp (std::complex<float> a, std::complex<float> b, float t) { return a + (b - a) * t; }
}

bool mint::sameSound (const Patch& a, const MintModel* ma, const Patch& b, const MintModel* mb)
{
    if (! samePatch (a, b))
        return false;
    const auto on = static_cast<size_t> (ParameterMap::indexOf ("mintOn"));
    return a[on] < 0.5f || ma == mb;
}

//============================================================================== build
std::shared_ptr<const MintModel> mint::build (const float* left, const float* right, int n, const SoundPrint& print)
{
    auto m = std::make_shared<MintModel>();
    if (left == nullptr || n < nSize || ! print.valid)
        return m;
    if (right == nullptr)
        right = left;

    std::vector<float> mid (static_cast<size_t> (n));
    float peak = 1.0e-9f;
    for (int i = 0; i < n; ++i)
    {
        mid[static_cast<size_t> (i)] = 0.5f * (left[i] + right[i]);
        peak = juce::jmax (peak, std::abs (left[i]), std::abs (right[i]));
    }
    const float norm = 1.0f / peak;

    const int T = n / hop + 1;
    m->numFrames = T;

    // ---- Pitch: the print's frame track, a measured string stretch, then refined from the partials' own phases.
    int voicedLoud = 0, loud = 0;
    for (size_t f = 0; f < print.pitch.size(); ++f)
        if (print.level[f] > 0.5f)
        {
            ++loud;
            voicedLoud += print.pitch[f] > 0.0f ? 1 : 0;
        }
    const bool pitched = print.f0 > 0.0f && (print.pitched || voicedLoud * 3 >= juce::jmax (1, loud));
    m->pitched = pitched;
    m->rootHz = pitched ? print.f0 : 261.63f;
    m->rootNote = hzToNote (m->rootHz);

    std::vector<float> res[2] { std::vector<float> (left, left + n), std::vector<float> (right, right + n) };

    if (pitched)
    {
        auto frameHz = pitchTrack (print, T, m->rootNote);
        if (frameHz.empty())
            frameHz.assign (static_cast<size_t> (T), m->rootHz);
        choosePitch (mid.data(), n, frameHz);

        const int steadyFrom = juce::jlimit (0, n - 1, static_cast<int> ((print.regionStart + 0.5) * print.hopSeconds * sr));
        const auto [B, scale] = measureStretch (mid.data(), n, steadyFrom, m->rootHz);
        m->inharmonicity = B;
        for (auto& f : frameHz)
            f *= scale;
        m->rootHz *= scale;
        m->rootNote = hzToNote (m->rootHz);

        // Partials up to 16 kHz (or just under Nyquist), at most maxPartials.
        int K = 0;
        while (K < MintModel::maxPartials && stretch (K + 1, B) * m->rootHz < juce::jmin (16000.0, sr * 0.46))
            ++K;
        K = juce::jmax (1, K);

        // Refinement passes: the partials' phase drift says how far f0 is off.
        auto f0s = perSample (frameHz, n, hop);
        auto phi = integrate (f0s);
        Demod d;
        constexpr int refinePasses = 3;
        for (int pass = 0; pass <= refinePasses; ++pass)
        {
            d = demodulate (left, right, n, phi, f0s, pass < refinePasses ? juce::jmin (K, 8) : K, B);
            if (pass == refinePasses)
                break;
            const int kUse = d.K;
            // Deviation (Hz) measured between frames j and j + 1, placed at their midpoint.
            std::vector<double> at;
            std::vector<float> dev;
            for (int j = 0; j + 1 < d.frames; ++j)
            {
                const int gap = d.centre[static_cast<size_t> (j + 1)] - d.centre[static_cast<size_t> (j)];
                double num = 0.0, den = 0.0;
                for (int k = 1; k <= kUse; ++k)
                {
                    const auto a0 = d.cL[static_cast<size_t> (j * kUse + k - 1)] + d.cR[static_cast<size_t> (j * kUse + k - 1)];
                    const auto a1 = d.cL[static_cast<size_t> ((j + 1) * kUse + k - 1)] + d.cR[static_cast<size_t> ((j + 1) * kUse + k - 1)];
                    const double w = std::min (std::norm (a0), std::norm (a1));
                    if (w <= 0.0)
                        continue;
                    const double dth = princarg (std::arg (a1) - std::arg (a0));
                    num += w * dth * sr / (twoPi * gap * stretch (k, B));
                    den += w;
                }
                at.push_back (0.5 * (d.centre[static_cast<size_t> (j)] + d.centre[static_cast<size_t> (j + 1)]));
                dev.push_back (den > 0.0 ? static_cast<float> (num / den) : 0.0f);
            }
            if (dev.empty())
                break;
            size_t j = 0;
            for (int t = 0; t < T; ++t)
            {
                const double x = static_cast<double> (t) * hop;
                while (j + 1 < at.size() && at[j + 1] <= x)
                    ++j;
                const size_t j1 = juce::jmin (at.size() - 1, j + 1);
                const float fr = j1 == j ? 0.0f : static_cast<float> (juce::jlimit (0.0, 1.0, (x - at[j]) / (at[j1] - at[j])));
                const float dv = dev[j] * (1.0f - fr) + dev[j1] * fr;
                auto& f = frameHz[static_cast<size_t> (t)];
                f = juce::jlimit (f * 0.8f, f * 1.25f, f + dv);
            }
            f0s = perSample (frameHz, n, hop);
            phi = integrate (f0s);
        }

        // The root is what the refined track says over the steady part (median, so vibrato
        // and the attack don't pull it): MINT FINE 0 then plays every key in tune.
        {
            const double toModel = print.hopSeconds * sr / hop;
            int a = juce::jlimit (0, T - 1, static_cast<int> ((print.regionStart + 0.5) * toModel));
            int e = juce::jlimit (0, T, static_cast<int> ((print.regionEnd + 0.5) * toModel));
            if (e - a < 4)
            {
                a = 0;
                e = T;
            }
            std::vector<float> steady (frameHz.begin() + a, frameHz.begin() + e);
            std::nth_element (steady.begin(), steady.begin() + static_cast<std::ptrdiff_t> (steady.size() / 2), steady.end());
            const float med = steady[steady.size() / 2];
            if (med > 0.0f && std::abs (std::log2 (med / m->rootHz)) < 0.5f)
            {
                m->rootHz = med;
                m->rootNote = hzToNote (med);
            }
        }

        // ---- Partials on the model grid, and the residual (sample - partials) for the noise.
        m->numPartials = K;
        m->freq.assign (static_cast<size_t> (K * T), 0.0f);
        m->ampL.assign (static_cast<size_t> (K * T), 0.0f);
        m->ampR.assign (static_cast<size_t> (K * T), 0.0f);
        m->spread.assign (static_cast<size_t> (K * T), 0.0f);
        m->startPhase.assign (static_cast<size_t> (K), 0.0f);
        std::vector<std::complex<float>> gridL (static_cast<size_t> (K * T)), gridR (static_cast<size_t> (K * T));
        size_t jj = 0;
        for (int t = 0; t < T; ++t)
        {
            const int x = t * hop;
            while (jj + 1 < d.centre.size() && d.centre[jj + 1] <= x)
                ++jj;
            const size_t j1 = juce::jmin (d.centre.size() - 1, jj + 1);
            const float fr = j1 == jj ? 0.0f
                                      : static_cast<float> (juce::jlimit (0.0, 1.0, static_cast<double> (x - d.centre[jj]) / (d.centre[j1] - d.centre[jj])));
            for (int k = 0; k < K; ++k)
            {
                const auto i0 = jj * static_cast<size_t> (K) + static_cast<size_t> (k), i1 = j1 * static_cast<size_t> (K) + static_cast<size_t> (k);
                gridL[static_cast<size_t> (k * T + t)] = lerp (d.cL[i0], d.cL[i1], fr);
                gridR[static_cast<size_t> (k * T + t)] = lerp (d.cR[i0], d.cR[i1], fr);
            }
        }

        // Residual: subtract each partial exactly as measured (complex amplitude, interpolated per sample).
        for (int k = 0; k < K; ++k)
        {
            const double ks = stretch (k + 1, B);
            const auto* gl = &gridL[static_cast<size_t> (k * T)];
            const auto* gr = &gridR[static_cast<size_t> (k * T)];
            float top = 0.0f;
            for (int t = 0; t < T; ++t)
                top = juce::jmax (top, std::abs (gl[t]), std::abs (gr[t]));
            if (top < peak * 1.0e-5f)
                continue;
            for (int i = 0; i < n; ++i)
            {
                const int t0 = i / hop;
                const int t1 = juce::jmin (T - 1, t0 + 1);
                const float fr = static_cast<float> (i - t0 * hop) / hop;
                const double ph = ks * phi[static_cast<size_t> (i)];
                const float c = static_cast<float> (std::cos (ph)), s = static_cast<float> (std::sin (ph));
                const auto vl = lerp (gl[t0], gl[t1], fr), vr = lerp (gr[t0], gr[t1], fr);
                res[0][static_cast<size_t> (i)] -= vl.real() * c - vl.imag() * s;
                res[1][static_cast<size_t> (i)] -= vr.real() * c - vr.imag() * s;
            }
        }

        // Stored form: level per side, and the frequency each partial runs at between frames.
        for (int k = 0; k < K; ++k)
        {
            const double ks = stretch (k + 1, B);
            const auto* gl = &gridL[static_cast<size_t> (k * T)];
            const auto* gr = &gridR[static_cast<size_t> (k * T)];
            m->startPhase[static_cast<size_t> (k)] = static_cast<float> (std::arg (gl[0]));
            float lastSpread = 0.0f;
            for (int t = 0; t < T; ++t)
            {
                const auto i = static_cast<size_t> (k * T + t);
                m->ampL[i] = std::abs (gl[t]) * norm;
                m->ampR[i] = std::abs (gr[t]) * norm;
                // Phase of right vs left, unwrapped along time so interpolating it never spins.
                if (std::abs (gl[t]) > peak * 1.0e-5f && std::abs (gr[t]) > peak * 1.0e-5f)
                    lastSpread += static_cast<float> (princarg (std::arg (gr[t] * std::conj (gl[t])) - lastSpread));
                m->spread[i] = lastSpread;
                const int c0 = juce::jmin (n - 1, t * hop), c1 = juce::jmin (n - 1, (t + 1) * hop);
                const double expected = ks * (phi[static_cast<size_t> (c1)] - phi[static_cast<size_t> (c0)]);
                double dev = 0.0;
                if (t + 1 < T && c1 > c0)
                {
                    const auto a0 = gl[t] + gr[t], a1 = gl[t + 1] + gr[t + 1];
                    if (std::abs (a0) > peak * 1.0e-4f && std::abs (a1) > peak * 1.0e-4f)
                        dev = princarg (std::arg (a1) - std::arg (a0));
                }
                const double cycles = c1 > c0 ? (expected + dev) / twoPi : ks * f0s[static_cast<size_t> (c0)] * hop / sr;
                const double hz = cycles * sr / hop;
                m->freq[i] = static_cast<float> (juce::jmax (0.0, hz) / m->rootHz);
            }
        }
    }

    // ---- Noise: the residual's power per band, per frame (both sides).
    {
        const auto edges = bandEdges();
        const int nb = static_cast<int> (edges.size()) - 1;
        m->bandHz.resize (static_cast<size_t> (nb));
        for (int b = 0; b < nb; ++b)
            m->bandHz[static_cast<size_t> (b)] = static_cast<float> (0.5 * (edges[static_cast<size_t> (b)] + edges[static_cast<size_t> (b + 1)] - 1) * sr / nSize);
        m->noiseL.assign (static_cast<size_t> (nb * T), 0.0f);
        m->noiseR.assign (static_cast<size_t> (nb * T), 0.0f);
        juce::dsp::FFT fft (MintModel::noiseOrder);
        const auto win = hannWindow (nSize);
        std::vector<float> bl (nSize * 2), br (nSize * 2), cplxL (nSize * 2), cplxR (nSize * 2);
        const float synthScale = norm / std::sqrt (noiseCalibration());
        double cross = 0.0, pl = 0.0, pr = 0.0;
        for (int t = 0; t < T; ++t)
        {
            powerSpectrum (res[0].data(), n, t * hop, win, fft, bl);
            powerSpectrum (res[1].data(), n, t * hop, win, fft, br);
            for (int b = 0; b < nb; ++b)
            {
                double sl = 0.0, srr = 0.0;
                const int lo = edges[static_cast<size_t> (b)], hi = edges[static_cast<size_t> (b + 1)];
                for (int k = lo; k < hi; ++k)
                {
                    sl += bl[static_cast<size_t> (k)];
                    srr += br[static_cast<size_t> (k)];
                }
                const auto i = static_cast<size_t> (b * T + t);
                m->noiseL[i] = static_cast<float> (std::sqrt (sl / (hi - lo))) * synthScale;
                m->noiseR[i] = static_cast<float> (std::sqrt (srr / (hi - lo))) * synthScale;
            }
            // Coherence of the two residuals (every 4th frame is plenty).
            if (t % 4 == 0)
            {
                std::fill (cplxL.begin(), cplxL.end(), 0.0f);
                std::fill (cplxR.begin(), cplxR.end(), 0.0f);
                const int a = t * hop - nSize / 2;
                for (int i = 0; i < nSize; ++i)
                    if (a + i >= 0 && a + i < n)
                    {
                        cplxL[static_cast<size_t> (i)] = res[0][static_cast<size_t> (a + i)] * win[static_cast<size_t> (i)];
                        cplxR[static_cast<size_t> (i)] = res[1][static_cast<size_t> (a + i)] * win[static_cast<size_t> (i)];
                    }
                fft.performRealOnlyForwardTransform (cplxL.data(), true);
                fft.performRealOnlyForwardTransform (cplxR.data(), true);
                for (int k = 1; k < nSize / 2; ++k)
                {
                    const auto ki = static_cast<size_t> (2 * k);
                    cross += cplxL[ki] * cplxR[ki] + cplxL[ki + 1] * cplxR[ki + 1];
                    pl += cplxL[ki] * cplxL[ki] + cplxL[ki + 1] * cplxL[ki + 1];
                    pr += cplxR[ki] * cplxR[ki] + cplxR[ki + 1] * cplxR[ki + 1];
                }
            }
        }
        m->noiseCoherence = pl > 0.0 && pr > 0.0 ? static_cast<float> (juce::jlimit (0.0, 1.0, cross / std::sqrt (pl * pr))) : 1.0f;
    }

    // ---- Loudness per frame, key-down and the sustain loop.
    m->frameLevel.assign (static_cast<size_t> (T), 0.0f);
    const auto edges = bandEdges();
    const float cal = noiseCalibration();
    for (int t = 0; t < T; ++t)
    {
        double e = 0.0;
        for (int k = 0; k < m->numPartials; ++k)
        {
            const auto i = static_cast<size_t> (k * T + t);
            e += 0.25 * (m->ampL[i] * m->ampL[i] + m->ampR[i] * m->ampR[i]);
        }
        for (int b = 0; b < m->numBands(); ++b)
        {
            const auto i = static_cast<size_t> (b * T + t);
            const int width = edges[static_cast<size_t> (b + 1)] - edges[static_cast<size_t> (b)];
            e += 0.5 * (m->noiseL[i] * m->noiseL[i] + m->noiseR[i] * m->noiseR[i]) * cal * width / (nSize * 0.375 * nSize);
        }
        m->frameLevel[static_cast<size_t> (t)] = static_cast<float> (std::sqrt (e));
    }

    const double printToModel = print.hopSeconds * sr / hop;
    m->holdFrame = juce::jlimit (1, T - 1, static_cast<int> (print.holdSeconds * sr / hop));
    const int ls = juce::jlimit (0, T - 1, static_cast<int> ((print.regionStart + 0.5) * printToModel));
    const int le = juce::jlimit (0, m->holdFrame, static_cast<int> ((print.regionEnd + 0.5) * printToModel));
    const bool longEnough = (le - ls) * hop >= 0.12 * sr;
    const bool sustains = longEnough && m->frameLevel[static_cast<size_t> (le)] >= 0.5f * m->frameLevel[static_cast<size_t> (ls)];
    if (sustains)
    {
        m->loopStart = ls;
        m->loopEnd = le;
    }
    return m;
}

//============================================================================== player
MintPlayer::MintPlayer() = default;
MintPlayer::~MintPlayer() = default;

void MintPlayer::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    rateRatio = MintModel::sampleRate / rate;
    phase.assign (MintModel::maxPartials, 0.0);
    lastL.assign (MintModel::maxPartials, 0.0f);
    lastR.assign (MintModel::maxPartials, 0.0f);
    lastF.assign (MintModel::maxPartials, 0.0f);
    lastD.assign (MintModel::maxPartials, 0.0f);
    fft = std::make_unique<juce::dsp::FFT> (MintModel::noiseOrder);
    grainL.assign (nSize * 2, 0.0f);
    grainR.assign (nSize * 2, 0.0f);
    window = hannWindow (nSize);
    olaL.assign (nSize, 0.0f);
    olaR.assign (nSize, 0.0f);
    model = nullptr;
}

void MintPlayer::setLoop (float scale)
{
    loopSet = scale;
    loopA = model->loopStart;
    loopB = model->loopEnd;
    if (! model->hasLoop() || std::abs (scale - 1.0f) < 1.0e-3f)
        return; // as measured (exact frames; a knob's round trip may land a hair off 100 %)
    const double mid = 0.5 * (loopA + loopB);
    const double half = 0.5 * (loopB - loopA) * juce::jmax (0.01f, scale);
    const double minLen = 0.03 * MintModel::sampleRate / MintModel::hop; // 30 ms
    loopA = juce::jmax (0.0, mid - half);
    loopB = juce::jmin (static_cast<double> (model->holdFrame), mid + half);
    if (loopB - loopA < minLen)
    {
        loopB = juce::jmin (static_cast<double> (model->holdFrame), loopA + minLen);
        loopA = juce::jmax (0.0, loopB - minLen);
    }
}

void MintPlayer::start (const MintModel* m, uint32_t seed, float startAt, float loopScale)
{
    model = (m != nullptr && m->numFrames > 1 && fft != nullptr) ? m : nullptr;
    pos = 0.0;
    keyDown = true;
    if (model != nullptr)
    {
        setLoop (loopScale);
        pos = juce::jlimit (0.0f, 1.0f, startAt) * (model->holdFrame - 1);
        if (looping() && pos >= loopB)
            pos = loopA + std::fmod (pos - loopA, loopB - loopA);
    }
    relPos = -1.0;
    relFade = 1.0f;
    relGain = 1.0f;
    firstBlock = true;
    rng.setSeed (static_cast<juce::int64> (seed) * 7919 + 17);
    std::fill (olaL.begin(), olaL.end(), 0.0f);
    std::fill (olaR.begin(), olaR.end(), 0.0f);
    olaRead = 0;
    untilGrain = 0;
    if (model != nullptr)
        for (int k = 0; k < model->numPartials; ++k)
            phase[static_cast<size_t> (k)] = model->startPhase[static_cast<size_t> (k)];
}

void MintPlayer::release()
{
    if (model == nullptr || ! keyDown)
        return;
    keyDown = false;
    if (pos < model->holdFrame - 1)
    {
        const auto level = [this] (double p)
        {
            const auto f = frameAt (p);
            return model->frameLevel[static_cast<size_t> (f.t0)] * (1.0f - f.frac) + model->frameLevel[static_cast<size_t> (f.t1)] * f.frac;
        };
        const float now = level (pos), there = level (model->holdFrame);
        relGain = there > 1.0e-6f ? juce::jlimit (0.25f, 4.0f, now / there) : 1.0f;
        relPos = model->holdFrame;
        relFade = 0.0f;
        relStep = static_cast<float> (1.0 / (0.03 * rate));
    }
}

MintPlayer::Frame MintPlayer::frameAt (double p) const
{
    Frame f;
    const int last = model->numFrames - 1;
    const double c = juce::jlimit (0.0, static_cast<double> (last), p);
    f.t0 = juce::jmin (last, static_cast<int> (c));
    f.t1 = juce::jmin (last, f.t0 + 1);
    f.frac = static_cast<float> (c - f.t0);
    return f;
}

void MintPlayer::sample (double p, int k, float& aL, float& aR, float& fq, float& sp) const
{
    const int T = model->numFrames;
    auto at = [&] (double q, float& l, float& r, float& f, float& d)
    {
        const auto fr = frameAt (q);
        const auto base = static_cast<size_t> (k * T);
        const auto i0 = base + static_cast<size_t> (fr.t0), i1 = base + static_cast<size_t> (fr.t1);
        l = model->ampL[i0] * (1.0f - fr.frac) + model->ampL[i1] * fr.frac;
        r = model->ampR[i0] * (1.0f - fr.frac) + model->ampR[i1] * fr.frac;
        f = model->freq[i0] * (1.0f - fr.frac) + model->freq[i1] * fr.frac;
        d = model->spread[i0] * (1.0f - fr.frac) + model->spread[i1] * fr.frac;
    };
    at (p, aL, aR, fq, sp);
    // Held: blend into the loop start just before the loop end.
    if (keyDown && looping())
    {
        const double len = loopB - loopA;
        const double xf = juce::jmin (len * 0.25, 0.06 * sr / hop);
        const double from = loopB - xf;
        if (p > from)
        {
            float l2, r2, f2, d2;
            at (p - len, l2, r2, f2, d2);
            const auto w = static_cast<float> ((p - from) / xf);
            aL += (l2 - aL) * w;
            aR += (r2 - aR) * w;
            fq += (f2 - fq) * w;
            sp += static_cast<float> (princarg (d2 - sp)) * w;
        }
    }
}

float MintPlayer::noiseAt (const std::vector<float>& track, int band, double p) const
{
    const auto f = frameAt (p);
    const auto base = static_cast<size_t> (band * model->numFrames);
    return track[base + static_cast<size_t> (f.t0)] * (1.0f - f.frac) + track[base + static_cast<size_t> (f.t1)] * f.frac;
}

void MintPlayer::makeGrain (const Settings& s)
{
    // The grain is centred half a grain ahead, so it lines up with the partials.
    const double step = s.speed * rateRatio / hop;
    const double ahead = (nSize / 2) * step;
    const double p = (relPos >= 0.0 && relFade > 0.5f ? relPos : pos) + ahead;
    const float gain = s.noise * (relPos >= 0.0 && relFade > 0.5f ? relGain : (relPos < 0.0 ? relGain : 1.0f))
                     * static_cast<float> (std::sqrt (rate / MintModel::sampleRate));
    const double ratio = std::exp2 (s.semitones / 12.0);
    const int nb = model->numBands();

    // Band magnitudes at this position, then per bin (interpolated across band centres, transposed).
    float bandL[64], bandR[64];
    const int useBands = juce::jmin (nb, 64);
    double q = p;
    if (keyDown && looping() && q >= loopB)
        q -= (loopB - loopA);
    for (int b = 0; b < useBands; ++b)
    {
        bandL[b] = noiseAt (model->noiseL, b, q);
        bandR[b] = noiseAt (model->noiseR, b, q);
    }
    float magL[nSize / 2 + 1], magR[nSize / 2 + 1];
    magL[0] = magR[0] = 0.0f;
    int b = 0;
    for (int k = 1; k <= nSize / 2; ++k)
    {
        const double hz = k * rate / nSize;
        const double rootHz = hz / ratio;
        while (b + 1 < useBands && model->bandHz[static_cast<size_t> (b + 1)] < rootHz)
            ++b;
        float l, r;
        if (rootHz <= model->bandHz[0] || b + 1 >= useBands)
        {
            const int e = rootHz <= model->bandHz[0] ? 0 : useBands - 1;
            l = bandL[e];
            r = bandR[e];
        }
        else
        {
            const float lo = model->bandHz[static_cast<size_t> (b)], hi = model->bandHz[static_cast<size_t> (b + 1)];
            const float t = juce::jlimit (0.0f, 1.0f, static_cast<float> ((rootHz - lo) / (hi - lo)));
            l = bandL[b] + (bandL[b + 1] - bandL[b]) * t;
            r = bandR[b] + (bandR[b + 1] - bandR[b]) * t;
        }
        const float tilt = s.tilt != 0.0f ? static_cast<float> (std::pow (hz / 1000.0, s.tilt)) : 1.0f;
        const float above = rootHz > 20000.0 ? 0.0f : 1.0f;
        magL[k] = l * gain * tilt * above;
        magR[k] = r * gain * tilt * above;
    }
    noiseGrain (magL, magR, model->noiseCoherence, rng, *fft, grainL, grainR);
    for (int i = 0; i < nSize; ++i)
    {
        const auto at = static_cast<size_t> ((olaRead + i) % nSize);
        olaL[at] += grainL[static_cast<size_t> (i)] * window[static_cast<size_t> (i)];
        olaR[at] += grainR[static_cast<size_t> (i)] * window[static_cast<size_t> (i)];
    }
}

void MintPlayer::render (float* left, float* right, int num, const Settings& s)
{
    if (model == nullptr || num <= 0)
        return;
    jassert (num <= blockSize);

    if (s.loop != loopSet)
    {
        setLoop (s.loop);
        if (keyDown && looping() && pos >= loopB) // a shorter loop: jump back into it
            pos = loopA + std::fmod (pos - loopA, loopB - loopA);
    }

    const double step = s.speed * rateRatio / hop; // frames per output sample
    const double endPos = pos + step * num;
    const double relEnd = relPos >= 0.0 ? relPos + step * num : -1.0;
    const float fadeEnd = relPos >= 0.0 ? juce::jmin (1.0f, relFade + relStep * num) : 1.0f;
    const float levelNow = relPos >= 0.0 ? 1.0f : relGain; // after a hand-over the release keeps its gain
    const double ratio = std::exp2 (s.semitones / 12.0);
    const double hzScale = ratio * model->rootHz;
    const double nyquist = rate * 0.45;
    const float inv = 1.0f / static_cast<float> (num);

    // ---- Partials
    if (s.partials > 0.0f)
    {
        for (int k = 0; k < model->numPartials; ++k)
        {
            float l1, r1, f1, d1;
            sample (endPos, k, l1, r1, f1, d1);
            float& f0 = lastF[static_cast<size_t> (k)];
            float& sp0 = lastD[static_cast<size_t> (k)];
            if (firstBlock)
            {
                f0 = f1;
                sp0 = d1;
            }
            float fm = 0.5f * (f0 + f1), dm = 0.5f * (sp0 + d1); // the block's middle
            f0 = f1;
            sp0 = d1;
            if (relPos >= 0.0)
            {
                float l2, r2, f2, d2;
                sample (relEnd, k, l2, r2, f2, d2);
                l1 = l1 * (1.0f - fadeEnd) + l2 * relGain * fadeEnd;
                r1 = r1 * (1.0f - fadeEnd) + r2 * relGain * fadeEnd;
                if (fadeEnd > 0.5f)
                {
                    fm = f2;
                    dm = d2;
                }
            }
            else
            {
                l1 *= levelNow;
                r1 *= levelNow;
            }
            const double hz = fm * hzScale;
            const float g = (hz < nyquist ? 1.0f : 0.0f) * s.partials
                          * (s.tilt != 0.0f ? static_cast<float> (std::pow (juce::jmax (20.0, hz) / 1000.0, s.tilt)) : 1.0f);
            l1 *= g;
            r1 *= g;

            auto& ph = phase[static_cast<size_t> (k)];
            const double dphi = twoPi * hz / rate;
            float& l0 = lastL[static_cast<size_t> (k)];
            float& r0 = lastR[static_cast<size_t> (k)];
            if (firstBlock)
            {
                // The sample's own attack starts at frame 0: start there, not from silence.
                float ls, rs, fs, ds;
                sample (pos, k, ls, rs, fs, ds);
                l0 = ls * g;
                r0 = rs * g;
            }
            if (juce::jmax (l0, r0, juce::jmax (l1, r1)) > 1.0e-5f) // -100 dB under the peak: not worth a sine
            {
                float c = static_cast<float> (std::cos (ph)), sn = static_cast<float> (std::sin (ph));
                const float rc = static_cast<float> (std::cos (dphi)), rs = static_cast<float> (std::sin (dphi));
                const float dc = std::cos (dm), ds = std::sin (dm); // right = cos (phase + spread)
                const float dl = (l1 - l0) * inv, dr = (r1 - r0) * inv;
                float al = l0, ar = r0;
                for (int i = 0; i < num; ++i)
                {
                    left[i] += al * c;
                    right[i] += ar * (c * dc - sn * ds);
                    const float nc = c * rc - sn * rs;
                    sn = sn * rc + c * rs;
                    c = nc;
                    al += dl;
                    ar += dr;
                }
            }
            l0 = l1;
            r0 = r1;
            ph += dphi * num;
            ph -= twoPi * std::floor (ph / twoPi);
        }
    }

    // ---- Noise (overlap-add of random-phase grains every `hop` output samples)
    for (int i = 0; i < num; ++i)
    {
        if (untilGrain <= 0)
        {
            if (s.noise > 0.0f)
                makeGrain (s);
            untilGrain = hop;
        }
        --untilGrain;
        left[i] += olaL[static_cast<size_t> (olaRead)];
        right[i] += olaR[static_cast<size_t> (olaRead)];
        olaL[static_cast<size_t> (olaRead)] = olaR[static_cast<size_t> (olaRead)] = 0.0f;
        olaRead = (olaRead + 1) % nSize;
    }

    firstBlock = false;
    pos = endPos;
    if (relPos >= 0.0)
    {
        relPos = relEnd;
        relFade = fadeEnd;
        if (relFade >= 1.0f)
        {
            pos = relPos;
            relPos = -1.0;
        }
    }
    else if (keyDown && looping() && pos >= loopB)
    {
        pos -= (loopB - loopA);
    }
    if (pos >= model->numFrames - 1 + (nSize / hop))
        model = nullptr; // played out (noise grains included)
}

//============================================================================== save / load
namespace
{
    constexpr juce::uint32 mintMagic = 0x544E494D; // "MINT"
    constexpr int mintVersion = 1;

    // Levels as 1/1024 octave steps down from 2^0 (0 = silent); frequencies as 1/2048 octave.
    juce::uint16 packAmp (float a) { return a <= 1.0e-7f ? 0 : static_cast<juce::uint16> (juce::jlimit (1, 65535, 1 + juce::roundToInt ((std::log2 (a) + 40.0f) * 1024.0f))); }
    float unpackAmp (juce::uint16 q) { return q == 0 ? 0.0f : std::exp2 ((q - 1) / 1024.0f - 40.0f); }
    juce::int16 packFreq (float r) { return static_cast<juce::int16> (juce::jlimit (-32767, 32767, juce::roundToInt (std::log2 (juce::jmax (1.0e-6f, r)) * 2048.0f))); }
    float unpackFreq (juce::int16 q) { return std::exp2 (q / 2048.0f); }
}

juce::MemoryBlock MintModel::toMemory() const
{
    juce::MemoryOutputStream raw;
    raw.writeInt (static_cast<int> (mintMagic));
    raw.writeInt (mintVersion);
    raw.writeFloat (rootHz);
    raw.writeFloat (rootNote);
    raw.writeBool (pitched);
    raw.writeFloat (inharmonicity);
    raw.writeInt (numFrames);
    raw.writeInt (numPartials);
    raw.writeInt (numBands());
    raw.writeFloat (noiseCoherence);
    raw.writeInt (holdFrame);
    raw.writeInt (loopStart);
    raw.writeInt (loopEnd);
    for (auto v : startPhase) raw.writeFloat (v);
    for (auto v : bandHz) raw.writeFloat (v);
    for (auto v : frameLevel) raw.writeFloat (v);
    for (auto v : freq) raw.writeShort (packFreq (v));
    for (auto v : ampL) raw.writeShort (static_cast<short> (packAmp (v)));
    for (auto v : ampR) raw.writeShort (static_cast<short> (packAmp (v)));
    for (auto v : spread) raw.writeShort (static_cast<short> (juce::jlimit (-32767, 32767, juce::roundToInt (v * 1024.0f))));
    for (auto v : noiseL) raw.writeShort (static_cast<short> (packAmp (v)));
    for (auto v : noiseR) raw.writeShort (static_cast<short> (packAmp (v)));

    juce::MemoryOutputStream zipped;
    {
        juce::GZIPCompressorOutputStream gz (zipped, 6);
        gz.write (raw.getData(), raw.getDataSize());
    }
    return zipped.getMemoryBlock();
}

std::shared_ptr<const MintModel> MintModel::fromMemory (const void* data, size_t size)
{
    if (data == nullptr || size < 8)
        return nullptr;
    juce::MemoryInputStream zipped (data, size, false);
    juce::GZIPDecompressorInputStream gz (zipped);
    juce::MemoryBlock block;
    gz.readIntoMemoryBlock (block, 64 * 1024 * 1024);
    juce::MemoryInputStream in (block, false);
    if (block.getSize() < 64 || static_cast<juce::uint32> (in.readInt()) != mintMagic || in.readInt() != mintVersion)
        return nullptr;

    auto m = std::make_shared<MintModel>();
    m->rootHz = in.readFloat();
    m->rootNote = in.readFloat();
    m->pitched = in.readBool();
    m->inharmonicity = in.readFloat();
    m->numFrames = in.readInt();
    m->numPartials = in.readInt();
    const int nb = in.readInt();
    m->noiseCoherence = in.readFloat();
    m->holdFrame = in.readInt();
    m->loopStart = in.readInt();
    m->loopEnd = in.readInt();
    const int T = m->numFrames, K = m->numPartials;
    if (T < 2 || T > 1000000 || K < 0 || K > maxPartials || nb < 1 || nb > 64 || ! (m->rootHz > 0.0f))
        return nullptr;
    const auto need = static_cast<size_t> (K + nb + T) * 4 + static_cast<size_t> (K * T * 4 + nb * T * 2) * 2;
    if (static_cast<size_t> (in.getNumBytesRemaining()) < need)
        return nullptr;

    auto floats = [&] (std::vector<float>& v, size_t count) { v.resize (count); for (auto& x : v) x = in.readFloat(); };
    floats (m->startPhase, static_cast<size_t> (K));
    floats (m->bandHz, static_cast<size_t> (nb));
    floats (m->frameLevel, static_cast<size_t> (T));
    m->freq.resize (static_cast<size_t> (K * T));
    for (auto& v : m->freq) v = unpackFreq (static_cast<juce::int16> (in.readShort()));
    for (auto* v : { &m->ampL, &m->ampR })
    {
        v->resize (static_cast<size_t> (K * T));
        for (auto& x : *v) x = unpackAmp (static_cast<juce::uint16> (in.readShort()));
    }
    m->spread.resize (static_cast<size_t> (K * T));
    for (auto& x : m->spread) x = in.readShort() / 1024.0f;
    for (auto* v : { &m->noiseL, &m->noiseR })
    {
        v->resize (static_cast<size_t> (nb * T));
        for (auto& x : *v) x = unpackAmp (static_cast<juce::uint16> (in.readShort()));
    }
    m->holdFrame = juce::jlimit (1, T - 1, m->holdFrame);
    m->loopStart = juce::jlimit (0, T - 1, m->loopStart);
    m->loopEnd = juce::jlimit (0, m->holdFrame, m->loopEnd);
    return m;
}
}
