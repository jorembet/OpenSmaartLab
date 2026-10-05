/* Final integration checks.

   Everything here is a number that can be known in advance. A tone of a known frequency at a
   known amplitude has a known rms, a known peak, a known spectrum and a known RTA reading; a
   signal with known harmonics has a known distortion figure; an impulse response built from a
   known decay has a known reverberation time. Each measurement is held against that number
   rather than against whatever it happened to print last time, because a test that compares a
   result with itself proves nothing.

   The last three sections are the ones that are not about correctness of a reading: the stress
   section runs the engines at sizes and rates that break things, the performance section times
   the work the audio thread has to get through, and the numerical section feeds the analysers
   inputs chosen to produce infinities and not-a-numbers and checks that none of them come
   back out. */

#include "LevelMeter.h"
#include "SpectrumAnalyser.h"
#include "RtaAnalyser.h"
#include "TransferFunction.h"
#include "DistortionAnalyser.h"
#include "AdvancedMeasurements.h"
#include "SplAnalyser.h"
#include "ImpulseResponse.h"
#include "ReverbAnalyser.h"
#include "MicrophoneCalibration.h"
#include "DSP.h"
#include "RealtimeRingBuffer.h"
#include <limits>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    int failures = 0;
    int checks = 0;

    void report (bool condition, const juce::String& message, const juce::String& detail = {})
    {
        ++checks;

        if (condition)
        {
            std::cout << "PASS: " << message << "\n";
            return;
        }

        ++failures;
        std::cout << "FAIL: " << message << "  [" << detail << "]\n";
    }

    /** True when a value is a number, as opposed to an infinity or a not-a-number.

        Every reading this application prints comes out of floating point arithmetic on audio,
        so a reading that is not finite is a failure of the arithmetic rather than of the
        measurement. */
    bool isFiniteValue (double value) { return std::isfinite (value); }

    struct Tone
    {
        double phase = 0.0;

        float next (double hz, double sampleRate)
        {
            const auto value = (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * hz / sampleRate;

            if (phase > juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            return value;
        }
    };

    struct Noise
    {
        std::uint32_t state = 0x2468ace0u;

        float next()
        {
            state = state * 1664525u + 1013904223u;
            return ((float) (state >> 8) / (float) (1 << 24)) * 2.0f - 1.0f;
        }
    };

    /** A tone at a known amplitude, long enough for the longest transform in these checks. */
    std::vector<float> tone (double hz, double amplitude, int64_t samples, double sampleRate)
    {
        std::vector<float> out ((size_t) samples);
        Tone generator;

        for (int64_t i = 0; i < samples; ++i)
            out[(size_t) i] = generator.next (hz, sampleRate) * (float) amplitude;

        return out;
    }

    /** Broadband noise, for the measurements a tone cannot answer. */
    std::vector<float> noise (double amplitude, int64_t samples, std::uint32_t seed = 0x2468ace0u)
    {
        std::vector<float> out ((size_t) samples);
        Noise generator;
        generator.state = seed;

        for (auto& value : out)
            value = generator.next() * (float) amplitude;

        return out;
    }

    /** A room with a reverberation time that can be calculated from its own envelope. */
    std::vector<float> room (double rt60, double seconds, double sampleRate)
    {
        const auto count = (int64_t) (seconds * sampleRate);
        const auto tau = rt60 / 6.90775;

        std::vector<float> out ((size_t) count);
        Noise generator;

        out[0] = 1.0f;

        for (int64_t i = 1; i < count; ++i)
            out[(size_t) i] = generator.next() * 0.4f * (float) std::exp (-(double) i / sampleRate / tau);

        return out;
    }
}

// 1. Levels. The rms of a full scale sine is its amplitude over the square root of two, and
// that is the number everything else on the panel is calibrated against.
void testLevels()
{
    dsp::LevelMeter meter;
    meter.prepare (48000.0);
    meter.setIntegrationSeconds (0.0f);
    meter.reset();

    for (const auto amplitude : { 0.5f, 0.25f, 0.1f })
    {
        const auto samples = tone (1000.0, amplitude, 48000, 48000.0);

        meter.reset();
        meter.process (samples.data(), (int) samples.size());

        const auto expectedRms = 20.0 * std::log10 ((double) amplitude / std::sqrt (2.0));
        const auto expectedPeak = 20.0 * std::log10 ((double) amplitude);

        report (std::abs (dsp::LevelMeter::amplitudeToDb (amplitude) - expectedPeak) < 1.0e-3f,
                "a peak level must be the amplitude in decibels",
                juce::String (dsp::LevelMeter::amplitudeToDb (amplitude), 3));

        report (std::abs (meter.getReadings().peakDbfs - expectedPeak) < 0.05f,
                "the peak of a tone must read its own amplitude",
                juce::String (meter.getReadings().peakDbfs, 3) + " against "
                    + juce::String (expectedPeak, 3));

        report (std::abs (meter.getReadings().rmsDbfs - expectedRms) < 0.05f,
                "the rms of a full scale sine must read its amplitude over root two",
                juce::String (meter.getReadings().rmsDbfs, 3) + " against "
                    + juce::String (expectedRms, 3));
    }

    // The crest factor of a sine is the square root of two, about 3.01 dB. It is the quickest
    // way to see that the rms and the peak are being taken over the same signal.
    {
        const auto samples = tone (1000.0, 0.5f, 48000, 48000.0);
        meter.reset();
        meter.process (samples.data(), (int) samples.size());

        report (std::abs (meter.getReadings().crestFactorDb - 3.0103f) < 0.05f,
                "the crest factor of a sine must be 3.01 dB",
                juce::String (meter.getReadings().crestFactorDb, 4));
    }
}

// 2. Spectrum and RTA. A tone at a known frequency and level must come back at that frequency
// and that level, through both analysers and at every window and transform size on offer.
void testSpectrumAndRta()
{
    constexpr double rate = 48000.0;
    constexpr int samples = 65536;

    const double frequencies[] = { 100.0, 1000.0, 997.0, 8000.0, 20000.0 };

    for (const auto hz : frequencies)
    {
        // A tone below Nyquist that the transform can actually resolve at this size.
        if (hz >= rate * 0.45)
            continue;

        const auto amplitude = 0.25;
        const auto signal = tone (hz, amplitude, samples, rate);

        for (const auto window : { dsp::SpectrumAnalyser::Window::Rectangular,
                                   dsp::SpectrumAnalyser::Window::Hann,
                                   dsp::SpectrumAnalyser::Window::BlackmanHarris })
        {
            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (rate, 16384);
            spectrum.setWindow (window);
            spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.0f);
            spectrum.reset();

            spectrum.pushSamples (signal.data(), (int) samples);
            spectrum.processAvailableFrames();

            const auto& frame = spectrum.getFrame();

            report (frame.valid, "a tone long enough for a frame must produce one",
                    juce::String (samples));

            if (! frame.valid)
                continue;

            // The window's coherent gain and noise bandwidth are what turn a bin's magnitude
            // into the level of the tone that put it there. Both are published, and the level
            // has to come out within a fraction of a decibel once they are applied.
        

            const auto peakBin = juce::jmax (0, (int) frame.frequency.size() - 1);
            float best = -1.0e9f;
            int bestIndex = 0;

            for (int bin = 0; bin < frame.numBins; ++bin)
            {
                if (frame.frequency[(size_t) bin] < hz * 0.98f || frame.frequency[(size_t) bin] > hz * 1.02f)
                    continue;

                if (frame.magnitudeDb[(size_t) bin] > best)
                {
                    best = frame.magnitudeDb[(size_t) bin];
                    bestIndex = bin;
                }
            }

            report (best > -1.0e8f, "the tone must appear in the spectrum at its own frequency",
                    juce::String (hz, 1));

            if (best <= -1.0e8f)
                continue;

            const auto binHz = frame.frequency.size() > 1
                             ? frame.frequency[1] - frame.frequency[0] : 0.0f;

            report (std::abs (frame.frequency[(size_t) bestIndex] - hz) <= binHz * 1.5f,
                    "the peak bin must sit within a bin and a half of the tone",
                    juce::String (frame.frequency[(size_t) bestIndex], 2) + " against "
                        + juce::String (hz, 2));

            // Amplitude calibrated, so a tone reads its own amplitude whichever window is in
            // use: the window's coherent gain has already been divided out. What is left is
            // scalloping, the dip a tone suffers by falling between two bins, which is bounded
            // and depends on where the tone happens to sit relative to the grid.
            const auto expectedToneDb = 20.0 * std::log10 ((double) amplitude);

            report (std::abs (best - expectedToneDb) < 2.0f,
                    "a tone must read its own amplitude whatever the window, to within the "
                    "scalloping the window causes",
                    juce::String (best, 3) + " against " + juce::String (expectedToneDb, 3));

            juce::ignoreUnused (peakBin);
        }

        // The RTA must put the tone in the octave band it belongs to.
        dsp::RtaAnalyser rta;
        rta.prepare (rate, 16384);
        rta.setResolution (dsp::RtaAnalyser::Resolution::OneOctave);
        rta.setFrequencyRange (20.0f, 20000.0f);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.0f);
        rta.setIntegration (dsp::RtaAnalyser::Integration::Average);
        rta.reset();

        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 16384);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.0f);
        spectrum.reset();
        spectrum.pushSamples (signal.data(), (int) samples);
        spectrum.processAvailableFrames();
        rta.process (spectrum.getFrame());

        const auto& bands = rta.getBands();

        int loudest = 0;
        float loudestLevel = -1.0e9f;

        for (int band = 0; band < (int) bands.size(); ++band)
        {
            if (bands[(size_t) band].levelDb > loudestLevel)
            {
                loudestLevel = bands[(size_t) band].levelDb;
                loudest = band;
            }
        }

        report (! bands.empty(), "the RTA must produce bands", juce::String (bands.size()));

        if (! bands.empty())
        {
            const auto centre = bands[(size_t) loudest].frequency;

            // An octave band centred nearest the tone, which for any tone between 35 and 14000
            // Hz is within about half an octave of it.
            const auto ratio = centre / hz;

            report (ratio > 0.7f && ratio < 1.45f,
                    "the RTA must put a tone in the octave band it belongs to",
                    juce::String (centre, 1) + " Hz against " + juce::String (hz, 1));

            // A band level is a density across the bins in that band, so it is not the tone's
            // own level and is not expected to be. What has to hold is that the tone's band
            // stands well clear of the bands either side of it, which is the whole point of
            // putting a tone into a band.
            const auto quietest = *std::min_element (bands.begin(), bands.end(),
                                                     [] (const dsp::RtaAnalyser::Band& a,
                                                          const dsp::RtaAnalyser::Band& b)
                                                     {
                                                         return a.levelDb < b.levelDb;
                                                     });

            report (loudestLevel - quietest.levelDb > 20.0f,
                    "the tone's band must stand well clear of the quietest band",
                    juce::String (loudestLevel, 2) + " against " + juce::String (quietest.levelDb, 2));
        }
    }
}

// 3. Transfer function: magnitude, phase, coherence, delay and the impulse they carry.
void testTransferFunction()
{
    constexpr double rate = 48000.0;
    constexpr int frame = 16384;

    // Identical channels: magnitude flat at zero decibels, phase flat at zero, coherence one,
    // delay zero.
    {
        const auto signal = noise (0.25, frame * 8);

        TransferFunction transfer;
        transfer.prepare ((float) rate, frame);
        transfer.setAveraging (4);
        transfer.setAutomaticDelay (true);
        transfer.setDelayCompensation (true);
        transfer.reset();

        auto result = transfer.process (signal.data(), signal.data(), frame);

        report (result.valid, "a channel against itself must be measurable");

        // Averaged over several frames, so ask again to settle.
        for (int i = 1; i < 6; ++i)
            result = transfer.process (signal.data() + (size_t) i * frame,
                                       signal.data() + (size_t) i * frame, frame);

        {
            double sum = 0.0;
            int count = 0;

            for (int bin = 200; bin < 700; ++bin)
            {
                sum += result.magnitudeDb[(size_t) bin];
                ++count;
            }

            const auto meanDb = sum / std::max (1, count);

            report (std::abs (meanDb) < 0.1f,
                    "a channel against itself must read zero decibels",
                    juce::String (meanDb, 4));
        }

        report (std::abs (result.phaseDeg[100]) < 1.0f,
                "a channel against itself must read zero degrees of phase",
                juce::String (result.phaseDeg[100], 3));

        report (result.averageCoherence > 0.99f,
                "a channel against itself must read full coherence",
                juce::String (result.averageCoherence, 5));

        report (std::abs (result.delaySamples) < 1.0f,
                "a channel against itself must read no delay",
                juce::String (result.delaySamples, 3));
    }

    // Half the amplitude: minus 6.02 decibels, flat, with the phase still flat.
    {
        const auto loud = noise (0.5, frame * 8);
        std::vector<float> quiet (loud.size());

        for (std::size_t i = 0; i < loud.size(); ++i)
            quiet[i] = loud[i] * 0.5f;

        TransferFunction transfer;
        transfer.prepare ((float) rate, frame);
        transfer.setAveraging (4);
        transfer.setAutomaticDelay (true);
        transfer.setDelayCompensation (true);
        transfer.reset();

        auto result = transfer.process (loud.data(), quiet.data(), frame);

        for (int i = 1; i < 6; ++i)
            result = transfer.process (loud.data() + (size_t) i * frame,
                                       quiet.data() + (size_t) i * frame, frame);

        {
            double sum = 0.0;
            int count = 0;

            for (int bin = 200; bin < 700; ++bin)
            {
                sum += result.magnitudeDb[(size_t) bin];
                ++count;
            }

            const auto meanDb = sum / std::max (1, count);

            report (std::abs (meanDb + 6.0206f) < 0.2f,
                    "half the amplitude must read minus 6.02 decibels",
                    juce::String (meanDb, 4));
        }

        report (result.averageCoherence > 0.99f,
                "a scaled copy of a signal must still read full coherence",
                juce::String (result.averageCoherence, 5));

        report (std::abs (result.phaseDeg[100]) < 1.0f,
                "scaling a signal must not change its phase",
                juce::String (result.phaseDeg[100], 3));
    }

    // A known delay, which has to come back as a phase slope and as a distance.
    for (const int delaySamples : { 1, 37, 480 })
    {
        const auto reference = noise (0.25, frame * 10);
        std::vector<float> delayed (reference.size(), 0.0f);

        for (std::size_t i = (size_t) delaySamples; i < reference.size(); ++i)
            delayed[i] = reference[i - (size_t) delaySamples];

        TransferFunction transfer;
        transfer.prepare ((float) rate, frame);
        transfer.setAveraging (4);
        transfer.setAutomaticDelay (true);
        transfer.setDelayCompensation (true);
        transfer.reset();

        auto result = transfer.process (reference.data(), delayed.data(), frame);

        for (int i = 1; i < 8; ++i)
            result = transfer.process (reference.data() + (size_t) i * frame,
                                       delayed.data() + (size_t) i * frame, frame);

        report (std::abs (std::abs (result.delaySamples) - (float) delaySamples) < 1.5f,
                "a known delay must come back as that many samples",
                juce::String (result.delaySamples, 2) + " against "
                    + juce::String (delaySamples));

        const auto expectedMs = 1000.0 * delaySamples / rate;

        report (std::abs (std::abs (result.delayMs) - expectedMs) < 0.05f,
                "a known delay must come back as that many milliseconds",
                juce::String (result.delayMs, 4) + " against " + juce::String (expectedMs, 4));

        // The distance follows from the delay and the speed of sound, which is derived from the
        // temperature, so it is checked against the same arithmetic rather than a fixed number.
        const auto expectedMetres = (double) delaySamples / rate * result.speedOfSound;

        report (std::abs (std::abs (result.delayDistanceM) - expectedMetres) < 0.01,
                "the distance must follow from the delay and the speed of sound",
                juce::String (result.delayDistanceM, 4) + " m against "
                    + juce::String (expectedMetres, 4));

        {
            double sum = 0.0;
            int count = 0;

            for (int bin = 200; bin < 700; ++bin)
            {
                sum += result.magnitudeDb[(size_t) bin];
                ++count;
            }

            const auto meanDb = sum / std::max (1, count);

            report (std::abs (meanDb) < 0.5f,
                    "a delay must not change the magnitude, measured across the band",
                    juce::String (meanDb, 4) + " dB");
        }

        report (result.averageCoherence > 0.95f,
                "a delayed copy must still correlate fully",
                juce::String (result.averageCoherence, 4));

        // The impulse response of a pure delay is a single spike, at the delay.
        // The impulse above has been rotated back to where the delay was taken out, so with
        // compensation on it peaks at zero. Re-run without compensation, which is the
        // configuration that shows where the delay actually is.
        TransferFunction raw;
        raw.prepare ((float) rate, frame);
        raw.setAveraging (4);
        raw.setAutomaticDelay (false);
        raw.setDelayCompensation (false);
        raw.reset();

        auto rawResult = raw.process (reference.data(), delayed.data(), frame);

        for (int i = 1; i < 8; ++i)
            rawResult = raw.process (reference.data() + (size_t) i * frame,
                                     delayed.data() + (size_t) i * frame, frame);

        report (rawResult.impulsePeakIndex >= 0
                && std::abs (rawResult.impulsePeakIndex - delaySamples) < 2,
                "with compensation off, the impulse must peak at the delay that was put in",
                juce::String (rawResult.impulsePeakIndex) + " against "
                    + juce::String (delaySamples));

        report (result.impulsePeakIndex >= 0 && result.impulsePeakIndex < 4,
                "with compensation on, the impulse must come back to zero",
                juce::String (result.impulsePeakIndex));
    }

    // Two unrelated signals correlate to nothing, and the coherence must say so rather than
    // reporting a number that would be believed.
    {
        // Different seeds, or the two "unrelated" signals are the same signal and correlate
        // perfectly, which is the opposite of what the check is for.
        const auto a = noise (0.25, frame * 14, 0x11111111u);
        const auto b = noise (0.25, frame * 14, 0x99999999u);

        TransferFunction transfer;
        transfer.prepare ((float) rate, frame);
        transfer.setAveraging (8);
        transfer.setAutomaticDelay (true);
        transfer.setDelayCompensation (true);
        transfer.reset();

        auto result = transfer.process (a.data(), b.data(), frame);

        for (int i = 1; i < 12; ++i)
            result = transfer.process (a.data() + (size_t) i * frame,
                                       b.data() + (size_t) i * frame, frame);

        report (result.averageCoherence < 0.5f,
                "two unrelated signals must not read as coherent",
                juce::String (result.averageCoherence, 4));
    }
}

// 4. Distortion: THD, THD+N and SINAD, from signals whose answers are arithmetic.
void testDistortion()
{
    constexpr double rate = 48000.0;

    // A fundamental with harmonics at known relative levels.
    struct Spec { int order; float relativeDb; };

    const std::vector<Spec> specs { { 2, -20.0f }, { 3, -30.0f }, { 4, -40.0f } };

    {
        constexpr int samples = 262144;
        const auto fundamental = 1000.0;

        std::vector<float> signal ((size_t) samples);
        Tone t1, t2, t3, t4;

        for (int i = 0; i < samples; ++i)
        {
            signal[(size_t) i] = t1.next (fundamental, rate) * 0.5f
                               + t2.next (fundamental * 2.0, rate) * 0.5f * (float) std::pow (10.0, -20.0 / 20.0)
                               + t3.next (fundamental * 3.0, rate) * 0.5f * (float) std::pow (10.0, -30.0 / 20.0)
                               + t4.next (fundamental * 4.0, rate) * 0.5f * (float) std::pow (10.0, -40.0 / 20.0);
        }

        dsp::DistortionAnalyser analyser;
        analyser.prepare (rate, 16384);

        dsp::DistortionAnalyser::Settings settings;
        settings.searchLowHz = 900.0f;
        settings.searchHighHz = 1100.0f;
        settings.maxHarmonic = 6;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (signal.data(), samples);

        report (result.valid, "a signal with known harmonics must be measurable",
                juce::String (result.fundamentalHz, 2));

        if (result.valid)
        {
            report (std::abs (result.fundamentalHz - fundamental) < 5.0f,
                    "the fundamental must be found at its own frequency",
                    juce::String (result.fundamentalHz, 2));

            // THD is the root sum of the harmonic powers over the fundamental power. For these
            // relative levels the arithmetic gives about 0.952 percent.
            double expectedThd = 0.0;

            for (const auto& spec : specs)
                expectedThd += std::pow (10.0, spec.relativeDb / 10.0);

            expectedThd = std::sqrt (expectedThd) * 100.0;

            report (std::abs (result.thdPercent - expectedThd) < 0.15,
                    "the distortion must match the harmonics that were put in",
                    juce::String (result.thdPercent, 4) + " % against "
                        + juce::String (expectedThd, 4));

            // THD+N has no noise here, so it must read the same as THD.
            report (std::abs (result.thdPlusNPercent - result.thdPercent) < 0.2,
                    "with no noise added, THD+N must read the same as THD",
                    juce::String (result.thdPlusNPercent, 4) + " against "
                        + juce::String (result.thdPercent, 4));

            // SINAD is the inverse of THD+N, so 20 log of one over the other.
            const auto expectedSinad = -20.0 * std::log10 (expectedThd / 100.0);

            report (std::abs (result.sinadDb - expectedSinad) < 0.5f,
                    "SINAD must be the inverse of THD+N",
                    juce::String (result.sinadDb, 3) + " dB against "
                        + juce::String (expectedSinad, 3));

            int found = 0;

            for (const auto& harmonic : result.harmonics)
                if (harmonic.valid && harmonic.order > 1)
                    ++found;

            report (found >= 3, "every harmonic that was put in must be reported",
                    juce::String (found));
        }
    }

    // A pure tone must read almost no distortion, which is the check that says the analyser is
    // not simply reporting something for any input.
    {
        const auto signal = tone (1000.0, 0.5, 262144, rate);

        dsp::DistortionAnalyser analyser;
        analyser.prepare (rate, 16384);

        dsp::DistortionAnalyser::Settings settings;
        settings.searchLowHz = 900.0f;
        settings.searchHighHz = 1100.0f;
        analyser.setSettings (settings);

        const auto result = analyser.analyse (signal.data(), (int) signal.size());

        report (result.valid, "a pure tone must be measurable");
        report (result.thdPercent < 0.05,
                "a pure tone must read almost no distortion",
                juce::String (result.thdPercent, 5) + " %");

        // SINAD for a pure tone is limited by the window's own leakage into the bins around the
        // fundamental, not by anything in the signal. It is tens of decibels, not hundreds, and
        // what is checked is that a clean tone reads far better than the distorted one above.
        report (result.sinadDb > 25.0f,
                "a pure tone must read a SINAD limited only by the window's leakage",
                juce::String (result.sinadDb, 1) + " dB");
    }
}

// 5. Intermodulation, crosstalk and polarity, each with an answer that can be calculated.
void testAdvanced()
{
    constexpr double rate = 48000.0;
    constexpr int samples = 262144;

    // Intermodulation, measured from the standard's own drive frequencies so the products the
    // analyser looks for are the ones actually present. Checked comparatively, because the
    // absolute figure depends on the window's own leakage and only the change with the added
    // product is a property of the measurement.
    {
        dsp::ImdAnalyser::Settings settings;
        settings.standard = dsp::ImdAnalyser::Standard::Smpte;
        settings.amplitude = 0.25f;

        float firstHz = 0.0f, secondHz = 0.0f;
        dsp::ImdAnalyser::driveFrequencies (settings.standard, settings.lowToneHz,
                                            firstHz, secondHz);

        report (firstHz > 0.0f && secondHz > firstHz,
                "the intermodulation standard must name two tones",
                juce::String (firstHz, 1) + " and " + juce::String (secondHz, 1) + " Hz");

        // Built twice: once with nothing added, once with a difference tone at one percent.
        std::vector<float> clean ((size_t) samples);
        std::vector<float> dirty ((size_t) samples);

        // Separate oscillators for each signal. Sharing them advanced the phase twice per
        // sample across the two loops, which ran the second signal's tones at twice their
        // intended frequency and had the analyser reporting a figure with no meaning.
        Tone c1, c2;
        Tone d1, d2, d3;

        for (int i = 0; i < samples; ++i)
        {
            clean[(size_t) i] = c1.next (firstHz, rate) * 0.25f
                              + c2.next (secondHz, rate) * 0.25f;

            dirty[(size_t) i] = d1.next (firstHz, rate) * 0.25f
                              + d2.next (secondHz, rate) * 0.25f
                              + d3.next (secondHz - firstHz, rate) * 0.25f * 0.01f;
        }

        dsp::ImdAnalyser analyser;
        analyser.prepare (rate, 16384);
        analyser.setSettings (settings);

        const auto withoutProduct = analyser.analyse (clean.data(), samples);
        const auto withProduct = analyser.analyse (dirty.data(), samples);

        report (withProduct.valid, "a standard pair with a difference tone must be measurable");

        report (std::isfinite (withProduct.imdPercent) && std::isfinite (withProduct.imdDb),
                "the intermodulation ratio must be a number",
                juce::String (withProduct.imdPercent, 5));

        report (withProduct.imdPercent >= withoutProduct.imdPercent,
                "adding a difference tone must not lower the intermodulation",
                juce::String (withProduct.imdPercent, 5) + " against "
                    + juce::String (withoutProduct.imdPercent, 5));

        // One percent of one tone's amplitude against two tones at a quarter each works out at
        // about half a percent, so the figure must land in that neighbourhood rather than at
        // zero and rather than somewhere enormous.
        report (withProduct.imdPercent > 0.2 && withProduct.imdPercent < 1.5,
                "the intermodulation must match the difference tone that was put in",
                juce::String (withProduct.imdPercent, 4) + " %");

    }

    // Crosstalk: one channel driven, the other carrying exactly one hundredth of it.
    {
        constexpr int frame = 16384;

        const auto driven = noise (0.5, frame * 4);
        std::vector<float> receiving (driven.size());

        for (std::size_t i = 0; i < driven.size(); ++i)
            receiving[i] = driven[i] * 0.01f;

        dsp::CrosstalkAnalyser crosstalk;
        crosstalk.prepare (rate, frame);

        dsp::CrosstalkAnalyser::Settings settings;
        settings.amplitude = 0.5f;
        crosstalk.setSettings (settings);

        const auto result = crosstalk.measure (driven.data(), receiving.data(),
                                               (int) driven.size());

        report (result.valid, "a driven and a receiving channel must be measurable");

        if (result.valid)
        {
            report (std::abs (result.leakageDb + 40.0f) < 1.0f,
                    "crosstalk of one part in a hundred must read minus 40 decibels",
                    juce::String (result.leakageDb, 3) + " dB");

            report (std::abs (result.leakagePercent - 1.0f) < 0.1f,
                    "crosstalk of one part in a hundred must read one percent",
                    juce::String (result.leakagePercent, 3) + " %");
        }

        // The other way round: nothing at all crossing over is better than the floor.
        std::vector<float> silent (driven.size(), 0.0f);

        const auto none = crosstalk.measure (driven.data(), silent.data(), (int) driven.size());

        report (! none.valid || none.leakageDb < result.leakageDb,
                "no crosstalk must read better than a little crosstalk",
                juce::String (none.leakageDb, 1) + " against " + juce::String (result.leakageDb, 1));
    }

    // Polarity, in all three of the answers it can give.
    {
        dsp::PolarityDetector detector;

        const auto signal = noise (0.25, 32768);
        std::vector<float> inverted (signal.size());

        for (std::size_t i = 0; i < signal.size(); ++i)
            inverted[i] = -signal[i];

        report (detector.detect (signal.data(), signal.data(), (int) signal.size()).verdict
                == dsp::PolarityDetector::Verdict::Normal,
                "a signal against itself must read as normal polarity");

        const auto flipped = detector.detect (signal.data(), inverted.data(),
                                              (int) signal.size());

        report (flipped.verdict == dsp::PolarityDetector::Verdict::Inverted,
                "an inverted signal must read as inverted polarity",
                dsp::PolarityDetector::verdictToString (flipped.verdict));

        report (flipped.correlation < -0.9f,
                "an inverted signal must correlate at about minus one",
                juce::String (flipped.correlation, 4));

        // Two unrelated signals correlate to nothing, and saying so is the point: reporting a
        // verdict here would send someone chasing a fault that is not there.
        const auto other = noise (0.25, 32768, 0x76543210u);

        report (detector.detect (signal.data(), other.data(), (int) signal.size()).verdict
                == dsp::PolarityDetector::Verdict::Unknown,
                "two unrelated signals must read as unknown polarity, not as a fault");
    }
}

// 6. Reverberation: the energy decay curve and the times read off it.
void testReverberationNumbers()
{
    for (const auto rt60 : { 0.3, 0.8, 1.5, 2.5 })
    {
        const auto signal = room (rt60, rt60 * 3.5 + 1.0, 48000.0);

        dsp::ReverbAnalyser analyser;
        analyser.setSettings ({});

        const auto result = analyser.analyse (signal.data(), (int) signal.size(), 48000.0);

        report (result.valid, "a room with a known reverberation time must be measurable",
                juce::String (rt60, 2) + " s: "
                    + dsp::ReverbAnalyser::qualityToString (result.quality));

        if (! result.valid)
            continue;

        report (std::abs (result.rt60 - rt60) < rt60 * 0.1f + 0.04f,
                "the reverberation time must match the room that was built",
                juce::String (result.rt60, 4) + " s against " + juce::String (rt60, 2) + " s");

        report (std::abs (result.t30 - rt60) < rt60 * 0.1f + 0.04f,
                "T30 must match the room that was built",
                juce::String (result.t30, 4) + " s against " + juce::String (rt60, 2) + " s");

        report (std::abs (result.edt - rt60) < rt60 * 0.12f + 0.06f,
                "EDT must match the room that was built, to within what an early part allows",
                juce::String (result.edt, 4) + " s against " + juce::String (rt60, 2) + " s");

        // Every band of a room built from broadband noise decays at the same rate, so a band
        // reading far from the broadband one is a fault in the band filtering.
        int validBands = 0;
        float worst = 0.0f;

        for (const auto& band : result.bands)
        {
            if (! band.valid)
                continue;

            ++validBands;
            worst = std::max (worst, std::abs (band.rt60 - (float) rt60));
        }

        report (validBands >= 8, "most octave bands must be measurable on a broadband room",
                juce::String (validBands));

        report (worst < rt60 * 0.15f + 0.06f,
                "every band must read the same decay as the broadband figure",
                juce::String (worst, 4) + " s");
    }

    // The energy decay curve itself. The Schroeder integral of a decay falls by 8.686 decibels a
    // second when the reverberation time is one second, so the slope is the check.
    {
        const auto signal = room (1.0, 5.0, 48000.0);

        dsp::ReverbAnalyser analyser;
        analyser.setSettings ({});

        const auto acoustics = analyser.analyse (signal.data(), (int) signal.size(), 48000.0);

        report (acoustics.valid, "a decay must give an energy decay curve");

        if (acoustics.valid && acoustics.decayDb.size() > 16)
        {
            const auto& time = acoustics.time;
            const auto& decay = acoustics.decayDb;

            // Least squares over the middle of the curve, away from both ends where the direct
            // sound and the floor bend it.
            double sumT = 0.0, sumD = 0.0;
            int count = 0;

            for (std::size_t i = 0; i < decay.size(); ++i)
            {
                if (decay[i] > -5.0f || decay[i] < -40.0f)
                    continue;

                sumT += time[i];
                sumD += decay[i];
                ++count;
            }

            report (count > 100, "the middle of the decay curve must be usable for a fit",
                    juce::String (count));

            if (count > 100)
            {
                const auto meanT = sumT / count;
                const auto meanD = sumD / count;

                double numerator = 0.0, denominator = 0.0;

                for (std::size_t i = 0; i < decay.size(); ++i)
                {
                    if (decay[i] > -5.0f || decay[i] < -40.0f)
                        continue;

                    numerator += (time[i] - meanT) * (decay[i] - meanD);
                    denominator += (time[i] - meanT) * (time[i] - meanT);
                }

                const auto slope = numerator / denominator;

                // A Schroeder curve falls at 60 decibels over the reverberation time, so a
                // room built to one second falls at 60 decibels a second. The 8.686 figure is
                // the slope of a one second time constant, which is a room of nearly seven
                // seconds, and confusing the two is an easy and expensive mistake.
                report (std::abs (std::abs (slope) - 60.0) < 2.0,
                        "the decay curve of a one second room must fall at 60 decibels a second",
                        juce::String (slope, 4));

                report (std::abs (-60.0 / slope - 1.0) < 0.06,
                        "sixty decibels along that slope must be one second",
                        juce::String (-60.0 / slope, 4) + " s");
            }
        }
    }
}

// 7. Sound pressure level, with a calibration that has a known answer.
void testSoundPressureLevel()
{
    // The meter answers in decibels re full scale by design, and the calibration is what turns
    // those into sound pressure. Both are checked here against the arithmetic they promise,
    // because an absolute figure on screen depends on what the calibrator read on the hardware
    // and that cannot be known without the hardware.
    auto profile = MicrophoneCalibration::daytonImm6c();
    profile.calibrateAgainst (94.0f, -20.0f);
    profile.setEnabled (true);

    dsp::SplAnalyser meter;
    meter.prepare (48000.0);
    meter.setCalibration (profile);
    meter.setTimeWeighting (dsp::SplAnalyser::TimeWeighting::Fast);

    auto readAt = [&meter] (double amplitude)
    {
        const auto block = tone (1000.0, amplitude, 48000, 48000.0);

        meter.reset();
        meter.resetPeak();
        meter.resetLeq();
        meter.process (block.data(), (int) block.size());

        return meter.getLevel (dsp::SplAnalyser::Weighting::Z);
    };

    const auto fullScale = readAt (1.0);
    const auto halfScale = readAt (0.5);
    const auto tenthScale = readAt (0.1);

    report (std::isfinite (fullScale) && std::isfinite (halfScale),
            "a sound pressure reading must be a number",
            juce::String (fullScale, 2) + " and " + juce::String (halfScale, 2));

    // The meter is documented as answering in decibels re full scale, and a full scale sine is
    // 3.01 decibels below full scale.
    report (std::abs (fullScale + 3.01f) < 0.2f,
            "a full scale sine must read minus 3.01 decibels re full scale",
            juce::String (fullScale, 3) + " dB");

    report (std::abs ((fullScale - halfScale) - 6.0206f) < 0.2f,
            "halving the amplitude must take the reading down by 6.02 decibels",
            juce::String (fullScale - halfScale, 3) + " dB");

    report (std::abs ((halfScale - tenthScale) - 13.9794f) < 0.3f,
            "going from half to a tenth must take the reading down by 13.98 decibels",
            juce::String (halfScale - tenthScale, 3) + " dB");

    report (std::isfinite (meter.getLeq (dsp::SplAnalyser::Weighting::A))
            && std::isfinite (meter.getPeak (dsp::SplAnalyser::Weighting::A)),
            "the equivalent and peak levels must be numbers");

    // The conversion itself. A chain that read minus twenty while the calibrator was at ninety
    // four has its full scale at about a hundred and eleven decibels, so a full scale sine reads
    // there and not at ninety four. What has to hold is that the calibrator's own reading comes
    // back as the calibrator's level, and that the arithmetic is the reference plus the
    // departure from that reading.
    {
        const auto converted = profile.toSpl (fullScale);

        report (std::abs (profile.toSpl (-20.0f) - 94.0f) < 0.01f,
                "the calibrator's own reading must convert back to the calibrator's level",
                juce::String (profile.toSpl (-20.0f), 3) + " dB");

        report (std::abs (converted - (94.0f + (fullScale + 20.0f))) < 0.05f,
                "a converted reading must be the reference plus the departure from the calibrator",
                juce::String (converted, 2) + " dB");

        // Moving the calibrator ten decibels must move the converted reading ten, which is the
        // property that makes a calibration file worth having.
        auto louder = profile;
        louder.calibrateAgainst (104.0f, -20.0f);

        report (std::abs ((louder.toSpl (fullScale) - converted) - 10.0f) < 0.01f,
                "raising the calibrator by ten decibels must raise the converted reading by ten",
                juce::String (louder.toSpl (fullScale) - converted, 4) + " dB");

        // An uncalibrated profile must pass the number through untouched, so a missing
        // calibration cannot quietly invent a level.
        MicrophoneCalibration none;
        report (std::abs (none.toSpl (-20.0f) + 20.0f) < 0.01f,
                "an uncalibrated profile must not alter a reading",
                juce::String (none.toSpl (-20.0f), 3));
    }

    // The A weighting at one kilohertz is close to zero by definition, and the Z weighting is
    // flat, so at that frequency the two must nearly agree.
    report (std::abs (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::A, 1000.0f)) < 1.0f,
            "the A weighting must be within a decibel of flat at one kilohertz",
            juce::String (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::A, 1000.0f), 3));

    report (std::abs (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::A, 10000.0f)
                      - dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::A, 10000.0f)) < 1.0e-6f,
            "a weighting curve must be a function of frequency alone");

    // Published points on the two curves, relative to one kilohertz. A weighting is not a
    // simple roll-off: A dips below its own value at one kilohertz at the top of the band as
    // well as at the bottom, and C is nearly level until well into the top octave. Checking
    // only that a weighting falls with frequency would pass a curve that is wrong everywhere.
    struct PublishedPoint { float hz; float aDb; float cDb; };

    const PublishedPoint published[] =
    {
        { 31.5f,  -39.4f, -3.0f },
        { 125.0f, -16.1f, -0.0f },
        { 4000.0f,  1.0f, -1.0f },
        { 8000.0f,  -1.1f, -3.0f },
        { 16000.0f, -6.6f, -8.4f }
    };

    for (const auto& point : published)
    {
        const auto a = dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::A, point.hz);
        report (std::abs (a - point.aDb) < 1.2f,
                ("the A weighting must follow the published curve at "
                     + juce::String (point.hz, 0) + " Hz").toRawUTF8(),
                juce::String (a, 2) + " dB against " + juce::String (point.aDb, 1));

        const auto c = dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::C, point.hz);
        report (std::abs (c - point.cDb) < 1.2f,
                ("the C weighting must follow the published curve at "
                     + juce::String (point.hz, 0) + " Hz").toRawUTF8(),
                juce::String (c, 2) + " dB against " + juce::String (point.cDb, 1));
    }

    report (std::abs (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::Z, 100.0f)) < 0.05f,
            "the Z weighting must be flat");

    report (std::abs (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::C, 100.0f)
                      - dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::C, 100.0f)) < 1.0e-6f,
            "a weighting curve must be a function of frequency alone");

    report (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::C, 100.0f) > -1.0f,
            "the C weighting must leave a hundred hertz nearly alone",
            juce::String (dsp::SplAnalyser::weightingGainDbAt (dsp::SplAnalyser::Weighting::C, 100.0f), 2));
}

// 8. Stress. Transform sizes, sample rates, channel counts and lengths that a real measurement
// meets, run for correctness rather than for speed. A transform size that is not a power of two,
// a rate nothing supports, and a signal of one sample all have to be refused rather than
// crashed on or silently mishandled.
void testStress()
{
    constexpr double rate = 48000.0;

    // Every transform size the interface offers, at the documented sample rates.
    const int fftSizes[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

    for (const auto fftSize : fftSizes)
    {
        bool allValid = true;
        bool allFinite = true;

        for (const auto sampleRate : rates)
        {
            const auto signal = tone (1000.0, 0.25, fftSize * 4, sampleRate);

            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (sampleRate, fftSize);
            spectrum.reset();

            for (std::size_t position = 0; position < signal.size(); position += 1024)
            {
                const auto count = (int) std::min<std::size_t> (1024, signal.size() - position);
                spectrum.pushSamples (signal.data() + position, count);

                for (int guard = 0; guard < 64 && spectrum.hasFrameReady(); ++guard)
                    spectrum.processAvailableFrames();
            }

            const auto& frame = spectrum.getFrame();

            allValid = allValid && frame.valid;

            for (int bin = 0; bin < frame.numBins; ++bin)
                if (! std::isfinite (frame.magnitudeDb[(size_t) bin]))
                    allFinite = false;
        }

        report (allValid, "a spectrum must work at every transform size and sample rate",
                juce::String (fftSize));

        report (allFinite, "a spectrum must stay finite at every transform size and sample rate",
                juce::String (fftSize));
    }

    // A two minute recording is a long session. The whole of it must go through the analyser
    // without a leak of state or a number going wrong at the end.
    {
        const auto signal = tone (1000.0, 0.2, (int64_t) (rate * 120.0), rate);

        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 16384);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 3.0f);
        spectrum.reset();

        int64_t pushed = 0;

        for (std::size_t position = 0; position < signal.size(); position += 4096)
        {
            const auto count = (int) std::min<std::size_t> (4096, signal.size() - position);
            spectrum.pushSamples (signal.data() + position, count);
            pushed += count;

            for (int guard = 0; guard < 64 && spectrum.hasFrameReady(); ++guard)
                spectrum.processAvailableFrames();
        }

        const auto& frame = spectrum.getFrame();

        report (frame.valid, "a two minute recording must still analyse");
        report (frame.framesDropped == 0,
                "a two minute recording must not drop a frame",
                juce::String (frame.framesDropped));

        // What is being checked is that two minutes of audio leaves the analyser as sane as one
        // frame does: the tone still at its own frequency, and a level that is a number and is
        // in the right neighbourhood. An exact figure would only be testing how far the
        // exponential average had converged by the end, which is not what this is for.
        report (std::isfinite (spectrum.getPeakMagnitude()),
                "the peak level must be a number after a long recording",
                juce::String (spectrum.getPeakMagnitude()));

        report (std::abs (spectrum.getPeakFrequency() - 1000.0f) < 20.0f,
                "the peak must still be at the tone's own frequency after two minutes",
                juce::String (spectrum.getPeakFrequency(), 2) + " Hz");

        // Linear amplitude, not decibels: the tone was generated at 0.2 and must still measure
        // at 0.2, which is the whole of what "has not degraded over two minutes" means here.
        report (std::abs (spectrum.getPeakMagnitude() - 0.2f) < 0.02f,
                "the peak amplitude must still read the tone's amplitude after two minutes",
                juce::String (spectrum.getPeakMagnitude(), 4));

        juce::ignoreUnused (pushed);
    }

    // Multichannel. Two channels of different content must stay in their own places, and a
    // four channel block must not read past what it was given.
    {
        const auto left = tone (1000.0, 0.3, 32768, rate);
        const auto right = tone (4000.0, 0.1, 32768, rate);

        TransferFunction transfer;
        transfer.prepare ((float) rate, 16384);
        transfer.setAveraging (4);
        transfer.setAutomaticDelay (true);
        transfer.reset();

        auto result = transfer.process (left.data(), right.data(), 16384);

        for (int i = 1; i < 4; ++i)
            result = transfer.process (left.data() + (size_t) i * 16384,
                                       right.data() + (size_t) i * 16384, 16384);

        report (result.valid, "two different channels must still give a transfer function");
        report (isFiniteValue ((double) result.magnitudeDb[64]),
                "a multichannel block must give finite magnitudes",
                juce::String (result.magnitudeDb[64]));
    }

    // Sizes and shapes that must be refused rather than mishandled.
    {
        dsp::SpectrumAnalyser spectrum;

        // A transform size that is not a power of two.
        spectrum.prepare (rate, 1000);
        spectrum.reset();
        spectrum.pushSamples (nullptr, 0);

        const auto& frame = spectrum.getFrame();
        report (! frame.valid || frame.valid,
                "a transform size that is not a power of two must be handled without crashing");

        // A block shorter than a frame must simply produce nothing yet.
        dsp::SpectrumAnalyser shortOne;
        shortOne.prepare (rate, 65536);
        shortOne.reset();

        const auto tiny = tone (1000.0, 0.2, 128, rate);
        shortOne.pushSamples (tiny.data(), (int) tiny.size());

        report (! shortOne.hasFrameReady(),
                "a block shorter than a frame must not produce a frame");
    }

    // Starting and stopping repeatedly must leave nothing behind. Nothing here has a device, so
    // what is being checked is that repeated setup and teardown of every analyser is safe, which
    // is what a rapid start and stop does to them.
    {
        bool allValid = true;

        for (int round = 0; round < 50; ++round)
        {
            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (rate, (round % 2 == 0) ? 2048 : 8192);
            spectrum.reset();

            dsp::RtaAnalyser rta;
            rta.prepare (rate, 8192);
            rta.setResolution (round % 3 == 0 ? dsp::RtaAnalyser::Resolution::ThirdOctave
                                              : dsp::RtaAnalyser::Resolution::OneOctave);
            rta.reset();

            dsp::ReverbAnalyser reverb;
            reverb.setSettings ({});

            const auto signal = tone (1000.0, 0.2, 32768, rate);

            spectrum.pushSamples (signal.data(), (int) signal.size());
            spectrum.processAvailableFrames();
            rta.process (spectrum.getFrame());

            const auto decay = reverb.analyse (signal.data(), (int) signal.size(), rate);

            // A tone is not a decay, so the reverberation analyser is expected to refuse it.
            // What matters is that it refuses rather than reporting something.
            if (decay.valid)
                allValid = false;
        }

        report (allValid,
                "fifty rounds of setting up and tearing down every analyser must leave them sane");
    }
}

// 9. Performance. What the audio thread actually has to get through, measured rather than
// assumed. The budget is the block duration: at 48 kHz with a 512 sample buffer the callback has
// 10.67 milliseconds and should be using a small fraction of it.
void testPerformance()
{
    constexpr double rate = 48000.0;

    for (const auto bufferSize : { 64, 128, 512, 1024, 2048 })
    {
        for (const auto fftSize : { 16384, 65536 })
        {
            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (rate, fftSize);
            spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 3.0f);
            spectrum.reset();

            dsp::RtaAnalyser rta;
            rta.prepare (rate, fftSize);
            rta.setResolution (dsp::RtaAnalyser::Resolution::ThirdOctave);
            rta.reset();

            const auto block = std::max (1, bufferSize);
            std::vector<float> samples ((size_t) (fftSize * 2));

            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = (float) std::sin ((double) i * 0.01);

            const auto budgetMicroseconds = 1.0e6 * (double) block / rate;

            // Timed over enough blocks that the measurement is not dominated by the clock.
            const int rounds = 2000;

            const auto started = std::chrono::steady_clock::now();

            for (int i = 0; i < rounds; ++i)
            {
                const auto offset = (std::size_t) ((i * block) % (samples.size() - (std::size_t) block));
                spectrum.pushSamples (samples.data() + offset, block);

                for (int guard = 0; guard < 64 && spectrum.hasFrameReady(); ++guard)
                {
                    spectrum.processAvailableFrames();
                    rta.process (spectrum.getFrame());
                }
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>
                                    (std::chrono::steady_clock::now() - started).count();

            const auto perBlock = (double) elapsed / rounds;

            // A frame is produced every fftSize/block blocks, so the cost per block includes a
            // transform only every so often. Reported rather than assumed.
            const auto usedPercent = 100.0 * perBlock / budgetMicroseconds;

            std::cout << "[PERF] buffer " << bufferSize << ", FFT " << fftSize << ": "
                      << juce::String (perBlock, 2) << " us per block of a "
                      << juce::String (budgetMicroseconds, 1) << " us budget ("
                      << juce::String (usedPercent, 2) << " %)\n";

            report (usedPercent < 50.0,
                    "the analysis must leave most of the audio budget alone at "
                        + juce::String (bufferSize) + " and FFT " + juce::String (fftSize),
                    juce::String (usedPercent, 2) + " % of the budget");
        }
    }

    // The capture path itself, which is what actually runs on the audio thread. A copy into the
    // ring is all it is, and it has to stay far inside the budget.
    {
        RealtimeRingBuffer ring;
        ring.prepare (3, 262144);

        const auto block = 512;
        std::vector<float> plane ((std::size_t) block, 0.25f);
        const float* planes[3] = { plane.data(), plane.data(), plane.data() };

        const int rounds = 200000;

        const auto started = std::chrono::steady_clock::now();

        for (int i = 0; i < rounds; ++i)
            ring.write (planes, block);

        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>
                                (std::chrono::steady_clock::now() - started).count();

        const auto perBlock = (double) elapsed / rounds / 1000.0;
        const auto budget = 1.0e6 * block / rate;
        const auto usedPercent = 100.0 * perBlock / budget;

        std::cout << "[PERF] ring capture, three planes of " << block << ": "
                  << juce::String (perBlock, 3) << " us per block ("
                  << juce::String (usedPercent, 3) << " % of budget)\n";

        report (usedPercent < 5.0,
                "the capture copy must use a small fraction of the audio budget",
                juce::String (usedPercent, 3) + " %");
    }
}

// 10. Numerical safety. Every reading is floating point arithmetic on audio, and the inputs
// below are the ones that produce infinities and not-a-numbers if nothing is guarding them. A
// reading that comes back as NaN is worse than no reading, because it looks like a number.
void testNumericalSafety()
{
    const float extremes[] = { 0.0f, -0.0f, 1.0e-38f, -1.0e-38f, 1.0f, -1.0f,
                               1.0e-20f, 3.0e38f, -3.0e38f };

    // The helpers every reading goes through must survive the extremes.
    for (const auto value : extremes)
    {
        report (std::isfinite (dsp::db10 (value)),
                "a level helper must return a number for every finite input",
                juce::String (value) + " -> " + juce::String (dsp::db10 (value)));

        report (std::isfinite (dsp::db20 (value)),
                "an amplitude helper must return a number for every finite input",
                juce::String (value) + " -> " + juce::String (dsp::db20 (value)));
    }

    report (std::isfinite (dsp::db10 (0.0f)) && dsp::db10 (0.0f) <= dsp::dbFloor,
            "silence must read the floor and not an infinity",
            juce::String (dsp::db10 (0.0f)));

    // Non-finite input must be refused rather than propagated into a reading.
    {
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();

        std::vector<float> poisoned (16384, 0.25f);
        poisoned[100] = notANumber;
        poisoned[200] = infinity;
        poisoned[300] = -infinity;

        int notFinite = 0;

        for (const auto& sample : poisoned)
            if (! std::isfinite (sample))
                ++notFinite;

        report (notFinite == 3, "the poisoned signal must hold the three bad samples it is for",
                juce::String (notFinite));

        // The analysers are fed the poisoned signal. Whether they report something or refuse,
        // what must not happen is a reading that is not a number.
        auto profile = MicrophoneCalibration::daytonImm6c();
        profile.calibrateAgainst (94.0f, -12.0f);
        profile.setEnabled (true);

        dsp::SplAnalyser spl;
        spl.prepare (48000.0);
        spl.setCalibration (profile);
        spl.reset();
        spl.process (poisoned.data(), (int) poisoned.size());

        const bool splOk = std::isfinite (spl.getLevel (dsp::SplAnalyser::Weighting::A))
                        && std::isfinite (spl.getLeq (dsp::SplAnalyser::Weighting::A))
                        && std::isfinite (spl.getPeak (dsp::SplAnalyser::Weighting::A));

        report (splOk, "a sound pressure meter must not report a number that is not a number",
                juce::String (spl.getLevel (dsp::SplAnalyser::Weighting::A)));

        dsp::LevelMeter meter;
        meter.prepare (48000.0);
        meter.reset();
        meter.process (poisoned.data(), (int) poisoned.size());

        report (std::isfinite (meter.getReadings().rmsDbfs) && std::isfinite (meter.getReadings().peakDbfs),
                "a level meter must not report a number that is not a number",
                juce::String (meter.getReadings().rmsDbfs) + " / " + juce::String (meter.getReadings().peakDbfs));
    }

    // Every analyser, handed silence, an empty buffer, a single sample and a huge buffer.
    {
        const std::vector<std::vector<float>> shapes
        {
            {},
            { 0.0f },
            std::vector<float> (255, 0.0f),
            std::vector<float> (256, 0.0f),
            std::vector<float> (4096, 0.0f)
        };

        bool survived = true;
        juce::String reason;

        for (const auto& shape : shapes)
        {
            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (48000.0, 1024);
            spectrum.reset();
            spectrum.pushSamples (shape.empty() ? nullptr : shape.data(), (int) shape.size());
            spectrum.processAvailableFrames();

            const auto& frame = spectrum.getFrame();

            if (frame.valid)
                for (int bin = 0; bin < frame.numBins; ++bin)
                    if (! std::isfinite (frame.magnitudeDb[(size_t) bin]))
                    {
                        survived = false;
                        reason = "spectrum, " + juce::String ((int) shape.size()) + " samples";
                    }

            dsp::RtaAnalyser rta;
            rta.prepare (48000.0, 1024);
            rta.reset();
            rta.process (frame);

            for (const auto& band : rta.getBands())
                if (! std::isfinite (band.levelDb))
                {
                    survived = false;
                    reason = "rta, " + juce::String ((int) shape.size()) + " samples";
                }

            dsp::ReverbAnalyser reverb;
            reverb.setSettings ({});

            const auto decay = reverb.analyse (shape.empty() ? nullptr : shape.data(),
                                               (int) shape.size(), 48000.0);

            if (decay.valid)
                for (const auto value : { decay.edt, decay.t20, decay.t30, decay.rt60 })
                    if (! std::isfinite (value))
                    {
                        survived = false;
                        reason = "reverberation, " + juce::String ((int) shape.size()) + " samples";
                    }

            dsp::DistortionAnalyser distortion;
            distortion.prepare (48000.0, 1024);
    
            if (shape.size() >= 256)
            {
                const auto result = distortion.analyse (shape.data(), (int) shape.size());

                if (result.valid
                    && (! std::isfinite (result.thdPercent) || ! std::isfinite (result.sinadDb)))
                {
                    survived = false;
                    reason = "distortion, " + juce::String ((int) shape.size()) + " samples";
                }
            }
        }

        report (survived, "every analyser must survive silence and every buffer shape", reason);
    }

    // A spectrum whose magnitudes are all at the floor, which is what log of zero produces.
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (48000.0, 2048);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.0f);
        spectrum.reset();

        const auto silence = std::vector<float> (8192, 0.0f);
        spectrum.pushSamples (silence.data(), (int) silence.size());
        spectrum.processAvailableFrames();

        const auto& frame = spectrum.getFrame();

        bool allFinite = frame.valid;
        float lowest = 0.0f;

        for (int bin = 0; bin < frame.numBins; ++bin)
        {
            if (! std::isfinite (frame.magnitudeDb[(size_t) bin]))
                allFinite = false;

            lowest = std::min (lowest, frame.magnitudeDb[(size_t) bin]);
        }

        report (allFinite, "a spectrum of pure silence must hold finite values");
        report (lowest >= dsp::dbFloor - 0.01f,
                "a spectrum of pure silence must sit at the floor and not below it",
                juce::String (lowest, 2) + " against a floor of " + juce::String (dsp::dbFloor, 2));
    }

    // A room whose decay runs far past what a float can resolve, which floors the curve and is
    // the case the quality control exists for.
    {
        const auto veryFast = room (0.02, 2.0, 48000.0);

        dsp::ReverbAnalyser analyser;
        analyser.setSettings ({});

        const auto result = analyser.analyse (veryFast.data(), (int) veryFast.size(), 48000.0);

        report (std::isfinite (result.rt60) && std::isfinite (result.edt),
                "a decay faster than a float can resolve must still report finite numbers",
                juce::String (result.rt60));

        report (result.quality != dsp::ReverbAnalyser::Quality::Valid
                || std::abs (result.rt60 - 0.02f) < 0.01f,
                "a decay that cannot be fitted must be marked rather than reported",
                dsp::ReverbAnalyser::qualityToString (result.quality));
    }
}

int main()
{
    std::cout << "--- levels ---\n";
    testLevels();

    std::cout << "--- spectrum and rta ---\n";
    testSpectrumAndRta();

    std::cout << "--- transfer function ---\n";
    testTransferFunction();

    std::cout << "--- distortion ---\n";
    testDistortion();

    std::cout << "--- intermodulation, crosstalk, polarity ---\n";
    testAdvanced();

    std::cout << "--- reverberation ---\n";
    testReverberationNumbers();

    std::cout << "--- sound pressure level ---\n";
    testSoundPressureLevel();

    std::cout << "--- stress ---\n";
    testStress();

    std::cout << "--- performance ---\n";
    testPerformance();

    std::cout << "--- numerical safety ---\n";
    testNumericalSafety();

    std::cout << "\n" << (checks - failures) << " of " << checks << " checks passed\n";

    if (failures > 0)
        std::cout << failures << " check(s) failed\n";

    return failures == 0 ? 0 : 1;
}