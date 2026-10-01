#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP.h"

class TransferFunction
{
public:
    struct Result
    {
        std::vector<float> freq;
        std::vector<float> magnitudeDb;
        std::vector<float> phaseDeg;
        std::vector<float> coherence;
        std::vector<float> refMagnitudeDb;
        std::vector<float> measMagnitudeDb;
        std::vector<float> impulseResponse;
        /** One entry per bin: false where the reference is too weak to measure. A blanked
            bin carries no magnitude and no coherence, so it must not be drawn or
            averaged in. */
        std::vector<char> binValid;
        int validBins = 0;
        int blankedBins = 0;
        float peakReferenceDb = dsp::dbFloor;
        float delaySamples = 0.0f;
        float delayMs = 0.0f;
        float averageCoherence = 0.0f;
        int fftSize = 0;
        float sampleRate = 48000.0f;
        int averages = 1;
        bool valid = false;
    };

    TransferFunction();
    ~TransferFunction();

    void prepare(float sampleRate, int fftSize);
    void reset();

    void setAveraging(int numAverages);
    int getAveraging() const { return averages; }

    void setDelayCompensation(bool shouldApply);
    bool isDelayCompensationEnabled() const { return delayCompensationEnabled; }

    void setAutomaticDelay(bool shouldTrack, float maxLagMs = 500.0f);
    bool isAutomaticDelayEnabled() const { return automaticDelayEnabled; }

    /** How far below the strongest reference bin a bin may sit and still be measured.

        A band limited signal leaves most of the spectrum empty, and the coherence there
        is the ratio of two near-zero powers: undefined, and reported as a confident 1.0
        if it is divided out anyway. Everything below this range is blanked instead.
    */
    void setBlankingRangeDb(float rangeDb);
    float getBlankingRangeDb() const { return blankingRangeDb; }

    /** Searches for the delay once, over the whole analysed block, and restarts the
        averages so every averaged frame shares one compensation. Returns the delay in
        samples, negative when the measurement arrives first. */
    float findDelay(const float* ref, const float* meas);

    void setManualDelay(float delaySamples);
    float getDelaySamples() const { return delaySamples; }
    float getDelayMs() const { return delayMs; }

    Result process(const float* ref, const float* meas, int numSamples);

    int getFftSize() const { return analyser.getFftSize(); }
    float getSampleRate() const { return sampleRate; }

private:
    void buildImpulseResponse(Result& result);

    dsp::BlockAnalyser analyser;
    dsp::DelayFinder delayFinder;
    juce::dsp::FFT inverseTransform { 14 };

    float sampleRate = 48000.0f;
    int fftSize = 16384;
    int averages = 8;
    int frameCounter = 0;

    bool delayCompensationEnabled = true;
    bool automaticDelayEnabled = true;
    float maxLagMs = 500.0f;

    float delaySamples = 0.0f;
    float delayMs = 0.0f;
    float smoothingAlpha = 1.0f;
    float blankingRangeDb = 60.0f;
    int delaySearchFrames = 0;

    dsp::Spectrum refSpectrum;
    dsp::Spectrum measSpectrum;

    std::vector<float> averagedRefPower;
    std::vector<float> averagedMeasPower;
    std::vector<dsp::Complex> averagedCross;
    bool initialised = false;

    std::vector<float> impulseScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransferFunction)
};
