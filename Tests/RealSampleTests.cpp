// Hidden bench: Cash Match on real recordings. Not part of the normal run.
//   CASH_SAMPLES=/path/to/wavs ./build/CashSynthTests "[.realsamples]"
// For each file: the match %, the per-part scores, an independent spectral
// distance (mel log-spectrogram, dB) and the match written next to it as
// <name>.match.wav so it can be listened to.
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Layers.h"
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
    double total = 0, total8 = 0, total3 = 0, mels = 0, mels8 = 0, mels3 = 0; int count = 0;
    for (auto& f : files)
    {
        if (f.getFileName().contains (".m") || f.getFileName().contains (".layers") || (only.isNotEmpty() && f.getFileNameWithoutExtension() != only))
            continue;
        REQUIRE (m.loadTarget (f).isEmpty());
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 7);
        const auto secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const auto r = m.getResult();
        const auto render = m.getRender (false);
        const float mel = melDistanceDb (m.getTargetAudio(), render);
        // M14: LAYERS at Detail 8 (made with EXACT above), then re-fitted at Detail 3.
        const auto layers8 = m.getRender (true);
        const float mel8 = melDistanceDb (m.getTargetAudio(), layers8);
        const int pc8 = r.layered.percent, used8 = r.layered.layers;
        cash::MatchBudget b3;
        b3.mode = cash::MatchBudget::Mode::layers;
        b3.detail = 3;
        b3.refit = true;
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        m.runBlocking (p.getPatch(), cash::defaultPatch (p.getState()), 7, b3);
        const auto secs3 = (juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0;
        const auto r3 = m.getResult();
        const float mel3 = melDistanceDb (m.getTargetAudio(), m.getRender (true));
        juce::String parts;
        for (int k = 0; k < cash::analysis::numParts; ++k)
            parts << cash::analysis::partName (k) << " " << cash::analysis::matchPercent (r.layered.parts[static_cast<size_t> (k)]) << "  ";
        std::printf ("%-10s EXACT %3d%% %5.2f dB | LAYERS d8 %3d%% %5.2f dB (%d used) | d3 %3d%% %5.2f dB | %5.1fs + %4.1fs, note %s | L8: %s\n",
                     f.getFileNameWithoutExtension().toRawUTF8(), r.exact.percent, mel, pc8, mel8, used8, r3.layered.percent, mel3,
                     secs, secs3, cash::analysis::noteName (m.getTargetPrint().midiNote).toRawUTF8(), parts.toRawUTF8());
        std::fflush (stdout);
        if (juce::SystemStats::getEnvironmentVariable ("CASH_DEBUG", {}).isNotEmpty())
        {
            const auto& tp = m.getTargetPrint();
            const auto lp = cash::analysis::analyse (layers8.getReadPointer (0), layers8.getReadPointer (1), layers8.getNumSamples(), tp.f0, &tp);
            for (auto* q : { &tp, &lp })
            {
                std::printf ("   env:");
                for (float e : q->envelope) std::printf (" %.2f", e);
                std::printf ("\n");
            }
            for (auto* q : { &tp, &lp })
                std::printf ("   %s: vib %.2f Hz %.2f st  trem %.2f Hz %.2f dB  wah %.2f Hz %.2f oct  width %.2f  hold %.2f s\n", q == &tp ? "target" : "layers",
                             q->vibrato.rate, q->vibrato.depth, q->tremolo.rate, q->tremolo.depth, q->wah.rate, q->wah.depth, q->width, q->holdSeconds);
        }
        if (juce::SystemStats::getEnvironmentVariable ("CASH_DEBUG", {}).isNotEmpty())
            for (int i = cash::ParameterMap::numParamsV8 - 89; i < cash::ParameterMap::numParams; ++i)
            {
                const auto* id = cash::ParameterMap::spec (i).id;
                auto* q = p.getState().getParameter (id);
                const float v = r.layered.patch[static_cast<size_t> (i)];
                if (std::abs (v - q->getDefaultValue()) > 1.0e-4f)
                    std::printf ("   %s = %s\n", id, q->getText (v, 32).toRawUTF8());
            }
        total += r.exact.percent; total8 += pc8; total3 += r3.layered.percent; mels += mel; mels8 += mel8; mels3 += mel3; ++count;
        writeWav (f.getSiblingFile (f.getFileNameWithoutExtension() + ".match.wav"), render);
        writeWav (f.getSiblingFile (f.getFileNameWithoutExtension() + ".layers.wav"), layers8);
    }
    if (count > 0)
        std::printf ("average EXACT %.1f%% (%.2f dB)  LAYERS d8 %.1f%% (%.2f dB)  d3 %.1f%% (%.2f dB)\n",
                     total / count, mels / count, total8 / count, mels8 / count, total3 / count, mels3 / count);
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

// M14 part 2: LAYERS worst case. 16 voices, all 8 tone layers on with their filters
// moving (env + LFO 2 on the cutoff), curved envelopes, both noise layers at 48 dB edges.
// Run in Release: ./build-release/CashSynthTests "[.layerscpu]"
TEST_CASE ("LAYERS CPU with 8 layers x 16 voices", "[.layerscpu]")
{
    juce::ScopedJuceInitialiser_GUI init;
    namespace L = cash::layers;
    using namespace cash::param;
    CashSynthAudioProcessor p;
    auto patch = cash::defaultPatch (p.getState());
    const auto set = [&] (const char* id, float real)
    {
        auto* q = p.getState().getParameter (id);
        patch[static_cast<size_t> (cash::ParameterMap::indexOf (id))] = q->convertTo0to1 (real);
    };
    for (auto* id : { osc1On, osc2On, subNoiseOn, filterOn, filterEnvOn, driveOn, lfo1On, unisonOn, glideOn })
        set (id, 0.0f);
    set (lfo2On, 1.0f);
    set (L::layersOn, 1.0f);
    set (L::detail, 8.0f);
    set (masterGain, 0.1f);
    for (int l = 0; l < L::numTone; ++l)
    {
        set (L::toneId (l, L::level), 0.5f);
        set (L::toneId (l, L::octave), static_cast<float> (l % 3 - 1));
        set (L::toneId (l, L::cutoff), 1500.0f + 300.0f * static_cast<float> (l));
        set (L::toneId (l, L::reso), 0.3f);
        set (L::toneId (l, L::envAmt), 0.3f);
        set (L::toneId (l, L::l2Cutoff), 0.2f);
        set (L::toneId (l, L::sustain), 0.6f);
        set (L::toneId (l, L::curve), 0.5f);
    }
    for (int n = 0; n < L::numNoise; ++n)
    {
        set (L::noiseId (n, L::nLevel), 0.3f);
        set (L::noiseId (n, L::nSlope), 3.0f);
        set (L::noiseId (n, L::nSustain), 0.5f);
    }
    p.setPatch (patch, "worst case");
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
    std::printf ("LAYERS: 8 tone + 2 noise layers, 16 voices, 5 s of audio in %.0f ms (%.1f%% of real time), %d voices active, peak %.3f\n",
                 ms, ms / 50.0, p.getEngine().getNumActiveVoices(), buf.getMagnitude (0, block));
    CHECK (p.getEngine().getNumActiveVoices() == 16);
}
