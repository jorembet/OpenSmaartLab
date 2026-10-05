// Phase 11 checks: intermodulation, crosstalk and polarity.
//
// Nothing here is a number typed into a test. Every signal is generated, transformed by the
// same code the application uses, and compared against a value worked out by hand from the
// amplitudes that went in. A measurement that cannot be checked against arithmetic is a
// measurement nobody should act on, and these are exactly the three that get acted on: an
// amplifier is judged on its intermodulation, a cable on its crosstalk, and a system on whether
// it is wired the right way up.
//
// The intermodulation cases matter most, because intermodulation products cannot be produced
// by adding sines. A product at 2f1-f2 is a property of a non-linearity, so to check the
// measurement the test has to supply a product of known amplitude directly, and then confirm
// the analyser finds it and does not count the tones themselves as products.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "AdvancedMeasurements.h"

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
    constexpr int fftSize = 32768;

    struct Tone
    {
        double frequency = 0.0;
        double amplitude = 0.0;
    };

    /** A signal built from named tones, so the amplitudes going in are known exactly. */
    std::vector<float> mix (const std::vector<Tone>& tones, double seconds = 1.5)
    {
        const auto count = (int) (seconds * rate);
        std::vector<float> samples ((size_t) count, 0.0f);

        for (int i = 0; i < count; ++i)
        {
            double value = 0.0;

            for (const auto& tone : tones)
                value += tone.amplitude
                       * std::sin (2.0 * dsp::pi * tone.frequency * (double) i / rate);

            samples[(size_t) i] = (float) value;
        }

        return samples;
    }

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

    /** A single impulse at the start, which is what makes polarity unambiguous. */
    std::vector<float> impulse (double amplitude, int atSample = 4, double seconds = 0.5)
    {
        std::vector<float> samples ((size_t) (seconds * rate), 0.0f);
        samples[(size_t) atSample] = (float) amplitude;
        return samples;
    }

    const dsp::ImdAnalyser::Product& productAt (
        const dsp::ImdAnalyser::Result& result, const juce::String& expression)
    {
        for (const auto& product : result.products)
            if (product.expression == expression)
                return product;

        return result.products.front();
    }
}

int main()
{
    // Test 1: SMPTE intermodulation with products of known amplitude.
    //
    // f1 = 1000 Hz at 0.5, f2 = 1500 Hz at 0.5, so the drive power is 0.25 + 0.25 = 0.5.
    // Products supplied at known amplitudes: 0.005 at 500 Hz (2f1-f2), 0.003 at 2000 Hz
    // (2f2-f1) and 0.002 at 2500 Hz (3f2-2f1).
    //
    // IMD = sqrt(0.005^2 + 0.003^2 + 0.002^2) / sqrt(0.5^2 + 0.5^2)
    //     = sqrt(3.8e-5) / 0.7071068
    //     = 0.0061644 / 0.7071068 = 0.0087182, which is 0.87182 %.
    {
        const auto signal = mix ({ { 1000.0, 0.5 }, { 1500.0, 0.5 },
                                   { 500.0, 0.005 }, { 2000.0, 0.003 }, { 2500.0, 0.002 } });

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::ImdAnalyser::Settings settings;
        settings.lowToneHz = 1000.0f;
        settings.standard = dsp::ImdAnalyser::Standard::Smpte;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (signal.data(), (int) signal.size());

        require (result.valid, "a two tone signal must be measurable");

        report (std::abs (result.firstHz - 1000.0f) < 0.1f
                    && std::abs (result.secondHz - 1500.0f) < 0.1f,
                "SMPTE must place the tones at 3:2",
                juce::String (result.firstHz, 1) + " and "
                    + juce::String (result.secondHz, 1) + " Hz");

        report (std::abs (result.firstAmplitude - 0.5f) < 0.01f,
                "the first tone must measure 0.5",
                juce::String (result.firstAmplitude, 5));

        report (std::abs (result.secondAmplitude - 0.5f) < 0.01f,
                "the second tone must measure 0.5",
                juce::String (result.secondAmplitude, 5));

        // Each product on its own, which is what the table is for.
        report (std::abs (productAt (result, "2f1-f2").amplitude - 0.005f) < 0.0008f,
                "the second order product at 500 Hz must measure 0.005",
                juce::String (productAt (result, "2f1-f2").amplitude, 6));

        report (std::abs (productAt (result, "2f2-f1").amplitude - 0.003f) < 0.0008f,
                "the second order product at 2000 Hz must measure 0.003",
                juce::String (productAt (result, "2f2-f1").amplitude, 6));

        report (std::abs (productAt (result, "3f2-2f1").amplitude - 0.002f) < 0.0008f,
                "the third order product at 2500 Hz must measure 0.002",
                juce::String (productAt (result, "3f2-2f1").amplitude, 6));

        const auto expectedPercent = 0.87182f;

        report (std::abs (result.imdPercent - expectedPercent) < 0.06f,
                "IMD must match the hand calculation",
                juce::String (result.imdPercent, 5) + " % against "
                    + juce::String (expectedPercent, 5) + " %");

        const auto expectedDb = 20.0f * std::log10 (0.0087182f);

        report (std::abs (result.imdDb - expectedDb) < 0.4f,
                "IMD in decibels must match the hand calculation",
                juce::String (result.imdDb, 4) + " dB against "
                    + juce::String (expectedDb, 4) + " dB");

        report (std::abs (20.0f * std::log10 (result.imdPercent / 100.0f) - result.imdDb) < 0.1f,
                "the percentage and the decibel figure must be the same number",
                juce::String (result.imdPercent, 5) + " % against "
                    + juce::String (result.imdDb, 4) + " dB");
    }

    // Test 2: two tones on their own must not produce intermodulation.
    //
    // This is the check that the analyser is not simply reporting something at whatever
    // frequency it was asked about. A linear sum of two sines has no products, so any IMD
    // reading here is the analyser finding its own noise floor rather than distortion.
    {
        const auto signal = mix ({ { 1000.0, 0.5 }, { 1500.0, 0.5 } });

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::ImdAnalyser::Settings settings;
        settings.lowToneHz = 1000.0f;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (signal.data(), (int) signal.size());

        require (result.valid, "two tones must be measurable");

        report (result.imdPercent < 0.05f,
                "two tones on their own must show almost no intermodulation",
                juce::String (result.imdPercent, 5) + " %");
    }

    // Test 3: an asymmetry that must not change the answer.
    //
    // The drive power is a root sum of squares of two amplitudes, not their sum, so changing
    // the ratio between the tones while keeping the total power the same must leave IMD
    // unchanged. Summing the amplitudes instead would move the figure by three decibels.
    {
        const auto even = mix ({ { 1000.0, 0.5 }, { 1500.0, 0.5 },
                                 { 500.0, 0.005 }, { 2000.0, 0.003 } });

        // Half the amplitude in the second tone, with the first raised so the total drive power
        // is exactly the same. Solving V1^2/2 + (V1/2)^2/2 = 0.25 gives V1 = 0.63246, and
        // without that correction the pair would simply be quieter and the figure would be
        // testing the wrong thing.
        const auto skewed = mix ({ { 1000.0, 0.63245553 }, { 1500.0, 0.31622777 },
                                   { 500.0, 0.005 }, { 2000.0, 0.003 } });

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::ImdAnalyser::Settings settings;
        settings.lowToneHz = 1000.0f;
        analyser.setSettings (settings);

        const auto a = analyser.analyse (even.data(), (int) even.size());
        const auto b = analyser.analyse (skewed.data(), (int) skewed.size());

        report (std::abs (a.imdPercent - 0.82462f) < 0.05f,
                "the even pair must match the hand calculation",
                juce::String (a.imdPercent, 5) + " % against 0.82462 %");

        report (std::abs (a.imdPercent - b.imdPercent) < 0.03f,
                "IMD must not depend on how the drive power is split between the tones",
                juce::String (a.imdPercent, 5) + " % even against "
                    + juce::String (b.imdPercent, 5) + " % skewed");

        report (std::abs (b.imdPercent - 0.82462f) < 0.06f,
                "the skewed pair must still match the hand calculation",
                juce::String (b.imdPercent, 5) + " % against 0.82462 %");
    }

    // Test 4: a real non-linearity must show up.
    //
    // The previous cases supply products directly. This one puts the tones through something
    // that actually bends, which is the case a reader cares about, and checks that the figure
    // rises well clear of the noise floor. The exact value is not asserted, because the products
    // of an arbitrary waveshaper are not a number worth hand calculating.
    {
        const auto count = (int) (1.5 * rate);
        std::vector<float> bent ((size_t) count, 0.0f);

        for (int i = 0; i < count; ++i)
        {
            const auto value = 0.5 * std::sin (2.0 * dsp::pi * 1000.0 * (double) i / rate)
                             + 0.5 * std::sin (2.0 * dsp::pi * 1500.0 * (double) i / rate);

            // A soft knee, which is what an amplifier does as it approaches its limit.
            bent[(size_t) i] = (float) (value - 0.12 * value * value * value);
        }

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::ImdAnalyser::Settings settings;
        settings.lowToneHz = 1000.0f;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (bent.data(), (int) bent.size());

        require (result.valid, "a bent signal must be measurable");

        report (result.imdPercent > 0.5f,
                "a third order bend must produce measurable intermodulation",
                juce::String (result.imdPercent, 4) + " %");

        report (productAt (result, "2f1-f2").amplitude > 0.001f,
                "a third order non-linearity must show a second order product too, because the "
                "cubic term mixes",
                juce::String (productAt (result, "2f1-f2").amplitude, 6));

        report (std::isfinite (result.imdDb) && std::isfinite (result.imdPercent),
                "a real non-linearity must not produce NaN or Inf");
    }

    // Test 5: CCIF, and the limit of it.
    {
        float first = 0.0f, second = 0.0f;
        dsp::ImdAnalyser::driveFrequencies (dsp::ImdAnalyser::Standard::Ccif, 1000.0f,
                                            first, second);

        report (std::abs (first - 1000.0f) < 0.1f && std::abs (second - 2000.0f) < 0.1f,
                "CCIF must place the tones an octave apart",
                juce::String (first, 1) + " and " + juce::String (second, 1) + " Hz");

        dsp::ImdAnalyser::driveFrequencies (dsp::ImdAnalyser::Standard::Smpte, 1000.0f,
                                            first, second);

        report (std::abs (second - 1500.0f) < 0.1f,
                "SMPTE must place the tones at three halves",
                juce::String (second, 1) + " Hz");

        // With an octave between the tones every product lands on a harmonic of the lower one,
        // so the measurement includes those harmonics whether or not they are intermodulation.
        // It is offered anyway, and the products are reported by name so a reader can see which
        // frequency each one came from.
        // The product is placed at 4 kHz, which is what 3f2-2f1 names when f2 = 2f1.
        const auto signal = mix ({ { 1000.0, 0.5 }, { 2000.0, 0.5 }, { 4000.0, 0.004 } });

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::ImdAnalyser::Settings settings;
        settings.lowToneHz = 1000.0f;
        settings.standard = dsp::ImdAnalyser::Standard::Ccif;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (signal.data(), (int) signal.size());

        require (result.valid, "a twin tone signal must be measurable");

        report (std::abs (productAt (result, "3f2-2f1").amplitude - 0.004f) < 0.0008f,
                "the third order product at 4 kHz must measure 0.004",
                juce::String (productAt (result, "3f2-2f1").amplitude, 6));

        // And the reason CCIF cannot be trusted on its own, shown rather than asserted: 4f1-f2
        // lands exactly on f2, so that row reads the drive tone itself.
        report (std::abs (productAt (result, "4f1-f2").amplitude - 0.5f) < 0.02f,
                "the band that coincides with the drive tone must read the drive tone, which is "
                "why CCIF figures are not separable",
                juce::String (productAt (result, "4f1-f2").amplitude, 4));

        report (result.products.size() >= 3,
                "CCIF must report the products it measured by name",
                juce::String ((int) result.products.size()) + " products");

        report (dsp::ImdAnalyser::standardName (dsp::ImdAnalyser::Standard::Ccif).isNotEmpty(),
                "both standards must be nameable in the selector");

        report (dsp::ImdAnalyser::getStandardNames().size() == 2,
                 "both standards must be offered");
    }

    // Test 6: crosstalk, with a known amount of leakage in each direction.
    //
    // Two passes, because that is what crosstalk takes: the left channel is driven while the
    // right is read, and then the other way round. The receiving channel carries only the
    // leakage, so its level against the drive is the figure a reader wants.
    {
        const auto tone = 997.0;
        const double drive = 0.5;

        // Leakage of -80 and -82 dB, as amplitudes: 0.5 * 10^(-80/20) and so on.
        const auto leftToRight = drive * std::pow (10.0, -80.0 / 20.0);
        const auto rightToLeft = drive * std::pow (10.0, -82.0 / 20.0);

        dsp::CrosstalkAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::CrosstalkAnalyser::Settings settings;
        settings.frequency = (float) tone;
        analyser.setSettings (settings);

        // Left driven: the left carries the tone and the right carries only what leaked.
        const auto leftDriven = mix ({ { tone, drive } });
        const auto rightLeaking = mix ({ { tone, leftToRight } });

        const auto lr = analyser.measure (leftDriven.data(), rightLeaking.data(),
                                          (int) leftDriven.size());

        require (lr.valid, "a driven pair must be measurable");

        report (std::abs (lr.leakageDb + 80.0f) < 1.0f,
                "L to R leakage must read -80 dB",
                juce::String (lr.leakageDb, 2) + " dB");

        report (std::abs (lr.leakagePercent - 0.01f) < 0.0015f,
                "L to R leakage must read a hundredth of a percent",
                juce::String (lr.leakagePercent, 5) + " %");

        report (std::abs (lr.driveLevelDb + 9.03f) < 0.5f,
                "the drive level must be reported, so a quiet figure can be traced",
                juce::String (lr.driveLevelDb, 2) + " dB");

        // Right driven, the other way.
        const auto rightDriven = mix ({ { tone, drive } });
        const auto leftLeaking = mix ({ { tone, rightToLeft } });

        const auto rl = analyser.measure (rightDriven.data(), leftLeaking.data(),
                                          (int) rightDriven.size());

        require (rl.valid, "the reverse direction must be measurable");

        report (std::abs (rl.leakageDb + 82.0f) < 1.0f,
                "R to L leakage must read -82 dB",
                juce::String (rl.leakageDb, 2) + " dB");

        // The two directions must be told apart, which is the reason for measuring both.
        report (std::abs ((lr.leakageDb - rl.leakageDb) - 2.0f) < 0.3f,
                "the two directions must not be reported as the same figure",
                juce::String (lr.leakageDb, 2) + " and " + juce::String (rl.leakageDb, 2) + " dB");

        // No drive at all: there is nothing to compare against, so the result must be refused
        // rather than divided into.
        const auto silent = std::vector<float> ((size_t) fftSize, 0.0f);

        const auto nothing = analyser.measure (silent.data(), silent.data(), fftSize);

        report (! nothing.valid,
                "with nothing driving the channel the measurement must be refused");

        report (std::isfinite (nothing.leakageDb),
                "a refused measurement must still be finite");

        require (dsp::CrosstalkAnalyser::levelToString (lr.leakageDb) == "-80.0 dB",
                 "a leakage figure must print with its unit and sign");
    }

    // Test 7: polarity, from an impulse that cannot be anything but right way up.
    {
        dsp::PolarityDetector detector;

        dsp::PolarityDetector::Settings settings;
        settings.threshold = 0.3f;
        settings.maxLagSamples = 64;
        detector.setSettings (settings);

        const auto reference = impulse (1.0);
        const auto normal = impulse (0.5);
        auto inverted = normal;
        auto delayed = impulse (0.5, 4 + 37);

        for (auto& sample : inverted)
            sample = -sample;

        auto inNormal = detector.detect (reference.data(), normal.data(),
                                         (int) normal.size());

        report (inNormal.verdict == dsp::PolarityDetector::Verdict::Normal,
                 "an impulse arriving the right way up must read NORMAL");

        report (inNormal.confident,
                "a clear impulse must be reported as confident");

        report (inNormal.correlation > 0.9f,
                "the correlation of an impulse with itself must be near one",
                juce::String (inNormal.correlation, 4));

        auto inInverted = detector.detect (reference.data(), inverted.data(),
                                           (int) inverted.size());

        report (inInverted.verdict == dsp::PolarityDetector::Verdict::Inverted,
                "an impulse arriving backwards must read INVERTED",
                dsp::PolarityDetector::verdictToString (inInverted.verdict));

        report (inInverted.correlation < -0.9f,
                "the correlation of an inverted impulse must be near minus one",
                juce::String (inInverted.correlation, 4));

        report (std::abs (inInverted.correlation + inNormal.correlation) < 1.0e-3f,
                "inverting the signal must flip the correlation exactly",
                juce::String (inInverted.correlation, 6) + " against "
                    + juce::String (inNormal.correlation, 6));

        // A delay is not a polarity error, which is the mistake this is most likely to make.
        auto inDelayed = detector.detect (reference.data(), delayed.data(),
                                          (int) delayed.size());

        report (inDelayed.verdict == dsp::PolarityDetector::Verdict::Normal,
                "a delayed impulse must still read NORMAL, because a delay is not an inversion",
                dsp::PolarityDetector::verdictToString (inDelayed.verdict));

        report (inDelayed.lagSamples == 37,
                "the lag must be reported, so the delay is accounted for",
                juce::String (inDelayed.lagSamples) + " samples");

        // Two unrelated signals must not produce a verdict. Calling that a wiring fault would
        // send someone looking for a polarity problem they do not have.
        Noise a (11), b (29);
        std::vector<float> one ((size_t) 4096), other ((size_t) 4096);

        for (size_t i = 0; i < one.size(); ++i)
        {
            one[i] = a.next();
            other[i] = b.next();
        }

        auto inUnknown = detector.detect (one.data(), other.data(), (int) one.size());

        report (inUnknown.verdict == dsp::PolarityDetector::Verdict::Unknown,
                "two unrelated signals must produce no verdict",
                dsp::PolarityDetector::verdictToString (inUnknown.verdict));

        report (! inUnknown.confident,
                "two unrelated signals must not be reported as confident");

        // Silence against silence has nothing to correlate, and must not divide into an answer.
        const auto bothSilent = std::vector<float> (4096, 0.0f);
        const auto inSilent = detector.detect (bothSilent.data(), bothSilent.data(),
                                               (int) bothSilent.size());

        report (inSilent.verdict == dsp::PolarityDetector::Verdict::Unknown,
                "silence must produce no verdict");

        report (std::isfinite (inSilent.correlation),
                "silence must not produce a NaN correlation");

        require (dsp::PolarityDetector::verdictToString (
                     dsp::PolarityDetector::Verdict::Normal) == "NORMAL",
                 "the normal verdict must read NORMAL");

        require (dsp::PolarityDetector::verdictToString (
                     dsp::PolarityDetector::Verdict::Inverted) == "INVERTED",
                 "the inverted verdict must read INVERTED");
    }

    // Test 8: nothing may come out as NaN or an infinity.
    {
        const auto silent = std::vector<float> ((size_t) fftSize, 0.0f);

        dsp::ImdAnalyser imd;
        imd.prepare (rate, fftSize);
        const auto noProducts = imd.analyse (silent.data(), fftSize);

        report (std::isfinite (noProducts.imdPercent) && std::isfinite (noProducts.imdDb),
                "silence must not produce NaN or Inf in the intermodulation figures");

        report (dsp::ImdAnalyser::imdRatio (0.0, { 1.0, 1.0 }) == 0.0f,
                 "IMD against a silent drive must be refused rather than divided");

        report (dsp::ImdAnalyser::imdRatio (1.0, {}) == 0.0f,
                 "IMD with no products must be zero");

        dsp::CrosstalkAnalyser crosstalk;
        crosstalk.prepare (rate, fftSize);
        const auto nothing = crosstalk.measure (silent.data(), silent.data(), fftSize);

        report (std::isfinite (nothing.leakageDb),
                "a silent pair must not produce NaN in the crosstalk figures");

        dsp::PolarityDetector polarity;
        const auto noInput = polarity.detect (nullptr, silent.data(), fftSize);

        report (noInput.verdict == dsp::PolarityDetector::Verdict::Unknown,
                "a missing reference must produce no verdict");

        report (dsp::CrosstalkAnalyser::levelToString (-999.0f) == "--",
                 "an unmeasurable leakage must print as unmeasurable");

        report (dsp::ImdAnalyser::levelToString (dsp::dbFloor) == "--",
                 "an unmeasurable intermodulation must print as unmeasurable");
    }

    if (failures == 0)
        std::cout << "PASS: intermodulation, crosstalk and polarity are all read from generated "
                     "signals and match the hand calculations" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}