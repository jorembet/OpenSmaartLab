#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace dsp
{
    /** One parametric peaking filter, the shape every room EQ tool fits and exports. */
    struct PeakingFilter
    {
        float frequency = 1000.0f;
        float gainDb = 0.0f;
        float q = 1.41f;
        bool enabled = true;
    };

    /** A target to correct towards.

        Room correction needs a curve to aim at, and "flat" is rarely the right one: a
        measurement taken at the listening position includes the room, and an anechoic
        target will ask for a boost that sounds harsh in the room it is played in. The
        targets here are the shapes a measurement is normally quoted against.
    */
    struct TargetCurve
    {
        enum class Kind
        {
            /** 0 dB everywhere. The honest default for a measurement made with the
                microphone where the listener sits. */
            Flat = 0,

            /** A gentle top end roll off, which is the usual shape for a speaker measured
                in a room: it keeps a measurement from asking for treble a room cannot
                deliver. */
            FlatWithTilt,

            /** A house curve with a small presence lift around 2 kHz and a rolled off
                bass, matching what most people expect a corrected system to sound like. */
            Harman,

            /** A steeper tilt for a bright room, where the top end is the problem. */
            FlatWithStrongTilt
        };

        Kind kind = Kind::Flat;

        /** The target level in dB at a frequency. */
        float levelAt (float frequency) const;

        static juce::StringArray getKindNames();
        static juce::String kindToString (Kind kind);
    };

    /** Fits a set of peaking filters that move a measured response towards a target.

        The fit is deliberately simple and inspectable rather than clever: the largest
        remaining error is found, a filter is placed on it with the gain and Q that cancel
        that much of it, and the whole thing is repeated a fixed number of times. Each
        step is explainable in one sentence, which matters when the result is going into a
        loudspeaker: a correction nobody can account for is one nobody should apply.

        The result is a *starting point for measurement*, not a finished EQ. It is fitted
        from one measurement in one position, and a room changes when you move.
    */
    class EqDesigner
    {
    public:
        struct Settings
        {
            /** Filters to fit. Zero leaves the measurement alone. */
            int maxFilters = 8;
            /** Gain a single filter may apply, in dB. */
            float maxFilterGainDb = 12.0f;
            /** Frequency range the fit is allowed to work in. */
            float lowFrequency = 100.0f;
            float highFrequency = 12000.0f;
            /** Points used to compare measured against target. */
            int designPoints = 60;
            /** Error below this is treated as solved. */
            float toleranceDb = 0.3f;
        };

        struct Result
        {
            std::vector<PeakingFilter> filters;

            /** Largest error left after fitting, in dB. This is the number that says
                whether the fit succeeded, and it is reported rather than assumed. */
            float residualDb = 0.0f;
            /** RMS error after fitting, in dB. */
            float rmsResidualDb = 0.0f;
            /** Largest error before fitting, for comparison. */
            float initialDb = 0.0f;

            int pointsUsed = 0;
            bool valid = false;
        };

        EqDesigner();

        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        void setTarget (const TargetCurve& newTarget);
        const TargetCurve& getTarget() const { return target; }

        /** Fits filters to a measured response.

            frequencies and measuredDb must be the same length and in ascending frequency
            order, which is what the spectrum analyser produces. Bins outside the settings
            range, or flagged invalid, are skipped: a fit that chases a bin the reference
            never excited spends filters on noise.

            sampleRate matters because the fitted filters have to be expressible as real
            biquads at this rate; a fit produced for 48 kHz is wrong at 96 kHz. */
        Result design (const std::vector<float>& frequencies,
                       const std::vector<float>& measuredDb,
                       double sampleRate = 48000.0,
                       const std::vector<char>& validBins = {}) const;

        /** The combined magnitude response of a filter set, in dB, sampled on the design
            grid. Used to check a fit and to draw what the filters will actually do. */
        static std::vector<float> filterResponseDb (const std::vector<PeakingFilter>& filters,
                                                   const std::vector<float>& frequencies,
                                                   double sampleRate);

        /** The target sampled on the same grid, so measured and target can be compared
            point for point. */
        static std::vector<float> targetResponseDb (const TargetCurve& target,
                                                    const std::vector<float>& frequencies);

        /** True when a frequency is inside the range the fit is allowed to touch. */
        static bool isInDesignRange (float frequency, const Settings& settings);

        /** The design grid: logarithmically spaced over the settings range, because that
            is the axis a correction is judged on. */
        static std::vector<float> designGrid (const Settings& settings);

        static juce::StringArray getExportFormats();
        static juce::String exportToText (const std::vector<PeakingFilter>& filters,
                                          double sampleRate);
        static juce::String exportToJson (const std::vector<PeakingFilter>& filters,
                                           double sampleRate);
        static juce::String exportToRew (const std::vector<PeakingFilter>& filters);

    private:
        /** Magnitude of one peaking filter at a frequency, in dB.

            The standard biquad peaking response, evaluated analytically rather than by
            running audio through an IIR: the solver asks the same question a few thousand
            times, and an FFT convolution per question would make the fit unusably slow. */
        static float peakingResponseDb (const PeakingFilter& filter, float frequency,
                                        double sampleRate);

        /** Worst absolute error left on the usable grid points, in dB.

            This is the single measure the fit is judged on, so it lives in one place: a
            step that does not reduce it, or a reported residual that does not come from
            it, would be two different opinions about the same fit. */
        static float worstErrorDb (const std::vector<PeakingFilter>& filters,
                                   const std::vector<float>& grid,
                                   const std::vector<float>& measuredDb,
                                   const std::vector<float>& targetDb,
                                   const std::vector<int>& usable,
                                   double sampleRate);

        /** RMS error on the usable grid points, in dB.

            Reported alongside the worst error because a fit can have a small worst error
            and still be lumpy everywhere else. One number alone hides that. */
        static float rmsErrorDb (const std::vector<PeakingFilter>& filters,
                                 const std::vector<float>& grid,
                                 const std::vector<float>& measuredDb,
                                 const std::vector<float>& targetDb,
                                 const std::vector<int>& usable,
                                 double sampleRate);

        Settings settings;
        TargetCurve target;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqDesigner)
    };
}
