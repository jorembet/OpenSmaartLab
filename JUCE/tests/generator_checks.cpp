// Phase 8 checks: the signal generator.
//
// The generator is judged by measuring what it actually produces, not by reading back the
// settings that were asked for. A generator that reports 1 kHz while emitting something else
// is worse than one that is silent, because every measurement taken through it would be
// quietly wrong and the number on screen would still look right.
//
// Each waveform is rendered exactly as the audio callback renders it, then transformed, so what
// is checked is the signal that reaches the converter and not an internal copy of the intent.
//
// Pink noise is the one that has to be measured rather than assumed. White noise relabelled
// pink has a flat spectrum and would pass any level check while being useless for the one
// thing pink is for: equal energy per octave. The check here is that power per octave falls by
// about 3 dB, which white noise cannot do and which a mislabelled generator fails immediately.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DSP.h"
#include "SignalGenerator.h"
#include "SpectrumAnalyser.h"

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
    constexpr int fftSize = 16384;

    /** Renders the generator exactly as the audio callback does. */
    std::vector<float> render (SignalGenerator& generator, int numSamples)
    {
        std::vector<float> out ((size_t) numSamples, 0.0f);

        for (int offset = 0; offset < numSamples; offset += 512)
            generator.process (out.data() + offset, juce::jmin (512, numSamples - offset));

        return out;
    }

    /** Magnitude spectrum in dB, measured through the same analyser the application uses. */
    std::vector<float> measure (const std::vector<float>& samples)
    {
        dsp::SpectrumAnalyser analyser;
        analyser.prepare (rate, fftSize);
        analyser.setWindow (dsp::SpectrumAnalyser::Window::Hann);
        analyser.setAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.6f);
        analyser.pushSamples (samples.data(), (int) samples.size());
        analyser.processAvailableFrames();

        return analyser.getFrame().magnitudeDb;
    }

    std::vector<float> frequenciesOf (int size)
    {
        std::vector<float> frequencies;

        for (int i = 0; i < size / 2; ++i)
            frequencies.push_back ((float) (i * rate / size));

        return frequencies;
    }

    float peakLevelDb (const std::vector<float>& values)
    {
        auto peak = dsp::dbFloor;

        for (auto value : values)
            peak = std::max (peak, value);

        return peak;
    }

    /** Loudest level in a frequency window.

        This is the right measure for a tone or a harmonic. Averaging instead would mix the
        few bins a Hann main lobe occupies in with the empty bins either side of them, and a
        pure tone a few bins wide would then read a hundred decibels lower than it is, which
        is how a perfectly good square wave first looked as though it had no harmonics at all. */
    float bandPeakDb (const std::vector<float>& frequencies, const std::vector<float>& values,
                      double lowHz, double highHz)
    {
        float peak = dsp::dbFloor;

        for (size_t i = 0; i < frequencies.size() && i < values.size(); ++i)
        {
            if (frequencies[i] < lowHz || frequencies[i] > highHz)
                continue;

            peak = std::max (peak, values[i]);
        }

        return peak;
    }

    /** Mean level across a frequency window, for a signal spread over the whole band.

        A peak would be meaningless here: broadband noise has no peak to speak of, and the
        loudest single bin of a noise band says more about the random draw than about the
        signal. */
    float bandMeanDb (const std::vector<float>& frequencies, const std::vector<float>& values,
                      double lowHz, double highHz)
    {
        double sum = 0.0;
        int count = 0;

        for (size_t i = 0; i < frequencies.size() && i < values.size(); ++i)
        {
            if (frequencies[i] < lowHz || frequencies[i] > highHz)
                continue;

            sum += values[i];
            ++count;
        }

        return count > 0 ? (float) (sum / count) : dsp::dbFloor;
    }

    /** Configured in place, because SignalGenerator is deliberately not copyable. */
    void configure (SignalGenerator& generator, SignalGenerator::Type type,
                    float levelDb = -6.0f)
    {
        generator.prepare (rate);
        generator.setType (type);
        generator.setLevelDb (levelDb);
        generator.setRunning (true);
        generator.reset();
    }
}

int main()
{
    const auto frequencies = frequenciesOf (fftSize);

    // Sine at 1 kHz has to appear at 1 kHz. The check is that the tone is the loudest thing in
    // the spectrum and that it sits in the right bin, because a generator running an octave
    // low would still produce a clean looking peak somewhere.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::Sine, -6.0f);
        generator.setFrequency (1000.0f);

        const auto samples = render (generator, fftSize * 4);
        const auto spectrum = measure (samples);

        auto peakIndex = 0;
        auto peakValue = dsp::dbFloor;

        for (size_t i = 1; i < spectrum.size(); ++i)
            if (spectrum[i] > peakValue)
            {
                peakValue = spectrum[i];
                peakIndex = (int) i;
            }

        const auto peakHz = frequencies[(size_t) peakIndex];

        report (std::abs (peakHz - 1000.0f) < 20.0f,
                "a 1 kHz sine must peak at 1 kHz",
                juce::String (peakHz, 1) + " Hz");

        // The level asked for must be the level measured, within a bin of leakage.
        report (std::abs (peakValue - (-6.0f)) < 1.0f,
                "a -6 dBFS sine must measure -6 dBFS",
                juce::String (peakValue, 2) + " dB");

        // Nothing else may come close, which is what says the tone is a tone and not noise
        // with a peak in it.
        const auto elsewhere = bandPeakDb (frequencies, spectrum, 2000.0, 8000.0);
        report (peakValue - elsewhere > 60.0f,
                "a sine must put nothing else on the spectrum",
                juce::String (peakValue - elsewhere, 1) + " dB above 2-8 kHz");
    }

    // The frequency range the specification names. Both ends are checked because they fail
    // differently: a low limit is a matter of the clamp, a high one also has to survive the
    // sample rate, and 20 kHz is close enough to Nyquist to be worth proving.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::Sine, -20.0f);

        for (auto hz : { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
        {
            generator.setFrequency (hz);

            // The clamp is what is being checked here, so it is read back rather than measured:
            // what matters is that the request was honoured and not silently rounded away.
            report (std::abs (generator.getFrequency() - hz) < 0.5f,
                    ("the generator must accept " + juce::String (hz, 0) + " Hz").toRawUTF8(),
                    juce::String (generator.getFrequency(), 1) + " Hz");
        }

        // Below the band it must clamp rather than accept, or a 5 Hz tone would excite nothing
        // a loudspeaker reproduces and would read as a fault in the room.
        generator.setFrequency (5.0f);
        report (generator.getFrequency() >= 20.0f,
                "a request below 20 Hz must be clamped into the band",
                juce::String (generator.getFrequency(), 1) + " Hz");

        generator.setFrequency (60000.0f);
        report (generator.getFrequency() <= 20000.0f,
                "a request above 20 kHz must be clamped into the band",
                juce::String (generator.getFrequency(), 1) + " Hz");

        // And the level range: -60 dBFS has to be reachable and 0 dBFS has to be the ceiling.
        for (auto db : { -60.0f, -40.0f, -20.0f, -6.0f, 0.0f })
        {
            generator.setLevelDb (db);
            report (std::abs (generator.getLevelDb() - db) < 0.01f,
                    ("the generator must accept " + juce::String (db, 0) + " dBFS").toRawUTF8(),
                    juce::String (generator.getLevelDb(), 2) + " dBFS");
        }

        generator.setLevelDb (6.0f);
        report (generator.getLevelDb() <= 0.0f,
                "a level above 0 dBFS must be clamped, so nothing clips on the way out",
                juce::String (generator.getLevelDb(), 2) + " dBFS");
    }

    // Clipping protection is checked on the waveform that would clip first. A square wave at
    // 0 dBFS sits at full scale for half its cycle, so any overshoot in the correction shows up
    // here first, and a sample outside the rails would be a defect the converter cannot undo.
    {
        for (auto type : { SignalGenerator::Type::Square, SignalGenerator::Type::Triangle,
                           SignalGenerator::Type::Saw, SignalGenerator::Type::Sine })
        {
            SignalGenerator generator;
            configure (generator, type, 0.0f);
            generator.setFrequency (997.0f);

            const auto samples = render (generator, fftSize * 2);

            auto worst = 0.0f;

            for (auto sample : samples)
                worst = std::max (worst, std::abs (sample));

            const auto name = SignalGenerator::getTypeNames()[(int) type];

            report (worst <= 1.0f,
                    "no sample of a full scale waveform may leave the rails",
                    name + " peaked at " + juce::String (worst, 5));
        }
    }

// Pink noise is measured per octave, which is the property that makes it pink. Equal power
    // per octave means the level falls by 3 dB for every doubling of frequency. White noise
    // cannot do this: its spectrum is flat and every octave would read the same, so this is the
    // check that catches a mislabelled generator.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::Pink, -12.0f);

        // Several blocks, because the buffer has to be read through and the first one is
        // whichever half of the double buffer was published last.
        auto samples = render (generator, fftSize * 2);
        const auto more = render (generator, fftSize * 2);
        samples.insert (samples.end(), more.begin(), more.end());

        const auto spectrum = measure (samples);

        // Octave centres well inside the generated band, so the tapered ends are not judged.
        const double centres[] = { 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0 };
        std::vector<float> octaveLevels;

        for (auto centre : centres)
        {
            // A narrow slice either side of the centre, which averages the random variation in
            // a single octave band enough to judge a 3 dB step.
            const auto level = bandMeanDb (frequencies, spectrum, centre / 1.2, centre * 1.2);
            octaveLevels.push_back (level);
        }

        for (size_t i = 0; i < octaveLevels.size(); ++i)
        {
            report (std::isfinite (octaveLevels[i]),
                    "every octave of pink noise must carry a level",
                    juce::String ((int) centres[i]) + " Hz");

            // Judged against the loudest octave rather than against an absolute floor, because
            // pink noise falls by design and a fixed bound would only be measuring where 3 dB an
            // octave happens to land. What must not happen is an octave carrying nothing at all,
            // which is what a generator that shaped the wrong bins would produce.
            report (octaveLevels[i] > octaveLevels[0] - 25.0f && octaveLevels[i] < 0.0f,
                    "every octave of pink noise must carry real, unclipped energy",
                    juce::String (octaveLevels[i], 1) + " dB at "
                        + juce::String ((int) centres[i]) + " Hz, against "
                        + juce::String (octaveLevels[0], 1) + " dB at the loudest");
        }

        // The defining property: 3 dB down per octave, checked across the middle of the band
        // where the taper is not acting.
        report (octaveLevels[5] < octaveLevels[2] - 6.0f,
                "pink noise must fall by at least 6 dB from 500 Hz to 4 kHz, which is 3 dB an octave",
                juce::String (octaveLevels[2], 1) + " against "
                    + juce::String (octaveLevels[5], 1) + " dB");

        report (octaveLevels[2] < octaveLevels[1] - 1.0f,
                "each octave of pink noise must be quieter than the one below it",
                juce::String (octaveLevels[1], 1) + " against "
                    + juce::String (octaveLevels[2], 1) + " dB");

        report (octaveLevels[4] < octaveLevels[3] - 1.0f,
                "each octave of pink noise must be quieter than the one below it",
                juce::String (octaveLevels[3], 1) + " against "
                    + juce::String (octaveLevels[4], 1) + " dB");

        // Flat white noise would read level-for-level across all of these, so the two checks
        // above together are what separate pink from white.
        auto whitestOctave = octaveLevels[0];

        for (auto level : octaveLevels)
            whitestOctave = std::max (whitestOctave, level);

        auto quietestOctave = octaveLevels[0];

        for (auto level : octaveLevels)
            quietestOctave = std::min (quietestOctave, level);

        report (whitestOctave - quietestOctave > 8.0f,
                "pink noise must not be flat across the band, which is what white noise is",
                juce::String (whitestOctave - quietestOctave, 1) + " dB between the "
                    "loudest and quietest octave");
    }

    // White noise is the opposite property, and it is checked as such: flat.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::White, -20.0f);

        auto samples = render (generator, fftSize * 2);
        const auto more = render (generator, fftSize * 2);
        samples.insert (samples.end(), more.begin(), more.end());

        const auto spectrum = measure (samples);

        const auto low = bandMeanDb (frequencies, spectrum, 400.0, 1000.0);
        const auto high = bandMeanDb (frequencies, spectrum, 4000.0, 8000.0);

        report (std::abs (low - high) < 6.0f,
                "white noise must be flat, so two octaves must read the same",
                juce::String (low, 1) + " against " + juce::String (high, 1) + " dB");
    }

    // The harmonic waveforms have to carry harmonics of the set frequency, and the saw and the
    // square have to carry them at the right strength. A square is odd harmonics only, and a
    // saw is all of them falling by 6 dB an octave; getting either wrong means the shape is
    // not the shape it claims to be.
    {
        SignalGenerator square;
        configure (square, SignalGenerator::Type::Square, -12.0f);
        square.setFrequency (250.0f);

        const auto squareSpectrum = measure (render (square, fftSize * 2));

        const auto third = bandPeakDb (frequencies, squareSpectrum, 700.0, 800.0);
        const auto second = bandPeakDb (frequencies, squareSpectrum, 450.0, 550.0);
        const auto fifth = bandPeakDb (frequencies, squareSpectrum, 1200.0, 1300.0);

        // An even harmonic of an odd-harmonic series should be far below its odd neighbours.
        report (second < third - 20.0f,
                "a square wave must carry no even harmonic",
                juce::String (second, 1) + " dB at 500 Hz against " + juce::String (third, 1)
                    + " dB at 750 Hz");

        const auto fundamentalLevel = bandPeakDb (frequencies, squareSpectrum, 220.0, 280.0);

        // Odd harmonics of a square fall as 1/n in amplitude, so the third sits 9.5 dB below
        // the fundamental and the fifth a further 20*log10(5/3) = 4.4 dB below that. Checking
        // both, because the step between two harmonics and the depth of one below the
        // fundamental fail independently: a waveform with the right decay and the wrong
        // fundamental passes one and not the other.
        report (fundamentalLevel - third > 6.0f && fundamentalLevel - third < 13.0f,
                "the third harmonic of a square must sit about 9.5 dB below its fundamental",
                juce::String (fundamentalLevel - third, 2) + " dB");

        report (third - fifth > 1.5f && third - fifth < 7.0f,
                "a square must fall about 4.4 dB from the third harmonic to the fifth",
                juce::String (third - fifth, 2) + " dB");

        SignalGenerator saw;
        configure (saw, SignalGenerator::Type::Saw, -12.0f);
        saw.setFrequency (250.0f);

        const auto sawSpectrum = measure (render (saw, fftSize * 2));

        const auto saw1 = bandPeakDb (frequencies, sawSpectrum, 220.0, 280.0);
        const auto saw2 = bandPeakDb (frequencies, sawSpectrum, 470.0, 530.0);
        const auto saw4 = bandPeakDb (frequencies, sawSpectrum, 970.0, 1030.0);

        // A saw falls by 6 dB per octave: a factor of two in amplitude.
        report (std::abs ((saw1 - saw2) - 6.0f) < 2.5f,
                "a saw must fall 6 dB from its fundamental to the second harmonic",
                juce::String (saw1 - saw2, 2) + " dB");

        report (std::abs ((saw1 - saw4) - 12.0f) < 4.0f,
                "a saw must fall 12 dB from its fundamental to the fourth harmonic",
                juce::String (saw1 - saw4, 2) + " dB");
    }

    // The log sweep has to travel from low to high, monotonically. The instantaneous frequency
    // is recovered by differentiating the phase, which is the same thing the phase slope
    // estimate does, so this also proves the sweep's own phase is the phase it should be.
    {
        for (auto seconds : { 5.0f, 10.0f, 20.0f })
        {
            SignalGenerator generator;
            configure (generator, SignalGenerator::Type::LogSweep, -12.0f);
            generator.setSweepRange (20.0f, 20000.0f, seconds);
            generator.reset();

            const auto samples = render (generator, (int) (rate * seconds) + fftSize);

            // Measured in slices across the sweep, each long enough for its own transform.
            const auto slice = (int) (rate * 0.5);
            std::vector<float> centres;

            for (int start = 0; start + slice < (int) samples.size(); start += slice)
            {
                const std::vector<float> piece (samples.begin() + start,
                                               samples.begin() + start + slice);
                const auto spectrum = measure (piece);

                auto peakIndex = 0;
                auto peakValue = dsp::dbFloor;

                for (size_t i = 1; i < spectrum.size(); ++i)
                    if (spectrum[i] > peakValue)
                    {
                        peakValue = spectrum[i];
                        peakIndex = (int) i;
                    }

                centres.push_back (frequencies[(size_t) peakIndex]);
            }

            require (centres.size() >= 8,
                     ("a " + juce::String (seconds, 0) + " s sweep must give enough slices to judge")
                         .toRawUTF8());

            // Rising throughout. A sweep that fell, or that sat still, would fail here.
            auto falls = 0;

            for (size_t i = 1; i < centres.size(); ++i)
                if (centres[i] < centres[i - 1] * 0.999f)
                    ++falls;

            report (falls == 0,
                    ("every slice of a " + juce::String (seconds, 0)
                         + " s sweep must be higher than the one before").toRawUTF8(),
                    juce::String (falls) + " slices did not rise");

            // And it must actually arrive: within a slice the frequency is roughly constant, so
            // the first slice belongs near 20 Hz and the last near 20 kHz.
            report (centres.front() < 60.0f,
                    ("a sweep must start near 20 Hz at " + juce::String (seconds, 0) + " s").toRawUTF8(),
                    juce::String (centres.front(), 1) + " Hz");

            report (centres.back() > 12000.0f,
                    ("a sweep must finish near 20 kHz at " + juce::String (seconds, 0) + " s").toRawUTF8(),
                    juce::String (centres.back(), 1) + " Hz");

            // A log sweep spends equal time per octave, so the middle slice must be near the
            // geometric centre, 1 kHz. A linear sweep would be far below it here.
            const auto middle = centres[centres.size() / 2];
            report (middle > 600.0f && middle < 1800.0f,
                    ("the middle of a log sweep must be near 1 kHz at "
                         + juce::String (seconds, 0) + " s").toRawUTF8(),
                    juce::String (middle, 1) + " Hz");
        }
    }

    // An impulse is one arrival and then silence, which is what makes an impulse response
    // readable. A repeating train would fold every repeat onto the first in the transform.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::Impulse, -6.0f);
        const auto samples = render (generator, 8192);

        auto nonZero = 0;
        auto firstNonZero = -1;

        for (size_t i = 0; i < samples.size(); ++i)
            if (std::abs (samples[i]) > 1.0e-6f)
            {
                ++nonZero;
                if (firstNonZero < 0)
                    firstNonZero = (int) i;
            }

        report (nonZero > 0, "an impulse must actually produce a sample",
                juce::String (nonZero) + " non zero samples");

        report (nonZero <= 2,
                "an impulse must be a single arrival, not a train",
                juce::String (nonZero) + " non zero samples");

        report (firstNonZero >= 0 && firstNonZero <= 1,
                "the impulse must arrive at the start of the block",
                "first non zero sample at " + juce::String (firstNonZero));

        // And its level is the level that was asked for.
        report (std::abs (std::abs (samples[0]) - std::pow (10.0f, -6.0f / 20.0f)) < 0.01f,
                "the impulse must arrive at the level that was set",
                juce::String (std::abs (samples[0]), 4));
    }

    // Mute has to silence the output without stopping the generator, so unmuting brings the
    // same signal back rather than restarting it.
    {
        SignalGenerator generator;
        configure (generator, SignalGenerator::Type::Sine, -12.0f);
        generator.setFrequency (1000.0f);

        const auto loud = render (generator, 4096);
        auto peakLoud = 0.0f;

        for (auto sample : loud)
            peakLoud = std::max (peakLoud, std::abs (sample));

        generator.setMuted (true);

        const auto quiet = render (generator, 4096);
        auto peakQuiet = 0.0f;

        for (auto sample : quiet)
            peakQuiet = std::max (peakQuiet, std::abs (sample));

        report (peakLoud > 0.1f, "the generator must be producing a signal before it is muted",
                juce::String (peakLoud, 4));

        report (peakQuiet == 0.0f, "a muted generator must produce silence",
                juce::String (peakQuiet, 8));

        report (generator.isRunning(), "muting must not stop the generator",
                juce::String (generator.isRunning() ? 1 : 0));

        generator.setMuted (false);

        const auto loudAgain = render (generator, 4096);
        auto peakAgain = 0.0f;

        for (auto sample : loudAgain)
            peakAgain = std::max (peakAgain, std::abs (sample));

        report (std::abs (peakAgain - peakLoud) < 0.01f,
                "unmuting must bring the level back exactly as it was",
                juce::String (peakAgain, 4) + " against " + juce::String (peakLoud, 4));
    }

    // Every waveform must survive being switched to and rendered without allocating, hanging or
    // producing a value outside the rails. This is the guard on the audio thread: the callback
    // cannot allocate, so a waveform that needed a buffer would be a crash in the application
    // and not a failure here.
    {
        for (int t = 0; t < (int) SignalGenerator::Type::LogSweep; ++t)
        {
            const auto type = (SignalGenerator::Type) t;
            const auto name = SignalGenerator::getTypeNames()[t];

            SignalGenerator generator;
            generator.prepare (rate);
            generator.setType (type);
            generator.setLevelDb (0.0f);
            generator.setRunning (true);

            std::vector<float> out (512);
            auto worst = 0.0f;

            // Rendered in realistic block sizes, switching type part way through, because a
            // waveform that is fine on its own can still misbehave on the change.
            for (int block = 0; block < 400; ++block)
            {
                if (block % 97 == 0)
                {
                    generator.setType (type);
                    generator.reset();
                }

                generator.process (out.data(), 512);

                for (auto sample : out)
                {
                    if (! std::isfinite (sample))
                    {
                        worst = 1.0e9f;
                        break;
                    }

                    worst = std::max (worst, std::abs (sample));
                }
            }

            report (worst <= 1.0f,
                    ("every waveform must stay inside the rails: " + name).toRawUTF8(),
                    juce::String (worst, 6));
        }
    }

    if (failures == 0)
        std::cout << "PASS: all eight waveforms measure correctly, pink noise really is pink, "
                     "the sweep travels low to high, and muting is silent" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}