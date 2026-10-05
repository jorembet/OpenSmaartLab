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

        /** The same phase folded into -180 to +180 degrees, kept alongside the unwrapped
            form rather than replacing it. A cursor reading one frequency at a time wants the
            wrapped number, while the trend line and the delay estimator need the continuous
            one, and recomputing either from the other loses precision. */
        std::vector<float> phaseDegWrapped;

        /** The unwrapped phase with its curvature averaged away. A straight line is left
            where it was, so the delay read from this curve is the same as from the raw one
            while the noise on it is smaller. */
        std::vector<float> phaseDegSmoothed;
        std::vector<float> coherence;
        std::vector<float> refMagnitudeDb;
        std::vector<float> measMagnitudeDb;
        std::vector<float> impulseResponse;

        /** Where the impulse response actually peaks, in samples.

            The stored impulse is rotated so its peak lands at sample 0, which is what makes it
            readable on screen but throws away the one number it was measured for. Kept
            separately, so the arrival can be read without having to guess what the rotation
            did. This is the supporting method behind the delay: it is derived from the same
            alignment as the correlation, so it agrees with it and cannot contradict it, but it
            is a different view of the same alignment rather than an independent witness. */
        int impulsePeakIndex = 0;
        float impulsePeakValue = 0.0f;
        /** Broadband level of the measured block, in dBFS. The spectrum says where the
            energy sits, these say how much of it there is: a level that is falling while
            the curve looks flat is the spectrum being normalised, not a quiet system. */
        float measRmsDb = dsp::dbFloor;
        float measPeakDb = dsp::dbFloor;
        float refRmsDb = dsp::dbFloor;
        float refPeakDb = dsp::dbFloor;
        /** One entry per bin: false where the reference is too weak to measure. A blanked
            bin carries no magnitude and no coherence, so it must not be drawn or
            averaged in. */
        std::vector<char> binValid;
        int validBins = 0;
        int blankedBins = 0;
        float peakReferenceDb = dsp::dbFloor;
        float delaySamples = 0.0f;
        float delayMs = 0.0f;

        /** The same delay read from the slope of the phase instead of from the correlation.

            Two independent methods agreeing is worth more than either alone: cross
            correlation resolves a whole sample, but it can lock onto a reflection that
            happens to be louder, while the phase slope cannot be moved by a single arrival and
            only sees the direct path. When they disagree, the correlation is usually the one
            that found a reflection, and saying so is more useful than silently preferring one. */
        float phaseSlopeDelaySamples = 0.0f;
        float phaseSlopeDelayMs = 0.0f;
        float phaseSlopeDegPerHz = 0.0f;
        float phaseSlopeScatterDeg = 0.0f;
        int phaseSlopeBins = 0;
        bool phaseSlopeValid = false;

        /** True when the two methods land within a tenth of a millisecond of each other. */
        bool delayMethodsAgree = false;

        /** Total path length for the measured delay, in metres, at the current temperature.
            One way travel added to the way back, so it describes a path and not a distance to
            a surface. Zero while the delay is unknown. */
        float delayDistanceM = 0.0f;

        /** Air temperature assumed when turning the delay into a distance. */
        float temperatureC = 20.0f;

        /** Speed of sound the distance was worked out with, in metres per second. */
        float speedOfSound = 343.0f;
        /** True when the last delay search found a peak it could vouch for.

            A correlation peak on its own is not a measurement: on a weak direct sound the
            correlation is a broad smear whose strongest point can sit anywhere, including at a
            lag no physical path could produce. Magnitude and coherence would not notice,
            because neither looks at phase. Carried per frame so a reader is told the delay is
            unproven rather than shown a confident length. */
        bool delayTrusted = false;
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

    /** Half width, in bins, of the moving average applied to the unwrapped phase.

        Zero leaves the phase as measured, which is the right choice when the phase is being
        read for a delay rather than looked at: averaging cannot sharpen the estimate that
        matters, it only makes the curve easier to follow. */
    void setPhaseSmoothingBins(int halfWidth) { phaseSmoothingBins = juce::jlimit (0, 32, halfWidth); }
    int getPhaseSmoothingBins() const noexcept { return phaseSmoothingBins; }

    /** Air temperature used to turn a delay into a distance.

        This changes the reported distance only. The delay itself is measured in samples and
        knows nothing about the air, so entering the wrong temperature cannot corrupt the
        measurement. */
    void setTemperature(float celsius) { temperatureC = juce::jlimit (-40.0f, 60.0f, celsius); }
    float getTemperature() const noexcept { return temperatureC; }

    void setDelayCompensation(bool shouldApply);
    bool isDelayCompensationEnabled() const { return delayCompensationEnabled; }

    void setAutomaticDelay(bool shouldTrack, float maxLagMs = 500.0f);
    bool isAutomaticDelayEnabled() const { return automaticDelayEnabled; }

    /** How well the aligned pair has to look like the same signal before its peak is
        believed.

        A correlation peak is only as good as the peak-to-noise of the correlation. When the
        direct sound is weak, which is what a microphone hears from a headphone driver or
        across a room, the correlation is a broad smear and its strongest point can sit
        anywhere, including at a lag no physical path could produce. Magnitude and coherence
        do not notice: both ignore phase, so they keep reporting a perfect measurement while
        the phase is rotated by a delay that was never there. */
    void setDelayConfidenceThreshold(float threshold) { delayConfidenceThreshold = juce::jlimit (0.0f, 1.0f, threshold); }
    /** False while the last search found a peak it could not vouch for. The compensation
        then holds the last delay it trusted rather than chasing noise. */
    bool isDelayTrusted() const noexcept { return delayTrusted; }

    /** How far below the strongest reference bin a bin may sit and still be measured.

        A band limited signal leaves most of the spectrum empty, and the coherence there
        is the ratio of two near-zero powers: undefined, and reported as a confident 1.0
        if it is divided out anyway. Everything below this range is blanked instead.
    */
    void setBlankingRangeDb(float rangeDb);
    float getBlankingRangeDb() const { return blankingRangeDb; }

    /** How far below the reference a bin's measurement may sit and still be drawn, in dB.

        The magnitude pane reaches down to about -36 dB, so a bin that far below the
        reference is off the bottom of the plot, and its phase is then noise dressed up as a
        measurement. Anything shallower than this stays visible: a crossover or a deep notch
        is a real part of the response. */
    void setMeasurementFloorDb(float floorDb) { measurementFloorDb = juce::jlimit (-200.0f, 0.0f, floorDb); }
    float getMeasurementFloorDb() const { return measurementFloorDb; }


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

    /** The search cannot see past half the analysed block. A reading at that limit means
        the peak was not found, so it must not be reported as a measured delay. */
    float getMaxReachableDelayMs() const noexcept
    {
        return std::min (maxLagMs, 1000.0f * (float) (analyser.getFftSize() / 2) / sampleRate);
    }

private:
    /** Runs the search and stores the result. Compensation and averaging are handled by
        the caller: the button restarts the average, the automatic path only does so when
        the delay really moved. */
    void trackDelay(const float* ref, const float* meas);

    void buildImpulseResponse(Result& result);

    dsp::BlockAnalyser analyser;
    dsp::DelayFinder delayFinder;
    juce::dsp::FFT inverseTransform { 14 };

    float sampleRate = 48000.0f;
    int fftSize = 16384;
    int averages = 8;
    int frameCounter = 0;
    int phaseSmoothingBins = 2;
    float temperatureC = 20.0f;

    bool delayCompensationEnabled = true;
    bool automaticDelayEnabled = true;
    float maxLagMs = 500.0f;

    float delaySamples = 0.0f;
    float delayMs = 0.0f;
    /** How far the delay may move before the averages are thrown away, in samples. Four
        samples is 83 us at 48 kHz: below that the frame is still the same measurement. */
    float delayJitterSamples = 4.0f;
    float compensatedDelaySamples = 0.0f;
    float delayConfidenceThreshold = 0.2f;
    bool delayTrusted = false;
    float smoothingAlpha = 1.0f;
    float blankingRangeDb = 60.0f;
    float measurementFloorDb = -60.0f;

    dsp::Spectrum refSpectrum;
    dsp::Spectrum measSpectrum;

    std::vector<float> averagedRefPower;
    std::vector<float> averagedMeasPower;
    std::vector<dsp::Complex> averagedCross;

    /** The same cross spectrum accumulated without the delay rotation.

        Compensation rotates the phase of every bin back to zero lag, which is what makes the
        displayed phase readable, but it also means the compensated phase has no slope left in
        it and cannot be used to measure the delay. Keeping the unrotated average is what lets
        the phase slope be a genuinely separate opinion on the delay rather than a restatement
        of the correlation, since it never sees the delay the correlation reported. */
    std::vector<dsp::Complex> averagedCrossRaw;
    bool initialised = false;
    bool averagesInvalid = true;

    std::vector<float> impulseScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransferFunction)
};
