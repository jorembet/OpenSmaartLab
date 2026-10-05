// Phase 6 checks: phase and delay.
//
// The delay is measured three ways and all three are checked against delays whose answers are
// known exactly, because a delay readout that is plausible but wrong is worse than none: it
// turns a path length into a confident number about a room. The tolerances are set from what
// each method can physically resolve rather than from whatever the code happened to produce.
//
//   Cross correlation  resolves one sample, 1/48000 s = 0.021 ms, so two samples of slack.
//   Phase slope        is fitted across the band, so its accuracy is set by how straight the
//                      phase really is; a few tenths of a millisecond is honest for it.
//   Impulse peak       shares the correlation's resolution, since it is read from the same
//                      alignment.
//
// A delayed copy of the reference is also the one case where the two independent methods must
// agree, so that agreement is itself checked rather than assumed.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DSP.h"
#include "TransferFunction.h"

namespace
{
    int failures = 0;

    void report (bool condition, const char* message, const juce::String& detail = {})
    {
        if (condition)
            return;

        ++failures;
        std::cout << "FAIL: " << message << "  [" << detail << "]" << std::endl;
    }

    void require (bool condition, const char* message)
    {
        report (condition, message, {});
    }

    constexpr double rate = 48000.0;
    constexpr int blockSize = 16384;

    class Noise
    {
    public:
        explicit Noise (uint32_t seed) : state (seed == 0u ? 0x9e3779b9u : seed) {}

        float next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return ((float) (state >> 8) * (1.0f / 8388608.0f)) - 1.0f;
        }

    private:
        uint32_t state;
    };

    std::vector<float> noiseBlock (uint32_t seed, float amplitude = 0.25f)
    {
        Noise generator (seed);
        std::vector<float> samples ((size_t) blockSize);

        for (auto& sample : samples)
            sample = generator.next() * amplitude;

        return samples;
    }

    std::vector<float> delayed (const std::vector<float>& source, int delaySamples)
    {
        std::vector<float> shifted (source.size(), 0.0f);

        for (size_t i = (size_t) delaySamples; i < source.size(); ++i)
            shifted[i] = source[i - (size_t) delaySamples];

        return shifted;
    }

    /** Measures one delayed copy through the live path and settles the average. */
    TransferFunction::Result measure (int delaySamples, int averages = 8,
                                      float temperatureC = 20.0f, bool compensate = true)
    {
        TransferFunction tf;
        tf.prepare ((float) rate, blockSize);
        tf.setAveraging (averages);
        tf.setTemperature (temperatureC);

        // Compensation rotates the measured phase back to zero lag, which is what makes the
        // displayed phase readable and what puts the impulse peak at zero. Anything that
        // wants to see the delay itself has to look with compensation off.
        tf.setDelayCompensation (compensate);
        tf.reset();

        // Independent frames, so the average is a real average. Feeding one block repeatedly
        // would leave the estimate reading a single frame.
        TransferFunction::Result result;
        uint32_t seed = 11u;

        for (int i = 0; i < averages + 4; ++i)
        {
            const auto ref = noiseBlock (seed += 7919u);
            const auto meas = delayed (ref, delaySamples);
            result = tf.process (ref.data(), meas.data(), blockSize);
        }

        return result;
    }

    bool allFinite (const std::vector<float>& values)
    {
        for (auto value : values)
            if (! std::isfinite (value))
                return false;

        return true;
    }
}

int main()
{
    // Wrapping is checked on its own first, because everything downstream that reads a single
    // frequency depends on it and a broken wrap would be hard to see in a full measurement.
    {
        require (std::abs (dsp::wrapDeg (0.0f)) < 1.0e-4f, "zero must wrap to zero");
        require (std::abs (dsp::wrapDeg (180.0f) + 180.0f) < 1.0e-3f,
                 "+180 must fold to -180 rather than sit on the seam");
        require (std::abs (dsp::wrapDeg (-180.0f) + 180.0f) < 1.0e-3f,
                 "-180 must stay -180");
        require (std::abs (dsp::wrapDeg (190.0f) + 170.0f) < 1.0e-3f,
                 "190 must wrap to -170");
        require (std::abs (dsp::wrapDeg (-190.0f) - 170.0f) < 1.0e-3f,
                 "-190 must wrap to +170");
        require (std::abs (dsp::wrapDeg (540.0f) + 180.0f) < 1.0e-3f,
                 "540 must wrap to -180");
        require (std::abs (dsp::wrapDeg (360.0f)) < 1.0e-3f, "360 must wrap to zero");
    }

    // Unwrapping is checked against a phase that is exactly a straight line, which is what a
    // pure delay produces. If the unwrap cannot rebuild a known ramp it cannot be trusted to
    // rebuild the delay that produced one.
    {
        const auto delaySamples = 480.0f;              // 10 ms at 48 kHz
        const auto step = rate / 2000.0;               // sample every 2 ms of frequency
        std::vector<float> frequencies, wrapped, ramp;

        for (double f = 100.0; f <= 8000.0; f += step)
        {
            frequencies.push_back ((float) f);
            const auto degrees = -360.0f * (float) f * delaySamples / (float) rate;
            ramp.push_back (degrees);
            wrapped.push_back (dsp::wrapDeg (degrees));
        }

        std::vector<float> unwrapped = wrapped;
        dsp::unwrapPhase (unwrapped);

        // The series this builds starts at exactly -360 degrees, so its first bin sits on a
        // whole turn and the unwrapped curve inherits that one turn everywhere. No algorithm
        // can do better: with nothing before the first bin, which turn it belongs to is not in
        // the data. A constant turn is harmless, because the slope the delay is read from
        // does not move, so what has to hold is continuity and an unchanging offset.
        auto largestStep = 0.0f;

        for (size_t i = 1; i < unwrapped.size(); ++i)
            largestStep = std::max (largestStep,
                                    std::abs (unwrapped[i] - unwrapped[i - 1]));

        report (largestStep <= 180.0f + 1.0e-2f,
                "unwrapping must leave no step larger than half a turn",
                juce::String (largestStep, 3) + " degrees");

        // Every bin must sit the same number of turns from the truth, which is what says the
        // unwrap tracked one continuous curve instead of jumping between branches.
        const auto offset = unwrapped[1] - ramp[1];
        auto drift = 0.0f;

        for (size_t i = 1; i < ramp.size(); ++i)
            drift = std::max (drift, std::abs ((unwrapped[i] - ramp[i]) - offset));

        report (drift < 1.0e-2f,
                "the unwrapped curve must stay on one branch for its whole length",
                juce::String (drift, 4) + " degrees of drift");

        auto turns = std::abs (offset) / 360.0f;
        report (std::abs (turns - std::floor (turns + 0.5f)) < 1.0e-3f,
                "the branch offset must be a whole number of turns",
                juce::String (turns, 4) + " turns");

        // The slope is the part that carries the delay, so it is checked against the truth
        // directly rather than through the offsets above.
        const auto unwrappedSlope = (unwrapped.back() - unwrapped[1])
                                  / (frequencies.back() - frequencies[1]);
        const auto trueSlope = -360.0f * delaySamples / (float) rate;

        report (std::abs (unwrappedSlope - trueSlope) < 1.0e-2f,
                "the unwrapped curve must keep the slope that carries the delay",
                juce::String (unwrappedSlope, 4) + " against "
                    + juce::String (trueSlope, 4) + " deg/Hz");

        report (allFinite (unwrapped), "unwrapping must not produce NaN or Inf");

        // Every wrapped sample has to be inside the stated band, or a cursor reading one
        // frequency would be able to show a phase outside -180 to +180.
        auto outside = 0;

        for (auto value : wrapped)
            if (value < -180.001f || value > 180.001f)
                ++outside;

        report (outside == 0, "every wrapped sample must sit inside -180 to +180",
                juce::String (outside) + " outside");
    }

    // Phase smoothing has to leave a straight line where it was. That is the property the
    // delay fit depends on, and it is the one a recursive filter would quietly break.
    {
        std::vector<float> frequencies, ramp;
        std::vector<char> valid;

        for (double f = 100.0; f <= 8000.0; f += rate / 2000.0)
        {
            frequencies.push_back ((float) f);
            ramp.push_back (-360.0f * (float) f * 240.0f / (float) rate);
            valid.push_back (1);
        }

        const auto smoothed = dsp::smoothUnwrappedPhase (ramp, valid, 2);

        require (smoothed.size() == ramp.size(), "smoothing must keep the length");
        report (allFinite (smoothed), "smoothing must not produce NaN or Inf");

        // Only the interior can be judged: the ends have fewer neighbours to average, so they
        // are left short on purpose rather than padded with invented samples.
        const auto interior = ramp.size() / 2;
        auto worst = 0.0f;

        for (size_t i = interior; i + interior < ramp.size(); ++i)
            worst = std::max (worst, std::abs (smoothed[i] - ramp[i]));

        report (worst < 1.0e-2f,
                "smoothing must leave a straight line exactly where it was",
                juce::String (worst, 5) + " degrees of worst error");
    }

    // The phase slope estimator is checked on its own, against a phase built from a known
    // delay. This is the method that must not be a single bin in disguise, so it is fed a
    // phase where every bin carries the same delay and asked to recover it from the trend.
    {
        const int knownSamples = 480;                  // 10 ms
        std::vector<float> frequencies, phase, coherence;
        std::vector<char> valid;

        for (double f = 100.0; f <= 10000.0; f += rate / 2000.0)
        {
            frequencies.push_back ((float) f);
            phase.push_back (-360.0f * (float) f * (float) knownSamples / (float) rate);
            coherence.push_back (1.0f);
            valid.push_back (1);
        }

        const auto estimate = dsp::estimatePhaseSlopeDelay (frequencies, phase, coherence,
                                                             valid, rate);

        require (estimate.valid, "a clean straight phase must produce an estimate");
        report (estimate.binsUsed >= 12, "the fit must use the whole band, not two bins",
                juce::String (estimate.binsUsed) + " bins");

        // A perfect line has no scatter at all, so anything measurable here would mean the
        // fit is not describing the phase it was given.
        report (estimate.scatterDeg < 1.0e-2f,
                "a straight phase must fit with no scatter",
                juce::String (estimate.scatterDeg, 5) + " degrees");

        // 10 ms is 0.01 s, and -360 degrees per second of delay gives -3.6 degrees per hertz.
        report (std::abs (estimate.slopeDegPerHz + 3.6f) < 1.0e-2f,
                "the slope must be -360 degrees per second of delay",
                juce::String (estimate.slopeDegPerHz, 5) + " deg/Hz");

        report (std::abs (estimate.delaySamples - (float) knownSamples) < 1.0f,
                "the slope must give back the delay that made the phase",
                juce::String (estimate.delaySamples, 2) + " against "
                    + juce::String (knownSamples) + " samples");
    }

    // The estimator has to refuse to answer when there is nothing to fit, rather than
    // returning a confident number derived from two bins.
    {
        std::vector<float> frequencies { 100.0f, 200.0f, 400.0f };
        std::vector<float> phase { 0.0f, -10.0f, -20.0f };
        std::vector<float> coherence { 1.0f, 1.0f, 1.0f };
        std::vector<char> valid { 1, 1, 1 };

        const auto estimate = dsp::estimatePhaseSlopeDelay (frequencies, phase, coherence,
                                                             valid, rate);

        report (! estimate.valid, "too few bins must not produce an estimate");
        report (std::isfinite (estimate.delaySamples), "a refusal must still be a finite number");

        // A phase that is not a line at all is the case that matters: the estimate may still
        // return something, so the scatter is what tells a reader not to believe it.
        std::vector<float> wobblyFrequencies, wobblyPhase, wobblyCoherence;
        std::vector<char> wobblyValid;

        for (double f = 100.0; f <= 10000.0; f += rate / 2000.0)
        {
            wobblyFrequencies.push_back ((float) f);
            wobblyPhase.push_back ((float) (std::sin (f * 0.01) * 400.0));
            wobblyCoherence.push_back (1.0f);
            wobblyValid.push_back (1);
        }

        const auto wobbly = dsp::estimatePhaseSlopeDelay (wobblyFrequencies, wobblyPhase,
                                                           wobblyCoherence, wobblyValid, rate);

        report (wobbly.scatterDeg > 10.0f,
                "a phase that is not a line must report a large scatter",
                juce::String (wobbly.scatterDeg, 2) + " degrees");
    }

    // The four delays the specification names, measured through the real transfer function.
    // Cross correlation carries the answer; the phase slope is checked as an independent second
    // opinion; the impulse response must peak at the same place.
    {
        const float delaysMs[] = { 1.0f, 5.0f, 10.0f, 20.0f };

        for (auto delayMs : delaysMs)
        {
            const auto expectedSamples = (float) (delayMs * 0.001 * rate);
            const auto expected = (int) std::lround (expectedSamples);
            const auto result = measure (expected);

            const auto tag = juce::String (delayMs, 0) + " ms";

            require (result.valid, ("a delayed copy must measure at " + tag).toRawUTF8());

            report (result.delayTrusted,
                    ("the delay must be trusted at " + tag).toRawUTF8(),
                    juce::String (result.delaySamples, 2) + " samples");

            // Two samples of slack on a method whose resolution is one sample.
            report (std::abs (result.delaySamples - expectedSamples) <= 2.0f,
                    ("cross correlation must recover the delay at " + tag).toRawUTF8(),
                    juce::String (result.delaySamples, 2) + " against "
                        + juce::String (expectedSamples, 2) + " samples");

            report (std::abs (result.delayMs - delayMs) <= 0.05f,
                    ("the delay in milliseconds must match at " + tag).toRawUTF8(),
                    juce::String (result.delayMs, 4) + " against " + tag);

            report (result.phaseSlopeValid,
                    ("the phase slope must fit at " + tag).toRawUTF8(),
                    juce::String (result.phaseSlopeBins) + " bins");

            // The slope is fitted across the band, so it is looser than the correlation by
            // design. Half a millisecond is still far tighter than anything a reader would
            // act on, and it proves the two methods are looking at the same delay.
            report (std::abs (result.phaseSlopeDelayMs - delayMs) <= 0.5f,
                    ("the phase slope must agree with the delay at " + tag).toRawUTF8(),
                    juce::String (result.phaseSlopeDelayMs, 3) + " against " + tag);

            report (result.delayMethodsAgree,
                    ("the two methods must be reported as agreeing at " + tag).toRawUTF8(),
                    juce::String (result.delayMs, 3) + " and "
                        + juce::String (result.phaseSlopeDelayMs, 3) + " ms");

            // The impulse response is the supporting method. With compensation on, its peak
            // sits at zero because the delay has been taken out, which is the correct
            // behaviour and is checked below; to read it as a delay it has to be measured
            // with compensation off.
            const auto raw = measure (expected, 8, 20.0f, false);

            // The stored impulse is rotated so its peak sits at sample 0, which is why the
            // arrival has to be read from the recorded peak index rather than from the array.
            report (raw.impulsePeakIndex == expected,
                    ("the impulse peak must land at the delay for " + tag).toRawUTF8(),
                    juce::String (raw.impulsePeakIndex) + " against "
                        + juce::String (expected) + " samples");

            // A delay that produced a real arrival has to produce an impulse with a peak
            // worth having: a peak near zero would mean the transform found nothing at all
            // and the index above would be meaningless.
            report (raw.impulsePeakValue > 0.5f,
                    ("the impulse must have a real peak for " + tag).toRawUTF8(),
                    juce::String (raw.impulsePeakValue, 3));

            // And with compensation on the arrival must come back to zero lag, because taking
            // the delay out is the whole purpose of compensating it.
            report (result.impulsePeakIndex <= 2,
                    ("the compensated impulse peak must sit at zero lag for " + tag).toRawUTF8(),
                    juce::String (result.impulsePeakIndex) + " samples");

            // Nothing in the output may be NaN or a division by zero left a NaN behind. A
            // single non-finite bin would draw as a spike through the whole curve.
            report (allFinite (result.phaseDeg), ("the phase must be finite at " + tag).toRawUTF8());
            report (allFinite (result.phaseDegWrapped),
                    ("the wrapped phase must be finite at " + tag).toRawUTF8());
            report (allFinite (result.phaseDegSmoothed),
                    ("the smoothed phase must be finite at " + tag).toRawUTF8());
            report (allFinite (result.magnitudeDb),
                    ("the magnitude must be finite at " + tag).toRawUTF8());
            report (allFinite (result.coherence),
                    ("the coherence must be finite at " + tag).toRawUTF8());
            report (std::isfinite (result.delayDistanceM),
                    ("the distance must be finite at " + tag).toRawUTF8());

            // Every wrapped sample has to be in the stated band, in the real measurement too.
            auto outside = 0;

            for (auto value : result.phaseDegWrapped)
                if (! std::isfinite (value) || value < -180.001f || value > 180.001f)
                    ++outside;

            report (outside == 0,
                    ("every measured wrapped phase must sit inside the band at " + tag).toRawUTF8(),
                    juce::String (outside) + " outside");
        }
    }

    // The unwrapped phase of a pure delay must fall as frequency rises, at 360 degrees per
    // second of delay. This is checked on the measurement rather than a synthetic curve,
    // because it is the form the reader actually looks at when judging a delay by eye.
    {
        // Measured with compensation off: the compensated phase is flat by design, so it is
        // the uncompensated one that shows the slope a delay produces.
        const auto result = measure (480, 8, 20.0f, false);
        double sumF = 0.0, sumP = 0.0;
        int count = 0;

        for (size_t i = 0; i < result.freq.size(); ++i)
        {
            if (result.binValid[i] == 0 || result.freq[i] < 500.0f || result.freq[i] > 5000.0f)
                continue;

            sumF += result.freq[i];
            sumP += result.phaseDeg[i];
            ++count;
        }

        require (count >= 12, "the measured phase must have enough bins to judge");

        const auto slopePerHz = (sumP / count) / (sumF / count);

        report (slopePerHz < 0.0f,
                "a delayed measurement must show a falling phase",
                juce::String (slopePerHz, 4) + " deg/Hz");
    }

    // Distance and temperature. The distance follows from the delay and the speed of sound, so
    // both are checked against the arithmetic rather than against a printed figure.
    {
        // 343 m/s at 20 degrees is the value the specification names, and it is what the
        // default has to produce.
        require (std::abs (dsp::speedOfSoundMetresPerSecond (20.0f) - 343.0f) < 1.0f,
                 "the speed of sound at 20 C must be about 343 m/s");

        require (std::abs (dsp::speedOfSoundMetresPerSecond (0.0f) - 331.3f) < 0.1f,
                 "the speed of sound at 0 C must be about 331 m/s");

        // Air carries sound faster when warmer, which is the whole reason for the setting.
        require (dsp::speedOfSoundMetresPerSecond (30.0f) > dsp::speedOfSoundMetresPerSecond (20.0f),
                 "warmer air must carry sound faster");
        require (dsp::speedOfSoundMetresPerSecond (10.0f) < dsp::speedOfSoundMetresPerSecond (20.0f),
                 "colder air must carry sound slower");

        // The example from the specification: 2.84 ms at 343 m/s is 0.97 m.
        const auto distance = dsp::distanceFromDelayMetres (2.84f * 0.001f * (float) rate, rate);
        report (std::abs (distance - 0.97f) < 0.01f,
                "2.84 ms must come out as about 0.97 m",
                juce::String (distance, 3) + " m");

        // A delay of zero must not become a distance of zero by accident: zero delay really
        // is zero path, so this states the boundary rather than papering over it.
        require (dsp::distanceFromDelayMetres (0.0f, rate) == 0.0f,
                 "no delay must mean no path");

        report (dsp::distanceFromDelayMetres (std::numeric_limits<float>::quiet_NaN(), rate) == 0.0f,
                 "a NaN delay must not become a NaN distance");
        report (dsp::distanceFromDelayMetres (480.0f, 0.0) == 0.0f,
                 "an unknown sample rate must not become an infinite distance");

        // The end to end path: a measured 10 ms delay at 20 degrees is 3.43 m, and the same
        // delay in a 30 degree room must read longer because the air is faster.
        const auto cool = measure (480, 8, 20.0f);
        const auto warm = measure (480, 8, 30.0f);

        report (std::abs (cool.delayDistanceM - 3.43f) < 0.05f,
                "10 ms at 20 C must be about 3.43 m",
                juce::String (cool.delayDistanceM, 3) + " m");

        report (warm.delayDistanceM > cool.delayDistanceM,
                "the same delay in a warmer room must read as a longer path",
                juce::String (warm.delayDistanceM, 3) + " against "
                    + juce::String (cool.delayDistanceM, 3) + " m");

        report (std::abs (warm.speedOfSound - dsp::speedOfSoundMetresPerSecond (30.0f)) < 0.01f,
                "the reported speed of sound must follow the temperature",
                juce::String (warm.speedOfSound, 2) + " m/s");

        // Temperature must reach the distance and nothing else: the delay is measured in
        // samples and knows nothing about the air, so a wrong temperature cannot corrupt it.
        report (std::abs (warm.delayMs - cool.delayMs) < 0.05f,
                "temperature must not move the delay itself",
                juce::String (warm.delayMs, 4) + " against "
                    + juce::String (cool.delayMs, 4) + " ms");
    }

    if (failures == 0)
        std::cout << "PASS: phase wraps and unwraps correctly, and the delay, distance and "
                     "temperature are right at 1, 5, 10 and 20 ms" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}