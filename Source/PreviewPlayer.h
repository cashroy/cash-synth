#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace cash
{
// Plays a mono clip (Cash Match's target or best match) over the synth output.
// play/stop on the message thread; mixInto on the audio thread, which only
// try-locks, so it never waits (it skips a block if the clip is being swapped).
class PreviewPlayer
{
public:
    void play (const juce::AudioBuffer<float>& clip, double clipSampleRate)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        buffer.makeCopyOf (clip);
        rate = clipSampleRate;
        position = 0.0;
        playing = buffer.getNumSamples() > 0;
    }

    void stop()
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        playing = false;
    }

    bool isPlaying() const { return playing.load(); }

    void mixInto (juce::AudioBuffer<float>& out, double hostRate)
    {
        if (! playing.load() || hostRate <= 0.0)
            return;
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked())
            return;

        const int len = buffer.getNumSamples();
        const double step = rate / hostRate;
        const float* src = buffer.getReadPointer (0);
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const int k = static_cast<int> (position);
            if (k + 1 >= len)
            {
                playing = false;
                break;
            }
            const float frac = static_cast<float> (position - k);
            const float s = (src[k] + (src[k + 1] - src[k]) * frac) * gain;
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, i, s);
            position += step;
        }
    }

private:
    juce::SpinLock lock;
    juce::AudioBuffer<float> buffer;
    double rate = 44100.0, position = 0.0;
    std::atomic<bool> playing { false };
    static constexpr float gain = 0.7f;
};
}
