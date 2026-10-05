#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "SpectrumAnalyser.h"

namespace dsp
{
    /** Real time analyser: an octave band readout built from a spectrum.

        The reading for a band is integrated across every bin the band covers, not taken
        from the bin nearest its centre. A single bin is not a band level: at 1/12 octave
        a band is a fraction of a bin wide at the top of the range and several bins wide at
        the bottom, so one bin either misses the band entirely or reports a level that
        depends on where a tone happened to fall. Integrating the power and taking the
        tenth root is the same measurement a sound level meter makes over a band, and it
        is what makes the same tone read the same level whatever FFT size is selected.

        Band centres follow the IEC 266 preferred series through dsp::octaveBandFrequencies,
        so the labels on screen are the nominal values a measurement is quoted in.
    */
    class RtaAnalyser
    {
    public:
        /** One band's readings. Every field describes the same band of the same frame. */
        struct Band
        {
            float frequency = 0.0f;
            /** Band level after averaging and smoothing, in dB. */
            float levelDb = -200.0f;
            /** Highest level since the hold was last cleared, decayed at the set rate. */
            float peakHoldDb = -200.0f;
            /** Lowest and highest level inside the tracking window. */
            float minDb = -200.0f;
            float maxDb = -200.0f;
            /** Bins the band covers. A band narrower than one bin is reported as
                unresolved rather than being given a number that depends on rounding. */
            int binCount = 0;
            bool resolved = false;
        };

    enum class Resolution
    {
        OneOctave = 1,
        ThirdOctave = 3,
        SixthOctave = 6,
        TwelfthOctave = 12,
        TwentyFourthOctave = 24
    };

    /** How the bins inside a band are turned into one number.

        The two are genuinely different measurements and neither is wrong, which is why
        real analysers offer both. Pink noise is what separates them: its energy per hertz
        is flat, so the energy in a band grows with the band's width, and a 1/3 octave table
        over 20 Hz to 20 kHz has bands a thousand times wider at the top than at the bottom.
    */
    enum class Integration
    {
        /** Energy summed across the band. This is the acoustic band level: a tone in the
            band reads its own amplitude, and pink noise shows the staircase it really is,
            rising about 10 dB per octave. */
        Sum = 0,

        /** Bins averaged across the band. This is what a plot of a spectrum looks like
            when it is sampled at band centres: pink noise reads flat, which makes it easy
            to read, but a tone spread over several bins reads below its own amplitude. */
        Average
    };

        RtaAnalyser();
        ~RtaAnalyser() = default;

        /** Allocates every buffer and builds the band table for the settings. */
        void prepare (double newSampleRate, int newFftSize);

        void setResolution (Resolution newResolution);
        Resolution getResolution() const { return resolution; }

        /** Lowest and highest band centre. Bands outside the range are not reported. */
        void setFrequencyRange (float lowFrequency, float highFrequency);
        float getRangeLow() const { return rangeLow; }
        float getRangeHigh() const { return rangeHigh; }

        void setAveraging (SpectrumAnalyser::Averaging mode, float timeConstantSeconds);

        /** How much faster a rise in level is followed than a fall. See
            SpectrumAnalyser::setAttackRatio for why the average is not symmetric. */
        void setAttackRatio (float ratio)
        {
            attackRatio = juce::jlimit (1.0f, 32.0f, ratio);

            // Recomputed here rather than only in setAveraging, so the ratio and the coefficient
            // cannot end up out of step whichever order the caller sets them in.
            attackAlpha = juce::jmin (1.0f, averagingAlpha * attackRatio);
        }
        float getAttackRatio() const noexcept { return attackRatio; }

        void setIntegration (Integration newIntegration);
        Integration getIntegration() const { return integration; }

        /** One pole smoothing applied after averaging, in seconds. Zero disables it. */
        void setSmoothingSeconds (float seconds);

        void setPeakHoldSeconds (float seconds);
        void setPeakDecayDbPerSecond (float dbPerSecond);

        /** How long the minimum and maximum keep tracking before they are released. */
        void setMinMaxWindowSeconds (float seconds);

        /** Integrates one spectrum and updates every band. */
        void process (const SpectrumAnalyser::Frame& frame);

        void clearPeakHold();
        void reset();

        const std::vector<Band>& getBands() const { return bands; }
        const std::vector<float>& getBandFrequencies() const { return bandFrequencies; }

        /** Lowest centre frequency the current settings produce. */
        float getLowestBandFrequency() const;
        float getHighestBandFrequency() const;

        int getBandCount() const { return (int) bands.size(); }

        /** Bands reported as too narrow for the selected FFT size. */
        int getUnresolvedBandCount() const;

        /** True when every band is resolved, which is what makes the readout trustworthy. */
        bool isFullyResolved() const { return getUnresolvedBandCount() == 0; }

        /** Seconds one frame represents, which is what every time constant is relative to. */
        double getFrameSeconds() const { return frameSeconds; }

        static juce::StringArray getResolutionNames();
        static juce::String resolutionToString (Resolution resolution);

    private:
        void rebuildBands();
        void updateHolds();
        static float bandEdgeLow (const std::vector<float>& centres, size_t index);
        static float bandEdgeHigh (const std::vector<float>& centres, size_t index);

        double sampleRate = 48000.0;
        int fftSize = 2048;
        double frameSeconds = 0.0;

        Resolution resolution = Resolution::ThirdOctave;
        float rangeLow = 20.0f;
        float rangeHigh = 20000.0f;

        SpectrumAnalyser::Averaging averagingMode = SpectrumAnalyser::Averaging::Exponential;
        float averagingTimeConstant = 0.5f;
        float averagingAlpha = 1.0f;
        float attackRatio = 4.0f;
        float attackAlpha = 1.0f;

        Integration integration = Integration::Sum;

        float smoothingSeconds = 0.0f;
        float smoothingAlpha = 1.0f;

        float peakHoldSeconds = 1.0f;
        float peakDecayDbPerSecond = 20.0f;
        float minMaxWindowSeconds = 5.0f;

        std::vector<float> bandFrequencies;
        std::vector<Band> bands;

        /** Running values that are not part of what the display shows directly. */
        std::vector<float> averagedDb;
        std::vector<float> smoothedDb;
        std::vector<double> samplesSinceHold;
        std::vector<double> samplesSinceMinMaxReset;

        bool primed = false;
        int framesProcessed = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RtaAnalyser)
    };
}
