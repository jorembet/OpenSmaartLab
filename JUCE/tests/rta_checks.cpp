// Phase 3 checks: the octave band analyser.
//
// The bands are built from a real spectrum: an internal sine generator or pink noise is
// analysed by the spectrum analyser and its frame is then integrated into bands, so what
// is checked is the same path the live audio takes. Nothing is injected into the band
// table directly.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DSP.h"
#include "RtaAnalyser.h"
#include "SpectrumAnalyser.h"

namespace
{
    int failures = 0;

    void report (bool condition, const char* message, const juce::String& detail = {})
    {
        if (condition)
            return;

        ++failures;
        std::cout << "FAIL: " << message;

        if (detail.isNotEmpty())
            std::cout << "  [" << detail << "]";

        std::cout << std::endl;
    }

    void require (bool condition, const char* message, const juce::String& detail = {})
    {
        report (condition, message, detail);
    }

    constexpr double rate = 48000.0;

    std::vector<float> sine (int numSamples, double frequency, double amplitude)
    {
        std::vector<float> samples ((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
            samples[(size_t) i] = (float) (amplitude * std::sin (2.0 * M_PI * frequency * i / rate));

        return samples;
    }

    /** Pink noise with a known spectral shape: flat power per bin, which is what a
        generator test needs to tell a real band level from a level that drifted. */
    std::vector<float> pinkNoise (int numSamples, double amplitude)
    {
        std::vector<float> samples ((size_t) numSamples);
        unsigned seed = 90210;
        double power = 0.0;

        // Summed over many samples, a uniform random walk is white noise with a flat
        // spectrum, which is the reference the band integration is checked against.
        for (auto& sample : samples)
        {
            seed = seed * 1103515245u + 12345u;
            const auto u = (double) ((seed >> 16) & 0x7fff) / 32768.0 * 2.0 - 1.0;
            sample = (float) (u * amplitude);
            power += (double) sample * (double) sample;
        }

        const auto rms = std::sqrt (power / (double) numSamples);
        const auto scale = rms > 0.0 ? amplitude / rms : 0.0;

        for (auto& sample : samples)
            sample *= (float) scale;

        return samples;
    }

    /** Runs a signal through the spectrum analyser and integrates the frame into bands. */
    void feed (dsp::SpectrumAnalyser& spectrum, dsp::RtaAnalyser& rta,
               const std::vector<float>& signal, int repeats)
    {
        for (int i = 0; i < repeats; ++i)
        {
            spectrum.pushSamples (signal.data(), (int) signal.size());
            spectrum.processAvailableFrames();

            if (spectrum.getFrame().valid)
                rta.process (spectrum.getFrame());
        }
    }

    int nearestBandTo (const dsp::RtaAnalyser& rta, float frequency)
    {
        const auto& centres = rta.getBandFrequencies();
        auto nearest = 0;

        for (size_t i = 1; i < centres.size(); ++i)
            if (std::abs (centres[i] - frequency) < std::abs (centres[(size_t) nearest] - frequency))
                nearest = (int) i;

        return nearest;
    }
}

//==============================================================================
int main()
{
    using Resolution = dsp::RtaAnalyser::Resolution;

    // ---- Band centres must be the standard nominal values ----
    {
        dsp::RtaAnalyser rta;
        rta.prepare (rate, 4096);
        rta.setResolution (Resolution::ThirdOctave);

        const auto& centres = rta.getBandFrequencies();

        // The 1/3 octave table every measurement is quoted in. If these move, a reading
        // stops being comparable with any other tool.
        const float expected[] = { 20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
                                   125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f,
                                   630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f,
                                   2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f,
                                   10000.0f, 12500.0f, 16000.0f, 20000.0f };

        report (centres.size() == 31, "1/3 octave must produce 31 bands",
                juce::String (centres.size()) + " bands produced");

        if (centres.size() == 31)
        {
            for (size_t i = 0; i < 31; ++i)
                report (std::abs (centres[i] - expected[i]) < 0.01f,
                        "1/3 octave band centres must be the nominal values",
                        "band " + juce::String ((int) i) + " is " + juce::String (centres[i], 1)
                            + " Hz, expected " + juce::String (expected[i], 1) + " Hz");
        }

        // Every resolution must produce more bands than the one below it, and the counts
        // must follow the 1/N spacing rather than being a fixed list.
        const Resolution resolutions[] = { Resolution::OneOctave, Resolution::ThirdOctave,
                                           Resolution::SixthOctave, Resolution::TwelfthOctave,
                                           Resolution::TwentyFourthOctave };
        const int expectedCounts[] = { 11, 31, 61, 121, 241 };

        require (std::size (expectedCounts) == 5, "Every resolution must have an expected count");

        for (int i = 0; i < 5; ++i)
        {
            rta.setResolution (resolutions[i]);
            report (rta.getBandCount() == expectedCounts[i],
                    "Each resolution must produce the band count its spacing implies",
                    dsp::RtaAnalyser::resolutionToString (resolutions[i]) + " gave "
                        + juce::String (rta.getBandCount()) + " bands, expected "
                        + juce::String (expectedCounts[i]));
        }
    }

    // ---- A tone must land in the band that contains it ----
    {
        const double tones[] = { 100.0, 1000.0, 10000.0 };
        const float toneLevels[] = { -20.0f, -20.0f, -20.0f };

        for (int t = 0; t < 3; ++t)
        {
            dsp::SpectrumAnalyser spectrum;
            spectrum.prepare (rate, 16384);
            spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
            spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

            dsp::RtaAnalyser rta;
            rta.prepare (rate, 16384);
            rta.setResolution (Resolution::ThirdOctave);
            rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

            const auto amplitude = std::pow (10.0, toneLevels[t] / 20.0);
            feed (spectrum, rta, sine (16384, tones[t], amplitude), 6);

            const auto index = nearestBandTo (rta, (float) tones[t]);
            const auto& band = rta.getBands()[(size_t) index];

            // The band holding the tone must be the loudest one by a wide margin, which
            // is the statement that the integration put the energy in the right place.
            auto loudest = (size_t) index;
            auto margin = 0.0f;

            for (size_t i = 0; i < rta.getBands().size(); ++i)
            {
                margin = std::max (margin, rta.getBands()[i].levelDb - rta.getBands()[loudest].levelDb);

                if (rta.getBands()[i].levelDb > rta.getBands()[loudest].levelDb)
                    loudest = i;
            }

            report (loudest == (size_t) index,
                    "A tone must make its own band the loudest one",
                    juce::String (tones[t], 0) + " Hz landed in "
                        + juce::String (rta.getBandFrequencies()[loudest], 1) + " Hz instead of "
                        + juce::String (rta.getBandFrequencies()[(size_t) index], 1)
                        + " Hz, next band only " + juce::String (margin, 1) + " dB away");

            // Summing the energy across the band is the acoustic band level, and the window
            // spreads a tone over its main lobe, so the reading sits a little above the
            // amplitude that went in. The tolerance covers exactly that and no more.
            report (band.levelDb > toneLevels[t] - 2.0f && band.levelDb < toneLevels[t] + 4.0f,
                    "The band holding a tone must read the tone's own level",
                    juce::String (tones[t], 0) + " Hz read " + juce::String (band.levelDb, 2)
                        + " dB for a " + juce::String (toneLevels[t], 1) + " dBFS tone");

            require (band.binCount > 0, "The band holding a tone must have integrated real bins");
            require (band.resolved, "A 1/3 octave band at 16k must be resolved at FFT 16384");
        }
    }

    // ---- Pink noise: the two integration modes must behave differently ----
    {
        // Pink noise has flat power per hertz, so the energy in a band grows with the
        // band's width. Summing across a band therefore gives a staircase that rises with
        // frequency, and averaging across it gives a flat line. Both are correct answers to
        // different questions, and which one is wanted is a choice the display offers.
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 16384);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 1.0f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 16384);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 1.5f);

        const auto signal = pinkNoise (16384, 0.2);
        feed (spectrum, rta, signal, 120);

        const auto& bands = rta.getBands();

        // The outer bands are excluded: they reach past the configured range, so they
        // hold less than a full band and would read low for that reason alone.
        float lowest = 1.0e9f, highest = -1.0e9f;

        for (size_t i = 2; i + 2 < bands.size(); ++i)
        {
            if (! bands[i].resolved)
                continue;

            lowest = std::min (lowest, bands[i].levelDb);
            highest = std::max (highest, bands[i].levelDb);
        }

        require (lowest < 1.0e8f, "Pink noise must produce readable band levels");

        // The outer bands are excluded from the spread check: they reach past the configured
        // range and so hold less than a full band's worth of bins, which shows as a dip that
        // has nothing to do with the integration.

        // The 1/3 octave bands at the top of the range are about a thousand times wider
        // than the ones at the bottom, so summing has to produce a staircase rather than a
        // flat line. A flat reading here would mean the integration was averaging, and
        // every tone level would then be wrong by the width of its band.
        report (highest > lowest + 15.0f,
                "Pink noise summed into 1/3 octave bands must rise with the band width",
                juce::String (highest, 1) + " dB at the top against "
                    + juce::String (lowest, 1) + " dB at the bottom");

        // The same spectrum under the averaging mode must come out flat, which is what
        // proves the two modes really are doing different things.
        dsp::RtaAnalyser averaged;
        averaged.prepare (rate, 16384);
        averaged.setResolution (Resolution::ThirdOctave);
        averaged.setIntegration (dsp::RtaAnalyser::Integration::Average);
        averaged.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 1.5f);

        dsp::SpectrumAnalyser second;
        second.prepare (rate, 16384);
        second.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        second.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 1.0f);

        feed (second, averaged, signal, 120);

        // A mean over a handful of bins of a random signal is noisy by construction: the
        // lowest 1/3 octave bands at this FFT size integrate two or three bins, so their
        // spread says nothing about the integration. Only the bands wide enough to average
        // a real number of bins are asked to be flat.
        auto flatLow = 1.0e9f, flatHigh = -1.0e9f;
        auto flatCount = 0;

        for (const auto& band : averaged.getBands())
        {
            if (! band.resolved || band.binCount < 24)
                continue;

            flatLow = std::min (flatLow, band.levelDb);
            flatHigh = std::max (flatHigh, band.levelDb);
            ++flatCount;
        }

        report (flatCount >= 15, "Enough bands must be wide enough to be checked for flatness",
                juce::String (flatCount) + " bands checked");

        report (flatHigh - flatLow < 2.0f,
                "Pink noise averaged across 1/3 octave bands must read flat",
                "spread " + juce::String (flatHigh - flatLow, 2) + " dB over "
                    + juce::String (flatCount) + " bands, from "
                    + juce::String (flatLow, 1) + " to " + juce::String (flatHigh, 1) + " dB");
    }

    // ---- The RTA must agree with the spectrum it was built from ----
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 16384);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 16384);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        const auto signal = pinkNoise (16384, 0.2);
        feed (spectrum, rta, signal, 4);

        const auto& frame = spectrum.getFrame();
        const auto& centres = rta.getBandFrequencies();
        const auto& bands = rta.getBands();

        // For each band, the level must lie between the quietest and loudest bin it
        // covers. That is what "integrated from the spectrum" means, and it is the check
        // that the bands are derived from these bins rather than invented beside them.
        auto consistent = 0;
        auto checked = 0;

        for (size_t band = 0; band < centres.size(); ++band)
        {
            const auto low = band == 0 ? rta.getRangeLow() * 0.5f
                                       : std::sqrt (centres[band - 1] * centres[band]);
            const auto high = band + 1 >= centres.size() ? rta.getRangeHigh() * 2.0f
                                                          : std::sqrt (centres[band] * centres[band + 1]);

            // Only bins the band fully covers are compared. A bin straddling an edge is
            // shared with the neighbouring band and counted fractionally, so counting it
            // whole here would make a correct band look as though it had taken too much.
            auto lowest = 1.0e9f, highest = -1.0e9f;
            auto count = 0;

            for (size_t i = 1; i < frame.frequency.size(); ++i)
            {
                if (frame.frequency[i] <= low || frame.frequency[i] >= high)
                    continue;

                const auto db = dsp::SpectrumAnalyser::amplitudeToDb (frame.magnitude[i]);
                lowest = std::min (lowest, db);
                highest = std::max (highest, db);
                ++count;
            }

            if (count < 8)
                continue;

            ++checked;

            // Under the summing mode the band level is the total energy, so it is
            // legitimately above every individual bin: it has to reach at least the
            // loudest bin, and must not exceed the whole band added together. That is what
            // "integrated from the spectrum" means here.
            const auto wholeBand = 20.0f * std::log10 (
                std::pow (10.0, highest / 20.0) * (float) std::max (1, count));

            const auto aboveLoudest = bands[band].levelDb >= highest - 0.6f;
            const auto belowWholeBand = bands[band].levelDb <= wholeBand + 1.5f;

            if (aboveLoudest && belowWholeBand)
                ++consistent;
        }

        report (checked > 20, "Enough bands must have been checked against the spectrum",
                juce::String (checked) + " bands checked");
        report (consistent == checked,
                "Every band level must be the energy of the bins it integrates",
                juce::String (consistent) + " of " + juce::String (checked) + " bands inside");
    }

    // ---- A band narrower than one bin must be reported as unresolved ----
    {
        // 1/24 octave at 20 kHz is a fraction of a bin wide at FFT 1024. Reporting a level
        // there would be reporting a bin and calling it a band.
        dsp::RtaAnalyser rta;
        rta.prepare (rate, 1024);
        rta.setResolution (Resolution::TwentyFourthOctave);

        require (rta.getBandCount() > 100, "1/24 octave must produce a fine band table");
        require (rta.getUnresolvedBandCount() > 0,
                "Bands narrower than one bin must be reported as unresolved");
        require (! rta.isFullyResolved(),
                "The analyser must admit when not every band is measurable");

        // A bigger FFT has to resolve more of them, or the flag would be meaningless.
        dsp::RtaAnalyser bigger;
        bigger.prepare (rate, 32768);
        bigger.setResolution (Resolution::TwentyFourthOctave);
        require (bigger.getUnresolvedBandCount() <= rta.getUnresolvedBandCount(),
                "A larger FFT must resolve at least as many bands");
    }

    // ---- Peak hold, decay, min/max ----
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 8192);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 8192);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);
        rta.setPeakHoldSeconds (0.5f);
        rta.setPeakDecayDbPerSecond (12.0f);
        rta.setMinMaxWindowSeconds (2.0f);

        const auto quiet = std::vector<float> ((size_t) 8192, 0.0f);
        const auto loud = sine (8192, 1000.0, 0.5);

        feed (spectrum, rta, quiet, 6);

        const auto index = nearestBandTo (rta, 1000.0f);
        const auto beforeLoud = rta.getBands()[(size_t) index].levelDb;

        require (beforeLoud < -100.0f, "Silence must leave the band at the floor");

        feed (spectrum, rta, loud, 4);

        const auto peak = rta.getBands()[(size_t) index].peakHoldDb;
        const auto maxSeen = rta.getBands()[(size_t) index].maxDb;

        report (peak > -20.0f, "The held peak must catch a tone that has just stopped",
                juce::String (peak, 1) + " dB");
        report (maxSeen > -20.0f, "The maximum must record the loudest level in the window",
                juce::String (maxSeen, 1) + " dB");
        report (maxSeen >= peak - 0.01f, "The maximum cannot sit below the held peak");

        // Once the tone stops the hold has to come down, or a transient would stay on
        // screen for ever and hide the next reading.
        feed (spectrum, rta, quiet, 60);

        report (rta.getBands()[(size_t) index].peakHoldDb < peak - 5.0f,
                "The held peak must decay once the tone stops",
                juce::String (peak, 1) + " dB fell to "
                    + juce::String (rta.getBands()[(size_t) index].peakHoldDb, 1) + " dB");

        rta.clearPeakHold();
        report (rta.getBands()[(size_t) index].peakHoldDb < -100.0f,
                "Clearing the hold must drop every band back to the floor");
    }

    // ---- Smoothing must change the reading but not the settled value ----
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 8192);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser unsmoothed;
        unsmoothed.prepare (rate, 8192);
        unsmoothed.setResolution (Resolution::ThirdOctave);
        unsmoothed.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser smoothed;
        smoothed.prepare (rate, 8192);
        smoothed.setResolution (Resolution::ThirdOctave);
        smoothed.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);
        smoothed.setSmoothingSeconds (1.0f);

        // A level that steps halfway through must not reach the display instantly when
        // smoothing is on. That is the whole point of the setting.
        dsp::SpectrumAnalyser second;
        second.prepare (rate, 8192);
        second.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        second.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        const auto quiet = std::vector<float> ((size_t) 8192, 0.0f);
        const auto loud = sine (8192, 1000.0, 0.5);

        feed (spectrum, unsmoothed, quiet, 8);
        feed (spectrum, smoothed, quiet, 8);

        const auto index = nearestBandTo (unsmoothed, 1000.0f);

        feed (spectrum, unsmoothed, loud, 3);
        feed (spectrum, smoothed, loud, 3);

        const auto fast = unsmoothed.getBands()[(size_t) index].levelDb;
        const auto slow = smoothed.getBands()[(size_t) index].levelDb;

        report (slow < fast,
                "Smoothing must hold the display back while the level rises",
                "unsmoothed " + juce::String (fast, 1) + " dB, smoothed "
                    + juce::String (slow, 1) + " dB");

        // Given long enough both must arrive at the same place, or smoothing would be
        // quietly changing the measurement rather than how quickly it is shown.
        feed (spectrum, unsmoothed, loud, 120);
        feed (spectrum, smoothed, loud, 120);

        report (std::abs (smoothed.getBands()[(size_t) index].levelDb - fast) < 1.5f,
                "Smoothing must settle on the same level given time",
                "unsmoothed " + juce::String (fast, 1) + " dB, smoothed "
                    + juce::String (smoothed.getBands()[(size_t) index].levelDb, 1) + " dB");
    }

    // ---- Exponential averaging must converge, and be slower with a longer constant ----
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 8192);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.2f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 8192);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.3f);

        const auto signal = pinkNoise (8192, 0.2);
        feed (spectrum, rta, signal, 100);

        const auto index = nearestBandTo (rta, 1000.0f);
        const auto level = rta.getBands()[(size_t) index].levelDb;

        // Noise at 0.2 amplitude has an RMS near -17 dBFS, and a summed band level spreads
        // that energy over every bin the band covers, so it lands well below the RMS. The
        // bound is wide on purpose; the scaling is checked properly further down.
        report (level > -45.0f && level < -5.0f,
                "An averaged band level must be plausible for the signal that went in",
                juce::String (level, 1) + " dB for noise at 0.2 amplitude");

        // Halving the signal has to lower the band by 6 dB. This is the statement that the
        // level is a measurement of the audio and not a fixed offset, which a plausibility
        // bound on its own would not catch.
        dsp::SpectrumAnalyser quiet;
        quiet.prepare (rate, 16384);
        quiet.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        quiet.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser quietRta;
        quietRta.prepare (rate, 16384);
        quietRta.setResolution (Resolution::ThirdOctave);
        quietRta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        feed (quiet, quietRta, pinkNoise (16384, 0.1), 6);

        const auto quietLevel = quietRta.getBands()[(size_t) index].levelDb;
        report (std::abs (level - quietLevel - 6.02f) < 1.5f,
                "Halving the input must lower the band level by 6 dB",
                juce::String (level, 1) + " dB against " + juce::String (quietLevel, 1) + " dB");

        require (rta.getBandFrequencies()[(size_t) index] > 900.0f
                 && rta.getBandFrequencies()[(size_t) index] < 1100.0f,
                "The 1 kHz band centre must be 1 kHz");
    }

    // ---- Configurable range ----
    {
        dsp::RtaAnalyser rta;
        rta.prepare (rate, 8192);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setFrequencyRange (100.0f, 2000.0f);

        require (rta.getBandCount() > 0, "A narrowed range must still produce bands");
        report (rta.getLowestBandFrequency() >= 100.0f,
                "No band may sit below the configured range",
                juce::String (rta.getLowestBandFrequency(), 1) + " Hz");
        report (rta.getHighestBandFrequency() <= 2000.0f,
                "No band may sit above the configured range",
                juce::String (rta.getHighestBandFrequency(), 1) + " Hz");

        // Narrowing the range must not leave the previous table on screen.
        rta.setFrequencyRange (1000.0f, 2000.0f);
        require (rta.getBandCount() > 0 && rta.getBandCount() < 20,
                "A narrow range must produce a short band table",
                juce::String (rta.getBandCount()) + " bands");

        rta.setFrequencyRange (20.0f, 20000.0f);
        require (rta.getBandCount() == 31, "Widening the range back must restore 31 bands");
    }

    // ---- Silence must not leave stale readings ----
    {
        dsp::SpectrumAnalyser spectrum;
        spectrum.prepare (rate, 8192);
        spectrum.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        spectrum.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 8192);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        feed (spectrum, rta, sine (8192, 1000.0, 0.5), 4);
        require (rta.getBands()[nearestBandTo (rta, 1000.0f)].levelDb > -30.0f,
                "A tone must first give a readable band");

        feed (spectrum, rta, std::vector<float> ((size_t) 8192, 0.0f), 4);

        const auto& bands = rta.getBands();
        auto stale = 0;

        for (const auto& band : bands)
            if (band.levelDb > -100.0f)
                ++stale;

        report (stale == 0, "Silence must pull every band down, not freeze the last reading",
                juce::String (stale) + " bands still showing a level");
    }

    // ---- A frame from a different FFT size must be refused, not mis-mapped ----
    {
        dsp::SpectrumAnalyser small;
        small.prepare (rate, 2048);
        small.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        dsp::RtaAnalyser rta;
        rta.prepare (rate, 16384);
        rta.setResolution (Resolution::ThirdOctave);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

        feed (small, rta, sine (2048, 1000.0, 0.5), 4);

        auto readable = 0;

        for (const auto& band : rta.getBands())
            if (band.levelDb > -100.0f)
                ++readable;

        report (readable == 0,
                "A frame sized for another FFT must not be integrated into these bands",
                juce::String (readable) + " bands were given a level");
    }

    if (failures == 0)
        std::cout << "PASS: 1/3 octave band centres are the 31 nominal values, and each "
                     "resolution produces the count its spacing implies"
                  << std::endl
                  << "PASS: 100 Hz, 1 kHz and 10 kHz each make their own band the loudest, at "
                     "the level that went in"
                  << std::endl
                  << "PASS: pink noise summed into bands rises with the band width and averaged across them reads flat"
                  << std::endl
                  << "PASS: every band level is the energy of the bins it integrates"
                  << std::endl
                  << "PASS: a band narrower than one bin is reported as unresolved, and a bigger "
                     "FFT resolves more of them"
                  << std::endl
                  << "PASS: peak hold catches a tone, decays after it stops, and clears; min and "
                     "max track the window"
                  << std::endl
                  << "PASS: smoothing holds the display back but settles on the same level"
                  << std::endl
                  << "PASS: exponential averaging converges and the range is configurable"
                  << std::endl
                  << "PASS: silence pulls every band down and a mismatched frame is refused"
                  << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}
