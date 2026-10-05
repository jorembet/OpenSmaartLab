#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace dsp
{
    inline constexpr float rtaBands[] = {
        20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
        630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300,
        8000, 10000, 12500, 16000, 20000
    };

    using Complex = std::complex<float>;
    using WindowingMethod = juce::dsp::WindowingFunction<float>::WindowingMethod;

    inline constexpr float pi = 3.14159265358979f;
    inline constexpr float radToDeg = 57.2957795130823f;
    inline constexpr float degToRad = 0.0174532925199433f;
    inline constexpr float dbFloor = -200.0f;

    inline float db20(float x)
    {
        return juce::jmax(20.0f * std::log10(std::max(std::abs(x), 1e-20f)), dbFloor);
    }

    inline float db10(float x)
    {
        return juce::jmax(10.0f * std::log10(std::max(std::abs(x), 1e-20f)), dbFloor);
    }

    inline float aWeightingDb(float freq)
    {
        const float f2 = freq * freq;

        if (f2 < 1.0e-12f)
            return -200.0f;

        const float num = 12194.0f * 12194.0f * f2 * f2;
        const float den = (f2 + 20.6f * 20.6f)
                        * std::sqrt((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f))
                        * (f2 + 12194.0f * 12194.0f);

        return juce::jmax(20.0f * std::log10(std::max(num / den, 1e-20f)) + 2.0f, -200.0f);
    }

    inline float cWeightingDb(float freq)
    {
        const float f2 = freq * freq;

        if (f2 < 1.0e-12f)
            return -200.0f;

        const float num = 12194.0f * 12194.0f * f2;
        const float den = (f2 + 20.6f * 20.6f) * (f2 + 12194.0f * 12194.0f);

        return juce::jmax(20.0f * std::log10(std::max(num / den, 1e-20f)) + 0.06f, -200.0f);
    }

    inline float weightingGainDb(float freq, char weighting)
    {
        if (weighting == 'A' || weighting == 'a')
            return aWeightingDb(freq);

        if (weighting == 'C' || weighting == 'c')
            return cWeightingDb(freq);

        return 0.0f;
    }

    inline float weightingGain(float freq, char weighting)
    {
        return std::pow(10.0f, weightingGainDb(freq, weighting) / 20.0f);
    }

    inline float unwrapDeg(float previous, float phase)
    {
        float delta = phase - previous;

        while (delta > 180.0f)
            delta -= 360.0f;

        while (delta < -180.0f)
            delta += 360.0f;

        return previous + delta;
    }

    inline void unwrapPhase(std::vector<float>& phase)
    {
        for (size_t i = 1; i < phase.size(); ++i)
            phase[i] = unwrapDeg(phase[i - 1], phase[i]);
    }

    inline void unwrapPhase(const std::vector<float>& magnitudeDb,
                            std::vector<float>& phase,
                            float rangeBelowPeakDb = 40.0f)
    {
        if (phase.size() < 2 || magnitudeDb.size() != phase.size())
            return;

        auto peak = -std::numeric_limits<float>::max();

        for (auto value : magnitudeDb)
            peak = std::max(peak, value);

        const auto threshold = peak - rangeBelowPeakDb;
        auto previous = phase[0];
        auto hasPrevious = magnitudeDb[0] > threshold;

        for (size_t i = 1; i < phase.size(); ++i)
        {
            if (magnitudeDb[i] > threshold)
            {
                phase[i] = hasPrevious ? unwrapDeg(previous, phase[i]) : phase[i];
                previous = phase[i];
                hasPrevious = true;
            }
            else
            {
                hasPrevious = false;
            }
        }
    }

    inline float wrapDeg(float phase)
    {
        auto wrapped = std::fmod (phase, 360.0f);

        if (wrapped > 180.0f)
            wrapped -= 360.0f;
        else if (wrapped < -180.0f)
            wrapped += 360.0f;

        // A bin reported as +180 where the trend says -180 draws a vertical jump in the
        // curve, so the boundary is resolved to one side rather than left ambiguous.
        if (wrapped >= 180.0f)
            wrapped = -180.0f;

        return wrapped;
    }

    /** Speed of sound in dry air in metres per second, for a temperature in Celsius.

        The familiar 343 m/s is the value at 20 degrees and only that: air carries sound about
        0.6 m/s faster for every degree above it, so a warm room measured as though it were
        cool reports a path length roughly 3 % short, which is most of a 10 cm error at the
        distances a room measurement is read at. This is the usual linear approximation, which
        holds well across the range a room occupies. */
    inline float speedOfSoundMetresPerSecond(float temperatureC = 20.0f)
    {
        const auto clamped = juce::jlimit (-40.0f, 60.0f, temperatureC);
        return 331.3f + 0.606f * clamped;
    }

    /** Path length in metres for a delay, given the speed of sound.

        One way travel. A measurement taken with a microphone in a room is the sum of the path
        to the source and the path back, so the figure describes a total path rather than a
        distance to any one surface, and it is labelled as a path on screen. */
    inline float distanceFromDelayMetres(float delaySamples, double sampleRate,
                                         float temperatureC = 20.0f)
    {
        if (! std::isfinite (delaySamples) || sampleRate <= 0.0)
            return 0.0f;

        return (float) ((double) delaySamples / sampleRate)
                   * speedOfSoundMetresPerSecond (temperatureC);
    }

    /** Smooths an unwrapped phase with a centred moving average.

        A one-pole filter is the wrong tool here because it flattens the very gradient the
        phase slope estimator reads. What matters is that a smoothed straight line stays
        straight: a centred average leaves any linear trend exactly where it was and removes
        only the curvature, which is why this is not a recursive filter.

        Bins marked invalid stay at zero so a smoothed curve cannot invent a reading where the
        measurement had none. */
    inline std::vector<float> smoothUnwrappedPhase(const std::vector<float>& phase,
                                                   const std::vector<char>& valid,
                                                   int halfWidth = 2)
    {
        std::vector<float> smoothed (phase.size(), 0.0f);

        if (phase.empty() || halfWidth < 0)
            return smoothed;

        const auto hasValidity = valid.size() == phase.size();
        const auto isValid = [&] (size_t i) { return ! hasValidity || valid[i] != 0; };

        for (size_t i = 0; i < phase.size(); ++i)
        {
            if (! isValid (i))
                continue;

            // The average is formed relative to the bin being smoothed and each contribution
            // is unwrapped onto it first. Averaging the stored values directly would cancel
            // towards zero wherever the curve crosses the branch cut, flattening exactly the
            // part of the curve a reader is looking at.
            double sum = 0.0;
            int count = 0;

            for (int offset = -halfWidth; offset <= halfWidth; ++offset)
            {
                const auto j = (long) i + offset;

                if (j < 0 || j >= (long) phase.size() || ! isValid ((size_t) j))
                    continue;

                sum += unwrapDeg (phase[i], phase[(size_t) j]);
                ++count;
            }

            if (count > 0)
                smoothed[i] = (float) (sum / (double) count);
        }

        return smoothed;
    }

    /** A delay read off the slope of the phase.

        A pure delay of t seconds rotates the phase by -360*f*t degrees, so the phase is a
        straight line against frequency with a slope of -360*t degrees per hertz. Fitting that
        line across every usable bin at once is what separates this from reading a delay off a
        single bin: one bin only says what happened at that frequency, and by 1 kHz a five
        millisecond delay has already turned two and a quarter times around. */
    struct PhaseSlopeEstimate
    {
        float delaySamples = 0.0f;
        float delayMs = 0.0f;
        float slopeDegPerHz = 0.0f;
        /** RMS departure from the fitted line, in degrees. A large value means the phase is
            not a straight line, which is what reflections and a wrapped curve do to it, and
            the estimate should not be leaned on when it is large. */
        float scatterDeg = 0.0f;
        int binsUsed = 0;
        bool valid = false;
    };

    inline PhaseSlopeEstimate estimatePhaseSlopeDelay(
        const std::vector<float>& frequencies,
        const std::vector<float>& phaseUnwrappedDeg,
        const std::vector<float>& coherence,
        const std::vector<char>& binValid,
        double sampleRate,
        float lowFrequency = 100.0f,
        float highFrequency = 10000.0f,
        float minCoherence = 0.5f)
    {
        PhaseSlopeEstimate estimate;

        if (frequencies.size() < 4 || frequencies.size() != phaseUnwrappedDeg.size()
            || sampleRate <= 0.0)
            return estimate;

        // Every point would otherwise be weighted equally, so the bins worth fitting are
        // gathered first. A bin that was never measured, or whose two signals did not agree,
        // carries no phase worth trusting and would only bend the line towards zero.
        std::vector<size_t> usable;

        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            if (frequencies[i] < lowFrequency || frequencies[i] > highFrequency)
                continue;

            if (binValid.size() == frequencies.size() && binValid[i] == 0)
                continue;

            if (! std::isfinite (phaseUnwrappedDeg[i]))
                continue;

            if (coherence.size() == frequencies.size() && coherence[i] < minCoherence)
                continue;

            usable.push_back (i);
        }

        estimate.binsUsed = (int) usable.size();

        // Fewer than a dozen points cannot define a slope. Two points would fit any line at
        // all, and the answer would be decided by which two bins happened to be kept.
        if (usable.size() < 12)
            return estimate;

        double sumF = 0.0, sumP = 0.0;

        for (auto i : usable)
        {
            sumF += frequencies[i];
            sumP += phaseUnwrappedDeg[i];
        }

        const auto meanF = sumF / (double) usable.size();
        const auto meanP = sumP / (double) usable.size();

        double numerator = 0.0, denominator = 0.0;

        for (auto i : usable)
        {
            const auto df = (double) frequencies[i] - meanF;
            numerator += df * ((double) phaseUnwrappedDeg[i] - meanP);
            denominator += df * df;
        }

        if (denominator <= 0.0)
            return estimate;

        const auto slope = numerator / denominator;
        const auto delaySeconds = -slope / 360.0;

        if (! std::isfinite (delaySeconds))
            return estimate;

        double sumSquares = 0.0;

        for (auto i : usable)
        {
            const auto predicted = meanP + slope * ((double) frequencies[i] - meanF);
            const auto residual = (double) phaseUnwrappedDeg[i] - predicted;
            sumSquares += residual * residual;
        }

        estimate.slopeDegPerHz = (float) slope;
        estimate.scatterDeg = (float) std::sqrt (sumSquares / (double) usable.size());
        estimate.delaySamples = (float) (delaySeconds * sampleRate);
        estimate.delayMs = (float) (delaySeconds * 1000.0);
        estimate.valid = true;
        return estimate;
    }

    inline void smoothMagnitudeDb(const std::vector<float>& freq,
                                  const std::vector<float>& magnitudeDb,
                                  int octaveFraction,
                                  std::vector<float>& out)
    {
        if (octaveFraction <= 0 || freq.size() < 4 || magnitudeDb.size() != freq.size())
        {
            out = magnitudeDb;
            return;
        }

        const size_t n = freq.size();
        const float ratio = std::pow(2.0f, 1.0f / (2.0f * (float)octaveFraction));

        out.assign(n, 0.0f);
        size_t start = 0;
        size_t end = 0;

        for (size_t i = 0; i < n; ++i)
        {
            const float lo = freq[i] / ratio;
            const float hi = freq[i] * ratio;

            while (start < n && freq[start] < lo)
                ++start;

            while (end < n && freq[end] <= hi)
                ++end;

            if (end <= start + 1)
            {
                out[i] = magnitudeDb[i];
                continue;
            }

            double sum = 0.0;

            for (size_t j = start; j < end; ++j)
                sum += std::pow(10.0, (double)magnitudeDb[j] / 20.0);

            out[i] = juce::jlimit(dbFloor, 40.0f, db20((float)(sum / (double)(end - start))));
        }
    }

    inline void smoothPhaseDeg(const std::vector<float>& freq,
                               const std::vector<float>& phaseDeg,
                               int octaveFraction,
                               std::vector<float>& out)
    {
        if (octaveFraction <= 0 || freq.size() < 4 || phaseDeg.size() != freq.size())
        {
            out = phaseDeg;
            return;
        }

        const size_t n = freq.size();
        const float ratio = std::pow(2.0f, 1.0f / (2.0f * (float)octaveFraction));

        out.assign(n, 0.0f);
        size_t start = 0;
        size_t end = 0;

        for (size_t i = 0; i < n; ++i)
        {
            const float lo = freq[i] / ratio;
            const float hi = freq[i] * ratio;

            while (start < n && freq[start] < lo)
                ++start;

            while (end < n && freq[end] <= hi)
                ++end;

            if (end <= start + 1)
            {
                out[i] = phaseDeg[i];
                continue;
            }

            double re = 0.0;
            double im = 0.0;

            for (size_t j = start; j < end; ++j)
            {
                const double angle = (double)phaseDeg[j] * degToRad;
                re += std::cos(angle);
                im += std::sin(angle);
            }

            out[i] = (float)std::atan2(im, re) * radToDeg;
        }
    }

    inline void smoothCoherence(const std::vector<float>& freq,
                                const std::vector<float>& coherence,
                                int octaveFraction,
                                std::vector<float>& out)
    {
        if (octaveFraction <= 0 || freq.size() < 4 || coherence.size() != freq.size())
        {
            out = coherence;
            return;
        }

        const size_t n = freq.size();
        const float ratio = std::pow(2.0f, 1.0f / (2.0f * (float)octaveFraction));

        out.assign(n, 0.0f);
        size_t start = 0;
        size_t end = 0;

        for (size_t i = 0; i < n; ++i)
        {
            const float lo = freq[i] / ratio;
            const float hi = freq[i] * ratio;

            while (start < n && freq[start] < lo)
                ++start;

            while (end < n && freq[end] <= hi)
                ++end;

            if (end <= start + 1)
            {
                out[i] = coherence[i];
                continue;
            }

            double sum = 0.0;

            for (size_t j = start; j < end; ++j)
                sum += coherence[j];

            out[i] = juce::jlimit(0.0f, 1.0f, (float)(sum / (double)(end - start)));
        }
    }

    struct Spectrum
    {
        std::vector<float> freq;
        std::vector<float> magnitudeDb;
        std::vector<float> phaseDeg;
        std::vector<Complex> data;
        int fftSize = 0;
        float sampleRate = 48000.0f;
        bool valid = false;
    };

    class BlockAnalyser
    {
    public:
        BlockAnalyser();

        void prepare(float newSampleRate, int newFftSize,
                     WindowingMethod method = juce::dsp::WindowingFunction<float>::hann);
        void reset();

        int getFftSize() const noexcept { return fftSize; }
        int getNumBins() const noexcept { return numBins; }
        float getBinFrequency(int index) const noexcept;
        float getCoherentGain() const noexcept { return coherentGain; }

        /** Mean square of the analysis window.

            A power taken from a windowed transform has to be divided by this to become the
            sound's power. It is three eighths for a Hann window, and assuming that rather than
            measuring it puts a fixed error into every level that a change of window would move
            without anyone noticing. */
        float getWindowMeanSquare() const noexcept { return windowMeanSquare; }

        void analyse(const float* block, Spectrum& target) const;
        float levelDb(const float* block, char weighting) const;
        float peakDb(const float* block) const;

        /** An extra correction in dB to apply to every bin, empty for none.

            This is how a microphone's frequency response reaches a broadband level. A
            measurement capsule is not flat, and its own calibration file says how far off it
            is at each frequency; without that correction a coloured microphone reports a
            level that is wrong by however much its response happens to differ at the
            frequencies the sound actually occupied. One number cannot stand in for it, because
            the error depends on the spectrum rather than on the level. */
        void setBinCorrectionDb(std::vector<float> correctionDb)
        {
            binCorrectionDb = std::move(correctionDb);
        }

        const std::vector<float>& getBinCorrectionDb() const noexcept { return binCorrectionDb; }

    private:
        void fillScratch(const float* block) const;

        int fftSize = 1024;
        int numBins = 1;
        float sampleRate = 48000.0f;
        float coherentGain = 1.0f;
        float windowMeanSquare = 1.0f;

        /** Empty for no correction. One entry per bin when there is one. */
        std::vector<float> binCorrectionDb;

        juce::dsp::FFT fft { 10 };
        juce::dsp::WindowingFunction<float> window { 1024, juce::dsp::WindowingFunction<float>::hann, false };
        mutable std::vector<float> scratch;
    };

    // Normalised correlation of overlapping, delay-aligned samples. No signal => 0.
    inline float delayConfidence(const std::vector<float>& ref, const std::vector<float>& mic, int lag)
    {
        if (ref.size() != mic.size() || std::abs(lag) >= (int) ref.size())
            return 0.0f;
        const int count = (int) ref.size() - std::abs(lag);
        if (count < 128) return 0.0f;
        const int r0 = std::max(0, -lag), m0 = std::max(0, lag);
        double rMean = 0.0, mMean = 0.0;
        for (int i = 0; i < count; ++i) { rMean += ref[r0+i]; mMean += mic[m0+i]; }
        rMean /= count; mMean /= count;
        double cross = 0.0, rPower = 0.0, mPower = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double r = ref[r0+i] - rMean, m = mic[m0+i] - mMean;
            cross += r*m; rPower += r*r; mPower += m*m;
        }
        return (float) (std::abs(cross) / std::sqrt(std::max(1.0e-30, rPower*mPower)));
    }

    /** Normalised correlation of overlapping, delay-aligned raw samples.

        The pointer form is what the live delay search uses, because it runs on every frame
        from the ring buffer and copying two FFT-sized blocks into vectors first would cost
        more than the search itself. Confidence is the same normalised cross correlation as
        the vector overload below, and 0 for silence, so a peak cannot look believable when
        there was nothing to correlate. */
    inline float delayConfidence(const float* ref, const float* mic, int numSamples, int lag)
    {
        if (ref == nullptr || mic == nullptr || numSamples <= 0
            || std::abs (lag) >= numSamples)
            return 0.0f;

        const auto count = numSamples - std::abs (lag);

        // Too few overlapping samples to say anything: the correlation of a handful of points
        // is decided by noise, and a delay search that trusts it would rotate the average on
        // no evidence.
        if (count < 128)
            return 0.0f;

        const auto r0 = std::max (0, -lag);
        const auto m0 = std::max (0, lag);

        double rMean = 0.0, mMean = 0.0;

        for (int i = 0; i < count; ++i)
        {
            rMean += ref[r0 + i];
            mMean += mic[m0 + i];
        }

        rMean /= count;
        mMean /= count;

        double cross = 0.0, rPower = 0.0, mPower = 0.0;

        for (int i = 0; i < count; ++i)
        {
            // The means are removed first, or a constant offset in either signal would count
            // as agreement at every lag and the peak would stop meaning anything.
            const auto r = (double) ref[r0 + i] - rMean;
            const auto m = (double) mic[m0 + i] - mMean;
            cross += r * m;
            rPower += r * r;
            mPower += m * m;
        }

        return (float) (std::abs (cross) / std::sqrt (std::max (1.0e-30, rPower * mPower)));
    }

    inline constexpr float preferredMantissas[] = { 1.00f, 1.25f, 1.60f, 2.00f, 2.50f,
                                                     3.15f, 4.00f, 5.00f, 6.30f, 8.00f };

    // The preferred R10 series carries only ten mantissas per decade, so it can label
    // 1/1, 1/2 and 1/3 octave but collapses finer fractions onto repeated values.
    // Finer bands therefore fall back to rounded significant digits, which stays unique
    // down to 1/12 octave (two digits for 1/4 and 1/6, three digits for 1/12).
    inline float labelFrequency(float frequency, int octaveFraction)
    {
        if (frequency <= 0.0f)
            return frequency;

        const auto digits = octaveFraction <= 3 ? 0 : (octaveFraction <= 6 ? 2 : 3);

        if (digits == 0)
        {
            const auto exponent = std::pow(10.0f, std::floor(std::log10(frequency)));
            const auto mantissa = frequency / exponent;
            auto best = preferredMantissas[0];
            auto bestError = std::abs(std::log10(mantissa / best));

            for (const auto candidate : preferredMantissas)
            {
                const auto error = std::abs(std::log10(mantissa / candidate));

                if (error < bestError)
                {
                    bestError = error;
                    best = candidate;
                }
            }

            return best * exponent;
        }

        const auto magnitude = std::pow(10.0f, (float) digits - std::ceil(std::log10(frequency)));
        return std::round(frequency * magnitude) / magnitude;
    }

    // 20 Hz to 20 kHz spans 10 octaves, so 1/N octave spacing gives N bands per
    // octave: 11 / 21 / 31 / 41 / 61 / 121 centres for 1/1 .. 1/12 octave.
    inline int bandCountToCount(int octaveFraction)
    {
        return 10 * juce::jmax(1, octaveFraction) + 1;
    }

    inline int nominalOctaveFraction(int bandCount)
    {
        int best = 1;
        auto bestError = std::numeric_limits<int>::max();

        for (int fraction = 1; fraction <= 12; ++fraction)
        {
            const auto expected = bandCountToCount(fraction);
            const auto error = std::abs(expected - bandCount);

            if (error < bestError)
            {
                bestError = error;
                best = fraction;
            }
        }

        return best;
    }

    inline float nominalFrequency(float frequency)
    {
        return labelFrequency(frequency, 3);
    }

    inline std::vector<float> octaveBandFrequencies(int octaveFraction, float minFrequency,
                                                    float maxFrequency)
    {
        const auto fraction = juce::jmax(1, octaveFraction);

        if (! (minFrequency > 0.0f) || ! (maxFrequency >= minFrequency))
            return {};

        const auto ratio = std::pow (2.0f, 1.0f / (float) fraction);

        std::vector<float> centres;

        // Walked rather than counted, because log2(max/min) * fraction is not a whole number
        // and rounding it decides the count by accident. Over 20 Hz to 20 kHz at 1/24 octave it
        // comes to 239.18, so a rounded count gives 240 bands and drops the top one. Walking
        // until the series passes the top of the range and then pinning the last centre to
        // maxFrequency gives the count the spacing implies: 11, 31, 61, 121 and 241. It also
        // keeps the top band labelled 20 kHz rather than the 20.48 kHz the raw series reaches,
        // which matters because that band would otherwise sit above the range the reader was
        // told they were looking at.
        auto frequency = minFrequency;

        for (;;)
        {
            if (frequency > maxFrequency)
                frequency = maxFrequency;

            centres.push_back (labelFrequency (frequency, fraction));

            if (frequency >= maxFrequency)
                break;

            frequency *= ratio;

            // A ratio of 1 or a non-finite range would never terminate; the band count is
            // bounded so a bad argument cannot hang the caller.
            if (centres.size() > 4096)
                break;
        }

        return centres;
    }

    inline std::vector<float> bandPeakDb(const std::vector<float>& frequencies,
                                         const std::vector<float>& magnitudes,
                                         const std::vector<float>& centres,
                                         float minFrequency = 0.0f)
    {
        std::vector<float> bands(centres.size(), dbFloor);
        if (frequencies.size() != magnitudes.size() || centres.empty()) return bands;

        // Nominal centres are not exactly geometric, so band edges are taken from the
        // midpoints between neighbours. Using centre * ratio would leave gaps that
        // silently drop content between bands.
        //
        // The lowest band stops at minFrequency rather than at DC. An audio interface
        // commonly sits a few mV off zero, and bin 0 carries that offset as large
        // energy, which would otherwise be read as the band's level.
        size_t bin = 0;

        for (size_t band = 0; band < centres.size(); ++band)
        {
            const auto low = band == 0 ? minFrequency
                                        : std::sqrt(centres[band - 1] * centres[band]);
            const auto high = band + 1 >= centres.size() ? std::numeric_limits<float>::infinity()
                                                          : std::sqrt(centres[band] * centres[band + 1]);

            while (bin < frequencies.size() && frequencies[bin] < low)
                ++bin;

            while (bin < frequencies.size() && frequencies[bin] < high)
            {
                bands[band] = std::max (bands[band], magnitudes[bin]);
                ++bin;
            }
        }

        return bands;
    }

    inline std::vector<float> linearBarFrequencies(int barCount, float minFrequency,
                                                float maxFrequency)
    {
        std::vector<float> centres((size_t) std::max(1, barCount));
        const auto span = (maxFrequency - minFrequency) / (float) std::max(1, barCount);

        for (size_t i = 0; i < centres.size(); ++i)
            centres[i] = minFrequency + span * ((float) i + 0.5f);

        return centres;
    }

    // Bins below minFrequency are skipped on purpose. On a linear axis the first bar
    // is wide, so a DC offset or sub-audio rumble in bin 0 would otherwise be picked
    // up as the maximum and read tens of dB above the rest of the spectrum.
    inline std::vector<float> linearPeakDb(const std::vector<float>& frequencies,
                                           const std::vector<float>& magnitudes,
                                           int barCount,
                                           float minFrequency,
                                           float maxFrequency)
    {
        std::vector<float> bars((size_t) std::max(1, barCount), dbFloor);
        if (frequencies.size() != magnitudes.size()) return bars;

        const auto span = (maxFrequency - minFrequency) / (float) std::max(1, barCount);

        for (size_t bin = 0; bin < frequencies.size(); ++bin)
        {
            if (frequencies[bin] < minFrequency)
                continue;

            const auto index = (int) ((frequencies[bin] - minFrequency) / span);

            if (index >= 0 && index < (int) bars.size())
                bars[(size_t) index] = std::max(bars[(size_t) index], magnitudes[bin]);
        }

        return bars;
    }

    /** Frequency window the generator spectrum is drawn over.

        The generator display has to follow the settings the user picked: a tone or a
        narrow pink noise band collapses into a single pixel on a fixed 20 Hz - 20 kHz
        axis, so the axis zooms onto the configured range instead. The rules live here
        rather than in the display component so the tests can assert them without
        pulling in the GUI modules.
    */
    struct FrequencyRange
    {
        float low = 20.0f;
        float high = 20000.0f;
    };

    inline FrequencyRange generatorViewRange(const juce::String& type,
                                             float bandLow, float bandHigh,
                                             float toneFrequency,
                                             float sweepStart, float sweepEnd)
    {
        auto low = 20.0f;
        auto high = 20000.0f;

        if (type.contains("Sine"))
        {
            low = high = toneFrequency;
        }
        else if (type.contains("Sweep"))
        {
            low = sweepStart;
            high = sweepEnd;
        }
        else if (type.contains("Pink"))
        {
            low = bandLow;
            high = bandHigh;
        }

        // A tone gives low == high, which is a valid zero octave span, so the guard
        // rejects only ranges that are inverted or non positive.
        if (! (low > 0.0f) || ! (high >= low))
            return { 20.0f, 20000.0f };

        // Padding grows as the configured range narrows, so the rolled-off edges stay
        // visible. It reaches one octave each side for a tone and disappears once the
        // range covers the full 20 Hz - 20 kHz band.
        const auto fullBandOctaves = std::log2(20000.0f / 20.0f);
        const auto spanOctaves = std::log2(high / low);
        const auto padOctaves = juce::jlimit(0.0f, 1.0f, (fullBandOctaves - spanOctaves) / 4.0f);

        return { std::max(5.0f, low * std::pow(2.0f, -padOctaves)),
                 std::min(48000.0f, high * std::pow(2.0f, padOctaves)) };
    }

    /** Picks the measurement and the reference out of the two captured input channels.

        The engine hands the channels back in physical order, so which one is the
        measurement is a wiring decision. Assigning both to the same channel would compare
        a signal with itself, so the caller must keep them apart. A missing second channel
        leaves the reference silent rather than copying the measurement, which would read
        as a perfectly flat transfer function.
    */
    inline void assignMeasurementAndReference(int measurementChannel,
                                              const std::vector<float>& left,
                                              const std::vector<float>& right,
                                              std::vector<float>& measurement,
                                              std::vector<float>& reference)
    {
        const auto onLeft = measurementChannel == 0;

        measurement = onLeft ? left : right;

        if (right.empty())
        {
            // Only one input is present, so the reference stays silent instead of
            // copying the measurement, which would read as a flat transfer function.
            reference.assign(measurement.size(), 0.0f);
            return;
        }

        reference = onLeft ? right : left;

        // The analysis walks both together, so a mismatch would read past the end of the
        // shorter one. Trimming keeps them aligned; padding would invent silence.
        const auto count = std::min(measurement.size(), reference.size());
        measurement.resize(count);
        reference.resize(count);
    }

    /** Round 1-2-5 frequencies inside a log axis, thinned to fit the available width.

        Labels stay on round values instead of arbitrary log positions, and dropping
        every other tick keeps a narrow zoom from printing labels on top of each other.
    */
    inline std::vector<float> logAxisTicks(float low, float high, int maxTicks)
    {
        std::vector<float> ticks;

        if (! (low > 0.0f) || ! (high > low) || maxTicks < 2)
            return ticks;

        for (int decade = 0; decade <= 9; ++decade)
        {
            const auto base = std::pow(10.0f, (float) decade);

            for (const auto mantissa : { 1.0f, 2.0f, 5.0f })
            {
                const auto freq = base * mantissa;

                if (freq >= low && freq <= high)
                    ticks.push_back(freq);
            }
        }

        while ((int) ticks.size() > maxTicks)
        {
            std::vector<float> thinned;

            for (size_t i = 0; i < ticks.size(); i += 2)
                thinned.push_back(ticks[i]);

            ticks.swap(thinned);
        }

        return ticks;
    }

    inline juce::String frequencyTickLabel(float frequency)
    {
        if (frequency >= 1000.0f)
        {
            const auto value = frequency / 1000.0f;
            return juce::String(value, std::abs(value - std::round(value)) < 0.05f ? 0 : 1) + "k";
        }

        return juce::String(std::max(1, juce::roundToInt(frequency)));
    }

    /** Measures the amplitude of individual spectral lines in a block.

        Anything defined in terms of particular tones rather than of a whole spectrum needs
        this: intermodulation products, the leakage between two channels, the harmonic series.
        All of them are one question with different frequencies, and answering it three ways
        would mean three chances to be wrong in three different ways.

        A line is measured by integrating the power across the window's main lobe rather than by
        reading the nearest bin. That costs three bins and removes both of the errors a single
        bin brings with it: the scalloping loss that a tone a fraction of a bin off centre would
        otherwise suffer, and the leakage of the loudest neighbour into whatever was being asked
        about. The powers come out on the same scale as a level reading, so they can be added
        and divided without a fudge factor between them.
    */
    class SpectralLineMeter
    {
    public:
        void prepare (double newSampleRate, int newFftSize)
        {
            // Held here rather than asked of the analyser, which does not expose the rate it
            // was prepared at and only uses it internally.
            rate = std::max (1.0, newSampleRate);
            analyser.prepare ((float) rate, newFftSize);
        }

        double getSampleRate() const noexcept { return rate; }

        /** Transforms a block, so the lines can be read from it. */
        bool analyse (const float* block, int numSamples)
        {
            spectrum.data.clear();

            if (block == nullptr || numSamples < analyser.getFftSize())
                return false;

            analyser.analyse (block, spectrum);

            if (! spectrum.valid || spectrum.data.empty())
                return false;

            const auto numBins = (int) spectrum.data.size();
            const auto windowScale = std::max (1.0e-6f, analyser.getWindowMeanSquare());

            if (linePowerByBin.size() != (size_t) numBins)
                linePowerByBin.assign ((size_t) numBins, 0.0);

            // Everything but DC, on the same scale a level reading is on. DC is left out because
            // an interface sitting a few millivolts off zero is a property of the converter and
            // not of the signal being measured.
            double total = 0.0;

            for (int i = 1; i < numBins - 1; ++i)
            {
                const auto power = 2.0 * (double) std::norm (spectrum.data[(size_t) i]);
                linePowerByBin[(size_t) i] = power
                    / ((double) analyser.getFftSize() * (double) analyser.getFftSize()
                       * (double) windowScale);
                total += linePowerByBin[(size_t) i];
            }

            linePowerByBin[0] = 0.0;

            if (numBins >= 2)
                linePowerByBin[(size_t) (numBins - 1)] = 0.0;

            totalPowerValue = total;
            return true;
        }

        bool isValid() const noexcept { return ! linePowerByBin.empty() && totalPowerValue > 0.0; }

        /** Power at one frequency, taking the main lobe either side of it. */
        double linePower (double frequency) const
        {
            if (linePowerByBin.empty())
                return 0.0;

            const auto centre = (int) std::lround (frequency / binWidth());
            const auto numBins = (int) linePowerByBin.size();
            const auto low = juce::jmax (1, centre - mainLobeBins);
            const auto high = juce::jmin (numBins - 2, centre + mainLobeBins);

            double total = 0.0;

            for (int i = low; i <= high; ++i)
                total += linePowerByBin[(size_t) i];

            return total;
        }

        /** Power at one frequency, taking the loudest bin within a search radius first.

            Used where the exact frequency is predicted but the peak may sit a bin or two away,
            as happens for an intermodulation product derived from a fundamental that was itself
            found rather than given. */
        double linePowerNear (double frequency, double radiusHz) const
        {
            if (linePowerByBin.empty())
                return 0.0;

            const auto centre = frequency / binWidth();
            const auto numBins = (int) linePowerByBin.size();
            const auto low = juce::jmax (1, (int) std::floor (centre - radiusHz / binWidth()));
            const auto high = juce::jmin (numBins - 2, (int) std::ceil (centre + radiusHz / binWidth()));

            auto bestBin = juce::jlimit (1, numBins - 2, (int) std::lround (centre));

            for (int i = low; i <= high; ++i)
                if (linePowerByBin[(size_t) i] > linePowerByBin[(size_t) bestBin])
                    bestBin = i;

            const auto half = juce::jmax (1, mainLobeBins);
            const auto from = juce::jmax (1, bestBin - half);
            const auto to = juce::jmin (numBins - 2, bestBin + half);

            double total = 0.0;

            for (int i = from; i <= to; ++i)
                total += linePowerByBin[(size_t) i];

            return total;
        }

        /** Power across everything except DC. */
        double totalPower() const noexcept { return totalPowerValue; }

        struct Peak
        {
            double frequency = 0.0;
            double power = 0.0;
            int bin = 0;
            bool valid = false;
        };

        /** The strongest line in a range, refined to a fraction of a bin.

            The refinement is a parabola through the peak and its neighbours in decibels, which
            is what lets every intermodulation product be placed from the fundamental rather than
            from a rounded number of bins per hertz. */
        Peak findPeak (double lowHz, double highHz) const
        {
            Peak peak;

            if (linePowerByBin.empty())
                return peak;

            const auto numBins = (int) linePowerByBin.size();
            const auto low = juce::jmax (1, (int) std::ceil (lowHz / binWidth()));
            const auto high = juce::jmin (numBins - 2, (int) std::floor (highHz / binWidth()));

            if (high <= low)
                return peak;

            auto best = low;

            for (int i = low; i <= high; ++i)
                if (linePowerByBin[(size_t) i] > linePowerByBin[(size_t) best])
                    best = i;

            auto fractional = (double) best;

            if (best > low && best < high)
            {
                const auto leftDb = 10.0 * std::log10 (std::max (1.0e-30, linePowerByBin[(size_t) (best - 1)]));
                const auto midDb = 10.0 * std::log10 (std::max (1.0e-30, linePowerByBin[(size_t) best]));
                const auto rightDb = 10.0 * std::log10 (std::max (1.0e-30, linePowerByBin[(size_t) (best + 1)]));

                const auto denominator = leftDb - 2.0 * midDb + rightDb;

                if (std::abs (denominator) > 1.0e-9)
                    fractional += juce::jlimit (-0.5, 0.5, 0.5 * (leftDb - rightDb) / denominator);
            }

            peak.bin = best;
            peak.power = linePowerNear (fractional * binWidth(), (double) mainLobeBins * binWidth());
            peak.frequency = fractional * binWidth();
            peak.valid = peak.power > 0.0;
            return peak;
        }

        double binWidth() const noexcept
        {
            return analyser.getFftSize() > 0 ? rate / (double) analyser.getFftSize() : 0.0;
        }

        /** Bins either side of a peak that are taken as part of its line.

            Three covers a Hann main lobe whole. Wider would begin to swallow the noise floor
            into whatever is being measured and report it as signal. */
        static constexpr int mainLobeBins = 2;

    private:
        BlockAnalyser analyser;
        Spectrum spectrum;
        mutable std::vector<double> linePowerByBin;
        double totalPowerValue = 0.0;
        double rate = 48000.0;
    };

    class DelayFinder
    {
    public:
        DelayFinder();
        ~DelayFinder();

        void prepare(int newFftSize);
        void reset();

        float analyse(const float* ref, const float* meas, float newSampleRate, float maxLagMs);
        float getLastDelaySamples() const noexcept { return lastDelaySamples; }
        float getLastDelayMs() const noexcept { return lastDelayMs; }

    private:
        float magnitudeAtSigned(int index) const;

        int fftSize = 16384;
        int numBins = 1;
        int maxLagSamples = 8192;

        juce::dsp::FFT fft { 14 };
        std::vector<float> refData;
        std::vector<float> measData;

        float lastDelaySamples = 0.0f;
        float lastDelayMs = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayFinder)
    };
}
