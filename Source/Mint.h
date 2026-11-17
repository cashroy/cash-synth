#pragma once

#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SoundPrint.h"
#include "ParameterMap.h"

// The Mint (M13): Cash Match's resynthesis layer. A sample is measured once into
// a MintModel: up to `maxPartials` sine partials (each with its own level per
// channel and frequency over time, so vibrato, beating and inharmonic strings
// come through) plus a noise floor per band (breath, bow, pick, hiss).
//
// The synth then MAKES the sound from those numbers: a bank of sine oscillators
// and shaped noise, per voice, on any key. No audio from the sample is ever
// played back. Held notes loop the steady part; letting go jumps to the
// sample's own release.
namespace juce::dsp { class FFT; }

namespace cash
{
struct MintModel
{
    static constexpr int hop = 128;          // samples per frame at sampleRate
    static constexpr int noiseOrder = 9, noiseSize = 1 << noiseOrder; // noise grains (512)
    static constexpr int maxPartials = 128;
    static constexpr double sampleRate = SoundPrint::sampleRate;

    float rootHz = 261.63f;   // the pitch the model plays back at with no transposing
    float rootNote = 60.0f;   // MIDI (fractional)
    bool pitched = false;
    float inharmonicity = 0.0f; // string stiffness B (partial k at k sqrt(1 + B k^2))

    int numFrames = 0, numPartials = 0;
    // Per partial, per frame: [k * numFrames + t]
    std::vector<float> freq;          // ratio to rootHz
    std::vector<float> ampL, ampR;    // linear, peak of the whole sound ~ 1
    std::vector<float> spread;        // right channel's phase lead over the left, radians (stereo image)
    std::vector<float> startPhase;    // per partial, radians at frame 0

    // Noise: per band, per frame [b * numFrames + t], magnitude per FFT bin (synthesis units)
    std::vector<float> bandHz;        // centre of each band at the root
    std::vector<float> noiseL, noiseR;
    float noiseCoherence = 1.0f;      // 1 = same noise both sides .. 0 = independent

    std::vector<float> frameLevel;    // rough loudness per frame (for the release hand-over)

    int holdFrame = 0;                // where the key went up in the sample
    int loopStart = 0, loopEnd = 0;   // held notes loop here (loopEnd <= loopStart: no loop)

    int numBands() const { return static_cast<int> (bandHz.size()); }
    double seconds() const { return numFrames * hop / sampleRate; }
    double holdSeconds() const { return holdFrame * hop / sampleRate; }
    bool hasLoop() const { return loopEnd > loopStart; }

    // Compact binary form for saving with a project / stash (gzip'd, 16-bit tracks).
    juce::MemoryBlock toMemory() const;
    static std::shared_ptr<const MintModel> fromMemory (const void* data, size_t size);
};

// Shared, immutable once built. Patches that switch the Mint on carry one along
// (ledger entries, vault notes, stashes, the project).
using MintRef = std::shared_ptr<const MintModel>;

namespace mint
{
    // Same sound: same knobs, and (if the Mint is on) the same model.
    bool sameSound (const Patch& a, const MintModel* ma, const Patch& b, const MintModel* mb);

    // Measures a sample (stereo, at SoundPrint::sampleRate, starting at its onset).
    // `print` is the sample's SoundPrint (pitch track, key-down time, steady part).
    std::shared_ptr<const MintModel> build (const float* left, const float* right, int numSamples, const SoundPrint& print);
}

// Plays a MintModel: one per voice. Real-time safe after prepare().
class MintPlayer
{
public:
    MintPlayer();
    ~MintPlayer();
    void prepare (double sampleRate);
    // Note on (model may be null: silent). `startAt` skips 0..1 of the attack/hold
    // part (up to the key-up point); `loopScale` sizes the held loop (1 = as measured).
    void start (const MintModel*, uint32_t seed, float startAt = 0.0f, float loopScale = 1.0f);
    void release();                               // note off: hand over to the sample's release
    void stop() { model = nullptr; }
    bool isPlaying() const { return model != nullptr; }
    const MintModel* getModel() const { return model; }
    double getPosition() const { return pos; }      // frames into the model's timeline
    double getLoopStart() const { return loopA; }   // the held loop in use (frames)
    double getLoopEnd() const { return loopB; }

    struct Settings
    {
        float semitones = 0.0f;  // pitch relative to the model's root
        float partials = 1.0f;   // level of the sine partials
        float noise = 1.0f;      // level of the noise
        float tilt = 0.0f;       // -1..1: up to 6 dB/octave darker..brighter around 1 kHz
        float speed = 1.0f;      // playback speed of the model's timeline
        float loop = 1.0f;       // held loop length, x the measured loop, round its middle
    };

    // Adds `num` samples (<= blockSize) to left/right.
    static constexpr int blockSize = 32;
    void render (float* left, float* right, int num, const Settings&);

private:
    struct Frame { int t0 = 0, t1 = 0; float frac = 0.0f; };
    Frame frameAt (double pos) const;
    void sample (double pos, int k, float& aL, float& aR, float& f, float& spread) const; // with loop blending
    float noiseAt (const std::vector<float>& track, int band, double pos) const;
    void makeGrain (const Settings&);
    void setLoop (float scale);  // loopA/loopB from the model's loop and the knob
    bool looping() const { return loopB > loopA; }

    const MintModel* model = nullptr;
    double rate = 44100.0;
    double pos = 0.0;            // frame position (fractional)
    bool keyDown = false;
    double loopA = 0.0, loopB = 0.0; // the held loop in frames (the model's, sized by the Loop knob)
    float loopSet = -1.0f;
    // Release hand-over: crossfade from `pos` (still running) to the release part.
    double relPos = -1.0;
    float relFade = 1.0f, relGain = 1.0f, relStep = 0.0f;

    std::vector<double> phase;   // per partial
    std::vector<float> lastL, lastR; // amplitude at the end of the last block (per partial)
    std::vector<float> lastF, lastD; // frequency and stereo spread there
    bool firstBlock = true;

    // Noise overlap-add
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> grainL, grainR, window, olaL, olaR;
    int olaRead = 0, untilGrain = 0;
    juce::Random rng;
    double rateRatio = 1.0;      // model rate / output rate
};
}
