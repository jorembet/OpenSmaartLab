#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "SpectrumAnalyser.h"

/** Frequency domain view of the captured input: magnitude against a logarithmic
    frequency axis, with the cursor reporting the frequency and the magnitude under it.
*/
class SpectrumAnalyserDisplay : public juce::Component
{
public:
    SpectrumAnalyserDisplay();
    ~SpectrumAnalyserDisplay() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

    void setFrame (const dsp::SpectrumAnalyser::Frame& frame);
    /** Takes the curve straight out of a published measurement, which is how the live
        display is fed: the worker owns the analyser, the display only draws. */
    void setSpectrum (const std::vector<float>& frequencies,
                      const std::vector<float>& levelsDb,
                      const juce::String& status);
    void setStatus (const juce::String& text);

    void setFftSize (int fftSize);
    int getFftSize() const { return (int) fftSizeSelector.getSelectedId(); }

    void setWindowType (dsp::SpectrumAnalyser::Window window);
    dsp::SpectrumAnalyser::Window getWindowType() const { return windowType; }

    void setAveragingMode (dsp::SpectrumAnalyser::Averaging mode);
    dsp::SpectrumAnalyser::Averaging getAveragingMode() const { return averagingMode; }

    void setAveragingSeconds (float seconds);
    float getAveragingSeconds() const { return (float) averagingSlider.getValue(); }

    void setOverlapPercent (float percent);
    float getOverlapPercent() const { return overlapSlider.getValue(); }

    /** Raised when a setting the worker needs changes, so the owner forwards it instead
        of the two sides having to know about each other. */
    std::function<void()> onFftSizeChanged;
    std::function<void()> onWindowChanged;
    std::function<void()> onAveragingChanged;

private:
    juce::Rectangle<float> plotArea() const;
    float frequencyToX (float frequency, const juce::Rectangle<float>& bounds) const;
    float xToFrequency (float x, const juce::Rectangle<float>& bounds) const;
    float levelToY (float db, const juce::Rectangle<float>& bounds) const;
    void drawGrid (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawCurve (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawCursor (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawFrequencyLabels (juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void updateReadout();

    juce::ComboBox fftSizeSelector;
    juce::ComboBox windowSelector;
    juce::ComboBox averagingSelector;
    juce::Slider averagingSlider;
    juce::Slider overlapSlider;
    juce::Label cursorLabel;
    juce::Label peakLabel;

    std::vector<float> frequency;
    std::vector<float> levelDb;

    dsp::SpectrumAnalyser::Window windowType = dsp::SpectrumAnalyser::Window::Hann;
    dsp::SpectrumAnalyser::Averaging averagingMode = dsp::SpectrumAnalyser::Averaging::Exponential;

    float topDb = 20.0f;
    float bottomDb = -100.0f;

    bool cursorVisible = false;
    float cursorX = 0.0f;
    juce::Point<float> cursorPosition;
    juce::String statusLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyserDisplay)
};
