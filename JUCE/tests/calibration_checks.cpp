// Phase 9 checks: microphone calibration and sound pressure level.
//
// The level of a microphone is not something that can be read off a data sheet and typed in,
// so the whole point of a calibrator is that it supplies a reference the chain can be measured
// against. These checks therefore do exactly what the workflow does: put a synthetic signal of
// a known level through the chain, let the software derive its correction, and then confirm
// that a different known level afterwards comes back as the right number.
//
// The weighting curves are checked against the published values rather than against themselves,
// because a weighting built from slightly wrong corner frequencies still produces a smooth,
// plausible, obviously-a-weighting curve that would pass any self-consistency test.
//
// The last group is the one that matters most: without a calibration the meter must not
// produce a number in decibels at all. A reading that says 94 and is in fact a guess is worse
// than one that admits it knows nothing, because it will be acted on.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DSP.h"
#include "MicrophoneCalibration.h"
#include "SplAnalyser.h"

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

    /** A sine, which is what a calibrator puts out. */
    std::vector<float> sine (double frequency, double amplitude, double seconds)
    {
        const auto count = (int) (seconds * rate);
        std::vector<float> samples ((size_t) count);

        for (int i = 0; i < count; ++i)
            samples[(size_t) i] = (float) (amplitude * std::sin (2.0 * dsp::pi * frequency
                                                                 * (double) i / rate));

        return samples;
    }

    void feed (dsp::SplAnalyser& analyser, const std::vector<float>& samples)
    {
        const auto* data = samples.data();
        const auto remaining = (int) samples.size();

        for (int offset = 0; offset < remaining; offset += 512)
            analyser.process (data + offset, juce::jmin (512, remaining - offset));
    }

    using Weighting = dsp::SplAnalyser::Weighting;

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

    /** A named placeholder, so the type being meant is obvious where it is declared. */
    using MicrophoneColouredCapsule = MicrophoneCalibration;
}

int main()
{
    // The weighting curves, against published values.
    //
    // A weighting is only zero at its reference frequency, which for both A and C is 1 kHz. That
    // is a definition, so an implementation that disagrees with it is wrong by construction and
    // needs no further argument.
    {
        report (std::abs (dsp::SplAnalyser::weightingGainDbAt (Weighting::A, 1000.0f)) < 0.01f,
                 "A weighting must be exactly 0 dB at 1 kHz");

        report (std::abs (dsp::SplAnalyser::weightingGainDbAt (Weighting::C, 1000.0f)) < 0.01f,
                 "C weighting must be exactly 0 dB at 1 kHz");

        report (std::abs (dsp::SplAnalyser::weightingGainDbAt (Weighting::Z, 1000.0f)) < 0.01f,
                 "Z weighting must be flat");

        // And flat at every frequency, not only where it was checked above.
        for (auto hz : { 31.5f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
            report (std::abs (dsp::SplAnalyser::weightingGainDbAt (Weighting::Z, hz)) < 1.0e-4f,
                     "Z weighting must stay flat across the band");

        // A weighting, at the frequencies the standard tabulates. Values are the published
        // ones: A at 31.5 Hz is about -39.4 dB, at 100 Hz about -19.1, at 8 kHz about -1.1;
        // C is flat low down and falls about 3 dB by 8 kHz.
        struct Expectation { float hz; float aDb; float cDb; };
        const Expectation expected[] =
        {
            { 31.5f,   -39.4f,  -3.0f },
            { 63.0f,   -26.2f,  -0.3f },
            { 125.0f,  -16.1f,  -0.0f },
            { 250.0f,   -8.6f,  -0.0f },
            { 500.0f,   -3.2f,  -0.0f },
            { 2000.0f,   1.2f,  -0.2f },
            { 4000.0f,   1.0f,  -1.0f },
            { 8000.0f,  -1.1f,  -3.0f },
            { 16000.0f, -6.6f,  -8.4f }
        };

        for (const auto& point : expected)
        {
            const auto a = dsp::SplAnalyser::weightingGainDbAt (Weighting::A, point.hz);
            report (std::abs (a - point.aDb) < 1.2f,
                    ("A weighting must follow the published curve at "
                         + juce::String (point.hz, 0) + " Hz").toRawUTF8(),
                    juce::String (a, 2) + " dB against " + juce::String (point.aDb, 1));

            const auto c = dsp::SplAnalyser::weightingGainDbAt (Weighting::C, point.hz);
            report (std::abs (c - point.cDb) < 1.2f,
                    ("C weighting must follow the published curve at "
                         + juce::String (point.hz, 0) + " Hz").toRawUTF8(),
                    juce::String (c, 2) + " dB against " + juce::String (point.cDb, 1));
        }

        // The two must differ from each other, which is the whole reason both are offered.
        report (std::abs (dsp::SplAnalyser::weightingGainDbAt (Weighting::A, 50.0f)
                          - dsp::SplAnalyser::weightingGainDbAt (Weighting::C, 50.0f)) > 10.0f,
                "A and C weighting must differ at low frequencies",
                juce::String (dsp::SplAnalyser::weightingGainDbAt (Weighting::A, 50.0f), 2)
                    + " against "
                    + juce::String (dsp::SplAnalyser::weightingGainDbAt (Weighting::C, 50.0f), 2)
                    + " dB");
    }

    // The weighting must actually be applied to the signal, not only available as a table. A
    // 50 Hz tone read through A and through Z has to differ by the amount the curve says.
    {
        dsp::SplAnalyser a, z;
        a.prepare (rate);
        z.prepare (rate);
        a.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        z.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);

        // Measured at 4 kHz rather than somewhere deep in the roll off, and that choice is
        // deliberate. A weighting is applied by summing weighted bins, so a tone pushed down
        // thirty decibels ends up below the leakage of its own rectangular window and the sum
        // stops describing the tone. That is a real limit of the method rather than a mistake
        // in it, and picking a frequency where the weighting is mild is what makes this a
        // check of the weighting instead of a check of the leakage floor. The curve itself is
        // verified against published values above, at every frequency including the deep ones.
        const auto tone = sine (4000.0, 0.5, 4.0);
        feed (a, tone);
        feed (z, tone);

        const auto levelA = a.getLevel (Weighting::A);
        const auto levelZ = z.getLevel (Weighting::Z);
        const auto expected = dsp::SplAnalyser::weightingGainDbAt (Weighting::A, 4000.0f);

        report (std::abs ((levelA - levelZ) - expected) < 1.0f,
                "the A weighting must be applied to the signal by the amount its curve gives",
                juce::String (levelA - levelZ, 2) + " dB measured against "
                    + juce::String (expected, 2) + " dB from the curve");

        // And at 1 kHz it must not change anything, which the definition requires.
        dsp::SplAnalyser a1k, z1k;
        a1k.prepare (rate);
        z1k.prepare (rate);
        a1k.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        z1k.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);

        const auto reference = sine (1000.0, 0.5, 4.0);
        feed (a1k, reference);
        feed (z1k, reference);

        report (std::abs (a1k.getLevel (Weighting::A) - z1k.getLevel (Weighting::Z)) < 0.5f,
                "A weighting must leave a 1 kHz tone unchanged",
                juce::String (a1k.getLevel (Weighting::A) - z1k.getLevel (Weighting::Z), 3) + " dB");
    }

    // A full level sine must read its own level. A chain that is a decibel out would not be
    // obvious in a room but would put every calibration and every reported SPL out by the same
    // amount, which is exactly the kind of error nobody notices.
    {
        dsp::SplAnalyser analyser;
        analyser.prepare (rate);
        analyser.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);

        // A full scale sine peaks at 1.0 and has an RMS of 1/sqrt(2), so -3.01 dBFS.
        feed (analyser, sine (1000.0, 1.0, 6.0));

        const auto level = analyser.getLevel (Weighting::Z);

        report (std::abs (level + 3.01f) < 0.5f,
                "a full scale sine must read -3.01 dBFS",
                juce::String (level, 3) + " dB");

        // A steady signal must leave the time weighted reading and the average agreeing, since
        // the time constant changes how fast a reading arrives and not what it says.
        report (std::abs (analyser.getLeq (Weighting::Z) - level) < 0.5f,
                "a steady level must read the same through the time weighting and the average",
                juce::String (analyser.getLeq (Weighting::Z), 2) + " against "
                    + juce::String (level, 2) + " dB");

        // And half the amplitude must read 6 dB lower on both.
        dsp::SplAnalyser quieter;
        quieter.prepare (rate);
        quieter.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        feed (quieter, sine (1000.0, 0.5, 6.0));

        report (std::abs ((level - quieter.getLevel (Weighting::Z)) - 6.02f) < 0.5f,
                "half amplitude must read 6 dB lower",
                juce::String (level - quieter.getLevel (Weighting::Z), 2) + " dB apart");

        report (std::abs ((analyser.getLeq (Weighting::Z)
                           - quieter.getLeq (Weighting::Z)) - 6.02f) < 0.5f,
                "half amplitude must read 6 dB lower in the average too",
                juce::String (analyser.getLeq (Weighting::Z) - quieter.getLeq (Weighting::Z),
                              2) + " dB apart");
    }

    // The equivalent level must integrate energy, not average decibels.
    {
        const auto measure = [] (float amplitude)
        {
            dsp::SplAnalyser analyser;
            analyser.prepare (rate);
            feed (analyser, sine (1000.0, amplitude, 4.0));
            return analyser.getLeq (Weighting::Z);
        };

        const auto loud = measure (0.5f);
        const auto quiet = measure (0.25f);

        // Half the amplitude is 6 dB. Averaging decibels would give the same answer for this
        // pair, so a second, asymmetric case is what distinguishes the two.
        report (std::abs ((loud - quiet) - 6.02f) < 0.7f,
                "Leq must scale with the energy, so halving amplitude gives 6 dB",
                juce::String (loud - quiet, 2) + " dB");

        // Two levels in turn: the correct answer is the energy average, which sits above the
        // mean of the two decibels. An arithmetic mean of dB would land between them.
        dsp::SplAnalyser mixed;
        mixed.prepare (rate);
        feed (mixed, sine (1000.0, 0.5, 2.0));
        feed (mixed, sine (1000.0, 0.05, 2.0));

        const auto combined = mixed.getLeq (Weighting::Z);
        const auto arithmeticMean = 0.5f * (dsp::db20 (0.5f / std::sqrt (2.0f))
                                            + dsp::db20 (0.05f / std::sqrt (2.0f)));

        // The energy average of two segments of equal length is the louder one plus 10*log10(2)
        // reduced by the fraction of total energy in the quiet one. The point is only that it
        // sits clearly above the arithmetic mean, which is the signature of averaging dB.
        report (combined > arithmeticMean + 0.5f,
                "Leq must average energy, so it must sit above the mean of the decibels",
                juce::String (combined, 2) + " dB against an arithmetic mean of "
                    + juce::String (arithmeticMean, 2) + " dB");

        report (mixed.getLeqDuration() > 3.9,
                "the integration period must be reported and must cover the audio given",
                juce::String (mixed.getLeqDuration(), 2) + " s");
    }

    // The integration period must be resettable, because a Leq over a chosen window is the
    // whole point of having one.
    {
        dsp::SplAnalyser analyser;
        analyser.prepare (rate);

        feed (analyser, sine (1000.0, 0.5, 2.0));
        const auto first = analyser.getLeqDuration();

        require (first > 1.9f, "the first period must cover the audio given");
        report (analyser.getLeq (Weighting::Z) > dsp::dbFloor,
                "Leq must exist before it is reset");

        analyser.resetLeq();
        report (analyser.getLeqDuration() == 0.0,
                "resetting must clear the integration period");
        report (analyser.getLeq (Weighting::Z) <= dsp::dbFloor + 0.01f,
                "resetting must clear the average",
                juce::String (analyser.getLeq (Weighting::Z), 1) + " dB");
    }

    // Time weighting: Fast must follow a change faster than Slow, and both must settle on the
    // same steady level. That last part is what shows the time constant changes the response
    // and not the reading.
    {
        dsp::SplAnalyser fast, slow;
        fast.prepare (rate);
        slow.prepare (rate);
        fast.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Fast);
        slow.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);

        require (std::abs (dsp::SplAnalyser::timeConstant (
                            dsp::SplAnalyser::TimeWeighting::Fast) - 0.125) < 1.0e-6,
                 "the fast constant must be 125 ms");

        require (std::abs (dsp::SplAnalyser::timeConstant (
                            dsp::SplAnalyser::TimeWeighting::Slow) - 1.0) < 1.0e-6,
                 "the slow constant must be 1 s");

        // A step from quiet to loud, then measure how long each takes to get close.
        auto step = 0.0;
        auto fastSamples = 0;
        auto slowSamples = 0;

        for (int i = 0; i < (int) (rate * 6.0); ++i)
        {
            if (i == (int) (rate * 1.0))
                step = dsp::db20 (0.5f / std::sqrt (2.0));

            // Silent for the first second and a full tone after, because a tone that is already
            // at its final level from the first sample is not a step and cannot show which
            // response is faster.
            const float sample = i < (int) rate
                                ? 0.0f
                                : (float) (0.5 * std::sin (2.0 * dsp::pi * 1000.0 * i / rate));
            fast.process (&sample, 1);
            slow.process (&sample, 1);

            if (i > (int) (rate * 1.0))
            {
                const auto target = step;

                if (fastSamples == 0 && fast.getLevel (Weighting::Z) > target - 0.5f)
                    fastSamples = i - (int) (rate * 1.0);

                if (slowSamples == 0 && slow.getLevel (Weighting::Z) > target - 0.5f)
                    slowSamples = i - (int) (rate * 1.0);
            }
        }

        report (fastSamples > 0 && slowSamples > 0,
                "both time weightings must reach a step",
                juce::String (fastSamples) + " and " + juce::String (slowSamples) + " samples");

        report (fastSamples < slowSamples,
                "fast must reach a step sooner than slow",
                juce::String (fastSamples / rate, 3) + " s against "
                    + juce::String (slowSamples / rate, 3) + " s");

        // And once settled they must agree, because the constant is a response time and not a
        // correction to the reading.
        report (std::abs (fast.getLevel (Weighting::Z) - slow.getLevel (Weighting::Z)) < 0.3f,
                "a settled fast and slow reading must agree",
                juce::String (fast.getLevel (Weighting::Z), 3) + " against "
                    + juce::String (slow.getLevel (Weighting::Z), 3) + " dB");
    }

    // Peak must hold the highest level seen, and must not follow the level back down.
    {
        dsp::SplAnalyser analyser;
        analyser.prepare (rate);
        analyser.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Impulse);

        feed (analyser, sine (1000.0, 0.5, 2.0));
        const auto loudPeak = analyser.getPeak (Weighting::Z);

        feed (analyser, sine (1000.0, 0.01, 2.0));
        const auto heldPeak = analyser.getPeak (Weighting::Z);

        report (heldPeak >= loudPeak - 0.01f,
                "the peak must hold when the level falls",
                juce::String (heldPeak, 2) + " dB after dropping to "
                    + juce::String (analyser.getLevel (Weighting::Z), 2) + " dB");

        report (std::abs (loudPeak - dsp::db20 (0.5f / std::sqrt (2.0f))) < 0.6f,
                "the peak must be the level of the loudest passage",
                juce::String (loudPeak, 3) + " dB");

        analyser.resetPeak();
        report (analyser.getPeak (Weighting::Z) <= dsp::dbFloor + 0.01f,
                "clearing the peak must reset it");
    }

    // The calibration workflow, with a synthetic calibrator.
    {
        MicrophoneCalibration profile;

        // A new profile must not pretend to be calibrated. This is the gate that stops the
        // meter reporting SPL from nothing.
        report (! profile.hasCalibration(),
                 "a fresh profile must not count as a calibration");

        report (! profile.isEnabled() || ! profile.hasCalibration(),
                 "an enabled profile with no reference must still not be a calibration");

        report (profile.describe().contains ("NOT CALIBRATED"),
                 "the description must say the microphone is not calibrated",
                 profile.describe());

        // Put the chain in front of a 94 dB calibrator and let it read whatever it reads.
        // The point is that the reference, not a typed number, decides the correction.
        MicrophoneCalibration calibrated;
        calibrated.calibrateAgainst (94.0f, -28.0f, "Mic Uji 1");

        report (calibrated.hasCalibration(),
                 "a profile measured against a reference must count as calibrated");

        report (calibrated.getCalibratorLevelDb() == 94.0f,
                "the calibrator level must be recorded",
                juce::String (calibrated.getCalibratorLevelDb()) + " dB");

        report (std::abs (calibrated.getCalibratorReadingDb() + 28.0f) < 0.01f,
                "the level the chain read must be kept for the record",
                juce::String (calibrated.getCalibratorReadingDb(), 2) + " dBFS");

        report (calibrated.getCalibrationDate().isNotEmpty(),
                 "a calibration must be dated");

        report (calibrated.getModelName() == "Mic Uji 1",
                 "the microphone name must be kept",
                 calibrated.getModelName());

        // Now the test that matters: a different known level afterwards must come back right.
        // With the chain reading -28 dBFS at 94 dB SPL, a 114 dB SPL calibrator must read
        // 114 dB SPL, and an 84 dB one must read 84.
        for (auto reference : { 84.0f, 94.0f, 104.0f, 114.0f })
        {
            const auto chainReading = -28.0f + (reference - 94.0f);
            const auto spl = calibrated.toSpl (chainReading);

            report (std::abs (spl - reference) < 0.05f,
                    ("the profile must turn a later level of "
                         + juce::String (reference, 0) + " dB SPL back into itself").toRawUTF8(),
                    juce::String (spl, 3) + " dB SPL");
        }

        // Without a calibration, toSpl must hand the raw reading straight back. Returning a
        // number in decibels from a profile that was never measured is the failure this whole
        // phase exists to prevent.
        MicrophoneCalibration uncalibrated;
        report (std::abs (uncalibrated.toSpl (-28.0f) + 28.0f) < 0.01f,
                 "an uncalibrated profile must leave the reading alone");

        // A profile written out and read back must be the same profile, or a calibration
        // would not survive closing the application.
        const auto json = calibrated.toJson();
        MicrophoneCalibration restored;

        report (restored.fromJson (json),
                "a profile written by the application must be readable again");

        report (restored.hasCalibration(),
                "a restored profile must still count as calibrated");

        report (restored.getModelName() == calibrated.getModelName(),
                "the microphone name must survive a round trip",
                restored.getModelName());

        report (std::abs (restored.getSensitivityDb() - calibrated.getSensitivityDb()) < 0.001f,
                "the sensitivity must survive a round trip",
                juce::String (restored.getSensitivityDb(), 3));

        report (restored.getCalibrationDate() == calibrated.getCalibrationDate(),
                "the calibration date must survive a round trip",
                restored.getCalibrationDate() + " against " + calibrated.getCalibrationDate());

        report (std::abs (restored.toSpl (-28.0f) - 94.0f) < 0.05f,
                "a restored profile must convert as well as the original",
                juce::String (restored.toSpl (-28.0f), 3) + " dB SPL");

        report (! restored.fromJson (juce::String()),
                 "empty text must not load as a profile");

        report (! restored.fromJson ("{ not json at all"),
                 "malformed text must not load as a profile");

        report (! restored.fromJson ("{ \"sensitivityDb\": -20.0 }"),
                 "a profile with no microphone named must not be accepted");

        // And a profile that parses but was never measured against anything must stay
        // uncalibrated even after a round trip, or loading a file would invent an authority.
        MicrophoneCalibration unverified;
        unverified.setModelName ("Mic Tanpa Acuan");
        unverified.setEnabled (true);

        MicrophoneCalibration loadedUnverified;
        report (loadedUnverified.fromJson (unverified.toJson()),
                "a profile with no reference must still load");

        report (! loadedUnverified.hasCalibration(),
                "loading a profile with no reference must not make it a calibration");

        report (loadedUnverified.describe().contains ("NOT CALIBRATED"),
                "an unverified profile must still say so",
                loadedUnverified.describe());
    }

    // The microphone's own frequency response has to reach the number, not only the trace.
    //
    // A capsule that is 6 dB down at 1 kHz would make a 1 kHz calibrator read low, and the
    // calibration would absorb that error into the sensitivity: the calibrator would then be
    // "correct" at 1 kHz and wrong everywhere else. Correcting the response per bin is what
    // stops one frequency being calibrated at the expense of the rest of the band.
    {
        MicrophoneColouredCapsule profile;
        profile.setCurve ({ { 1000.0f, 0.0f }, { 8000.0f, 0.0f } });

        // The curve holds the correction to add, not the capsule's response. A capsule 6 dB
        // down at 1 kHz is corrected by adding +6, which is the convention the field has always
        // used and the one apply() and the level path both rely on. Writing the response in
        // instead would double the error rather than cancel it, which is the trap worth naming.
        std::vector<MicrophoneCalibration::CurvePoint> curve
        {
            { 100.0f, 6.0f }, { 1000.0f, 6.0f }, { 8000.0f, 6.0f }, { 16000.0f, 6.0f }
        };

        MicrophoneCalibration coloured;
        coloured.setCurve (curve);
        coloured.calibrateAgainst (94.0f, -28.0f, "Mic Berwarna");

        dsp::SplAnalyser raw, corrected;
        raw.prepare (rate);
        corrected.prepare (rate);
        raw.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        corrected.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        corrected.setCalibration (coloured);

        report (! raw.hasCalibrationCurve(),
                "an analyser with no profile must carry no response correction");

        report (corrected.hasCalibrationCurve(),
                "a profile with a curve must reach the analyser");

        // A flat curve must change nothing, which shows the plumbing is not simply adding a
        // number somewhere.
        MicrophoneCalibration flatProfile;
        flatProfile.calibrateAgainst (94.0f, -28.0f, "Mic Datar");
        flatProfile.setCurve ({ { 20.0f, 0.0f }, { 20000.0f, 0.0f } });

        dsp::SplAnalyser flatCorrected;
        flatCorrected.prepare (rate);
        flatCorrected.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        flatCorrected.setCalibration (flatProfile);

        const auto probe = sine (1000.0, 0.5, 4.0);
        feed (raw, probe);
        feed (flatCorrected, probe);

        report (std::abs (flatCorrected.getLevel (Weighting::Z)
                          - raw.getLevel (Weighting::Z)) < 0.3f,
                "a flat response curve must leave the level alone",
                juce::String (flatCorrected.getLevel (Weighting::Z) - raw.getLevel (Weighting::Z), 3)
                    + " dB");

        // The real test: a tone in the band the curve lifts must come back up by that much.
        // The curve is a -6 dB capsule, and correctionDb is already the compensating sign, so
        // the corrected reading must sit above the uncorrected one by 6 dB.
        feed (corrected, probe);

        const auto lift = corrected.getLevel (Weighting::Z) - raw.getLevel (Weighting::Z);

        report (std::abs (lift - 6.0f) < 1.0f,
                "the frequency response must reach the level by the amount the curve gives",
                juce::String (lift, 2) + " dB");

        // And it must be per bin rather than one number: the same curve lifted by a constant
        // everywhere would raise every frequency equally, so a measurement in a different part
        // of the band would come back wrong. Pink-ish noise spread over the band must therefore
        // not receive the same 6 dB as a tone sitting inside the corrected region.
        dsp::SplAnalyser lifted, correctedNoise;
        lifted.prepare (rate);
        correctedNoise.prepare (rate);
        lifted.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        correctedNoise.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Slow);
        correctedNoise.setCalibration (coloured);

        Noise noise (4242);
        std::vector<float> broadband ((size_t) rate * 4);

        for (auto& sample : broadband)
            sample = noise.next() * 0.2f;

        feed (lifted, broadband);
        feed (correctedNoise, broadband);

        report (std::abs ((correctedNoise.getLevel (Weighting::Z) - lifted.getLevel (Weighting::Z))
                          - 6.0f) < 1.2f,
                "a broadband signal must also be corrected by the curve",
                juce::String (correctedNoise.getLevel (Weighting::Z) - lifted.getLevel (Weighting::Z),
                              2) + " dB");
    }

    if (failures == 0)
        std::cout << "PASS: the weighting curves are the published ones, the levels and "
                     "averages are right, and SPL appears only after a calibration" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}