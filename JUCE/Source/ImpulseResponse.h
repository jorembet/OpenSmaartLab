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

    /** RT60 estimated separately in octave bands. A room that rings mostly at one
        frequency shows up as a tall bar there, which the broadband figure hides. */
    struct BandRt60 { float frequency = 0.0f; float rt60 = 0.0f; bool valid = false; };
    static std::vector<BandRt60> bandRt60(const float* ir, int numSamples, float sampleRate,
                                          const std::vector<float>& centres);

    /** Short-time magnitude of the impulse response, normalised per frame, as a
        waterfall. Times down the rows, bands across the columns. */
    struct Waterfall
    {
        std::vector<std::vector<float>> framesDb;
        std::vector<float> bandCentres;
        std::vector<float> times;
        bool valid = false;
    };
    static Waterfall waterfall(const float* ir, int numSamples, float sampleRate,
                               const std::vector<float>& centres, int frameSize = 2048,
                               int hopSize = 1024);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImpulseResponse)
};
