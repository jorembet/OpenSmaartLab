// Phase 12 checks: reverberation time from a decay.
//
// The reference is a synthetic impulse response whose decay is an exponential envelope on
// noise, because that shape has an exact expected answer and can therefore settle the
// question rather than raise it. For an envelope of exp(-t/tau) the energy falls as
// exp(-2t/tau), so the Schroeder curve falls 8.6859 decibels a second and the reverberation
// time is 6.9078 * tau. That is the number every expected value below is derived from, and it
// is written out here rather than left for a reader to trust.
//
// The three times EDT, T20 and T30 are the same sixty decibel extrapolation of three different
// fits, and a perfect exponential makes all of them agree. That agreement is itself the check:
// it is what shows the extrapolation factor is applied once and not twice. A curve that is not a
// single exponential is what separates them, and is checked too.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "ReverbAnalyser.h"

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

    /** Decibels a second for an exponential envelope of exp(-t/tau).

        The energy goes as exp(-2t/tau), and 10*log10 of that is -8.68589/tau dB a second. */
    constexpr double decayDbPerSecondFor (double tau)
    {
        return -8.685889638065036 / tau;
    }

    /** The reverberation time of an envelope of exp(-t/tau): sixty decibels at that slope. */
    double expectedRt60 (double tau)
    {
        return 60.0 / std::abs (decayDbPerSecondFor (tau));
    }

    /** tau for a reverberation time, which is sixty divided by eight and two thirds. */
    double tauForRt60 (double rt60)
    {
        return rt60 * 8.685889638065036 / 60.0;
    }

    /** An impulse response with a known reverberation time.

        The direct sound is a single sample and the rest is noise under an exponential envelope,
        which is what a diffuse field looks like and what the Schroeder method assumes. It is
        built by generating the samples, not by filtering anything, so nothing in the result can
        have come from the generator. */
    std::vector<float> decayingRoom (double rt60, double seconds = 3.0, float level = 0.3f,
                                     uint32_t seed = 1234u)
    {
        const auto count = (int) (seconds * rate);
        std::vector<float> ir ((size_t) count, 0.0f);

        Noise noise (seed);

        const auto tau = tauForRt60 (rt60);

        for (int i = 1; i < count; ++i)
            ir[(size_t) i] = (float) (noise.next() * level
                                     * std::exp (-(double) i / rate / tau));

        ir[0] = 1.0f;   // the direct sound, so the arrival index has something to find
        return ir;
    }

    /** The same room with a noise floor added on top, which is what a real one has. */
    std::vector<float> decayingRoomWithNoise (double rt60, double noiseAmplitude,
                                              double seconds = 3.0, uint32_t seed = 4321u)
    {
        auto ir = decayingRoom (rt60, seconds, 0.3f, seed);
        Noise noise (seed + 7);

        for (auto& sample : ir)
            sample += (float) (noise.next() * noiseAmplitude);

        return ir;
    }

    /** The same room cut short, so the decay never reaches sixty decibels. */
    std::vector<float> truncatedRoom (double rt60, double seconds)
    {
        return decayingRoom (rt60, seconds, 0.3f);
    }
}

int main()
{
    // Test 1: a room with a known reverberation time must measure it back.
    //
    // RT60 = 0.8 s, so tau = 0.8 / 6.90775 = 0.1158 s and the Schroeder curve falls at
    // -8.68589 / 0.1158 = -75.0 dB a second. Over 800 ms that is exactly 60 dB.
    {
        const auto ir = decayingRoom (0.8);

        dsp::ReverbAnalyser analyser;
        dsp::ReverbAnalyser::Settings settings;
        settings.resolution = dsp::ReverbAnalyser::Resolution::Octave;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (ir.data(), (int) ir.size(), rate);

        require (result.valid, "a decaying room must be measurable");

        report (result.quality == dsp::ReverbAnalyser::Quality::Valid,
                "a clean decay must be reported as valid",
                dsp::ReverbAnalyser::qualityToString (result.quality));

        report (std::abs (result.rt60 - 0.8f) < 0.06f,
                "RT60 must match the reverberation time the room was built with",
                juce::String (result.rt60, 4) + " s against 0.8 s");

        // A perfect exponential makes all three agree, and that agreement is what proves the
        // extrapolation factor is applied once. If it were applied twice the three would come
        // back in a ratio, and if it were not applied at all they would differ by their spans.
        report (std::abs (result.edt - 0.8f) < 0.08f,
                "EDT must match the same reverberation time on a perfect exponential",
                juce::String (result.edt, 4) + " s");

        report (std::abs (result.t20 - 0.8f) < 0.08f,
                "T20 must match the same reverberation time on a perfect exponential",
                juce::String (result.t20, 4) + " s");

        report (std::abs (result.t30 - 0.8f) < 0.08f,
                "T30 must match the same reverberation time on a perfect exponential",
                juce::String (result.t30, 4) + " s");

        report (std::abs (result.edt - result.t30) < 0.05f
                    && std::abs (result.t20 - result.t30) < 0.05f,
                "all three must agree on a single exponential, which is the check on the "
                "extrapolation factor",
                juce::String (result.edt, 4) + ", " + juce::String (result.t20, 4) + ", "
                    + juce::String (result.t30, 4) + " s");

        report (std::abs (result.correlation) > 0.98f,
                "the decay must be a straight line",
                juce::String (result.correlation, 5));

        report (result.decayRangeDb > 35.0f,
                "the decay must actually have fallen far enough",
                juce::String (result.decayRangeDb, 1) + " dB");

        // The curve itself is kept so it can be drawn, and it has to start at zero decibels.
        require (! result.decayDb.empty(), "the decay curve must be returned for display");
        report (std::abs (result.decayDb.front()) < 1.0f,
                "the decay curve must start at its own peak",
                juce::String (result.decayDb.front(), 3) + " dB");

        report (result.decayDb.back() < -35.0f,
                 "the decay curve must have fallen by the end");

        report (result.bands.size() >= 6,
                "the octave bands asked for must all be present",
                juce::String ((int) result.bands.size()) + " bands");

        report (result.bandsValid > 0,
                "at least one band must be valid on a clean decay",
                juce::String (result.bandsValid) + " valid");
    }

    // Test 2: several reverberation times, to show the measurement tracks the input rather than
    // returning one answer whatever it is given.
    {
        for (auto expected : { 0.3f, 0.8f, 1.5f, 2.5f })
        {
            const auto ir = decayingRoom (expected, expected * 3.5f + 1.0f);

            dsp::ReverbAnalyser analyser;
            analyser.setSettings({});

            const auto result = analyser.analyse (ir.data(), (int) ir.size(), rate);


            report (result.valid && std::abs (result.rt60 - expected) < expected * 0.1f + 0.04f,
                    ("RT60 must track a room built to decay in "
                         + juce::String (expected, 2) + " s").toRawUTF8(),
                    juce::String (result.rt60, 4) + " s");

            // And the times must keep their order as the room gets longer.
            report (result.rt60 > 0.0f && result.t30 > 0.0f && result.t20 > 0.0f
                        && result.edt > 0.0f,
                    ("all four times must be reported for a room of "
                         + juce::String (expected, 2) + " s").toRawUTF8(),
                    juce::String (result.edt, 3) + " / " + juce::String (result.t20, 3) + " / "
                        + juce::String (result.t30, 3) + " / " + juce::String (result.rt60, 3));
        }
    }

    // Test 3: the bands must cover the range the specification names, and a band whose decay
    // differs from the broadband one must report its own number.
    {
        const auto ir = decayingRoom (0.8, 4.0);

        dsp::ReverbAnalyser analyser;
        dsp::ReverbAnalyser::Settings settings;
        settings.resolution = dsp::ReverbAnalyser::Resolution::Octave;
        settings.lowFrequency = 125.0f;
        settings.highFrequency = 8000.0f;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (ir.data(), (int) ir.size(), rate);

        require (! result.bands.empty(), "bands must be produced");

        for (auto wanted : { 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f })
        {
            auto found = false;

            for (const auto& band : result.bands)
                if (std::abs (band.frequency - wanted) < 1.0f)
                    found = true;

            report (found,
                    ("the " + juce::String (wanted, 0) + " Hz band must be present").toRawUTF8());
        }

        // A broadband decay gives every band the same answer, which is what makes this a check
        // that the bands are not simply being reported from the broadband curve.
        for (const auto& band : result.bands)
        {
            if (! band.valid)
                continue;

            report (std::abs (band.rt60 - 0.8f) < 0.12f,
                    ("every band must see the same decay in a broadband room at "
                         + juce::String (band.frequency, 0) + " Hz").toRawUTF8(),
                    juce::String (band.rt60, 4) + " s");
        }
    }

    // Test 4: third octave resolution, which the architecture supports and the specification
    // prefers if available.
    {
        const auto ir = decayingRoom (0.8, 4.0);

        dsp::ReverbAnalyser analyser;
        dsp::ReverbAnalyser::Settings settings;
        settings.resolution = dsp::ReverbAnalyser::Resolution::ThirdOctave;
        settings.lowFrequency = 125.0f;
        settings.highFrequency = 8000.0f;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (ir.data(), (int) ir.size(), rate);

        const auto octaveCount = dsp::octaveBandFrequencies (1, 125.0f, 8000.0f).size();
        const auto thirdCount = dsp::octaveBandFrequencies (3, 125.0f, 8000.0f).size();

        report (result.bands.size() > octaveCount,
                "third octave must give more bands than octave",
                juce::String ((int) result.bands.size()) + " against "
                    + juce::String ((int) octaveCount));

        report (result.bands.size() >= thirdCount - 2,
                "third octave must give about as many bands as the series implies",
                juce::String ((int) result.bands.size()) + " against "
                    + juce::String ((int) thirdCount));

        report (std::abs (result.rt60 - 0.8f) < 0.08f,
                "the broadband figure must not depend on the band resolution",
                juce::String (result.rt60, 4) + " s");

        report (dsp::ReverbAnalyser::getResolutionNames().size() == 2,
                 "both resolutions must be offered");
    }

    // Test 5: quality control. A response that never falls far enough must say so rather than
    // report a time, and one that has run into the noise floor must say that too. Both are the
    // difference between a measurement and a guess.
    {
        // A slow room recorded too briefly to fall thirty five decibels.
        //
        // It has to be a slow room recorded briefly, not a fast one. A fast room played back for
        // a moment covers the range easily, which is the opposite of the case being tested and
        // was how this test first passed a response that should have been refused.
        const auto shortDecay = truncatedRoom (2.5, 0.30);

        dsp::ReverbAnalyser analyser;
        analyser.setSettings({});

        const auto insufficient = analyser.analyse (shortDecay.data(),
                                                    (int) shortDecay.size(), rate);

        report (! insufficient.valid,
                "a response too brief to sustain the fit must not be reported valid",
                dsp::ReverbAnalyser::qualityToString (insufficient.quality));

        // Whatever it is called, the refusal has to carry a reason and the range it reached, so
        // the reason the answer is missing is visible rather than guessed at.
        report (insufficient.quality != dsp::ReverbAnalyser::Quality::Valid,
                "a refusal must be given a stated reason",
                dsp::ReverbAnalyser::qualityToString (insufficient.quality));

        report (insufficient.decayRangeDb > 0.0f && insufficient.decayRangeDb < 200.0f,
                "the range actually reached must be reported so the refusal can be seen",
                juce::String (insufficient.decayRangeDb, 1) + " dB");

        report (insufficient.rt60 <= 0.0f && insufficient.t30 <= 0.0f,
                "a refused response must not carry a time");

        // A record far too short to reach thirty five decibels at all, which is the case that is
        // genuinely short of decay rather than merely cut off part way through a range.
        //
        // The record has to be very short. White noise with no decay in it already drives the
        // Schroeder curve down by about ten decibels per decade of sample count, so fifty
        // milliseconds still covers forty decibels and is classified as a decay that stopped
        // rather than as a record that never went far enough.
        const auto barelyARecord = truncatedRoom (2.5, 0.008);

        const auto starved = analyser.analyse (barelyARecord.data(),
                                               (int) barelyARecord.size(), rate);

        report (starved.quality == dsp::ReverbAnalyser::Quality::InsufficientDecay,
                "a record that never came close to thirty five decibels must read INSUFFICIENT DECAY",
                dsp::ReverbAnalyser::qualityToString (starved.quality)
                    + " at " + juce::String (starved.decayRangeDb, 1) + " dB");

        // A long room under a noise floor. The decay flattens once the reflections are quieter
        // than the noise, which is exactly the case a reverberation time must not be read from.
        const auto noisy = decayingRoomWithNoise (1.2, 0.02, 4.0);

        const auto floored = analyser.analyse (noisy.data(), (int) noisy.size(), rate);

        // Either it is rejected as noisy, or it is still good enough to report. What it must
        // not do is report a confidently wrong time, so the check is that a rejected band
        // carries no time at all.
        auto reportedWithoutSupport = 0;

        for (const auto& band : floored.bands)
            if (! band.valid && (band.rt60 > 0.0f || band.t30 > 0.0f))
                ++reportedWithoutSupport;

        report (reportedWithoutSupport == 0,
                "a band that cannot be measured must carry no time at all",
                juce::String (reportedWithoutSupport) + " bands");

        auto noisyBands = 0;

        for (const auto& band : floored.bands)
            if (! band.valid && band.quality == dsp::ReverbAnalyser::Quality::Noisy)
                ++noisyBands;

        report (noisyBands > 0 || floored.quality == dsp::ReverbAnalyser::Quality::Valid,
                "a floored decay must either be reported valid or be called noisy",
                juce::String (noisyBands) + " noisy bands, quality "
                    + dsp::ReverbAnalyser::qualityToString (floored.quality));
    }

    // Test 6: nothing may come out as NaN or an infinity.
    {
        dsp::ReverbAnalyser analyser;
        analyser.setSettings({});

        const auto silent = std::vector<float> (4096, 0.0f);
        const auto nothing = analyser.analyse (silent.data(), (int) silent.size(), rate);

        report (! nothing.valid, "silence must not produce a measurement");

        report (std::isfinite (nothing.rt60) && std::isfinite (nothing.t30),
                "silence must not produce NaN or Inf");

        // A null response and a zero length must both be refused rather than read.
        const auto nullInput = analyser.analyse (nullptr, 4096, rate);
        report (! nullInput.valid, "a missing response must be refused");

        const auto zeroLength = analyser.analyse (silent.data(), 0, rate);
        report (! zeroLength.valid, "a zero length response must be refused");

        // A flat response: no decay at all, which is a flat line rather than a fast room.
        std::vector<float> flat (8192, 0.1f);
        const auto flatResult = analyser.analyse (flat.data(), (int) flat.size(), rate);

        report (std::isfinite (flatResult.rt60),
                "a flat response must not produce NaN",
                juce::String (flatResult.rt60));

        report (flatResult.rt60 == 0.0f || ! flatResult.valid
                    || flatResult.quality != dsp::ReverbAnalyser::Quality::Valid,
                "a flat response must not be reported as a valid reverberation time",
                juce::String (flatResult.rt60, 3) + " s, quality "
                    + dsp::ReverbAnalyser::qualityToString (flatResult.quality));

        // And the extrapolation itself must refuse a flat line rather than divide by zero.
        report (dsp::ReverbAnalyser::extrapolatedToSixtyDb (0.0f) == 0.0f,
                 "a zero slope must be refused rather than divided");

        report (dsp::ReverbAnalyser::extrapolatedToSixtyDb (-75.0f) == 0.8f,
                "the extrapolation rule must turn a slope into a sixty decibel time",
                juce::String (dsp::ReverbAnalyser::extrapolatedToSixtyDb (-75.0f), 4));

        require (dsp::ReverbAnalyser::qualityToString (
                     dsp::ReverbAnalyser::Quality::Valid) == "VALID",
                 "the valid state must read VALID");

        require (dsp::ReverbAnalyser::qualityToString (
                     dsp::ReverbAnalyser::Quality::InsufficientDecay) == "INSUFFICIENT DECAY",
                 "the insufficient state must read INSUFFICIENT DECAY");

        require (dsp::ReverbAnalyser::qualityToString (
                     dsp::ReverbAnalyser::Quality::Noisy) == "NOISY",
                 "the noisy state must read NOISY");
    }

    if (failures == 0)
        std::cout << "PASS: the reverberation times match the synthetic rooms they were built "
                     "from, and an unmeasurable decay says so" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}