#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <complex>

using namespace ringstop;

RingstopProcessor::RingstopProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    strengthParam = apvts.getRawParameterValue ("strength");
    engagedParam  = apvts.getRawParameterValue ("engaged");
    releaseParam  = apvts.getRawParameterValue ("autorelease");
    modeParam     = apvts.getRawParameterValue ("mode");
}

RingstopProcessor::~RingstopProcessor() { detector.release(); }

juce::AudioProcessorValueTreeState::ParameterLayout RingstopProcessor::createLayout()
{
    using namespace juce;
    return {
        std::make_unique<AudioParameterFloat> (ParameterID { "strength", 1 }, "Strength",
                                               NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
                                               AudioParameterFloatAttributes().withLabel ("%")),
        std::make_unique<AudioParameterBool>  (ParameterID { "engaged", 1 }, "Engaged", true),
        std::make_unique<AudioParameterBool>  (ParameterID { "autorelease", 1 }, "Auto-release", true),
        std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                StringArray { "Live", "Ring-out" }, 0)
    };
}

bool RingstopProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void RingstopProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    mono.assign ((size_t) juce::jmax (samplesPerBlock, 512), 0.0f);
    for (auto& ch : states) ch.fill ({});
    curDepth.fill (0.0f);
    curFreq.fill (0.0f);
    for (auto& d : appliedDepth) d.store (0.0f);
    setLatencySamples (0);   // IIR notches add no latency
    detector.prepare (sampleRate);
}

void RingstopProcessor::releaseResources() { detector.release(); }

float RingstopProcessor::notchMagnitudeDb (double f, double fc, float cutDb, double sampleRate)
{
    const double A  = std::pow (10.0, -cutDb / 40.0);
    const double w0 = juce::MathConstants<double>::twoPi * juce::jlimit (20.0, sampleRate * 0.45, fc) / sampleRate;
    const double alpha = std::sin (w0) / (2.0 * kNotchQ), cosw = std::cos (w0);
    const double b0 = 1 + alpha * A, b1 = -2 * cosw, b2 = 1 - alpha * A;
    const double a0 = 1 + alpha / A, a1 = -2 * cosw, a2 = 1 - alpha / A;
    const std::complex<double> z1 = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / sampleRate);
    const std::complex<double> z2 = z1 * z1;
    const auto h = (b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2);
    return (float) (20.0 * std::log10 (std::abs (h) + 1e-12));
}

void RingstopProcessor::updateCoefficients (int k, float freqHz, float cutDb)
{
    // RBJ "peaking EQ" biquad with negative gain = narrow notch
    const double f  = juce::jlimit (20.0, sr * 0.45, (double) freqHz);
    const double A  = std::pow (10.0, -cutDb / 40.0);
    const double w0 = juce::MathConstants<double>::twoPi * f / sr;
    const double alpha = std::sin (w0) / (2.0 * kNotchQ);
    const double cosw = std::cos (w0);
    const double a0 = 1.0 + alpha / A;

    auto& c = coeffs[(size_t) k];
    c.b0 = (float) ((1.0 + alpha * A) / a0);
    c.b1 = (float) ((-2.0 * cosw) / a0);
    c.b2 = (float) ((1.0 - alpha * A) / a0);
    c.a1 = (float) ((-2.0 * cosw) / a0);
    c.a2 = (float) ((1.0 - alpha / A) / a0);
}

void RingstopProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels();
    const int numSamples = buffer.getNumSamples();
    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const bool engaged = engagedParam->load() > 0.5f;
    detector.setStrength (strengthParam->load() / 100.0f);
    detector.setEnabled (engaged);
    detector.setAutoRelease (releaseParam->load() > 0.5f);
    detector.setRingOutMode (modeParam->load() > 0.5f);

    auto blockPeak = [&]
    {
        float p = 0;
        for (int ch = 0; ch < numIn; ++ch) p = juce::jmax (p, buffer.getMagnitude (ch, 0, numSamples));
        return p;
    };
    inputPeak.store (juce::jmax (inputPeak.load(), blockPeak()));

    // Smooth depth changes: fast attack to catch feedback, slower release
    const float blockSec = (float) (numSamples / sr);
    const float attackStep  = 300.0f * blockSec;   // dB per block
    const float releaseStep = 40.0f  * blockSec;
    const int numCh = juce::jmin (numIn, kMaxChannels);

    for (int k = 0; k < kNumNotches; ++k)
    {
        const float target = engaged ? sharedSlots[(size_t) k].depthDb.load() : 0.0f;
        const float freq   = sharedSlots[(size_t) k].freqHz.load();
        float& cur = curDepth[(size_t) k];

        const float prev = cur;
        cur = target > cur ? juce::jmin (target, cur + attackStep)
                           : juce::jmax (target, cur - releaseStep);
        appliedDepth[(size_t) k].store (cur);

        if (cur < 0.01f)
        {
            if (prev >= 0.01f)
                for (auto& ch : states) ch[(size_t) k] = {};
            cur = 0.0f;
            continue;   // inactive: skip processing entirely
        }

        if (std::abs (cur - prev) > 1.0e-6f || std::abs (freq - curFreq[(size_t) k]) > 0.01f)
        {
            curFreq[(size_t) k] = freq;
            updateCoefficients (k, freq, cur);
        }

        const auto c = coeffs[(size_t) k];
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& s = states[(size_t) ch][(size_t) k];
            float* x = buffer.getWritePointer (ch);
            float z1 = s.z1, z2 = s.z2;
            for (int i = 0; i < numSamples; ++i)   // transposed direct form II
            {
                const float in = x[i];
                const float y  = c.b0 * in + z1;
                z1 = c.b1 * in - c.a1 * y + z2;
                z2 = c.b2 * in - c.a2 * y;
                x[i] = y;
            }
            s.z1 = z1; s.z2 = z2;
        }
    }

    outputPeak.store (juce::jmax (outputPeak.load(), blockPeak()));

    // Feed the post-notch signal to the detector so it can deepen cuts that aren't enough
    if (numCh > 0)
    {
        for (int pos = 0; pos < numSamples; pos += (int) mono.size())
        {
            const int n = juce::jmin ((int) mono.size(), numSamples - pos);
            for (int i = 0; i < n; ++i)
            {
                float v = 0;
                for (int ch = 0; ch < numCh; ++ch) v += buffer.getReadPointer (ch)[pos + i];
                mono[(size_t) i] = v / (float) numCh;
            }
            detector.pushSamples (mono.data(), n);
        }
    }
}

void RingstopProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void RingstopProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* RingstopProcessor::createEditor() { return new RingstopEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RingstopProcessor(); }
