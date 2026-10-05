#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "DSP.h"

namespace dsp
{
    /** Measures distortion in a periodic signal, by separating it from its own harmonics.

        The whole question is where a harmonic's amplitude comes from, and the obvious answer
        is wrong in three separate ways. Reading the bin nearest two times the fundamental picks
        up whatever is nearest rather than whatever is there; reading one bin also suffers
        scalloping, where a tone a fraction of a bin off centre loses up to several decibels to
        the window and a distortion figure then moves around as the frequency drifts; and
        adding amplitudes instead of powers understates a pair of harmonics that arrive together.

        So each harmonic is measured by integrating the power across the window's main lobe,
        which is what a Hann window is for, and the harmonics are then combined as powers.

        Nothing here needs the signal to be periodic in advance. The fundamental is found, and
        everything else is placed relative to it.
    */
    class DistortionAnalyser
    {
    public:
        struct Settings
        {
            /** Where to look for the fundamental. */
            float searchLowHz = 20.0f;
            float searchHighHz = 20000.0f;

            /** Highest harmonic order reported. Ten covers the point where a signal is
                dominated by artefacts rather than by the distortion of the thing being tested. */
            int maxHarmonic = 10;

            /** Bins either side of a harmonic's peak that are taken as part of it.

                A Hann window puts a tone into three bins, so three either side collects the
                main lobe whole. Wider would start swallowing the noise floor into the
                harmonic and reporting it as distortion that is not there. */
            int halfWidthBins = 2;

            /** Blocks averaged together. A single block of a noisy signal reports a noise
                floor that moves block to block, which reads as a changing distortion. */
            int averaging = 1;
        };

        struct Harmonic
        {
            int order = 0;
            float frequency = 0.0f;
            /** Peak amplitude, linear. */
            float amplitude = 0.0f;
            /** This harmonic's own level in dB re full scale. */
            float levelDb = 0.0f;
            /** Relative to the fundamental, in dB. A negative number is the usual sign. */
            float relativeDb = 0.0f;
            bool valid = false;
        };

        struct Result
        {
            float fundamentalHz = 0.0f;
            float fundamentalLevelDb = 0.0f;
            /** H1 first, then the harmonics in order. */
            std::vector<Harmonic> harmonics;

            /** Total harmonic distortion, as a percentage and in decibels. */
            float thdPercent = 0.0f;
            float thdDb = 0.0f;

            /** Everything that is neither the fundamental nor a harmonic. */
            float noiseLevelDb = 0.0f;

            /** Total harmonic distortion plus noise. The figure that says how much of a
                signal is not the thing being measured, as opposed to how much of it is
                non-linearity. */
            float thdPlusNPercent = 0.0f;
            float thdPlusNDb = 0.0f;

            /** Signal to noise and distortion, in decibels. */
            float sinadDb = 0.0f;

            /** Fraction of the spectrum the harmonic bands actually covered, so a caller can
                tell a measured noise floor from an almost entirely unmeasured one. */
            double analysedFraction = 0.0;

            int numBins = 0;
            bool valid = false;
        };

        DistortionAnalyser();

        void prepare (double sampleRate, int fftSize);

        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        /** Measures one block. Allocation free once prepared. */
        Result analyse (const float* samples, int numSamples) const;

        /** The total harmonic distortion a set of harmonic powers implies, as a ratio.

            Exposed on its own because it is the arithmetic the whole class turns on, and it is
            worth being able to check against a hand calculation directly: the powers in, one
            ratio out. */
        static float thdRatio (const std::vector<double>& harmonicPowers,
                               double fundamentalPower);

        static juce::String percentToString (float percent);
        static juce::String levelToString (float db);

    private:
        /** Power in the bins around a centre, in the same units the total is summed in.

            The two-sided factor is applied to the interior bins exactly as the shared level
            calculation does it, so a band measured here and a level measured elsewhere are on
            one scale and can be compared without a fudge factor. */
        double bandPower (const dsp::Spectrum& spectrum, int centreBin) const;

        /** The peak bin within a small window, refined to a fraction of a bin.

            The refinement matters more than it looks: a harmonic a tenth of a bin off centre
            still puts most of its energy inside three bins, but one placed a whole bin out by
            mistake would integrate the wrong bins entirely. */
        int findPeakNear (const dsp::Spectrum& spectrum, double centreBin,
                          double searchRadius) const;

        /** Converts linear power to a peak amplitude, which is what a harmonic table shows. */
        static float powerToAmplitude (double power)
        {
            return power <= 0.0 ? 0.0f : (float) std::sqrt (2.0 * power);
        }

        Settings settings;
        dsp::BlockAnalyser analyser;
        /** Written by the const analyse, because the transform is const and fills a
            caller supplied spectrum. Mutable for that reason and no other. */
        mutable dsp::Spectrum spectrum;
        mutable std::vector<double> harmonicPowers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DistortionAnalyser)
    };
}