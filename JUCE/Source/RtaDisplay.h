#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "RtaAnalyser.h"

/** Octave band view of the captured audio.

    Bands are drawn as tiles on a logarithmic frequency axis, with the held peak drawn
    across them and the minimum and maximum marked, so a band can be read as a number
    rather than as the height of a line.
*/
class RtaDisplay : public juce::Component
{
public:
    RtaDisplay();
    ~RtaDisplay() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseDown (const juce::MouseEvent& event) override;

    /** Takes the bands the worker measured and copies them for painting. */
    void setBands (const std::vector<dsp::RtaAnalyser::Band>& newBands,
                   const std::vector<float>& newFrequencies,
                   const juce::String& statusText);

    void setResolution (dsp::RtaAnalyser::Resolution resolution);
    dsp::RtaAnalyser::Resolution getResolution() const { return resolution; }

    void setRange (float lowFrequency, float highFrequency);
    float getRangeLow() const { return rangeLow; }
    float getRangeHigh() const { return rangeHigh; }

    void setShowPeakHold (bool shouldShow) { showPeakHold = shouldShow; repaint(); }
    void setShowMinMax (bool shouldShow) { showMinMax = shouldShow; repaint(); }
    void setShowBandValues (bool shouldShow) { showBandValues = shouldShow; repaint(); }

    /** Raised when a setting the worker needs changes. */
    std::function<void()> onSettingsChanged;
    std::function<void()> onClearPeakHold;

private:
    juce::Rectangle<float> plotArea() const;
    float frequencyToX (float frequency, const juce::Rectangle<float>& bounds) const;
    float xToFrequency (float x, const juce::Rectangle<float>& bounds) const;
    float levelToY (float db, const juce::Rectangle<float>& bounds) const;

    void drawGrid (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawBands (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawBandLabels (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawCursor (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void updateReadout();

    int nearestBand (float frequency) const;

    juce::ComboBox resolutionSelector;
    juce::ComboBox rangeSelector;
    juce::TextButton peakHoldButton { "Peak Hold" };
    juce::TextButton minMaxButton { "Min/Max" };
    juce::TextButton valuesButton { "Nilai" };
    juce::TextButton clearHoldButton { "Reset Peak" };
    juce::Label cursorLabel;
    juce::Label infoLabel;

    std::vector<dsp::RtaAnalyser::Band> bands;
    std::vector<float> bandFrequencies;
    juce::String statusLine;

    dsp::RtaAnalyser::Resolution resolution = dsp::RtaAnalyser::Resolution::ThirdOctave;
    float rangeLow = 20.0f;
    float rangeHigh = 20000.0f;

    float topDb = 24.0f;
    float bottomDb = -48.0f;

    bool showPeakHold = true;
    bool showMinMax = false;
    bool showBandValues = false;

    bool cursorVisible = false;
    float cursorX = 0.0f;
    juce::Point<float> cursorPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RtaDisplay)
};
