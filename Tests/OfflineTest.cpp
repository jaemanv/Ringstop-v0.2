// Offline tests for the feedback classifier.
// Each scenario runs audio through a fresh plugin instance (roughly in real time,
// so the analysis thread keeps pace) and checks what got notched.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Source/PluginProcessor.h"
#include <cmath>
#include <functional>
#include <iostream>
#include <random>

constexpr double sr = 48000.0;
constexpr int block = 256;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

struct Result { int notches = 0, fixedNotches = 0; std::vector<float> freqs; double changeDb = 0; };

// gen(t) returns one sample; tone(t) optionally returns the part to measure attenuation of
static Result run (double seconds, std::function<float (double)> gen, bool ringOut = false,
                   std::function<float (double)> measured = nullptr)
{
    RingstopProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, block);
    if (ringOut)
        if (auto* p = proc.apvts.getParameter ("mode")) p->setValueNotifyingHost (1.0f);
    proc.prepareToPlay (sr, block);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double inE = 0, outE = 0;
    long n = 0;
    std::vector<float> ref (block);

    for (int b = 0; b < (int) (seconds * sr / block); ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const double t = (double) n++ / sr;
            const float x = gen (t);
            ref[(size_t) i] = measured ? measured (t) : x;
            buf.setSample (0, i, x); buf.setSample (1, i, x);
        }
        proc.processBlock (buf, midi);
        if (b * block / sr > seconds - 1.5)   // measure the last 1.5 s
        {
            if (measured)
            {   // project output onto the measured component (correlation)
                double num = 0, den = 0;
                for (int i = 0; i < block; ++i) { num += buf.getSample (0, i) * ref[(size_t) i]; den += ref[(size_t) i] * ref[(size_t) i]; }
                inE += den; outE += num * num / (den + 1e-12);
            }
            else
                for (int i = 0; i < block; ++i) { inE += ref[(size_t) i] * ref[(size_t) i]; outE += buf.getSample (0, i) * buf.getSample (0, i); }
        }
        juce::Thread::sleep (1);
    }

    Result r;
    for (auto& s : proc.sharedSlots)
        if (s.depthDb.load() > 0)
        {
            ++r.notches; r.fixedNotches += s.fixed.load() ? 1 : 0;
            r.freqs.push_back (s.freqHz.load());
        }
    r.changeDb = 10.0 * std::log10 ((outE + 1e-12) / (inE + 1e-12));
    proc.releaseResources();
    return r;
}

// A sung vowel: harmonic-rich, with natural vibrato
static float vowel (double t, double f0, double vibCents)
{
    const double f = f0 * std::pow (2.0, vibCents / 1200.0 * std::sin (twoPi * 5.5 * t));
    static double phase = 0; static double lastT = -1;
    if (t < lastT) phase = 0;
    lastT = t;
    phase += twoPi * f / sr;
    float y = 0;
    for (int h = 1; h <= 8; ++h) y += (float) (0.25 / h * std::sin (h * phase));
    return y;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::mt19937 rng (1);
    std::normal_distribution<float> noise (0.0f, 0.01f);
    int failures = 0;
    auto report = [&] (const char* name, const Result& r, bool pass, const char* expect)
    {
        std::cout << (pass ? "PASS  " : "FAIL  ") << name << "\n      expected: " << expect
                  << "\n      notches: " << r.notches;
        for (auto f : r.freqs) std::cout << "  " << (int) std::round (f) << " Hz";
        std::cout << "\n      level change: " << std::round (r.changeDb * 10) / 10 << " dB\n\n";
        failures += pass ? 0 : 1;
    };

    // A. Feedback building up (pure tone growing 20 dB/s)
    {
        auto fb = [] (double t) { return (float) (std::min (0.5, 0.005 * std::pow (10.0, t)) * std::sin (twoPi * 1850.0 * t)); };
        auto r = run (5.0, [&] (double t) { return fb (t) + noise (rng); }, false, fb);
        report ("A  Feedback builds up at 1850 Hz", r,
                r.notches == 1 && std::abs (r.freqs[0] - 1850) < 5 && r.changeDb < -10, "one notch at 1850 Hz, tone cut >10 dB");
    }
    // B. Singer with vibrato: must be left alone
    {
        auto r = run (6.0, [&] (double t) { return vowel (t, 220.0, 40.0) + noise (rng); });
        report ("B  Sung note with vibrato (220 Hz)", r, r.notches == 0 && std::abs (r.changeDb) < 0.5, "no notches, sound unchanged");
    }
    // C. Held instrument note, no vibrato, rich in harmonics: must be left alone
    {
        auto r = run (5.0, [&] (double t) { return vowel (t, 440.0, 0.0) + noise (rng); });
        report ("C  Held 440 Hz note, no vibrato", r, r.notches == 0 && std::abs (r.changeDb) < 0.5, "no notches, sound unchanged");
    }
    // D. Feedback starts while the singer is singing
    {
        auto fb = [] (double t) { return t < 1.5 ? 0.0f : (float) (std::min (0.4, 0.003 * std::pow (10.0, t - 1.5)) * std::sin (twoPi * 2600.0 * t)); };
        auto r = run (6.0, [&] (double t) { return vowel (t, 196.0, 35.0) + fb (t) + noise (rng); }, false, fb);
        bool ok = r.notches >= 1;
        for (auto f : r.freqs) ok &= std::abs (f - 2600) < 10;
        report ("D  Feedback at 2600 Hz during singing", r, ok && r.changeDb < -10, "only 2600 Hz notched, voice untouched");
    }
    // E. Ring-out mode: steady ringing at soundcheck gets a fixed notch
    {
        auto fb = [] (double t) { return (float) (std::min (0.3, 0.01 * std::pow (10.0, t * 0.5)) * std::sin (twoPi * 630.0 * t)); };
        auto r = run (5.0, [&] (double t) { return fb (t) + noise (rng); }, true, fb);
        report ("E  Ring-out mode at 630 Hz", r, r.fixedNotches == 1 && std::abs (r.freqs[0] - 630) < 5, "one FIXED notch at 630 Hz");
    }

    std::cout << (failures == 0 ? "All tests passed\n" : "Some tests failed\n");
    return failures;
}
