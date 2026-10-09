#include "FeedbackDetector.h"
#include <algorithm>
#include <cmath>

namespace ringstop
{
static float clamp01 (float x) { return juce::jlimit (0.0f, 1.0f, x); }

FeedbackDetector::FeedbackDetector (SharedSlots& s)
    : juce::Thread ("Ringstop analysis"), shared (s)
{
    fifoBuffer.resize ((size_t) fifo.getTotalSize());
    history.resize (fftSize, 0.0f);
    fftData.resize (2 * fftSize, 0.0f);
    spectrumDb.resize (fftSize / 2, -120.0f);
    displaySpectrum.resize (fftSize / 2, -120.0f);
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i)   // Hann window
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1));
    tracks.reserve (kMaxTracks);
}

FeedbackDetector::~FeedbackDetector() { release(); }

void FeedbackDetector::prepare (double sampleRate)
{
    release();
    currentSampleRate.store (sampleRate);
    fifo.reset();
    std::fill (history.begin(), history.end(), 0.0f);
    historyWritePos = samplesSinceFrame = 0;
    tracks.clear();
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    startThread (juce::Thread::Priority::normal);
}

void FeedbackDetector::release()
{
    if (isThreadRunning())
        stopThread (1000);
}

void FeedbackDetector::pushSamples (const float* data, int numSamples) noexcept
{
    int start1, size1, start2, size2;
    fifo.prepareToWrite (numSamples, start1, size1, start2, size2);
    if (size1 > 0) std::copy (data, data + size1, fifoBuffer.data() + start1);
    if (size2 > 0) std::copy (data + size1, data + size1 + size2, fifoBuffer.data() + start2);
    fifo.finishedWrite (size1 + size2);
}

void FeedbackDetector::copySpectrum (std::vector<float>& dest, double& binHz) const
{
    const juce::ScopedLock sl (displayLock);
    dest = displaySpectrum;
    binHz = currentSampleRate.load() / fftSize;
}

void FeedbackDetector::run()
{
    while (! threadShouldExit())
    {
        drainFifo();
        handleRequests();

        const double now = juce::Time::getMillisecondCounterHiRes();
        releaseTick (now, now - lastTickMs);
        lastTickMs = now;
        wait (3);
    }
}

void FeedbackDetector::handleRequests()
{
    const bool all  = clearAllRequested.exchange (false);
    const bool live = clearLiveRequested.exchange (false);
    if (all || live)
        for (int k = 0; k < kNumNotches; ++k)
            if (all || ! slots[(size_t) k].fixed) { slots[(size_t) k] = {}; publish (k); }

    const int remove = removeRequested.exchange (-1);
    if (remove >= 0 && remove < kNumNotches) { slots[(size_t) remove] = {}; publish (remove); }
}

void FeedbackDetector::drainFifo()
{
    int start1, size1, start2, size2;
    fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

    auto consume = [this] (const float* src, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            history[(size_t) historyWritePos] = src[i];
            historyWritePos = (historyWritePos + 1) % fftSize;
            if (++samplesSinceFrame >= hopSize)
            {
                samplesSinceFrame = 0;
                analyseFrame();
            }
        }
    };
    consume (fifoBuffer.data() + start1, size1);
    consume (fifoBuffer.data() + start2, size2);
    fifo.finishedRead (size1 + size2);
}

bool FeedbackDetector::isLocalPeak (int c, float minAboveNeighbours, float minLevel, int radius) const
{
    const int n = (int) spectrumDb.size();
    if (c < 13 || c >= n - 13) return false;
    int best = c;
    for (int i = c - radius; i <= c + radius; ++i)
        if (spectrumDb[(size_t) i] > spectrumDb[(size_t) best]) best = i;
    const float v = spectrumDb[(size_t) best];
    if (v < minLevel) return false;
    if (v < spectrumDb[(size_t) best - 1] || v < spectrumDb[(size_t) best + 1]) return false;   // must be a true peak, not a slope
    float nb = 0;
    for (int k = 4; k <= 12; ++k) nb += spectrumDb[(size_t) (best - k)] + spectrumDb[(size_t) (best + k)];
    return v - nb / 18.0f >= minAboveNeighbours;
}

// 1 = pure tone (feedback-like). Lower when there are harmonics above it or a
// fundamental below it, as with a sung or played note.
float FeedbackDetector::harmonicPurity (int bin, float level, double binHz) const
{
    const double f = bin * binHz;
    float purity = 1.0f;
    for (int h = 2; h <= 3; ++h)
        if (isLocalPeak ((int) std::round (f * h / binHz), 12.0f, level - 25.0f))
            purity -= 0.5f;
    // Is this one harmonic of a lower note? (checks the 2nd up to the 8th harmonic)
    for (int d = 2; d <= 8; ++d)
        if (f / d >= 60.0 && isLocalPeak ((int) std::round (f / d / binHz), 12.0f, level - 30.0f, 1))
            { purity -= 0.7f; break; }
    return clamp01 (purity);
}

float FeedbackDetector::classify (Track& t, double frameSec) const
{
    // Pitch stability in cents (vibrato on voices/instruments is typically 20–60 cents)
    double mean = 0;
    for (int i = 0; i < t.count; ++i) mean += t.freq[i];
    mean /= t.count;
    double var = 0;
    for (int i = 0; i < t.count; ++i)
    {
        const double c = 1200.0 * std::log2 (t.freq[i] / mean);
        var += c * c;
    }
    const float stdCents = (float) std::sqrt (var / t.count);

    // Level growth in dB/s: least-squares slope over a range of the history (oldest first)
    auto slopeOver = [&] (int from, int to)
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const int n = to - from;
        for (int i = from; i < to; ++i)
        {
            const int idx = (t.head - t.count + i + kHistory) % kHistory;
            const double x = i * frameSec, y = t.level[idx];
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double den = n * sxx - sx * sx;
        return den > 0 ? (float) ((n * sxy - sx * sy) / den) : 0.0f;
    };
    // Feedback keeps growing; a note's attack jumps once then holds. So growth has to
    // show up in BOTH halves of the history, and a very steep jump counts as an onset.
    float slope = 0.0f;
    if (t.count >= 20)
    {
        const int half = t.count / 2;
        slope = std::min (slopeOver (0, half), slopeOver (half, t.count));
    }

    const float fStability   = clamp01 ((20.0f - stdCents) / 15.0f);
    const float fProminence  = clamp01 ((t.prominence - 8.0f) / 20.0f);
    const float fGrowth      = clamp01 (slope / 15.0f) * clamp01 ((100.0f - slope) / 40.0f);
    const float fPurity      = t.purity;
    const float fPersistence = clamp01 ((float) (t.age * frameSec / 1.5));

    const float z = weights.bias
                  + weights.stability   * fStability
                  + weights.prominence  * fProminence
                  + weights.growth      * fGrowth
                  + weights.purity      * fPurity
                  + weights.persistence * fPersistence * (0.3f + 0.7f * fPurity);   // a long note with harmonics is music, not feedback
    #if RINGSTOP_DEBUG_CLASSIFIER
    if (t.age % 40 == 0)
        printf ("f=%.0f age=%d std=%.2fc prom=%.1f slope=%.1f pur=%.2f -> p=%.3f\n",
                t.freq[(t.head + kHistory - 1) % kHistory], t.age, stdCents, t.prominence, slope, fPurity, 1.0f / (1.0f + std::exp (-z)));
    #endif
    return 1.0f / (1.0f + std::exp (-z));
}

void FeedbackDetector::analyseFrame()
{
    ++frameCounter;

    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((historyWritePos + i) % fftSize)] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float norm = 4.0f / (float) fftSize;   // full-scale sine ≈ 0 dBFS
    const int numBins = fftSize / 2;
    for (int i = 0; i < numBins; ++i)
        spectrumDb[(size_t) i] = 20.0f * std::log10 (fftData[(size_t) i] * norm + 1.0e-9f);

    {
        const juce::ScopedLock sl (displayLock);
        displaySpectrum = spectrumDb;
    }

    if (! enabled.load()) { riskProb.store (0.0f); return; }

    const bool   ringOut  = ringOutMode.load();
    const float  s        = ringOut ? std::max (strength.load(), 0.85f) : strength.load();
    const double sr       = currentSampleRate.load();
    const double binHz    = sr / fftSize;
    const double frameSec = (double) hopSize / sr;
    const auto&  sp       = spectrumDb;

    const int lo = std::max (13, (int) std::ceil (70.0 / binHz));
    const int hi = std::min (numBins - 14, (int) std::floor (14000.0 / binHz));
    if (hi <= lo) return;

    // 1. Candidate peaks: loose pre-filter, the classifier makes the real decision
    const float floorDb = -60.0f - 12.0f * s;
    struct Peak { int bin; float db, prominence; };
    std::array<Peak, 8> found; int numFound = 0;

    for (int i = lo; i < hi; ++i)
    {
        const float v = sp[(size_t) i];
        if (v < floorDb) continue;
        if (v < sp[(size_t) i - 1] || v < sp[(size_t) i + 1] || v < sp[(size_t) i - 2] || v < sp[(size_t) i + 2]) continue;
        float nb = 0;
        for (int k = 4; k <= 12; ++k) nb += sp[(size_t) (i - k)] + sp[(size_t) (i + k)];
        const float prom = v - nb / 18.0f;
        if (prom < 10.0f) continue;

        if (numFound < (int) found.size()) found[(size_t) numFound++] = { i, v, prom };
        else
        {
            auto* weakest = std::min_element (found.begin(), found.end(), [] (auto& a, auto& b) { return a.db < b.db; });
            if (v > weakest->db) *weakest = { i, v, prom };
        }
    }

    // 2. Follow each peak over time
    for (int f = 0; f < numFound; ++f)
    {
        const auto& pk = found[(size_t) f];
        const int i = pk.bin;
        const float a = sp[(size_t) i - 1], b = sp[(size_t) i], c = sp[(size_t) i + 1];
        const float den = a - 2.0f * b + c;
        const float delta = std::abs (den) > 1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
        const float freq = (float) ((i + delta) * binHz);
        const float purity = harmonicPurity (i, pk.db, binHz);

        Track* t = nullptr;
        for (auto& tr : tracks)
            if (std::abs (tr.bin - i) <= 2 && tr.lastSeenFrame != frameCounter) { t = &tr; break; }

        if (t == nullptr)
        {
            if ((int) tracks.size() >= kMaxTracks) continue;
            tracks.push_back ({});
            t = &tracks.back();
            t->prominence = pk.prominence;
            t->purity = purity;
        }
        t->bin = i;
        t->lastSeenFrame = frameCounter;
        ++t->age;
        t->add (freq, pk.db);
        t->prominence = 0.8f * t->prominence + 0.2f * pk.prominence;
        t->purity     = 0.85f * t->purity + 0.15f * purity;
    }

    tracks.erase (std::remove_if (tracks.begin(), tracks.end(),
                                  [&] (const Track& t) { return frameCounter - t.lastSeenFrame > 6; }),
                  tracks.end());

    // 3. Classify and act
    const float threshold = 0.95f - 0.25f * s;   // probability needed to cut
    const int   needHot   = std::max (2, (int) std::round ((0.12 - 0.08 * s) / frameSec));
    const int   cooldown  = (int) std::round (0.15 / frameSec);
    const double now = juce::Time::getMillisecondCounterHiRes();
    float maxP = 0, maxF = 0;

    for (auto& t : tracks)
    {
        if (t.lastSeenFrame != frameCounter || t.count < 10) continue;
        t.probability = classify (t, frameSec);
        if (t.probability > maxP) { maxP = t.probability; maxF = t.freq[(t.head + kHistory - 1) % kHistory]; }

        if (t.cooldown > 0) { --t.cooldown; continue; }
        t.hotFrames = t.probability > threshold ? t.hotFrames + 1 : std::max (0, t.hotFrames - 1);

        if (t.hotFrames >= needHot)
        {
            double fm = 0;   // average of the last 8 estimates for an accurate centre
            const int n = std::min (8, t.count);
            for (int k = 1; k <= n; ++k) fm += t.freq[(t.head - k + kHistory) % kHistory];
            placeCut ((float) (fm / n), now);
            t.hotFrames = 0;
            t.cooldown = cooldown;   // give the cut time to work before deepening
        }
    }

    riskProb.store (maxP);
    riskFreq.store (maxF);
}

void FeedbackDetector::placeCut (float freqHz, double nowMs)
{
    const bool  ringOut  = ringOutMode.load();
    const float maxDepth = ringOut ? 24.0f : 9.0f + 15.0f * strength.load();

    int index = -1;
    for (int k = 0; k < kNumNotches; ++k)
        if (slots[(size_t) k].active && std::abs (std::log2 (slots[(size_t) k].freqHz / freqHz)) < 0.03f)
            { index = k; break; }

    if (index >= 0)
    {
        // Deepen only as much as needed, in small steps, to protect the sound
        auto& sl = slots[(size_t) index];
        sl.depthDb = std::min (maxDepth, sl.depthDb + 3.0f);
        sl.freqHz  = 0.7f * sl.freqHz + 0.3f * freqHz;
        sl.fixed   = sl.fixed || ringOut;
    }
    else
    {
        for (int k = 0; k < kNumNotches && index < 0; ++k)
            if (! slots[(size_t) k].active) index = k;

        if (index < 0)   // full: recycle the live notch quiet for longest, else the oldest fixed one
        {
            for (int pass = 0; pass < 2 && index < 0; ++pass)
                for (int k = 0; k < kNumNotches; ++k)
                    if ((pass == 1 || ! slots[(size_t) k].fixed)
                        && (index < 0 || slots[(size_t) k].lastHitMs < slots[(size_t) index].lastHitMs))
                        index = k;
        }
        slots[(size_t) index] = { true, ringOut, freqHz, 4.0f, nowMs };
    }

    slots[(size_t) index].lastHitMs = nowMs;
    publish (index);
}

void FeedbackDetector::releaseTick (double nowMs, double elapsedMs)
{
    if (! autoRelease.load()) return;

    for (int k = 0; k < kNumNotches; ++k)
    {
        auto& sl = slots[(size_t) k];
        if (sl.active && ! sl.fixed && nowMs - sl.lastHitMs > 20000.0)   // quiet 20 s → fade at 1 dB/s
        {
            sl.depthDb -= (float) (elapsedMs / 1000.0);
            if (sl.depthDb <= 1.0f) sl = {};
            publish (k);
        }
    }
}

void FeedbackDetector::publish (int k)
{
    const auto& sl = slots[(size_t) k];
    if (sl.active) shared[(size_t) k].freqHz.store (sl.freqHz);
    shared[(size_t) k].fixed.store (sl.fixed);
    shared[(size_t) k].depthDb.store (sl.active ? sl.depthDb : 0.0f);
}
} // namespace ringstop
