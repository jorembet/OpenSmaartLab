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
