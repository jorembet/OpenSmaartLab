#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include "DSP.h"

namespace dsp
{
    /** Reverberation time from an impulse response, by Schroeder backward integration.

        The method itself is the classical one and is not in dispute: the energy remaining in
        the tail is accumulated backwards, and the resulting curve is a straight line whose
        slope is the decay rate. What is in dispute, in every implementation of it, is how that
        line is turned into the numbers a reader acts on, and there are three ways to get it
        wrong that all produce a plausible figure:

        - fitting one bin, or reading a single point off the curve, which measures noise;
        - extrapolating the fitted range by the wrong factor. A range spanning twenty decibels
          reaches sixty decibels in three times its own duration, and treating the twenty
          decibel span as the answer understates T20 by exactly that;
        - reporting a time from a curve that never fell far enough to support it, which is a
          number with no measurement behind it.

        So the three times are fitted over three different ranges and each is extrapolated to
        sixty decibels by the same rule, and a band only reports a time when the decay actually
        reached far enough and the tail is not the noise floor.
    */
    class ReverbAnalyser
    {
    public:
        enum class Resolution { Octave, ThirdOctave };

        /** Whether a band's figure can be believed, and if not, why not.

            Stated rather than left as a missing number, because a blank cell and a real zero
            look the same on screen and a reader cannot tell a band that was not measured from
            one that measured nothing.
        */
        enum class Quality
        {
            Valid,
            /** The curve never fell far enough for the range that was fitted. */
            InsufficientDecay,
            /** The curve did fall, but its tail flattened: the impulse response ran into the
                noise floor, so the slope at the end is the floor and not the room. */
            Noisy
        };

        /** One band, with all four times over the same band. */
        struct Band
        {
            float frequency = 0.0f;
            float edt = 0.0f;
            float t20 = 0.0f;
            float t30 = 0.0f;
            float rt60 = 0.0f;
            float correlation = 0.0f;

            /** How far the decay actually fell, in decibels. Stated so a figure that looks
                short can be seen to be the consequence of a short impulse response rather than
                of a fast room. */
            float decayRangeDb = 0.0f;

            Quality quality = Quality::InsufficientDecay;
            bool valid = false;
        };

        struct Settings
        {
            Resolution resolution = Resolution::Octave;
            float lowFrequency = 125.0f;
            float highFrequency = 8000.0f;

            /** A fit below this correlation is reported as noisy rather than as a time.

                A Schroeder curve of a real room is very close to a straight line. One that is
                not has something else in the response, and the slope of a bent line is an
                average over the bend that describes nothing in particular. */
            float minCorrelation = 0.95f;

            /** How much shallower the tail may be than the fitted slope before it is called the
                noise floor. Three times is generous: a room's decay slows slightly as it goes,
                but it does not flatten. */
            float maxTailFlattening = 3.0f;

            /** Ignore this much of the head before the decay is considered to have started. */
            float directArrivalTolerance = 0.001f;
        };

        struct Result
        {
            std::vector<Band> bands;

            /** The same four times taken across the whole response rather than per band. */
            float edt = 0.0f;
            float t20 = 0.0f;
            float t30 = 0.0f;
            float rt60 = 0.0f;
            float correlation = 0.0f;
            float decayRangeDb = 0.0f;
            float directArrivalMs = 0.0f;

            Quality quality = Quality::InsufficientDecay;

            /** The broadband decay curve, kept so it can be drawn rather than only quoted. */
            std::vector<float> time;
            std::vector<float> decayDb;

            int bandsValid = 0;
            bool valid = false;
        };

        ReverbAnalyser();

        void setSettings (const Settings& newSettings);
        const Settings& getSettings() const { return settings; }

        /** Measures an impulse response. Allocation free once prepared. */
        Result analyse (const float* ir, int numSamples, double sampleRate);

        static juce::StringArray getResolutionNames();
        static juce::String qualityToString (Quality quality);

        /** The time for the curve to fall sixty decibels, given a slope in decibels a second.

            Every one of the four times comes through here, because they differ only in the range
            they were fitted over and this is where that difference is turned into an answer.
            Putting the rule in one place is what keeps the four from drifting apart. */
        static float extrapolatedToSixtyDb (float slopeDbPerSecond);

    private:
        struct Curve
        {
            std::vector<float> time;
            std::vector<float> decayDb;
            bool valid = false;
        };

        /** Schroeder backward integration over a response already stripped of its direct sound. */
        static Curve schroederCurve (const float* ir, int numSamples, double sampleRate,
                                     int startSample);

        /** Band limits a centre frequency for the chosen resolution. */
        static void bandEdges (Resolution resolution, float centre, float& low, float& high);

        /** The number of one third octave bands between two frequencies. */
        static int bandIndexFor (Resolution resolution, float frequency, float lowFrequency);

        /** Narrows a response to one band by keeping only its bins, then transforming back. */
        std::vector<float> bandFiltered (const float* ir, int numSamples, double sampleRate,
                                         float lowHz, float highHz);

        /** Fills in the four times and the verdict for one curve. */
        Band measureCurve (const Curve& curve) const;

        Settings settings;

        std::vector<float> scratch;
        std::vector<float> bandBuffer;
        mutable Curve bandScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbAnalyser)
    };
}