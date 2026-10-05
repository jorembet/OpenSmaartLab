#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP.h"
#include "MicrophoneCalibration.h"
#include <array>
#include <cmath>
#include <vector>

namespace dsp
{
    /** Sound pressure level, with the frequency and time weighting a standard meter applies.

        Kept apart from the meter component so it can be measured on its own. A level reading is
        the one number in this application a reader is tempted to act on, so every part of it is
        a place where a plausible wrong answer is possible, and each is handled explicitly:

        - the frequency weighting is applied per FFT bin from the published A and C curves, not
          as a single broadband gain. A weighting is only right at one frequency if it is a
          number, and A and C differ from each other by nearly 40 dB in the middle of the band.
        - the time weighting is applied to energy, never to decibels. Averaging dB values is not
          the same as averaging sound energy, and the gap widens with the spread of the levels
          being averaged.
        - the equivalent level integrates energy over a period it reports, and can be reset so a
          new averaging period starts where the reader says it does.

        All three weightings are carried at once, so LAeq, LCeq and LZeq are all available
        without reconfiguring between readings.
    */
    class SplAnalyser
    {
    public:
        /** Frequency rating. Z is the zero rating, which is flat. */
        enum class Weighting { Z, A, C };

        /** Time weighting, with the exponential constants IEC 61672 names for each.

            Impulse is here because a peak reading wants the fastest response that still catches
            a transient, rather than the slow one a general level reading uses. */
        enum class TimeWeighting
        {
            Fast,     ///< 125 ms
            Slow,     ///< 1 s
            Impulse   ///< 35 ms
        };

        static constexpr int numWeightings = 3;

        /** Samples per weighting block.

            Short enough that the fastest time weighting still has several blocks to work with,
            and contiguous with no overlap so no sample is counted twice into the average. */
        static constexpr int blockSize = 1024;

        SplAnalyser();

        void prepare (double sampleRate);
        void reset();
        void resetPeak();

        /** Applies a microphone's frequency response to every reading.

            A measurement capsule is not flat, and its calibration file records how far off it is
            at each frequency. Carrying that as a per-bin correction rather than as one number
            is the difference between correcting a measurement and guessing at it: the error
            depends on which frequencies the sound occupied, so a single offset cannot stand in
            for a curve. */
        void setCalibration (const MicrophoneCalibration& profile);
        bool hasCalibrationCurve() const { return blockAnalyser.getBinCorrectionDb().size() > 0; }

        void setTimeWeighting (TimeWeighting newTimeWeighting);
        TimeWeighting getTimeWeighting() const { return timeWeighting; }

        /** Clears the equivalent level accumulators, starting a new averaging period here. */
        void resetLeq();

        /** Consumes audio. Allocation free once prepared, so it is safe to call from a
            arbitrary thread, though not from an audio callback: it transforms. */
        void process (const float* samples, int numSamples);

        /** The current time weighted level, in dB re full scale, before any calibration. */
        float getLevel (Weighting weighting) const;

        /** The equivalent level over the integration period, in dB. */
        float getLeq (Weighting weighting) const;

        /** The highest level seen since the last reset, in dB. */
        float getPeak (Weighting weighting) const;

        /** Seconds of audio actually integrated. Reported, not assumed: an equivalent level
            over a period shorter than the time constant has not settled, and comparing it with
            one that has would be comparing two different measurements. */
        double getLeqDuration() const { return (double) integratedSamples / sampleRate; }

        /** The number of weighting blocks that have gone into the average. */
        int getBlockCount() const { return blockCount; }

        /** The gain a frequency weighting applies at one frequency, in dB relative to its own
            value at 1 kHz.

            This is the part that can be checked against published tables, and checking it is
            the only way to know the curve was built from the right corner frequencies rather
            than merely sounding plausible. */
        static float weightingGainDbAt (Weighting weighting, float frequency);

        /** The time constant of a time weighting, in seconds. */
        static double timeConstant (TimeWeighting timeWeighting);

        static juce::StringArray getWeightingNames();
        static juce::StringArray getTimeWeightingNames();

    private:
        static int indexOf (Weighting weighting) noexcept
        {
            return weighting == Weighting::A ? 1 : (weighting == Weighting::C ? 2 : 0);
        }

        /** Folds one block into the three weightings and advances the readings. */
        void processBlock (const float* block) noexcept;

        double sampleRate = 48000.0;
        TimeWeighting timeWeighting = TimeWeighting::Fast;

        /** Per weighting: the smoothed energy of the display, the integrated energy of the
            average, and the highest level seen. */
        std::array<double, numWeightings> timeEnergy { { 0.0, 0.0, 0.0 } };
        std::array<double, numWeightings> leqEnergy { { 0.0, 0.0, 0.0 } };
        std::array<float, numWeightings> peakLevel { { -200.0f, -200.0f, -200.0f } };

        /** Smoothing coefficient for the selected time constant, in energy. */
        double alpha = 0.0;

        /** Scratch, sized in prepare so process never allocates. */
        std::array<float, blockSize> timeDomain { {} };
        int pending = 0;

        /** The transform and the weighting curves, both already in use and already checked
            elsewhere in the application.

            Reused rather than reimplemented on purpose. An FFT scale derived a second time is
            a second chance to be wrong by a constant factor, and a constant factor in a level
            reading is indistinguishable from a microphone with the wrong calibration. */
        dsp::BlockAnalyser blockAnalyser;

        std::int64_t integratedSamples = 0;
        int blockCount = 0;

        /** Blocks behind the equivalent level, which is what its average divides by. */
        std::int64_t leqBlocks = 0;
        bool started = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplAnalyser)
    };
}