#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
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

        void analyse(const float* block, Spectrum& target) const;
        float levelDb(const float* block, char weighting) const;
        float peakDb(const float* block) const;

    private:
        void fillScratch(const float* block) const;

        int fftSize = 1024;
        int numBins = 1;
        float sampleRate = 48000.0f;
        float coherentGain = 1.0f;

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
        const auto intervals = (int) std::lround(std::log2(maxFrequency / minFrequency) * (float) fraction);
        std::vector<float> centres;
        centres.reserve((size_t) intervals + 1);

        for (int i = 0; i <= intervals; ++i)
            centres.push_back(labelFrequency(minFrequency * std::pow(2.0f, (float) i / (float) fraction),
                                             fraction));

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
        for (size_t band = 0; band < centres.size(); ++band)
        {
            const auto low = band == 0 ? minFrequency
                                        : std::sqrt(centres[band - 1] * centres[band]);
            const auto high = band + 1 >= centres.size() ? std::numeric_limits<float>::infinity()
                                                          : std::sqrt(centres[band] * centres[band + 1]);

            for (size_t bin = 0; bin < frequencies.size(); ++bin)
                if (frequencies[bin] >= low && frequencies[bin] < high)
                    bands[band] = std::max(bands[band], magnitudes[bin]);
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
