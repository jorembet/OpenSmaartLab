#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include "DSP.h"

class SignalGenerator
{
public:
    enum class Type { Pink, White, Sine, LogSweep };

    SignalGenerator();
    ~SignalGenerator();

    static juce::StringArray getTypeNames();
    static Type typeFromName(const juce::String& name);

    void prepare(double sampleRate);

    void setType(Type newType);
    Type getType() const { return type; }

    void setLevelDb(float newLevelDb);
    float getLevelDb() const { return levelDb; }

    void setFrequency(float newFrequency);
    float getFrequency() const { return frequency; }

    void setSweepRange(float startFrequency, float endFrequency, float durationSeconds);
    float getSweepStart() const { return sweepStart; }
    float getSweepEnd() const { return sweepEnd; }
    float getSweepDuration() const { return sweepDuration; }
    float getSweepProgress() const { return sweepProgress; }

    /** Band limits used for pink noise and for the default sweep range. */
    void setBandLimits (float lowFrequency, float highFrequency);
    float getBandLow() const { return bandLow; }
    float getBandHigh() const { return bandHigh; }

    struct BandPreset
    {
        juce::String name;
        float lowFrequency = 20.0f;
        float highFrequency = 20000.0f;
    };

    /** One band per driver, from subwoofer to tweeter.

        Room correction is done one driver at a time: coherence and level are only
        meaningful inside the band that the driver actually reproduces, so a preset per
        driver keeps that measurement honest instead of averaging a tweeter with a
        subwoofer.
    */
    static juce::Array<BandPreset> getBandPresets();

    /** Index of the preset these limits match, or -1 when the band is a custom one. */
    static int findBandPreset (float lowFrequency, float highFrequency);

    void setRunning(bool shouldRun);
    bool isRunning() const { return running; }

    void reset();

    void process(float* output, int numSamples, float levelScale = 1.0f);

private:
    void generatePink(std::vector<float>& buffer);

    Type type = Type::Pink;
    float levelDb = -24.0f;
    float amplitude = 0.063f;
    float frequency = 1000.0f;
    float sweepStart = 20.0f;
    float sweepEnd = 20000.0f;
    float sweepDuration = 10.0f;
    float sweepProgress = 0.0f;

    // Pink noise covers the audible band. Below 20 Hz most speakers barely move and
    // the power is wasted; above 20 kHz it is inaudible and only stresses the
    // converter.
    float pinkLowFrequency = 20.0f;
    float pinkHighFrequency = 20000.0f;
    float bandLow = 20.0f;
    float bandHigh = 20000.0f;

    bool running = false;
    bool prepared = false;

    double sampleRate = 48000.0;
    int sweepLength = 480000;
    int sweepPosition = 0;
    double phase = 0.0;
    float sweepPhase = 0.0f;

    std::vector<float> pinkBuffer;
    int pinkPosition = 0;

    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalGenerator)
};
