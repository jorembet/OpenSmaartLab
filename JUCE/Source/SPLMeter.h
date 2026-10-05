#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <functional>
#include <vector>
#include "DSP.h"
#include "MicrophoneCalibration.h"
#include "SplAnalyser.h"

class SPLMeter : public juce::Component
{
public:
    SPLMeter();
    ~SPLMeter() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void prepare(float sampleRate, int blockSize);
    void process (const float* ref, const float* meas, int numSamples,
                  float inputGainDb = 0.0f);
    void reset();

    char getWeighting() const { return weighting; }
    bool isFastResponse() const { return fastResponse; }

    /** The correction added to a weighted level to turn it into SPL.

        Only meaningful once a calibration exists, which is why it is set from a profile rather
        than dialled in: an offset nobody measured is not a calibration. */
    float getCalibration() const { return calibrationOffsetDb; }

    /** Supplies the profile. A profile that has not been checked against a reference leaves the
        meter reporting dBFS and saying so, rather than reporting a number in SPL that nothing
        is known about. */
    void setCalibration (const MicrophoneCalibration& profile);

    /** True when the microphone reading can be shown as calibrated dB SPL. */
    bool isCalibrated() const { return calibrated; }

    /** The equivalent levels: LAeq, LCeq and LZeq, all carried at once. */
    float getLeq (char weightingChar) const;
    float getLeq (dsp::SplAnalyser::Weighting weighting) const;

    /** Seconds integrated into the equivalent level. */
    double getLeqDuration() const;

    /** Starts a new averaging period. */
    void resetLeq();

    static char weightingFromName (const juce::String& name);
    static juce::String weightingName (char weightingChar);

    float getLevel(int channel) const;
    float getPeak(int channel) const;
    float getLeq(int channel) const;
    float getMinimum(int channel) const;
    float getMaximum(int channel) const;
    int getClipCount(int channel) const;
    bool isClipActive (int channel) const;
    float getSamplePeakDbfs (int channel) const;
    float getClipPeakDbfs (int channel) const;

    /** Current A/C/Z-weighted microphone level before SPL calibration and before software trim. */
    float getMeasuredMicLevelDbfs() const;

    /** Called when the user asks to calibrate against an acoustic calibrator. */
    std::function<void()> onCalibrationRequested;

private:
    static dsp::SplAnalyser::Weighting ratingOf (char weightingChar);

    struct ChannelState
    {
        float level = dsp::dbFloor;
        float fast = dsp::dbFloor;
        float slow = dsp::dbFloor;
        float peak = dsp::dbFloor;
        float leq = dsp::dbFloor;
        float minimum = dsp::dbFloor;
        float maximum = dsp::dbFloor;
        double fastEnergy = 0.0;
        double slowEnergy = 0.0;
        double leqEnergy = 0.0;
        int leqBlocks = 0;
        int clipCount = 0;
        int clipHoldSamplesRemaining = 0;
        int clipReleaseSamplesRemaining = 0;
        int nearFullScaleRun = 0;
        int nearFullScalePolarity = 0;
        float nearFullScaleSample = 0.0f;
        float samplePeakDbfs = -200.0f;
        float clipWindowPeakDbfs = -200.0f;
        bool started = false;
        bool clipLatched = false;
    };

    static bool hasHardClip (const float* samples, int count, ChannelState& state);

    void updateChannel(ChannelState& state, float levelDb, float peakDb, float alpha);
    void drawChannelMeter(juce::Graphics& g, const juce::Rectangle<float>& area,
                          int channel, const juce::String& title);
    void drawHistory(juce::Graphics& g, const juce::Rectangle<float>& area);

    juce::ComboBox weightingSelector;
    juce::ComboBox responseSelector;
    juce::TextButton calibrationButton { "Kalibrasi Mic" };
    juce::TextButton resetButton { "Reset" };
    juce::Label calibrationLabel;
    juce::Label infoLabel;

    char weighting = 'A';
    bool fastResponse = true;

    /** Correction from the active profile, in dB, and whether it may be believed.

        Kept as one value with one flag rather than as a single offset, because the failure that
        matters is a plausible looking SPL reading taken from a calibration that was never
        measured against anything, and that has to be impossible to display. */
    float calibrationOffsetDb = 0.0f;
    float lastInputGainDb = 0.0f;
    bool calibrated = false;

    /** The engine, one per channel, so LAeq, LCeq and LZeq all exist at the same time. */
    dsp::SplAnalyser analysers[2];

    MicrophoneCalibration microphoneCalibration;

    float currentSampleRate = 48000.0f;
    int analysisBlock = 4096;
    int pendingSamples = 0;

    std::vector<float> pendingRef;
    std::vector<float> pendingMeas;
    std::vector<float> pendingRawRef;
    std::vector<float> pendingRawMeas;

    ChannelState channels[2];

    std::vector<float> historyRef;
    std::vector<float> historyMeas;
    std::vector<float> historyPeakRef;
    std::vector<float> historyPeakMeas;
    std::vector<float> historyTime;
    // Pairs of (time, channel): channel 0 = Ref, 1 = Mic.
    std::vector<std::pair<float, int>> clipEvents;
    juce::CriticalSection historyLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SPLMeter)
};
