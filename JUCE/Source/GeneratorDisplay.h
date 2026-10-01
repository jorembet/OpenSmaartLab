#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

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

private:
    struct Spectrum
    {
        std::vector<float> freq;
        std::vector<float> magnitudeDb;
        float sampleRate = 48000.0f;
        bool valid = false;
    };

    void analyse();
    juce::Rectangle<float> waveformArea() const;
    juce::Rectangle<float> spectrumArea() const;
    void drawHeader(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawWaveform(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawSpectrum(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    float octaveSlope() const;

    std::vector<float> samples;
    Spectrum spectrum;
    float sampleRate = 48000.0f;
    float peak = 0.0f;
    float rms = 0.0f;

    juce::String type = "Pink Noise";
    juce::String outputDevice;
    float levelDb = -24.0f;
    float sweepProgress = 0.0f;
    bool running = false;

    juce::dsp::FFT fft { 12 };
    juce::dsp::WindowingFunction<float> window { 4096, juce::dsp::WindowingFunction<float>::hann, false };
    mutable std::vector<float> scratch;
    float coherentGain = 0.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorDisplay)
};
