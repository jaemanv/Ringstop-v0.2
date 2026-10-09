#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace ringstop::ui
{
    const juce::Colour bg     { 0xff121821 }, raised { 0xff1b2333 }, well  { 0xff0d121a },
                       ink    { 0xffe8ebf1 }, muted  { 0xff8b95a8 }, line  { 0xff2a3446 },
                       amber  { 0xfff2a33a }, live   { 0xffff5d73 }, fixed { 0xff6fb1ff },
                       ok     { 0xff4cc38a };

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }
}

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (juce::String labelText) : label (std::move (labelText)) {}
    void pushPeak (float linear);
    void paint (juce::Graphics&) override;
private:
    juce::String label;
    float levelDb = -60.0f, holdDb = -60.0f;
    int holdFrames = 0;
};

//==============================================================================
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (RingstopProcessor& p);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    RingstopProcessor& proc;
    std::vector<float> spectrum, smoothed;
    double binHz = 11.7;
};

//==============================================================================
class NotchBank : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit NotchBank (RingstopProcessor& p);
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
private:
    void timerCallback() override { repaint(); }
    juce::Rectangle<float> tileBounds (int index) const;
    int tileAt (juce::Point<float>) const;
    RingstopProcessor& proc;
    int hovered = -1;
};

//==============================================================================
class RiskMeter : public juce::Component
{
public:
    void setRisk (float probability, float freqHz, bool active);
    void paint (juce::Graphics&) override;
private:
    float shown = 0, freq = 0;
    bool isActive = true;
};

//==============================================================================
class ModeSelector : public juce::Component, private juce::Timer
{
public:
    explicit ModeSelector (RingstopProcessor& p);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
private:
    void timerCallback() override { repaint(); }
    RingstopProcessor& proc;
};

//==============================================================================
class PowerButton : public juce::ToggleButton
{
public:
    PowerButton() { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

//==============================================================================
class RingstopLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RingstopLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return ringstop::ui::font (14.0f, true); }
};

//==============================================================================
class RingstopEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RingstopEditor (RingstopProcessor&);
    ~RingstopEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawCard (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, const juce::String& right = {});

    RingstopProcessor& proc;
    RingstopLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    SpectrumView spectrum;
    LevelMeter inMeter { "IN" }, outMeter { "OUT" };
    NotchBank notchBank;
    RiskMeter risk;
    ModeSelector mode;
    PowerButton power;

    juce::Slider strength;
    juce::ToggleButton autoRelease { "Auto-release live notches" };
    juce::TextButton clearLive { "Clear live" }, clearAll { "Clear all" };

    juce::Rectangle<int> knobCard, controlCard, notchCard, hintArea;

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SA> strengthAtt;
    std::unique_ptr<BA> powerAtt, releaseAtt;

    bool lastEngaged = true, lastRingOut = false;
    int lastNotchCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RingstopEditor)
};
