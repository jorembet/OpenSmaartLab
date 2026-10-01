#pragma once

#include <juce_core/juce_core.h>
#include "DSP.h"

class ImpulseResponse
{
public:
    struct Acoustics
    {
        float edt = 0.0f;
        float t20 = 0.0f;
        float t30 = 0.0f;
        float rt60 = 0.0f;
        float correlation = 0.0f;
        float c50 = 0.0f;
        float c80 = 0.0f;
        float d50 = 0.0f;
        float centreTime = 0.0f;
        float directArrivalMs = 0.0f;
        int directArrivalSample = 0;

        std::vector<float> time;
        std::vector<float> decayDb;
        std::vector<float> energyDb;
        bool valid = false;
    };

    struct DecayFit
    {
        float upperLevelDb = 0.0f;
        float lowerLevelDb = 0.0f;
        float startTime = 0.0f;
        float endTime = 0.0f;
        float slope = 0.0f;
        float correlation = 0.0f;
        bool valid = false;
    };

    static int findDirectArrival(const float* ir, int numSamples);
    static Acoustics analyse(const float* ir, int numSamples, float sampleRate);
    static DecayFit fitDecay(const std::vector<float>& time,
                             const std::vector<float>& decayDb,
                             float upperLevelDb, float lowerLevelDb);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImpulseResponse)
};
