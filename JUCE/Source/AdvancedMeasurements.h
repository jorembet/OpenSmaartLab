#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "DSP.h"

namespace dsp
{
    /** Intermodulation distortion from a two tone test.

        Two tones put through anything non-linear do not stay two tones. Every product of the
        two frequencies appears, and the size of each one says how the non-linearity behaves: a
        second order product means the amplifier's compression bends symmetrically, a third
        order one means it does not.

        The tones are placed at a ratio that keeps the products clear of the tones themselves,
        because a product that lands on top of the tone producing it cannot be measured at all.
        That is the whole reason SMPTE uses three halves rather than two.
    */
    class ImdAnalyser
    {
    public:
        enum class Standard
        {
            /** Second and third order products from a ratio of 3:2.

                With f2 = 1.5 f1 the products land at 0.5 f1, 2 f1 and 2.5 f1, none of which
                coincides with a drive tone, so each one can be measured on its own. */
            Smpte,

            /** Third order products from a ratio of 2:1.

                The older twin tone test. Its products land exactly on the harmonics of the
                lower tone, so this cannot tell a harmonic from the product that shares its
                frequency. It is offered because the figures exist for it and readers ask, not
                because it separates anything. */
            Ccif
        };

        static juce::StringArray getStandardNames();
        static juce::String standardName (Standard standard);

        /** The two drive frequencies for a standard, given the lower one.

            CCIF keeps its high tone an octave above so a band limited signal still reaches
            both; SMPTE's ratio of three halves is fixed by the standard. */
        static void driveFrequencies (Standard standard, float lowHz, float& firstHz, float& secondHz);

        struct Product
        {
            /** The expression that produces this product, e.g. "2f1-f2". */
            juce::String expression;
            float frequency = 0.0f;
            float amplitude = 0.0f;
            /** Relative to the total drive power, in decibels. */
            float relativeDb = -200.0f;
        };

        struct Result
        {
            float firstHz = 0.0f;
            float secondHz = 0.0f;
            float firstAmplitude = 0.0f;
            float secondAmplitude = 0.0f;
            /** Intermodulation distortion as a percentage of the drive and in decibels. */
            float imdPercent = 0.0f;
            float imdDb = 0.0f;
            std::vector<Product> products;
            bool valid = false;
        };

        struct Settings
        {
            /** The lower drive tone. SMPTE fixes the ratio; CCIF uses this and doubles it. */
            float lowToneHz = 1000.0f;
            Standard standard = Standard::Smpte;
            /** Drives the tones used, so a caller can follow whatever the analyser is doing. */
            float amplitude = 0.25f;
        };

        ImdAnalyser();

        void prepare (double sampleRate, int fftSize);
        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        /** Not const: the transform writes into the meter, which is what holds the last
            analysed block. Nothing else in the class changes state. */
        Result analyse (const float* samples, int numSamples);

        /** IMD as a ratio of the products to the drive, from powers.

            Exposed on its own because it is the arithmetic the class turns on, and it is worth
            being able to hand it a pair of numbers and check the answer: the drive is a root sum
            of squares of two amplitudes, not one amplitude and not their sum. */
        static float imdRatio (double drivePower, const std::vector<double>& productPowers);

        static juce::String percentToString (float percent);
        static juce::String levelToString (float db);

    private:
        Settings settings;
        SpectralLineMeter meter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImdAnalyser)
    };

    /** Leakage from one channel of a stereo pair into the other.

        Measured by driving one channel and reading the other, which is the only arrangement
        that means anything: a number produced by driving both channels at once is measuring
        the sum of the leakage and the crosstalk of the chain, and would not distinguish a
        balanced input from a shorted one.

        A single tone is the right stimulus. Broadband noise would be ambiguous, because noise
        reaching the wrong channel could be either crosstalk or simply the noise floor of the
        converters, and the two are not worth telling apart at the levels a good channel
        achieves.
    */
    class CrosstalkAnalyser
    {
    public:
        struct Result
        {
            /** Leakage in decibels below the drive. Negative, and the number a reader is looking
                for: a good channel pair sits well below -60. */
            float leakageDb = 0.0f;
            float leakagePercent = 0.0f;

            /** What was measured on each side, so an implausible figure can be traced to a
                quiet drive rather than assumed to be good isolation. */
            float driveLevelDb = 0.0f;
            float receiveLevelDb = 0.0f;

            float frequency = 0.0f;
            bool valid = false;
        };

        struct Settings
        {
            /** A tone that is not a multiple of anything in the chain, so it cannot be mistaken
                for a clock or a switching artefact of its own. */
            float frequency = 997.0f;
            float amplitude = 0.5f;
            /** A receiving level at or below this is treated as no measurement rather than as
                an infinitely quiet channel. */
            float floorDb = -120.0f;
        };

        CrosstalkAnalyser();

        void prepare (double sampleRate, int fftSize);
        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        /** Leakage from one channel into the other, for a channel pair where one side is
            driven and the other is not.

            Both directions cannot come from one pair of channels, and this is the reason rather
            than an omission: with a single pair, driving the left makes the right carry only the
            leakage, and there is no way at the same time to know how much the right would
            itself have driven into the left. A pair measured both ways at once is not two
            readings either, because each channel then carries its own tone and the other's
            leakage, and the two sum.

            So the application drives one channel, reads this, then drives the other and reads
            it again. Each call is one direction, honestly obtained. */
        Result measure (const float* driven, const float* receiving, int numSamples);

        static juce::String levelToString (float db);

    private:
        Settings settings;
        SpectralLineMeter meter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrosstalkAnalyser)
    };

    /** Whether a measurement channel carries the reference signal the right way up.

        Answered by correlating the two and looking at the sign of the peak, because a polarity
        error leaves the level, the spectrum and the coherence all completely unchanged. Only
        something that compares the waveform against its reference can see it, and a system can
        measure well and still be wired backwards.

        The correlation is taken over a small range of lags rather than at zero only, since the
        measurement path has a delay in it and a peak a sample late is still a correctly wired
        channel.
    */
    class PolarityDetector
    {
    public:
        enum class Verdict
        {
            Normal,
            Inverted,
            /** Not enough in common between the two signals to call it. */
            Unknown
        };

        struct Report
        {
            Verdict verdict = Verdict::Unknown;
            /** Signed normalised correlation at the chosen lag, between minus one and one. */
            float correlation = 0.0f;
            int lagSamples = 0;
            /** What the correlation had to beat to be believed. */
            float threshold = 0.0f;
            bool confident = false;
        };

        struct Settings
        {
            /** Correlation below this is reported as no verdict at all.

                Two unrelated signals correlate to about nothing, and calling that a wiring
                fault would send someone looking for a polarity problem they do not have. */
            float threshold = 0.3f;
            /** Lags either side of zero to search, covering the loop and device latency. */
            int maxLagSamples = 64;
        };

        PolarityDetector();

        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        /** Compares a reference against a measurement. */
        Report detect (const float* reference, const float* measurement, int numSamples) const;

        static juce::String verdictToString (Verdict verdict);

    private:
        Settings settings;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolarityDetector)
    };
}