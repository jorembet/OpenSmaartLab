#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include "DSP.h"

class GeneratorDisplay : public juce::Component
{
public:
    GeneratorDisplay();
    ~GeneratorDisplay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setRunning(bool shouldRun);
    bool isRunning() const { return running; }

    void setSignal(const juce::String& type, float levelDb, float sweepProgress,
                   const juce::String& outputDevice);
    void setSamples(const std::vector<float>& samples, float sampleRate);

    /** Band limits, tone frequency and sweep range currently set on the generator, so
        the spectrum plot follows the settings instead of a fixed 20 Hz - 20 kHz axis. */
    void setGeneratorSettings(float bandLow, float bandHigh, float toneFrequency,
                              float sweepStart, float sweepEnd);

    /** Drops the captured signal, so a stopped generator cannot leave an old spectrum
        on screen next to newly changed settings. */
    void clear();

    /** Tilt per octave as printed in the header, averaged over the captured blocks.
        Public because it is a figure the user reads, so the checks can confirm it
        describes the signal being generated. */
    float octaveSlope() const { return slopeValue; }

    /** Third octave averaged level at a frequency, the curve the tilt is measured on. */
    float probeLevelDb(float frequency) const;

    /** FFT window size required to resolve the currently selected low frequency. */
    int getRequiredSamples() const { return analysisSize; }

private:
    struct Spectrum
    {
        std::vector<float> freq;
        std::vector<float> magnitudeDb;
        float sampleRate = 48000.0f;
        bool valid = false;
    };

    void analyse();
    void updateAnalysisSize();
    void prepareAnalysisSize(int size);
    float measureSlope() const;
    void updateSlope();
    juce::Rectangle<float> waveformArea() const;
    juce::Rectangle<float> spectrumArea() const;
    void drawHeader(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawWaveform(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawSpectrum(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    dsp::FrequencyRange viewRange() const;
    juce::String settingsLabel() const;

    std::vector<float> samples;
    Spectrum spectrum;
    float sampleRate = 48000.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float slopeValue = 0.0f;
    int slopeFrames = 0;

    juce::String type = "Pink Noise";
    juce::String outputDevice;
    float levelDb = -24.0f;
    float sweepProgress = 0.0f;
    bool running = false;

    float bandLow = 20.0f;
    float bandHigh = 20000.0f;
    float toneFrequency = 1000.0f;
    float sweepStart = 20.0f;
    float sweepEnd = 20000.0f;

    int analysisSize = 4096;
    juce::dsp::FFT fft { 12 };
    juce::dsp::WindowingFunction<float> window { 4096, juce::dsp::WindowingFunction<float>::hann, false };
    mutable std::vector<float> scratch;
    mutable std::vector<float> slopeCurve;
    float coherentGain = 0.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorDisplay)
};
