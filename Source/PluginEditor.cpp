#include "PluginEditor.h"
#include <cmath>

using namespace ringstop;
namespace C = ringstop::ui;

static bool isEngaged (RingstopProcessor& p) { return p.apvts.getRawParameterValue ("engaged")->load() > 0.5f; }
static bool isRingOut (RingstopProcessor& p) { return p.apvts.getRawParameterValue ("mode")->load() > 0.5f; }

static juce::String formatFreq (float f)
{
    return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz"
                        : juce::String (juce::roundToInt (f)) + " Hz";
}

//============================================================================== LevelMeter
void LevelMeter::pushPeak (float linear)
{
    const float db = juce::jlimit (-60.0f, 0.0f, juce::Decibels::gainToDecibels (linear, -60.0f));
    levelDb = db > levelDb ? db : juce::jmax (db, levelDb - 1.5f);   // fall ~45 dB/s at 30 fps
    if (db >= holdDb) { holdDb = db; holdFrames = 45; }
    else if (--holdFrames <= 0) holdDb = juce::jmax (-60.0f, holdDb - 1.0f);
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    auto text = b.removeFromBottom (18);
    g.setColour (C::muted);
    g.setFont (C::font (11.0f, true));
    g.drawText (label, text, juce::Justification::centred);

    g.setColour (C::well);
    g.fillRoundedRectangle (b, 5.0f);
    auto inner = b.reduced (4.0f);

    const int segments = 30;
    const float segH = inner.getHeight() / segments;
    const int lit = juce::roundToInt ((levelDb + 60.0f) / 60.0f * segments);
    for (int i = 0; i < segments; ++i)
    {
        const float frac = (float) i / segments;
        const auto col = frac > 0.9f ? C::live : frac > 0.75f ? C::amber : C::ok;
        g.setColour (i < lit ? col : col.withAlpha (0.10f));
        g.fillRoundedRectangle (inner.getX(), inner.getBottom() - (i + 1) * segH + 1.0f,
                                inner.getWidth(), segH - 2.0f, 1.5f);
    }
    if (holdDb > -59.0f)
    {
        const float y = inner.getBottom() - (holdDb + 60.0f) / 60.0f * inner.getHeight();
        g.setColour (C::ink);
        g.fillRect (inner.getX(), y, inner.getWidth(), 2.0f);
    }
}

//============================================================================== SpectrumView
SpectrumView::SpectrumView (RingstopProcessor& p) : proc (p) { startTimerHz (30); }

void SpectrumView::timerCallback()
{
    proc.detector.copySpectrum (spectrum, binHz);
    if (smoothed.size() != spectrum.size()) smoothed = spectrum;
    for (size_t i = 0; i < spectrum.size(); ++i)   // visual smoothing only
        smoothed[i] = spectrum[i] > smoothed[i] ? spectrum[i] : 0.75f * smoothed[i] + 0.25f * spectrum[i];
    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (C::well);
    g.fillRoundedRectangle (area, 10.0f);
    const auto b = area.reduced (12.0f, 10.0f).withTrimmedBottom (14.0f);

    const float fMin = 50.0f, fMax = 16000.0f, dbMin = -100.0f, dbMax = 0.0f;
    auto X = [&] (float f)  { return b.getX() + std::log (f / fMin) / std::log (fMax / fMin) * b.getWidth(); };
    auto Y = [&] (float db) { return b.getY() + (1.0f - (juce::jlimit (dbMin, dbMax, db) - dbMin) / (dbMax - dbMin)) * b.getHeight(); };

    // Grid
    g.setFont (C::font (11.0f));
    for (float f : { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = X (f);
        g.setColour (C::line.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, b.getY(), b.getBottom());
        g.setColour (C::muted);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                    (int) x - 20, (int) b.getBottom() + 2, 40, 14, juce::Justification::centred);
    }
    for (float db = -80.0f; db <= -20.0f; db += 20.0f)
    {
        g.setColour (C::line.withAlpha (0.4f));
        g.drawHorizontalLine ((int) Y (db), b.getX(), b.getRight());
    }

    // Spectrum
    if (! smoothed.empty())
    {
        juce::Path p;
        bool first = true;
        for (size_t i = (size_t) std::ceil (fMin / binHz); i < smoothed.size() && i * binHz < fMax; ++i)
        {
            const float x = X ((float) (i * binHz)), y = Y (smoothed[i]);
            if (first) { p.startNewSubPath (x, b.getBottom()); p.lineTo (x, y); first = false; }
            else p.lineTo (x, y);
        }
        juce::Path stroke (p);
        p.lineTo (b.getRight(), b.getBottom());
        p.closeSubPath();
        g.setGradientFill (juce::ColourGradient (C::amber.withAlpha (0.28f), 0, b.getY(),
                                                 C::amber.withAlpha (0.02f), 0, b.getBottom(), false));
        g.fillPath (p);
        g.setColour (C::amber);
        g.strokePath (stroke, juce::PathStrokeType (1.4f));
    }

    const bool engaged = isEngaged (proc);
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;

    // Combined notch filter curve (what is actually being removed)
    const float curveTop = b.getY() + 6.0f, curveRange = b.getHeight() * 0.55f;   // 0 .. -30 dB
    auto CY = [&] (float db) { return curveTop + juce::jlimit (0.0f, 1.0f, -db / 30.0f) * curveRange; };
    bool anyActive = false;
    for (auto& d : proc.appliedDepth) anyActive |= d.load() > 0.05f;

    if (anyActive)
    {
        juce::Path curve;
        const int points = juce::jmax (100, (int) b.getWidth() / 2);
        for (int i = 0; i <= points; ++i)
        {
            const float f = fMin * std::pow (fMax / fMin, (float) i / points);
            float db = 0;
            for (int k = 0; k < kNumNotches; ++k)
            {
                const float depth = proc.appliedDepth[(size_t) k].load();
                if (depth > 0.05f)
                    db += RingstopProcessor::notchMagnitudeDb (f, proc.sharedSlots[(size_t) k].freqHz.load(), depth, sr);
            }
            if (i == 0) curve.startNewSubPath (X (f), CY (db)); else curve.lineTo (X (f), CY (db));
        }
        juce::Path fill (curve);
        fill.lineTo (b.getRight(), curveTop);
        fill.lineTo (b.getX(), curveTop);
        fill.closeSubPath();
        g.setColour (C::live.withAlpha (engaged ? 0.16f : 0.05f));
        g.fillPath (fill);
        g.setColour (C::ink.withAlpha (engaged ? 0.9f : 0.3f));
        g.strokePath (curve, juce::PathStrokeType (1.6f));
    }

    // Notch markers with labels
    g.setFont (C::font (11.0f, true));
    for (int k = 0; k < kNumNotches; ++k)
    {
        const float target = proc.sharedSlots[(size_t) k].depthDb.load();
        if (target <= 0.0f) continue;
        const float f = proc.sharedSlots[(size_t) k].freqHz.load();
        const float x = X (f), y = CY (-target);
        const auto col = (proc.sharedSlots[(size_t) k].fixed.load() ? C::fixed : C::live).withAlpha (engaged ? 1.0f : 0.35f);
        g.setColour (col);
        g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
        g.drawText (formatFreq (f), (int) x - 40, (int) y + 6, 80, 14, juce::Justification::centred);
    }

    // What the classifier is currently watching
    const float riskP = proc.detector.getRiskProbability();
    if (engaged && riskP > 0.35f)
    {
        const float x = X (proc.detector.getRiskFrequency());
        const float dashes[] = { 4.0f, 4.0f };
        g.setColour (C::amber.withAlpha (juce::jmin (1.0f, riskP)));
        g.drawDashedLine ({ x, b.getY(), x, b.getBottom() }, dashes, 2, 1.5f);
        const float r = 10.0f + 8.0f * riskP;
        g.drawEllipse (x - r, b.getBottom() - 40 - r, r * 2, r * 2, 2.0f);
        g.setFont (C::font (11.0f, true));
        g.drawText (juce::String (juce::roundToInt (riskP * 100)) + "%", (int) (x - 30), (int) (b.getBottom() - 40 - 7), 60, 14,
                    juce::Justification::centred);
    }

    // Legend
    auto legend = juce::Rectangle<float> (b.getRight() - 250, b.getY(), 250, 16);
    g.setFont (C::font (11.0f));
    auto item = [&] (juce::Colour c, const juce::String& t, float w)
    {
        auto r = legend.removeFromLeft (w);
        g.setColour (c);
        g.fillEllipse (r.getX(), r.getCentreY() - 4, 8, 8);
        g.setColour (C::muted);
        g.drawText (t, r.withTrimmedLeft (12), juce::Justification::centredLeft);
    };
    item (C::live, "Live notch", 84);
    item (C::fixed, "Fixed notch", 90);
    item (C::amber, "Watching", 76);

    if (! engaged)
    {
        g.setColour (C::bg.withAlpha (0.55f));
        g.fillRoundedRectangle (area, 10.0f);
        g.setColour (C::amber);
        g.setFont (C::font (22.0f, true));
        g.drawText ("BYPASSED", area, juce::Justification::centred);
    }
}

//============================================================================== NotchBank
NotchBank::NotchBank (RingstopProcessor& p) : proc (p) { startTimerHz (12); }

juce::Rectangle<float> NotchBank::tileBounds (int index) const
{
    const int cols = getWidth() >= 520 ? 8 : 4;
    const int rows = kNumNotches / cols;
    const float gap = 6.0f;
    const float w = (getWidth() - gap * (cols - 1)) / cols;
    const float h = (getHeight() - gap * (rows - 1)) / rows;
    return { (index % cols) * (w + gap), (index / cols) * (h + gap), w, h };
}

int NotchBank::tileAt (juce::Point<float> p) const
{
    for (int k = 0; k < kNumNotches; ++k)
        if (tileBounds (k).contains (p)) return k;
    return -1;
}

void NotchBank::paint (juce::Graphics& g)
{
    for (int k = 0; k < kNumNotches; ++k)
    {
        const auto r = tileBounds (k);
        const float depth = proc.sharedSlots[(size_t) k].depthDb.load();
        const bool active = depth > 0.0f;
        const bool isFixed = proc.sharedSlots[(size_t) k].fixed.load();
        const auto accent = isFixed ? C::fixed : C::live;

        g.setColour (active && k == hovered ? C::line : C::well);
        g.fillRoundedRectangle (r, 6.0f);

        if (! active)
        {
            g.setColour (C::muted.withAlpha (0.35f));
            g.setFont (C::font (12.0f));
            g.drawText (juce::String (k + 1), r, juce::Justification::centred);
            continue;
        }

        g.setColour (accent);
        g.fillRoundedRectangle (r.withWidth (3.0f), 1.5f);

        auto inner = r.reduced (9.0f, 6.0f);
        g.setColour (C::ink);
        g.setFont (C::font (14.0f, true));
        g.drawText (formatFreq (proc.sharedSlots[(size_t) k].freqHz.load()), inner.removeFromTop (18),
                    juce::Justification::centredLeft);

        auto row = inner.removeFromTop (15);
        g.setColour (C::muted);
        g.setFont (C::font (11.0f));
        g.drawText ("-" + juce::String (juce::roundToInt (depth)) + " dB", row, juce::Justification::centredLeft);
        g.setColour (accent);
        g.setFont (C::font (10.0f, true));
        g.drawText (isFixed ? "FIXED" : "LIVE", row, juce::Justification::centredRight);

        const auto bar = inner.removeFromBottom (3.0f);
        g.setColour (C::line);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jmin (1.0f, depth / 24.0f)), 1.5f);

        if (k == hovered)
        {
            g.setColour (C::ink);
            g.setFont (C::font (13.0f, true));
            g.drawText (juce::CharPointer_UTF8 ("\xc3\x97"), r.withTrimmedLeft (r.getWidth() - 18).withHeight (18),
                        juce::Justification::centred);
        }
    }
}

void NotchBank::mouseMove (const juce::MouseEvent& e)
{
    const int h = tileAt (e.position);
    const bool clickable = h >= 0 && proc.sharedSlots[(size_t) h].depthDb.load() > 0.0f;
    setMouseCursor (clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (h != hovered) { hovered = h; repaint(); }
}

void NotchBank::mouseExit (const juce::MouseEvent&) { hovered = -1; repaint(); }

void NotchBank::mouseDown (const juce::MouseEvent& e)
{
    const int k = tileAt (e.position);
    if (k >= 0 && proc.sharedSlots[(size_t) k].depthDb.load() > 0.0f)
        proc.detector.requestRemove (k);
}

//============================================================================== RiskMeter
void RiskMeter::setRisk (float p, float f, bool active)
{
    shown = p > shown ? p : 0.85f * shown + 0.15f * p;
    if (p > 0.3f) freq = f;
    isActive = active;
    repaint();
}

void RiskMeter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    auto top = b.removeFromTop (20);
    g.setFont (C::font (12.0f, true));
    g.setColour (C::muted);
    g.drawText ("FEEDBACK RISK", top, juce::Justification::centredLeft);

    juce::String status = ! isActive ? "Bypassed"
                        : shown < 0.3f ? "All clear"
                        : "Watching " + formatFreq (freq);
    g.setColour (! isActive ? C::muted : shown < 0.3f ? C::ok : shown < 0.7f ? C::amber : C::live);
    g.drawText (status, top, juce::Justification::centredRight);

    auto bar = b.removeFromTop (12).withTrimmedTop (2);
    g.setColour (C::well);
    g.fillRoundedRectangle (bar, 5.0f);
    if (isActive && shown > 0.01f)
    {
        g.setGradientFill (juce::ColourGradient::horizontal (C::ok, bar.getX(), C::live, bar.getRight()));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (10.0f, bar.getWidth() * shown)), 5.0f);
    }
}

//============================================================================== ModeSelector
ModeSelector::ModeSelector (RingstopProcessor& p) : proc (p) { startTimerHz (8); }

void ModeSelector::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (C::well);
    g.fillRoundedRectangle (b, b.getHeight() / 2);
    const bool ringOut = isRingOut (proc);
    auto seg = b.reduced (3.0f);
    auto left = seg.removeFromLeft (seg.getWidth() / 2), right = seg;

    g.setColour (ringOut ? C::fixed : C::amber);
    g.fillRoundedRectangle (ringOut ? right : left, left.getHeight() / 2);

    g.setFont (C::font (13.0f, true));
    g.setColour (ringOut ? C::muted : C::bg);
    g.drawText ("LIVE", left, juce::Justification::centred);
    g.setColour (ringOut ? C::bg : C::muted);
    g.drawText ("RING-OUT", right, juce::Justification::centred);
}

void ModeSelector::mouseDown (const juce::MouseEvent& e)
{
    if (auto* p = proc.apvts.getParameter ("mode"))
    {
        const float v = p->convertTo0to1 (e.x > getWidth() / 2 ? 1.0f : 0.0f);
        p->beginChangeGesture();
        p->setValueNotifyingHost (v);
        p->endChangeGesture();
    }
    repaint();
}

//============================================================================== PowerButton
void PowerButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    auto b = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = getToggleState();
    const auto col = on ? C::ok : C::muted;

    g.setColour (C::well);
    g.fillEllipse (b);
    g.setColour (col.withAlpha (highlighted ? 1.0f : 0.85f));
    g.drawEllipse (b.reduced (1.0f), on ? 2.5f : 1.5f);

    const auto c = b.getCentre();
    const float r = b.getWidth() * 0.22f;
    juce::Path icon;
    icon.addCentredArc (c.x, c.y, r, r, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
    icon.startNewSubPath (c.x, c.y - r * 1.35f);
    icon.lineTo (c.x, c.y - r * 0.2f);
    g.strokePath (icon, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//============================================================================== LookAndFeel
RingstopLookAndFeel::RingstopLookAndFeel()
{
    setColour (juce::ToggleButton::textColourId, C::ink);
    setColour (juce::TextButton::textColourOffId, C::ink);
    setColour (juce::Slider::textBoxTextColourId, C::ink);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, C::ink);
}

void RingstopLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                            float a0, float a1, juce::Slider&)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto c = bounds.getCentre();
    const float angle = a0 + pos * (a1 - a0);

    juce::Path track, value;
    track.addCentredArc (c.x, c.y, r - 5, r - 5, 0, a0, a1, true);
    value.addCentredArc (c.x, c.y, r - 5, r - 5, 0, a0, angle, true);
    const juce::PathStrokeType stroke (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (C::well);  g.strokePath (track, stroke);
    g.setColour (C::amber); g.strokePath (value, stroke);

    const float capR = r * 0.66f;
    g.setGradientFill (juce::ColourGradient (C::line, c.x, c.y - capR, C::well, c.x, c.y + capR, false));
    g.fillEllipse (c.x - capR, c.y - capR, capR * 2, capR * 2);
    g.setColour (C::line.brighter (0.2f));
    g.drawEllipse (c.x - capR, c.y - capR, capR * 2, capR * 2, 1.0f);

    g.setColour (C::ink);
    g.drawLine (juce::Line<float> (c.getPointOnCircumference (capR * 0.35f, angle),
                                   c.getPointOnCircumference (capR * 0.85f, angle)), 3.5f);
}

void RingstopLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                                bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? C::line.brighter (0.1f) : highlighted ? C::line : C::well);
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (C::line);
    g.drawRoundedRectangle (r, 7.0f, 1.0f);
}

void RingstopLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto sw = r.removeFromLeft (38).withSizeKeepingCentre (34, 18);
    const bool on = b.getToggleState();
    g.setColour (on ? C::amber : C::well);
    g.fillRoundedRectangle (sw, 9.0f);
    g.setColour (C::line.withAlpha (highlighted ? 1.0f : 0.7f));
    g.drawRoundedRectangle (sw, 9.0f, 1.0f);
    g.setColour (on ? C::ink : C::muted);
    g.fillEllipse (on ? sw.getRight() - 16 : sw.getX() + 2, sw.getY() + 2, 14, 14);
    g.setColour (C::ink);
    g.setFont (C::font (13.0f));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (8), juce::Justification::centredLeft);
}

//============================================================================== Editor
RingstopEditor::RingstopEditor (RingstopProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p), notchBank (p), mode (p)
{
    setLookAndFeel (&lnf);

    strength.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    strength.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 24);
    strength.setTextValueSuffix (" %");
    strength.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);

    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &inMeter, &outMeter, &notchBank, &risk,
                                                             &mode, &power, &strength, &autoRelease, &clearLive, &clearAll })
        addAndMakeVisible (c);

    strengthAtt = std::make_unique<SA> (proc.apvts, "strength", strength);
    powerAtt    = std::make_unique<BA> (proc.apvts, "engaged", power);
    releaseAtt  = std::make_unique<BA> (proc.apvts, "autorelease", autoRelease);
    clearLive.onClick = [this] { proc.detector.requestClearLive(); };
    clearAll.onClick  = [this] { proc.detector.requestClearAll(); };

    power.setTooltip ("Engage / bypass");
    notchBank.setTooltip ("Click a notch to remove it");

    setResizable (true, true);
    setResizeLimits (780, 560, 1600, 1000);
    setSize (980, 640);
    startTimerHz (30);
}

RingstopEditor::~RingstopEditor() { setLookAndFeel (nullptr); }

void RingstopEditor::timerCallback()
{
    inMeter.pushPeak (proc.inputPeak.exchange (0.0f));
    outMeter.pushPeak (proc.outputPeak.exchange (0.0f));
    const bool engaged = isEngaged (proc), ringOut = isRingOut (proc);
    risk.setRisk (proc.detector.getRiskProbability(), proc.detector.getRiskFrequency(), engaged);

    int count = 0;
    for (auto& s : proc.sharedSlots) count += s.depthDb.load() > 0.0f ? 1 : 0;

    if (engaged != lastEngaged || ringOut != lastRingOut || count != lastNotchCount)
    {
        lastEngaged = engaged; lastRingOut = ringOut; lastNotchCount = count;
        repaint();
    }
}

void RingstopEditor::drawCard (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& right)
{
    g.setColour (C::raised);
    g.fillRoundedRectangle (r.toFloat(), 12.0f);
    auto head = r.reduced (16, 0).removeFromTop (34);
    g.setFont (C::font (12.0f, true));
    g.setColour (C::muted);
    g.drawText (title, head, juce::Justification::centredLeft);
    if (right.isNotEmpty())
        g.drawText (right, head, juce::Justification::centredRight);
}

void RingstopEditor::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);

    // Top bar
    auto top = getLocalBounds().reduced (20, 0).removeFromTop (70);
    g.setColour (C::ink);
    g.setFont (C::font (26.0f, true));
    g.drawText ("RINGSTOP", top.withTrimmedTop (12).removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (C::muted);
    g.setFont (C::font (12.0f));
    g.drawText ("Intelligent feedback suppression", top.withTrimmedTop (42).removeFromTop (18), juce::Justification::centredLeft);

    const bool engaged = isEngaged (proc);
    auto status = power.getBounds().translated (-120, 0).withWidth (110);
    g.setFont (C::font (13.0f, true));
    g.setColour (engaged ? C::ok : C::amber);
    g.drawText (engaged ? "ACTIVE" : "BYPASSED", status, juce::Justification::centredRight);

    // Cards
    int count = 0, fixedCount = 0;
    for (auto& s : proc.sharedSlots)
        if (s.depthDb.load() > 0.0f) { ++count; fixedCount += s.fixed.load() ? 1 : 0; }

    drawCard (g, knobCard, "STRENGTH");
    drawCard (g, controlCard, "DETECTION");
    drawCard (g, notchCard, "NOTCH FILTERS",
              juce::String (count) + " / " + juce::String (kNumNotches)
              + (fixedCount > 0 ? "   (" + juce::String (fixedCount) + " fixed)" : juce::String()));

    // Strength range hints
    g.setFont (C::font (11.0f));
    g.setColour (C::muted);
    auto hints = knobCard.reduced (16, 12).removeFromBottom (16);
    g.drawText ("Gentle", hints, juce::Justification::centredLeft);
    g.drawText ("Aggressive", hints, juce::Justification::centredRight);

    // Mode explanation
    g.setFont (C::font (12.0f));
    g.setColour (C::muted);
    g.drawFittedText (isRingOut (proc)
                          ? "Ring-out: raise the gain slowly at soundcheck. Every notch found is kept fixed for the show."
                          : "Live: ringing is classified as feedback or music. Only feedback is cut; live notches release after 20 s of quiet.",
                      hintArea, juce::Justification::topLeft, 3);
}

void RingstopEditor::resized()
{
    auto area = getLocalBounds().reduced (20, 0);
    auto top = area.removeFromTop (70);
    power.setBounds (top.removeFromRight (46).withSizeKeepingCentre (46, 46));
    mode.setBounds (top.withSizeKeepingCentre (230, 36));

    area.removeFromBottom (20);
    auto bottom = area.removeFromBottom (236);
    area.removeFromBottom (14);

    inMeter.setBounds (area.removeFromLeft (24));
    area.removeFromLeft (10);
    outMeter.setBounds (area.removeFromRight (24));
    area.removeFromRight (10);
    spectrum.setBounds (area);

    knobCard = bottom.removeFromLeft (200);
    bottom.removeFromLeft (14);
    controlCard = bottom.removeFromLeft (290);
    bottom.removeFromLeft (14);
    notchCard = bottom;

    strength.setBounds (knobCard.reduced (20, 0).withTrimmedTop (34).withTrimmedBottom (28));

    auto c = controlCard.reduced (16, 0).withTrimmedTop (36);
    risk.setBounds (c.removeFromTop (36));
    c.removeFromTop (10);
    autoRelease.setBounds (c.removeFromTop (28));
    c.removeFromTop (8);
    auto buttons = c.removeFromTop (32);
    clearLive.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 4));
    buttons.removeFromLeft (8);
    clearAll.setBounds (buttons);
    c.removeFromTop (10);
    hintArea = c.withTrimmedBottom (12);

    notchBank.setBounds (notchCard.reduced (16, 0).withTrimmedTop (36).withTrimmedBottom (16));
}
