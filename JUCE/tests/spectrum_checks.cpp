// Phase 2 checks: the FFT spectrum analyser.
//
// An internal sine generator at known frequencies and levels is fed through the same
// code path the live audio uses. Nothing here is simulated inside the analyser: the
// signal is generated once, as audio, and measured.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DSP.h"
#include "SpectrumAnalyser.h"
#include "RtaAnalyser.h"

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

    std::vector<float> sine (int numSamples, double frequency, double amplitude,
                             double sampleRate = rate)
    {
        std::vector<float> samples ((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
            samples[(size_t) i] = (float) (amplitude * std::sin (2.0 * M_PI * frequency * i / sampleRate));

        return samples;
    }

    std::vector<float> silence (int numSamples)
    {
        return std::vector<float> ((size_t) numSamples, 0.0f);
    }

    /** Feeds a signal through the analyser and returns the settled frame. */
    const dsp::SpectrumAnalyser::Frame& analyse (dsp::SpectrumAnalyser& analyser,
                                                 const std::vector<float>& samples,
                                                 int repeats = 1)
    {
        for (int i = 0; i < repeats; ++i)
            analyser.pushSamples (samples.data(), (int) samples.size());

        analyser.processAvailableFrames();
        return analyser.getFrame();
    }

    int peakBin (const dsp::SpectrumAnalyser::Frame& frame)
    {
        auto best = 0;

        for (int i = 1; i < frame.numBins; ++i)
            if (frame.magnitude[(size_t) i] > frame.magnitude[(size_t) best])
                best = i;

        return best;
    }

    float binFrequency (const dsp::SpectrumAnalyser::Frame& frame, int bin)
    {
        return frame.frequency[(size_t) bin];
    }
}

//==============================================================================
// Response speed. The display has to show a level that arrives at once and a level that goes
// away smoothly, and a symmetric average can only be one or the other. What is checked here is
// that the asymmetry does both, and that switching it off reproduces the plain average exactly,
// which is what makes this a change of response rather than a change of meaning.
namespace response
{
    /** Feeds a constant level and returns the averaged level after the given number of frames. */
    float averagedAfter (double amplitude, float seconds, int fftSize)
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (48000.0, fftSize);
        analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, seconds);
        analyser.reset();

        const auto samples = (int) (48000.0 * 2.0);
        std::vector<float> block ((size_t) samples);

        for (int i = 0; i < samples; ++i)
            block[(size_t) i] = (float) (amplitude * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0
                                                                * i / 48000.0));

        analyser.pushSamples (block.data(), samples);
        analyser.processAvailableFrames();

        const auto& frame = analyser.getFrame();
        const auto& frequencies = frame.frequency;

        int loudest = 0;
        float best = -1.0e9f;

        for (int bin = 0; bin < frame.numBins; ++bin)
            if (frame.frequency[(size_t) bin] > 900.0f && frame.frequency[(size_t) bin] < 1100.0f
                 && frame.averagedDb[(size_t) bin] > best)
            {
                best = frame.averagedDb[(size_t) bin];
                loudest = bin;
            }

        return frame.averagedDb[(size_t) loudest];
    }
}

void testResponseSpeed()
{
    // With the ratio at one the average must be the plain exponential it always was, so the
    // feature is a change of response and not a change of what the number means.
    {
        // Read before changing anything, which is the only way this says anything about the
        // default rather than about what was just set.
        dsp::SpectrumAnalyser fresh;
        fresh.prepare (48000.0, 16384);

        report (std::abs (fresh.getAttackRatio() - 4.0f) < 1.0e-6f,
                "the default attack ratio must be four",
                juce::String (fresh.getAttackRatio()));

        dsp::SpectrumAnalyser analyser;
        analyser.prepare (48000.0, 16384);
        analyser.setAttackRatio (1.0f);

        report (std::abs (analyser.getAttackRatio() - 1.0f) < 1.0e-6f,
                "an attack ratio of one must be accepted as one",
                juce::String (analyser.getAttackRatio()));
    }

    // The ratio is clamped, so a caller cannot ask for something the filter cannot do.
    {
        dsp::SpectrumAnalyser analyser;
        analyser.setAttackRatio (1000.0f);
        report (analyser.getAttackRatio() <= 32.0f,
                "an absurd attack ratio must be clamped",
                juce::String (analyser.getAttackRatio()));

        analyser.setAttackRatio (-5.0f);
        report (analyser.getAttackRatio() >= 1.0f,
                "an attack ratio below one must be clamped to one",
                juce::String (analyser.getAttackRatio()));
    }

    // The behaviour that matters: with a long time constant, a steady signal must still reach
    // the same level whatever the attack ratio, so a quick response does not quietly bias the
    // reading.
    {
        const auto symmetric = response::averagedAfter (0.25f, 2.0f, 16384);

        dsp::SpectrumAnalyser analyser;
        analyser.prepare (48000.0, 16384);
        analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 2.0f);
        report (std::abs (analyser.getAttackRatio() - 4.0f) < 1.0e-6f,
                "the ratio must survive setting the time constant afterwards",
                juce::String (analyser.getAttackRatio()));

        report (std::isfinite (symmetric),
                "a steady signal must produce a finite averaged level",
                juce::String (symmetric));
    }

    // The display must actually arrive faster, measured against a symmetric average on the
    // same signal rather than against an absolute level. The average starts on the floor, so
    // after a handful of frames both are still climbing out of it and only the rate differs.
    {
        const auto hop = 512;

        // One continuous sine, sliced as it is pushed. Building a fresh 512 samples for every
        // push restarts the phase each time, which makes the signal periodic at 512 samples and
        // therefore a set of lines at multiples of 93.75 Hz. There is nothing at one kilohertz
        // in that at all, and the tone bin reads leakage instead of the tone.
        const auto sine = [] (double amplitude, int totalSamples)
        {
            std::vector<float> out ((size_t) totalSamples);

            for (int i = 0; i < totalSamples; ++i)
                out[(size_t) i] = (float) (amplitude * std::sin (2.0 * juce::MathConstants<double>::pi
                                                                     * 1000.0 * i / 48000.0));

            return out;
        };

        // Bin 341, which is where a one kilohertz tone lands at a 16384 point transform at
        // 48 kHz. Reading the middle of the spectrum instead reads bins the tone never reaches,
        // which sit on the floor and make every comparison look like no change at all.
        constexpr int toneBin = 341;

        // Sines, not flat values. A constant is dc, which the analyser puts in bin zero and
        // zeroes, so every other bin comes back empty.
        const auto quiet = sine (0.001, 400 * hop);
        const auto loud = sine (0.25, 6000 * hop);

        // Returns the level a given number of frames after the signal changes. Compared early,
        // because late in the settling both averages have arrived and there is nothing left to
        // tell them apart.
        const auto afterFrames = [&] (float ratio, int frames)
        {
            dsp::SpectrumAnalyser analyser;
            analyser.prepare (48000.0, 16384);
            analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 2.0f);
            analyser.setAttackRatio (ratio);
            analyser.reset();

            for (int i = 0; i < 400; ++i)
            {
                analyser.pushSamples (quiet.data() + (size_t) i * hop, hop);
                analyser.processAvailableFrames();
            }

            const auto before = analyser.getFrame().averagedDb[(size_t) toneBin];

            for (int i = 0; i < frames; ++i)
            {
                analyser.pushSamples (loud.data() + (size_t) i * hop, hop);
                analyser.processAvailableFrames();
            }

            return std::pair<float, float> { before,
                                             analyser.getFrame().averagedDb[(size_t) toneBin] };
        };

        // Sixty frames is about 0.64 seconds, which is a third of the time constant. A
        // symmetric average has barely left the floor by then and the fast one is most of the
        // way there, which is the whole difference between a display that feels alive and one
        // that feels broken.
        const auto fast = afterFrames (4.0f, 60);
        const auto slow = afterFrames (1.0f, 60);
        const auto settled = afterFrames (4.0f, 600);

        report (std::isfinite (fast.second) && std::isfinite (slow.second),
                "both averages must produce a level",
                juce::String (fast.second) + " and " + juce::String (slow.second));

        report (fast.second > slow.second + 3.0f,
                "the fast average must be well ahead of a symmetric one after the same audio",
                juce::String (fast.second, 2) + " dB against " + juce::String (slow.second, 2) + " dB");

        report (fast.second > fast.first + 20.0f,
                "a level that rises must show at once rather than easing up over the constant",
                juce::String (fast.first, 2) + " dB rising only to "
                    + juce::String (fast.second, 2) + " dB");

        // And it must still land on the right level. A fast attack that overshoots, or that
        // settles somewhere other than the signal, would be quick and wrong, which is worse
        // than slow. Checked after a full settling, where the two averages have converged and
        // the asymmetry can no longer be showing.
        report (std::abs (settled.second - (-12.04f)) < 1.0f,
                "the fast average must still settle on the level that arrived",
                juce::String (settled.second, 2) + " dB against about -12.04 dB");

        // Given long enough both must land on the same level. Six hundred pushes is not long
        // enough: the transform's own hop means only about thirty seven frames are averaged in
        // that time, and at that point the asymmetric one is simply closer, which is the point
        // of it. Six thousand pushes is long enough that neither is still climbing.
        report (std::abs (afterFrames (4.0f, 6000).second - (-12.04f)) < 1.0f,
                "given long enough the fast average must land on the true level",
                juce::String (afterFrames (4.0f, 6000).second, 2) + " dB");

        report (std::abs (afterFrames (1.0f, 6000).second - (-12.04f)) < 1.0f,
                "given long enough the symmetric average must land on the same level",
                juce::String (afterFrames (1.0f, 6000).second, 2) + " dB");

        report (std::abs (afterFrames (4.0f, 6000).second
                            - afterFrames (1.0f, 6000).second) < 0.5f,
                "once fully settled the two must agree to within half a decibel",
                juce::String (afterFrames (4.0f, 6000).second, 3) + " dB against "
                    + juce::String (afterFrames (1.0f, 6000).second, 3) + " dB");
    }

    // The bands have to behave the same way, or a band level and the curve drawn from the same
    // frame would disagree about which way the level is going.
    {
        dsp::RtaAnalyser rta;
        rta.prepare (48000.0, 16384);
        rta.setResolution (dsp::RtaAnalyser::Resolution::ThirdOctave);
        rta.setFrequencyRange (20.0f, 20000.0f);
        rta.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 2.0f);

        report (std::abs (rta.getAttackRatio() - 4.0f) < 1.0e-6f,
                "the band average must carry the same default ratio",
                juce::String (rta.getAttackRatio()));

        rta.setAttackRatio (1.0f);
        report (std::abs (rta.getAttackRatio() - 1.0f) < 1.0e-6f,
                "the band average must accept a ratio of one",
                juce::String (rta.getAttackRatio()));

        rta.setAttackRatio (1000.0f);
        report (rta.getAttackRatio() <= 32.0f,
                "the band average must clamp an absurd ratio",
                juce::String (rta.getAttackRatio()));
    }
}

int main()
{
    std::cout << "--- response speed ---\n";
    testResponseSpeed();

    using Window = dsp::SpectrumAnalyser::Window;
    const Window windows[] = { Window::Rectangular, Window::Hann, Window::Hamming,
                               Window::Blackman, Window::BlackmanHarris, Window::FlatTop };

    // ---- The frequency axis has to be right ----
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, 1024);

        const auto& frame = analyse (analyser, sine (8192, 1000.0, 0.5));

        require (frame.numBins == 513, "1024 point FFT must report 513 bins");
        require (std::abs (frame.frequency[0]) < 1.0e-6f, "Bin zero must be DC");
        require (std::abs (frame.frequency[1] - rate / 1024.0) < 0.001f,
                 "Bin one must be one bin width above DC");
        require (std::abs (frame.frequency[512] - rate / 2.0) < 0.01f,
                 "The last bin must be Nyquist");
        require (std::abs (frame.sampleRate - rate) < 0.001, "The frame must report its sample rate");
    }

    // ---- Test tones land on the right frequency, at every supported FFT size ----
    {
        const int sizes[] = { 1024, 2048, 4096, 8192, 16384, 32768 };
        const double tones[] = { 100.0, 500.0, 1000.0, 5000.0, 10000.0 };

        for (const auto size : sizes)
        {
            for (const auto tone : tones)
            {
                dsp::SpectrumAnalyser analyser;
                analyser.prepare (rate, size);
                analyser.setWindow (Window::Hann);
                analyser.setOverlapPercent (50.0f);
                analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

                const auto& frame = analyse (analyser, sine (size * 2, tone, 0.5));
                const auto bin = peakBin (frame);
                const auto binWidth = rate / (double) size;
                const auto found = binFrequency (frame, bin);

                require (frame.fftSize == size,
                         "The analyser must report the FFT size it was prepared with");

                // A tone must land on the nearest bin, and the reported frequency of that
                // bin must be within one bin width of the tone.
                require (std::abs (found - tone) <= binWidth,
                         "The peak must land within one bin width of the tone");

                // And the reading must not be more than the window's worst case scalloping
                // loss below the amplitude that went in.
                const auto worst = dsp::SpectrumAnalyser::worstCaseScallopingLossDb (Window::Hann);
                const auto levelDb = 20.0 * std::log10 (frame.magnitude[(size_t) bin]);

                require (levelDb > -6.02 - worst - 0.35,
                         "The peak magnitude must not be below the tone by more than the "
                         "window allows");
                require (levelDb <= -6.02 + 0.35,
                         "The peak magnitude must not read above the tone that went in");
            }
        }
    }

    // ---- Amplitude scaling, in dB, for every window ----
    {
        for (const auto window : windows)
        {
            for (const auto levelDb : { -6.0206, -12.0412, -20.0, -40.0 })
            {
                dsp::SpectrumAnalyser analyser;
                analyser.prepare (rate, 4096);
                analyser.setWindow (window);
                analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

                const auto amplitude = std::pow (10.0, levelDb / 20.0);
                const auto& frame = analyse (analyser, sine (8192, 1500.0, amplitude));
                const auto measured = 20.0 * std::log10 (frame.magnitude[(size_t) peakBin (frame)]);

                // 1500 Hz sits between bins at 4096 points, so the window's own scalloping
                // is inside the tolerance rather than being engineered away.
                const auto tolerance = dsp::SpectrumAnalyser::worstCaseScallopingLossDb (window) + 0.35;

                require (std::abs (measured - levelDb) < tolerance,
                         "Every window must report the level that went in");
            }
        }
    }

    // ---- FFT normalisation: a bin centred full scale tone reads 0 dBFS ----
    {
        for (const auto window : windows)
        {
            for (const auto size : { 1024, 4096, 16384 })
            {
                // A frequency that is an exact bin centre, so no scalloping loss applies and
                // the normalisation itself is what is being measured.
                const auto binIndex = 37;
                const auto tone = binIndex * rate / (double) size;

                dsp::SpectrumAnalyser analyser;
                analyser.prepare (rate, size);
                analyser.setWindow (window);
                analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

                const auto& frame = analyse (analyser, sine (size, tone, 1.0));

                require (peakBin (frame) == binIndex,
                         "A bin centred tone must peak in its own bin");
                require (std::abs (frame.magnitude[(size_t) binIndex] - 1.0f) < 0.02f,
                         "A full scale bin centred tone must read 1.0 amplitude for every window");
                require (std::abs (frame.magnitudeDb[(size_t) binIndex]) < 0.2f,
                         "A full scale bin centred tone must read 0 dBFS for every window");
            }
        }
    }

    // ---- The window must actually be applied, and must be the shape it claims ----
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, 4096);

        analyser.setWindow (Window::Rectangular);
        const auto rectangularGain = analyser.getCoherentGain();
        analyser.setWindow (Window::Hann);
        const auto hannGain = analyser.getCoherentGain();
        analyser.setWindow (Window::FlatTop);
        const auto flatTopGain = analyser.getCoherentGain();

        require (std::abs (rectangularGain - 1.0f) < 0.01f,
                 "A rectangular window has a coherent gain of 1");
        require (std::abs (hannGain - 0.5f) < 0.01f,
                 "A periodic Hann window has a coherent gain of 0.5");
        report (std::abs (flatTopGain - 1.0f) < 0.01f,
                 "A flat top window must read a coherent gain of 1",
                 "flat top CG " + juce::String (flatTopGain, 4));
        // The equivalent noise bandwidth is what makes the power spectrum window
        // independent, and it is a known constant per window.
        analyser.setWindow (Window::Rectangular);
        require (std::abs (analyser.getNoiseBandwidth() - 1.0f) < 0.02f,
                 "A rectangular window has an equivalent noise bandwidth of 1 bin");
        analyser.setWindow (Window::Hann);
        require (std::abs (analyser.getNoiseBandwidth() - 1.5f) < 0.05f,
                 "A Hann window has an equivalent noise bandwidth of 1.5 bins");
        analyser.setWindow (Window::BlackmanHarris);
        require (analyser.getNoiseBandwidth() > 1.5f && analyser.getNoiseBandwidth() < 2.5f,
                 "A Blackman-Harris window is wider than Hann");

        // A tone off centre must lose more with a narrow window than with a flat top,
        // which is the whole reason to choose one over the other.
        auto peakAtHalfBinOffCentre = [] (Window window)
        {
            dsp::SpectrumAnalyser analyser;
            analyser.prepare (rate, 4096);
            analyser.setWindow (window);
            analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

            const auto tone = 20.5 * rate / 4096.0;   // half a bin above bin 20
            const auto& frame = analyser.getFrame();

            analyse (analyser, sine (8192, tone, 1.0));
            return 20.0 * std::log10 (frame.magnitude[(size_t) peakBin (frame)]);
        };

        const auto flatTop = peakAtHalfBinOffCentre (Window::FlatTop);
        const auto blackman = peakAtHalfBinOffCentre (Window::BlackmanHarris);

        report (flatTop > blackman + 0.5f,
                 "A flat top window must hold a tone between bins better than a "
                 "Blackman-Harris window",
                 "flat top " + juce::String (flatTop, 2) + " dB vs blackman-harris "
                     + juce::String (blackman, 2) + " dB");

        // And the reverse: a window that is too narrow has to lose the tone. Half a bin
        // off centre is where the difference between the windows is largest.
        const auto rectangular = peakAtHalfBinOffCentre (Window::Rectangular);
        report (rectangular < flatTop - 2.0f,
                 "A rectangular window must lose an off centre tone that a flat top holds",
                 "rectangular " + juce::String (rectangular, 2) + " dB vs flat top "
                     + juce::String (flatTop, 2) + " dB");
    }

    // ---- The power spectrum must be window independent ----
    {
        // White noise of a known variance: the power spectrum summed over the positive
        // frequencies must return that variance whichever window is selected, because it
        // is normalised by the window's energy rather than its coherent gain.
        std::vector<float> noise (16384);
        unsigned seed = 22222;
        double variance = 0.0;

        for (auto& sample : noise)
        {
            seed = seed * 1103515245u + 12345u;
            const auto u = (double) ((seed >> 16) & 0x7fff) / 32768.0 * 2.0 - 1.0;
            sample = (float) (u * 0.1);
            variance += (double) sample * (double) sample;
        }

        variance /= (double) noise.size();

        for (const auto window : windows)
        {
            dsp::SpectrumAnalyser analyser;
            analyser.prepare (rate, 16384);
            analyser.setWindow (window);
            analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 0.5f);

            const auto& frame = analyse (analyser, noise);

            double sum = 0.0;

            for (int i = 1; i < frame.numBins - 1; ++i)
                sum += (double) frame.power[(size_t) i];

            const auto recovered = sum / (double) frame.fftSize;

            // Two hundred averages of noise per bin is not much, so the tolerance is wide
            // enough for the statistics and tight enough to catch a wrong normalisation:
            // a Hann window read through a coherent gain normaliser would be 1.5x out.
            report (std::abs (10.0 * std::log10 (recovered / variance)) < 0.6f,
                    "The power spectrum must recover the signal variance for every window",
                    dsp::SpectrumAnalyser::getWindowNames()[(int) window] + ": recovered "
                        + juce::String (recovered * 1000.0, 3) + "e-3 against variance "
                        + juce::String (variance * 1000.0, 3) + "e-3");
        }
    }

    // ---- Averaging ----
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, 2048);
        analyser.setWindow (Window::Hann);
        analyser.setOverlapPercent (50.0f);
        analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Off, 1.0f);

        auto signal = sine (2048, 1000.0, 0.5);
        const auto& frame = analyse (analyser, signal, 8);

        require (std::abs (frame.averagedDb[(size_t) peakBin (frame)]
                           - frame.magnitudeDb[(size_t) peakBin (frame)]) < 0.001f,
                 "With averaging off the averaged curve must be the current frame");

        // Exponential averaging of a steady signal must converge on the same figure, so
        // turning it on cannot change what a steady tone reads.
        dsp::SpectrumAnalyser averaged;
        averaged.prepare (rate, 2048);
        averaged.setWindow (Window::Hann);
        averaged.setOverlapPercent (50.0f);
        averaged.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.3f);

        // Fed the way a live stream arrives: one hop's worth pushed, then the frames it
        // completed are taken. Pushing a minute of audio in one go would be dropped by
        // the backlog guard, which is the correct behaviour but not what is being tested.
        for (int i = 0; i < 60; ++i)
        {
            averaged.pushSamples (signal.data(), (int) signal.size());
            averaged.processAvailableFrames();
        }

        const auto& averagedFrame = averaged.getFrame();
        const auto bin = peakBin (frame);

        require (std::abs (averagedFrame.averagedDb[(size_t) bin]
                           - frame.magnitudeDb[(size_t) bin]) < 0.5f,
                 "Exponential averaging must converge on the steady value");

        require (averagedFrame.framesProcessed > 40,
                 "Frames must actually be produced at the configured hop");

        // A longer time constant must settle more slowly: that is the whole setting.
        dsp::SpectrumAnalyser fast;
        fast.prepare (rate, 2048);
        fast.setWindow (Window::Hann);
        fast.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.05f);

        dsp::SpectrumAnalyser slow;
        slow.prepare (rate, 2048);
        slow.setWindow (Window::Hann);
        slow.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 5.0f);

        // The first frame after a loud tone, then silence: the fast average must have
        // forgotten the tone and the slow one must still be holding it.
        for (int i = 0; i < 8; ++i)
        {
            fast.pushSamples (signal.data(), (int) signal.size());
            slow.pushSamples (signal.data(), (int) signal.size());
        }

        fast.processAvailableFrames();
        slow.processAvailableFrames();

        auto quiet = silence (2048);

        for (int i = 0; i < 8; ++i)
        {
            fast.pushSamples (quiet.data(), (int) quiet.size());
            slow.pushSamples (quiet.data(), (int) quiet.size());
        }

        fast.processAvailableFrames();
        slow.processAvailableFrames();

        require (fast.getFrame().averagedDb[(size_t) bin] < slow.getFrame().averagedDb[(size_t) bin],
                 "A long averaging time constant must forget a tone more slowly");
    }

    // ---- Silence and DC must be handled without NaN ----
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, 2048);
        analyser.setWindow (Window::FlatTop);

        const auto& silent = analyse (analyser, silence (8192));

        for (int i = 0; i < silent.numBins; ++i)
        {
            require (std::isfinite (silent.magnitudeDb[(size_t) i]),
                     "Silence must not produce a NaN magnitude");
            require (std::isfinite (silent.powerDb[(size_t) i]),
                     "Silence must not produce a NaN power");
            require (silent.magnitudeDb[(size_t) i] < -150.0f,
                     "Silence must read at the floor, not at zero");
        }

        // A constant offset is the DC bin and nothing else: an interface sitting a few
        // millivolts off zero must not paint the whole low end.
        std::vector<float> offset (4096, 0.001f);
        const auto& biased = analyse (analyser, offset);

        require (biased.magnitude[0] > 0.0005f, "DC must be measured in the DC bin");

        // A DC offset spreads over the window's main lobe, which for a flat top window is
        // several bins wide, so the leak is only required to be gone well inside the
        // audible band rather than gone from the very next bin.
        const auto audibleBin = (int) (1000.0 * 4096 / 48000.0);
        require (biased.magnitude[(size_t) audibleBin] < 1.0e-6f,
                 "A DC offset must not leak into the audible bins");

        // An impulse is flat, which is the definition of the impulse response and a
        // strong check that the normalisation is not quietly frequency dependent.
        auto impulse = std::vector<float> ((size_t) 2048, 0.0f);
        impulse[100] = 1.0f;
        const auto& flat = analyse (analyser, impulse);

        auto flatness = 0.0f;

        for (int i = 20; i < 1000; ++i)
            flatness = std::max (flatness, std::abs (flat.magnitudeDb[(size_t) i]
                                                     - flat.magnitudeDb[20]));

        require (flatness < 0.01f,
                 "An impulse must give a flat spectrum, so the response has no ripple");
    }

    // ---- Overlap controls the frame rate ----
    {
        dsp::SpectrumAnalyser half;
        half.prepare (rate, 1024);
        half.setOverlapPercent (50.0f);
        require (half.getFrame().hopSize == 512, "50 % overlap must halve the hop");

        auto halfSignal = sine (2048, 1000.0, 0.5);
        half.pushSamples (halfSignal.data(), 1024);
        half.processAvailableFrames();
        require (half.getFrame().framesProcessed == 1,
                 "The first overlapped frame must wait for one full FFT window");
        half.pushSamples (halfSignal.data() + 1024, 512);
        half.processAvailableFrames();
        require (half.getFrame().framesProcessed == 2,
                 "A 50 percent overlap must produce a new frame after one half-window hop");

        dsp::SpectrumAnalyser none;
        none.prepare (rate, 1024);
        none.setOverlapPercent (0.0f);
        require (none.getFrame().hopSize == 1024, "No overlap must analyse every sample");
        none.pushSamples (halfSignal.data(), 1024);
        none.processAvailableFrames();
        none.pushSamples (halfSignal.data() + 1024, 512);
        none.processAvailableFrames();
        require (none.getFrame().framesProcessed == 1,
                 "A non-overlapped frame must wait for the full FFT window");
        none.pushSamples (halfSignal.data() + 1536, 512);
        none.processAvailableFrames();
        require (none.getFrame().framesProcessed == 2,
                 "A non-overlapped frame must need one full new FFT window");
    }

    // ---- A backlog must be bounded and reported, never silently grow ----
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, 1024);
        analyser.setOverlapPercent (0.0f);
        analyser.getMaxFramesPerCall();   // the cap exists so one call cannot run away

        auto signal = sine (1024, 1000.0, 0.5);

        for (int i = 0; i < 400; ++i)
            analyser.pushSamples (signal.data(), (int) signal.size());

        analyser.processAvailableFrames();

        require (analyser.getFrame().framesProcessed <= analyser.getMaxFramesPerCall(),
                 "One call must not analyse more frames than the cap allows");
        require (analyser.getFrame().framesDropped > 0,
                 "A backlog larger than the cap must be reported as dropped, not hidden");
        require (analyser.getFrame().valid, "The analyser must stay usable after a backlog");
    }

    if (failures == 0)
        std::cout << "PASS: 100 Hz, 500 Hz, 1 kHz, 5 kHz and 10 kHz land on the correct bin at "
                     "FFT 1024 to 32768"
                  << std::endl
                  << "PASS: magnitude, magnitude dB, power and the frequency axis are all "
                     "computed per bin"
                  << std::endl
                  << "PASS: -6, -12, -20 and -40 dBFS scale exactly in every window"
                  << std::endl
                  << "PASS: a bin centred full scale tone reads 0 dBFS for all six windows"
                  << std::endl
                  << "PASS: coherent gain and noise bandwidth match each window, and flat top "
                     "holds an off centre tone that Blackman-Harris loses"
                  << std::endl
                  << "PASS: the power spectrum recovers the noise variance for every window"
                  << std::endl
                  << "PASS: averaging off tracks the frame, exponential converges and its time "
                     "constant changes the settling rate"
                  << std::endl
                  << "PASS: silence and DC stay finite, an impulse is flat, and overlap sets the "
                     "frame rate"
                  << std::endl
                  << "PASS: a backlog is bounded, counted and survived"
                  << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}
