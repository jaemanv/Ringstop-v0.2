#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "FeedbackDetector.h"

class RingstopProcessor : public juce::AudioProcessor
{
public:
    RingstopProcessor();
    ~RingstopProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    ringstop::SharedSlots sharedSlots;
    ringstop::FeedbackDetector detector { sharedSlots };

    // For the GUI
    std::array<std::atomic<float>, ringstop::kNumNotches> appliedDepth {};   // dB actually applied
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };              // linear, GUI resets
    static float notchMagnitudeDb (double freqHz, double centreHz, float cutDb, double sampleRate);

private:
    struct Coeffs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State  { float z1 = 0, z2 = 0; };
    static constexpr int kMaxChannels = 2;

    void updateCoefficients (int slot, float freqHz, float cutDb);

    std::array<Coeffs, ringstop::kNumNotches> coeffs;
    std::array<std::array<State, ringstop::kNumNotches>, kMaxChannels> states;
    std::array<float, ringstop::kNumNotches> curDepth {}, curFreq {};
    std::vector<float> mono;
    double sr = 48000.0;

    std::atomic<float>* strengthParam = nullptr;
    std::atomic<float>* engagedParam  = nullptr;
    std::atomic<float>* releaseParam  = nullptr;
    std::atomic<float>* modeParam     = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RingstopProcessor)
};
