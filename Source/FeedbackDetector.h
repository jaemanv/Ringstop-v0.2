#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>

namespace ringstop
{
constexpr int   kNumNotches = 16;
constexpr float kNotchQ     = 30.0f;   // ~1/20 octave: very narrow, keeps the tone intact

// Written by the analysis thread, read by the audio thread and GUI. Lock-free.
struct NotchSlotShared
{
    std::atomic<float> freqHz  { 1000.0f };
    std::atomic<float> depthDb { 0.0f };    // positive cut amount; 0 = slot unused
    std::atomic<bool>  fixed   { false };   // placed during ring-out, never auto-released
};

using SharedSlots = std::array<NotchSlotShared, kNumNotches>;

/** Weights of the feedback classifier. Each feature is scaled 0..1 and the
    weighted sum goes through a logistic function to give a probability that a
    peak is feedback rather than music. These are hand-tuned now; they can later
    be learned from labelled recordings (logistic regression) without changing
    any other code. */
struct ClassifierWeights
{
    float bias        = -7.0f;
    float stability   =  4.0f;   // pitch steadiness: feedback has no vibrato
    float prominence  =  2.5f;   // how far the peak stands above its neighbours
    float growth      =  2.5f;   // feedback grows exponentially
    float purity      =  2.0f;   // feedback is a pure tone; notes have harmonics
    float persistence =  2.5f;   // how long it has rung at the same pitch
};

/** Finds feedback in the (post-notch) signal on a background thread and
    tells the audio thread where to place notch filters. */
class FeedbackDetector : private juce::Thread
{
public:
    static constexpr int fftOrder = 12;              // 4096-point FFT
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int hopSize  = 512;

    explicit FeedbackDetector (SharedSlots& slotsToControl);
    ~FeedbackDetector() override;

    void prepare (double sampleRate);
    void release();

    // Audio thread: never blocks, never allocates. Drops samples if the FIFO is full.
    void pushSamples (const float* data, int numSamples) noexcept;

    // Any thread
    void setStrength (float zeroToOne) noexcept       { strength.store (zeroToOne); }
    void setEnabled (bool shouldDetect) noexcept      { enabled.store (shouldDetect); }
    void setAutoRelease (bool shouldRelease) noexcept { autoRelease.store (shouldRelease); }
    void setRingOutMode (bool ringOut) noexcept       { ringOutMode.store (ringOut); }
    void requestClearLive() noexcept                  { clearLiveRequested.store (true); }
    void requestClearAll() noexcept                   { clearAllRequested.store (true); }
    void requestRemove (int slot) noexcept            { removeRequested.store (slot); }

    // GUI
    void copySpectrum (std::vector<float>& dest, double& binHz) const;
    float getRiskProbability() const noexcept { return riskProb.load(); }
    float getRiskFrequency() const noexcept   { return riskFreq.load(); }

private:
    static constexpr int kHistory   = 32;
    static constexpr int kMaxTracks = 24;

    struct Track
    {
        int   bin = 0, age = 0, lastSeenFrame = 0, hotFrames = 0, cooldown = 0;
        float freq[kHistory] {}, level[kHistory] {};
        int   count = 0, head = 0;
        float prominence = 0, purity = 1, probability = 0;

        void add (float f, float l)
        {
            freq[head] = f; level[head] = l;
            head = (head + 1) % kHistory;
            count = std::min (count + 1, kHistory);
        }
    };

    struct SlotState { bool active = false, fixed = false; float freqHz = 0, depthDb = 0; double lastHitMs = 0; };

    void run() override;
    void handleRequests();
    void drainFifo();
    void analyseFrame();
    float harmonicPurity (int bin, float level, double binHz) const;
    bool  isLocalPeak (int centre, float minAboveNeighbours, float minLevel, int radius = 2) const;
    float classify (Track& t, double frameSec) const;
    void placeCut (float freqHz, double nowMs);
    void releaseTick (double nowMs, double elapsedMs);
    void publish (int slotIndex);

    SharedSlots& shared;
    std::array<SlotState, kNumNotches> slots;
    std::vector<Track> tracks;
    ClassifierWeights weights;

    juce::AbstractFifo fifo { 1 << 15 };
    std::vector<float> fifoBuffer;

    std::vector<float> history;          // circular, fftSize long
    int historyWritePos = 0, samplesSinceFrame = 0, frameCounter = 0;

    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftData, spectrumDb;

    mutable juce::CriticalSection displayLock;
    std::vector<float> displaySpectrum;

    std::atomic<double> currentSampleRate { 48000.0 };
    std::atomic<float>  strength { 0.5f }, riskProb { 0.0f }, riskFreq { 0.0f };
    std::atomic<bool>   enabled { true }, autoRelease { true }, ringOutMode { false };
    std::atomic<bool>   clearLiveRequested { false }, clearAllRequested { false };
    std::atomic<int>    removeRequested { -1 };
    double lastTickMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDetector)
};
} // namespace ringstop
