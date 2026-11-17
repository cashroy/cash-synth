// Hidden bench: Cash Match on real recordings. Not part of the normal run.
//   CASH_SAMPLES=/path/to/wavs ./build/CashSynthTests "[.realsamples]"
// For each file: the match %, the per-part scores, an independent spectral
// distance (mel log-spectrogram, dB) and the match written next to it as
// <name>.match.wav so it can be listened to.
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "CashMatch.h"
#include "SoundPrint.h"
#include "Mint.h"
#include <juce_dsp/juce_dsp.h>

namespace
{
// Mean absolute dB difference of 64-band mel spectrograms (2048 window, 512 hop),
// floored at -80 dB under the target's peak, weighted by the target's loudness.
// Both are normalised to their own peak frame first. 0 dB = identical.
float melDistanceDb (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    constexpr int order = 11, size = 1 << order, hop = 512, bands = 64;
    const double sr = cash::SoundPrint::sampleRate;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann, false);

    auto mel = [] (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); };
    const double lo = mel (30.0), hi = mel (16000.0);

    auto spectrogram = [&] (const juce::AudioBuffer<float>& x)
    {
        const int n = x.getNumSamples();
        std::vector<std::array<float, bands>> out;
        std::vector<float> buf (size * 2);
        for (int pos = 0; pos + size / 2 < n || pos == 0; pos += hop)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < size && pos + i < n; ++i)
            {
                float m = 0.0f;
                for (int ch = 0; ch < x.getNumChannels(); ++ch)
                    m += x.getSample (ch, pos + i);
                buf[static_cast<size_t> (i)] = m / static_cast<float> (x.getNumChannels());
            }
            window.multiplyWithWindowingTable (buf.data(), size);
            fft.performFrequencyOnlyForwardTransform (buf.data());
            std::array<float, bands> e {};
            for (int k = 1; k < size / 2; ++k)
            {
                const double m = mel (k * sr / size);
                const int band = static_cast<int> ((m - lo) / (hi - lo) * bands);
                if (band >= 0 && band < bands)
                    e[static_cast<size_t> (band)] += buf[static_cast<size_t> (k)] * buf[static_cast<size_t> (k)];
            }
            out.push_back (e);
            if (pos + size / 2 >= n)
                break;
        }
        return out;
    };

    auto sa = spectrogram (a), sb = spectrogram (b);
    sb.resize (sa.size(), std::array<float, bands> {});
    auto peakOf = [] (const std::vector<std::array<float, bands>>& s)
    {
        float p = 1.0e-20f;
        for (auto& f : s) { float t = 0; for (auto v : f) t += v; p = std::max (p, t); }
        return p;
    };
    const float pa = peakOf (sa), pb = peakOf (sb);
    double sum = 0.0, wsum = 0.0;
    for (size_t f = 0; f < sa.size(); ++f)
    {
        float frameE = 0; for (auto v : sa[f]) frameE += v;
        const double w = std::max (0.0, 80.0 + 10.0 * std::log10 (frameE / pa + 1.0e-12)); // 0 at -80 dB
        for (int k = 0; k < bands; ++k)
        {
            const double da = std::max (-80.0, 10.0 * std::log10 (sa[f][static_cast<size_t> (k)] / pa + 1.0e-12));
            const double db = std::max (-80.0, 10.0 * std::log10 (sb[f][static_cast<size_t> (k)] / pb + 1.0e-12));
            const double bw = std::max (0.0, da + 80.0); // loud bands count more
            sum += w * bw * std::abs (da - db);
            wsum += w * bw;
        }
    }
    return wsum > 0.0 ? static_cast<float> (sum / wsum) : 0.0f;
}

void writeWav (const juce::File& f, const juce::AudioBuffer<float>& audio)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    if (os == nullptr)
        return;
    auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (cash::SoundPrint::sampleRate)
                                                .withNumChannels (audio.getNumChannels()).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (os, opts))
        w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}
}

TEST_CASE ("Cash Match on real recordings", "[.realsamples]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File (juce::SystemStats::getEnvironmentVariable ("CASH_SAMPLES", {}));
    REQUIRE (dir.isDirectory());
    const auto only = juce::SystemStats::getEnvironmentVariable ("CASH_ONLY", {});

    CashSynthAudioProcessor p;
    auto& m = p.getMatcher();
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
    std::sort (files.begin(), files.end());
    double total = 0; int count = 0;
    for (auto& f : files)
    {
        if (f.getFileName().contains (".m") || (only.isNotEmpty() && f.getFileNameWithoutExtension() != only))
            continue;
        REQUIRE (m.loadTarget (f).isEmpty());
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 7);
        const auto secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const auto r = m.getResult();
        const auto render = m.getBestRender();
        const float mel = melDistanceDb (m.getTargetAudio(), render);
        juce::String parts;
        for (int k = 0; k < cash::analysis::numParts; ++k)
            parts << cash::analysis::partName (k) << " " << cash::analysis::matchPercent (r.parts[static_cast<size_t> (k)]) << "  ";
        std::printf ("%-12s match %3d%%  mel %5.2f dB  (%5.1fs, %d renders, note %s)  %s\n",
                     f.getFileNameWithoutExtension().toRawUTF8(), r.percent, mel, secs, r.tried,
                     cash::analysis::noteName (m.getTargetPrint().midiNote).toRawUTF8(), parts.toRawUTF8());
        std::fflush (stdout);
        total += r.percent; ++count;
        writeWav (f.getSiblingFile (f.getFileNameWithoutExtension() + ".match.wav"), render);
    }
    if (count > 0)
        std::printf ("average match %.1f%%\n", total / count);
}

// The Mint alone: measure the sample, rebuild it with the player at its own pitch.
TEST_CASE ("Mint resynthesis on real recordings", "[.mintbench]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File (juce::SystemStats::getEnvironmentVariable ("CASH_SAMPLES", {}));
    REQUIRE (dir.isDirectory());
    const auto only = juce::SystemStats::getEnvironmentVariable ("CASH_ONLY", {});
    CashSynthAudioProcessor p;
    auto& m = p.getMatcher();
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
    std::sort (files.begin(), files.end());
    double total = 0; int count = 0;
    for (auto& f : files)
    {
        if (f.getFileName().contains (".m") || (only.isNotEmpty() && f.getFileNameWithoutExtension() != only))
            continue;
        REQUIRE (m.loadTarget (f).isEmpty());
        const auto& target = m.getTargetAudio();
        const auto& tp = m.getTargetPrint();
        const int n = target.getNumSamples();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto model = cash::mint::build (target.getReadPointer (0), target.getReadPointer (1), n, tp);
        const auto secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

        cash::MintPlayer player;
        player.prepare (cash::SoundPrint::sampleRate);
        player.start (model.get(), 1);
        juce::AudioBuffer<float> out (2, n);
        out.clear();
        const int hold = static_cast<int> (tp.holdSeconds * cash::SoundPrint::sampleRate);
        cash::MintPlayer::Settings s;
        for (int pos = 0; pos < n; pos += cash::MintPlayer::blockSize)
        {
            const int num = juce::jmin (cash::MintPlayer::blockSize, n - pos);
            if (hold >= pos && hold < pos + num)
                player.release();
            player.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), num, s);
        }
        const float mel = melDistanceDb (target, out);
        const auto rp = cash::analysis::analyse (out.getReadPointer (0), out.getReadPointer (1), n, tp.f0, &tp);
        const auto parts = cash::analysis::distanceParts (tp, rp);
        const int pct = cash::analysis::matchPercent (cash::analysis::combine (tp, parts));
        const auto bytes = model->toMemory().getSize();
        juce::String ps;
        for (int k = 0; k < cash::analysis::numParts; ++k)
            ps << cash::analysis::partName (k) << " " << cash::analysis::matchPercent (parts[static_cast<size_t> (k)]) << "  ";
        std::printf ("%-12s mint %3d%%  mel %5.2f dB  build %4.1fs  partials %3d  B %.5f  loop %s  %zu KB  %s\n",
                     f.getFileNameWithoutExtension().toRawUTF8(), pct, mel, secs, model->numPartials, model->inharmonicity,
                     model->hasLoop() ? "yes" : "no ", bytes / 1024, ps.toRawUTF8());
        std::fflush (stdout);
        total += pct; ++count;
        writeWav (f.getSiblingFile (f.getFileNameWithoutExtension() + ".mint.wav"), out);
    }
    if (count > 0)
        std::printf ("average mint %.1f%%\n", total / count);
}

// CPU: 16 notes of a minted sample at once, through the processor.
TEST_CASE ("Mint CPU with 16 voices", "[.mintcpu]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File (juce::SystemStats::getEnvironmentVariable ("CASH_SAMPLES", {}));
    const auto name = juce::SystemStats::getEnvironmentVariable ("CASH_ONLY", "ebass");
    CashSynthAudioProcessor p;
    REQUIRE (p.getMatcher().loadTarget (dir.getChildFile (name + ".wav")).isEmpty());
    p.getMatcher().runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 1);
    REQUIRE (p.depositMatch());
    constexpr int block = 512;
    p.prepareToPlay (44100.0, block);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer on;
    for (int k = 0; k < 16; ++k)
        on.addEvent (juce::MidiMessage::noteOn (1, 36 + k * 2, 1.0f), 0);
    const int blocks = static_cast<int> (5.0 * 44100.0 / block);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer none;
        p.processBlock (buf, b == 0 ? on : none);
    }
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::printf ("%s: %d partials, 16 voices, 5 s of audio in %.0f ms (%.1f%% of real time), %d voices active at the end\n",
                 name.toRawUTF8(), p.getMint()->numPartials, ms, ms / 50.0, p.getEngine().getNumActiveVoices());
}
