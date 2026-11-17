#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>

namespace cash
{
// Lock-free tap of the output for the bill's portrait scope. The audio thread
// pushes a mono mix; the UI reads the newest samples. A torn read just means
// one slightly odd frame on screen, never a lock or allocation.
class ScopeBuffer
{
public:
    static constexpr int capacity = 4096; // power of two

    void push (const juce::AudioBuffer<float>& buffer) noexcept
    {
        const int channels = juce::jmin (2, buffer.getNumChannels());
        if (channels == 0)
            return;
        const float scale = 1.0f / static_cast<float> (channels);
        auto w = writePos.load (std::memory_order_relaxed);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float s = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                s += buffer.getSample (ch, i);
            data[w++ & mask].store (s * scale, std::memory_order_relaxed);
        }
        writePos.store (w, std::memory_order_release);
    }

    // Copies the newest n samples (oldest first). n is clamped to capacity.
    void read (float* dest, int n) const noexcept
    {
        n = juce::jlimit (0, capacity, n);
        const auto end = writePos.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dest[i] = data[(end - static_cast<uint32_t> (n - i)) & mask].load (std::memory_order_relaxed);
    }

    uint32_t getNumWritten() const noexcept { return writePos.load (std::memory_order_acquire); }

private:
    static constexpr uint32_t mask = capacity - 1;
    std::array<std::atomic<float>, capacity> data {};
    std::atomic<uint32_t> writePos { 0 };
};
}
