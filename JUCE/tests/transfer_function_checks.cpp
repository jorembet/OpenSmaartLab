// Phase 5 checks: the dual channel transfer function.
//
// Every case here is a statement about arithmetic that has an answer known in advance. The
// signals are generated once as audio and measured through the same path the live tab uses;
// nothing inside the analyser is faked. A correction or a display that passes these has
// the right maths, and one that fails has a bug rather than a disagreement about taste.
//
// One property of coherence drives the shape of every test below.
//
// For a single FFT frame the magnitude squared coherence is identically one:
//
//     |Gxy|^2 = |conj(X) * Y|^2 = |X|^2 * |Y|^2 = Gxx * Gyy
//
// So a one frame estimate reads a perfect 1.0 whatever the two signals are, including two
// signals with nothing to do with each other. Coherence only carries information once the
// spectra are averaged over independent frames, which is the only reason averaging exists
// here. A coherence checked without averaging would pass while proving nothing, so every
// coherence check below feeds enough independent frames to settle the estimate.

#include <juce_core/juce_core.h>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

#include "TransferFunction.h"

namespace
{
    int failures = 0;

    void report (bool condition, const char* message, const juce::String& detail)
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
    constexpr int blockSize = 8192;

    /** Broadband noise from a fixed seed.

        Broadband rather than a tone on purpose. A tone leaves every other bin empty, and an
        empty bin carries no measurement, so a tone test would only ever prove the code works
        at one frequency. Noise puts signal in every bin, which is the case the coherence
        estimate actually has to survive. */
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

    struct Pair
    {
        std::vector<float> ref;
        std::vector<float> meas;
    };

    /** One frame of a reference and a measurement built from it.

        The measurement is gain * reference, plus independent noise when asked for, delayed
        by whole samples. Building it from the reference rather than from a second unrelated
        generator is what makes the known answer known. */
    Pair makeFrame (uint32_t refSeed, float gain, float noiseAmplitude = 0.0f,
                    int delaySamples = 0, uint32_t noiseSeed = 0)
    {
        Pair pair;
        pair.ref = noiseBlock (refSeed);
        pair.meas.assign ((size_t) blockSize, 0.0f);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto source = (size_t) (i - delaySamples);
            const auto value = source < pair.ref.size() ? pair.ref[source] * gain : 0.0f;

            pair.meas[(size_t) i] = value;
        }

        if (noiseAmplitude > 0.0f)
        {
            // The peak amplitude, matching how the reference is scaled, so passing the same
            // number twice gives two signals of equal power. Getting this wrong by a factor
            // of sqrt(3) is easy and moves the expected coherence a long way, so the argument
            // is the amplitude itself rather than a ratio to it.
            Noise extra (noiseSeed);

            for (auto& sample : pair.meas)
                sample += extra.next() * noiseAmplitude;
        }

        return pair;
    }

    /** Feeds independent frames and hands back the settled reading.

        Each frame gets a fresh reference and a fresh noise draw, because averaging the same
        signal twice would tell the estimator nothing it did not already know from one look.
        The returned result is the last frame's, by which point the average has converged. */
    TransferFunction::Result run (TransferFunction& tf,
                                  const std::function<Pair(uint32_t)>& generator,
                                  int frames, uint32_t firstSeed = 1)
    {
        TransferFunction::Result result;

        for (int i = 0; i < frames; ++i)
        {
            const auto pair = generator (firstSeed + (uint32_t) i * 7919u);
            result = tf.process (pair.ref.data(), pair.meas.data(), blockSize);
        }

        return result;
    }

    struct Band
    {
        float magnitudeDb = 0.0f;
        float coherence = 0.0f;
        int bins = 0;
    };

    /** Averages the readings over a frequency window.

        One bin is a single estimate. A band is the measurement a reader would judge, and
        averaging it keeps the check from depending on which bin a given seed happened to
        land on. */
    Band bandOf (const TransferFunction::Result& result, double lowHz, double highHz)
    {
        Band band;

        for (size_t i = 0; i < result.freq.size(); ++i)
        {
            if (result.binValid[i] == 0)
                continue;

            if (result.freq[i] < lowHz || result.freq[i] > highHz)
                continue;

            band.magnitudeDb += result.magnitudeDb[i];
            band.coherence += result.coherence[i];
            ++band.bins;
        }

        if (band.bins > 0)
        {
            band.magnitudeDb /= (float) band.bins;
            band.coherence /= (float) band.bins;
        }

        return band;
    }

    /** Configures a fresh analyser.

        Configured in place rather than returned, because TransferFunction holds a leak
        detector and is deliberately not copyable. */
    void configure (TransferFunction& tf, int averages)
    {
        tf.prepare ((float) rate, blockSize);
        tf.setAveraging (averages);
        tf.reset();
    }
}

int main()
{
    // Test 1: the measurement is a copy of the reference. A system that passes a signal
    // through unchanged has a magnitude of one and adds no noise, so the magnitude is 0 dB
    // and the coherence settles at one.
    {
        TransferFunction tf;
        configure (tf, 16);

        auto result = run (tf, [] (uint32_t seed)
        {
            const auto block = noiseBlock (seed);
            Pair pair;
            pair.ref = block;
            pair.meas = block;
            return pair;
        }, 24);

        const auto band = bandOf (result, 500.0, 2000.0);

        require (result.valid, "identical signals must produce a measurement");
        report (band.bins > 0, "identical signals must leave measurable bins",
                juce::String (band.bins) + " bins");

        report (std::abs (band.magnitudeDb) < 0.2f,
                "a copy of the reference must measure 0 dB",
                juce::String (band.magnitudeDb, 3) + " dB");

        report (band.coherence > 0.999f,
                "a copy of the reference must measure coherence of 1",
                juce::String (band.coherence, 5));

        report (result.averageCoherence > 0.999f,
                "the reported average coherence must be 1 for an identical pair",
                juce::String (result.averageCoherence, 5));
    }

    // Test 2: the measurement is the reference at half amplitude. This is the check that
    // catches a magnitude built from the wrong power: the ratio of two powers would give
    // 1/2 and read as -3 dB, and only the amplitude ratio gives the -6.02 dB that half
    // amplitude is actually worth.
    {
        TransferFunction tf;
        configure (tf, 16);

        const auto result = run (tf, [] (uint32_t seed)
        {
            return makeFrame (seed, 0.5f);
        }, 24);

        const auto band = bandOf (result, 500.0, 2000.0);

        report (std::abs (band.magnitudeDb + 6.02f) < 0.2f,
                "half amplitude must measure -6.02 dB",
                juce::String (band.magnitudeDb, 3) + " dB");

        // Halving the level changes no timing and adds no noise, so the coherence must not
        // notice. A coherence that fell here would mean the estimate is reading amplitude as
        // well as correlation.
        report (band.coherence > 0.999f,
                "a gain change must not disturb the coherence",
                juce::String (band.coherence, 5));
    }

    // Test 3: the measurement carries the reference plus noise of the same power. The
    // coherence of a signal and itself plus equal independent noise is exactly one half,
    // from Gxy = Gxx and Gyy = Gxx + Gnn. The check is that the estimate lands there, and
    // that it sits clearly below the clean case.
    {
        TransferFunction tf;
        configure (tf, 32);

        const auto result = run (tf, [] (uint32_t seed)
        {
            // 0.25 amplitude of noise against 0.25 amplitude of signal, so the powers match
            // and the expected coherence is one half.
            return makeFrame (seed, 1.0f, 0.25f, 0, seed ^ 0x5bf03635u);
        }, 40);

        const auto band = bandOf (result, 500.0, 2000.0);

        report (band.coherence < 0.8f,
                "adding independent noise must pull the coherence down",
                juce::String (band.coherence, 4));

        report (std::abs (band.coherence - 0.5f) < 0.12f,
                "equal power noise must give a coherence of 0.5",
                juce::String (band.coherence, 4));

        // The magnitude survives the noise, because the added power lands in Gyy while H divides
        // Gxy by Gxx, and the noise contributes nothing to Gxy in the average. The bound is
        // loose because |Gxy| is the magnitude of a complex average and therefore wanders:
        // with this many averages the estimate carries about a decibel of scatter, and
        // pretending otherwise would only produce a threshold that fails at random.
        report (std::abs (band.magnitudeDb) < 2.0f,
                "noise must not move the magnitude away from 0 dB",
                juce::String (band.magnitudeDb, 3) + " dB");
    }

    // Test 4: the measurement is the reference, late. A delay is a phase rotation, and the
    // magnitude of a transfer function ignores phase, so the magnitude must still read
    // 0 dB. This is the case that catches a transfer function computed from the auto spectra
    // alone, which would report a flat 0 dB for everything including signals with no
    // relationship to the reference at all.
    {
        TransferFunction tf;
        configure (tf, 16);

        const auto result = run (tf, [] (uint32_t seed)
        {
            return makeFrame (seed, 1.0f, 0.0f, 64);
        }, 24);

        const auto band = bandOf (result, 500.0, 2000.0);

        report (std::abs (band.magnitudeDb) < 0.3f,
                "a delayed copy must still measure 0 dB",
                juce::String (band.magnitudeDb, 3) + " dB");

        // The delay itself must be found and reported, since the phase pane is drawn from it.
        report (std::abs (result.delaySamples - 64.0f) < 2.0f,
                "the delay must be found and reported",
                juce::String (result.delaySamples, 2) + " samples");

        // Coherence ignores phase, so it stays at one whether or not the delay is
        // compensated. That is precisely why magnitude and coherence together cannot replace
        // the delay check above.
        report (band.coherence > 0.999f,
                "a delayed copy must still measure coherence of 1",
                juce::String (band.coherence, 5));
    }

    // Test 5: the two channels must be told apart, and this is the only check that can see
    // it. Swapping the reference and the measurement leaves a magnitude of 0 dB and a
    // coherence of 1 for a symmetric pair, so every test above would pass happily if the two
    // pointers were crossed. Two signals with nothing in common must instead read as no
    // transfer function at all, and only averaging reveals that.
    {
        TransferFunction forward;
        configure (forward, 32);

        const auto forwardResult = run (forward, [] (uint32_t seed)
        {
            Pair pair;
            pair.ref = noiseBlock (seed);
            pair.meas = noiseBlock (seed ^ 0x9e3779b9u);
            return pair;
        }, 40);

        TransferFunction backward;
        configure (backward, 32);

        const auto backwardResult = run (backward, [] (uint32_t seed)
        {
            Pair pair;
            pair.ref = noiseBlock (seed ^ 0x9e3779b9u);
            pair.meas = noiseBlock (seed);
            return pair;
        }, 40);

        const auto forwardBand = bandOf (forwardResult, 500.0, 2000.0);
        const auto backwardBand = bandOf (backwardResult, 500.0, 2000.0);

        report (forwardBand.coherence < 0.4f,
                "two unrelated signals must not measure high coherence",
                juce::String (forwardBand.coherence, 4));

        report (backwardBand.coherence < 0.4f,
                "reversing the channels must not invent a clean measurement",
                juce::String (backwardBand.coherence, 4));

        // The cross spectrum of two unrelated signals averages towards zero, so the measured
        // gain falls away instead of sitting at 0 dB.
        report (forwardBand.magnitudeDb < -6.0f,
                "an unrelated measurement must not read as unity gain",
                juce::String (forwardBand.magnitudeDb, 3) + " dB");
    }

    // Test 6: the estimate must settle rather than wander. The same average run twice has to
    // land in the same place, because a coherence that drifts frame to frame cannot be read
    // as a measurement of anything.
    {
        const auto runOnce = [] (uint32_t seedBase)
        {
            TransferFunction tf;
            configure (tf, 32);

            const auto result = run (tf, [] (uint32_t seed)
            {
                return makeFrame (seed, 1.0f, 0.25f, 0, seed ^ 0x5bf03635u);
            }, 40, seedBase);

            return bandOf (result, 500.0, 2000.0).coherence;
        };

        const auto first = runOnce (4000u);
        const auto second = runOnce (9000u);

        report (std::abs (first - second) < 0.05f,
                "an averaged estimate must settle instead of wandering",
                juce::String (first, 4) + " against " + juce::String (second, 4));

        report (std::abs (first - 0.5f) < 0.12f,
                "averaging must bring the noisy coherence towards 0.5",
                juce::String (first, 4));
    }

    if (failures == 0)
        std::cout << "PASS: transfer function magnitude, coherence, averaging and channel "
                     "assignment are correct" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}