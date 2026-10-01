#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <vector>
#include "DSP.h"

class SPLMeter : public juce::Component
{
public:
    SPLMeter();
    ~SPLMeter() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void prepare(float sampleRate, int blockSize);
    void process(const float* ref, const float* meas, int numSamples);
    void reset();

    char getWeighting() const { return weighting; }
    bool isFastResponse() const { return fastResponse; }
    float getCalibration() const { return calibration; }
    void setCalibrationOffset (float offsetDb) { calibration = offsetDb; }

    float getLevel(int channel) const;
    float getPeak(int channel) const;
    float getLeq(int channel) const;
    float getMinimum(int channel) const;
    float getMaximum(int channel) const;

private:
    struct ChannelState
    {
        float level = 0.0f;
        float fast = 0.0f;
        float slow = 0.0f;
        float peak = 0.0f;
        float leq = 0.0f;
        float minimum = 0.0f;
        float maximum = -200.0f;
        double fastEnergy = 0.0;
        double slowEnergy = 0.0;
        double leqEnergy = 0.0;
        int leqBlocks = 0;
        bool started = false;
    };

    void updateChannel(ChannelState& state, float levelDb, float peakDb, float alpha);
    void drawChannelMeter(juce::Graphics& g, const juce::Rectangle<float>& area,
                          int channel, const juce::String& title);
    void drawHistory(juce::Graphics& g, const juce::Rectangle<float>& area);

    dsp::BlockAnalyser analyser;

    juce::ComboBox weightingSelector;
    juce::ComboBox responseSelector;
    juce::Slider calibrationSlider;
    juce::TextButton resetButton { "Reset" };
    juce::Label calibrationLabel;
    juce::Label infoLabel;

    char weighting = 'A';
    bool fastResponse = true;
    float calibration = 120.0f;

    float currentSampleRate = 48000.0f;
    int analysisBlock = 4096;
    int pendingSamples = 0;

    std::vector<float> pendingRef;
    std::vector<float> pendingMeas;

    ChannelState channels[2];

    std::vector<float> historyRef;
    std::vector<float> historyMeas;
    std::vector<float> historyTime;
    juce::CriticalSection historyLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SPLMeter)
};
