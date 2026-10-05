#include "DSP.h"
#include <limits>

namespace dsp
{

BlockAnalyser::BlockAnalyser()
{
    prepare(48000.0f, 1024);
}

void BlockAnalyser::prepare(float newSampleRate, int newFftSize,
                            WindowingMethod method)
{
    const auto order = juce::jlimit(6, 21, (int) std::round(std::log2((double) std::max(64, newFftSize))));
    const auto newSize = 1 << order;

    sampleRate = std::max(1.0f, newSampleRate);
    fftSize = newSize;
    numBins = fftSize / 2 + 1;

    fft = juce::dsp::FFT(order);
    window.fillWindowingTables((size_t) fftSize, method, false);
    scratch.assign((size_t) fftSize * 2, 0.0f);

    std::vector<float> probe((size_t) fftSize, 1.0f);
    window.multiplyWithWindowingTable(probe.data(), fftSize);

    double total = 0.0;
    double squares = 0.0;

    for (auto value : probe)
    {
        total += (double) value;
        squares += (double) value * value;
    }

    coherentGain = (float) (total / std::max (1.0, (double) fftSize));

    // The window's mean square, which is how much of the sound's own power it lets through.
    // A power taken from a windowed transform is this much smaller than the sound's power, and
    // leaving it out puts every reading a fixed few decibels low. Measured from the window
    // rather than assumed, because that assumption has to hold for whatever window is in use
    // rather than only for Hann.
    windowMeanSquare = (float) (squares / std::max (1.0, (double) fftSize));
}

void BlockAnalyser::reset()
{
    juce::FloatVectorOperations::clear(scratch.data(), (int) scratch.size());
}

float BlockAnalyser::getBinFrequency(int index) const noexcept
{
    return (float) index * sampleRate / (float) fftSize;
}

void BlockAnalyser::fillScratch(const float* block) const
{
    juce::FloatVectorOperations::clear(scratch.data(), (int) scratch.size());
    juce::FloatVectorOperations::copy(scratch.data(), block, fftSize);
    window.multiplyWithWindowingTable(scratch.data(), fftSize);
}

void BlockAnalyser::analyse(const float* block, Spectrum& target) const
{
    target.fftSize = fftSize;
    target.sampleRate = sampleRate;

    if (block == nullptr)
    {
        target.valid = false;
        return;
    }

    fillScratch(block);
    fft.performRealOnlyForwardTransform(scratch.data(), true);

    target.freq.resize((size_t) numBins);
    target.magnitudeDb.resize((size_t) numBins);
    target.phaseDeg.resize((size_t) numBins);
    target.data.resize((size_t) numBins);

    const auto normalise = 2.0f / ((float) fftSize * std::max(coherentGain, 1.0e-6f));

    for (int i = 0; i < numBins; ++i)
    {
        const auto re = scratch[i * 2];
        const auto im = scratch[i * 2 + 1];
        const Complex value(re, im);

        target.data[(size_t) i] = value;
        target.freq[(size_t) i] = getBinFrequency(i);

        auto amplitude = std::abs(value) * normalise;

        if (i == 0 || i == numBins - 1)
            amplitude *= 0.5f;

        target.magnitudeDb[(size_t) i] = db20(amplitude);
        target.phaseDeg[(size_t) i] = std::atan2(im, re) * radToDeg;
    }

    target.valid = true;
}

float BlockAnalyser::levelDb(const float* block, char weighting) const
{
    if (block == nullptr)
        return dbFloor;

    juce::FloatVectorOperations::clear(scratch.data(), (int) scratch.size());
    juce::FloatVectorOperations::copy(scratch.data(), block, fftSize);
    fft.performRealOnlyForwardTransform(scratch.data(), true);

    double sum = 0.0;

    for (int i = 0; i < numBins; ++i)
    {
        const auto gain = weightingGain(getBinFrequency(i), weighting);
        const auto re = (double) (scratch[i * 2] * gain);
        const auto im = (double) (scratch[i * 2 + 1] * gain);

        auto power = re * re + im * im;

        // The correction is a level, so it scales the power rather than the amplitude. Applied
        // per bin and before the sum, which is the whole point: applying it after the sum would
        // be a single number standing in for a spectrum.
        if (i < (int) binCorrectionDb.size() && binCorrectionDb[(size_t) i] != 0.0f)
            power *= std::pow(10.0, (double) binCorrectionDb[(size_t) i] / 10.0);

        sum += (i == 0 || i == numBins - 1) ? power : 2.0 * power;
    }

    return db10((float) (sum / ((double) fftSize * (double) fftSize)));
}

float BlockAnalyser::peakDb(const float* block) const
{
    if (block == nullptr)
        return dbFloor;

    return db20(juce::FloatVectorOperations::findMaximum(block, fftSize));
}

DelayFinder::DelayFinder()
{
    prepare(16384);
}

DelayFinder::~DelayFinder() = default;

void DelayFinder::prepare(int newFftSize)
{
    const auto order = juce::jlimit(6, 21, (int) std::round(std::log2((double) std::max(64, newFftSize))));
    const auto newSize = 1 << order;

    fftSize = newSize;
    numBins = fftSize / 2 + 1;
    fft = juce::dsp::FFT(order);

    refData.assign((size_t) fftSize * 2, 0.0f);
    measData.assign((size_t) fftSize * 2, 0.0f);
}

void DelayFinder::reset()
{
    lastDelaySamples = 0.0f;
    lastDelayMs = 0.0f;
}

float DelayFinder::analyse(const float* ref, const float* meas,
                           float newSampleRate, float maxLagMs)
{
    if (ref == nullptr || meas == nullptr || newSampleRate <= 0.0f)
        return lastDelaySamples;

    juce::FloatVectorOperations::clear(refData.data(), (int) refData.size());
    juce::FloatVectorOperations::clear(measData.data(), (int) measData.size());
    juce::FloatVectorOperations::copy(refData.data(), ref, fftSize);
    juce::FloatVectorOperations::copy(measData.data(), meas, fftSize);

    fft.performRealOnlyForwardTransform(refData.data(), true);
    fft.performRealOnlyForwardTransform(measData.data(), true);

    for (int i = 0; i < numBins; ++i)
    {
        const auto ar = refData[(size_t) i * 2];
        const auto ai = refData[(size_t) i * 2 + 1];
        const auto br = measData[(size_t) i * 2];
        const auto bi = measData[(size_t) i * 2 + 1];

        refData[(size_t) i * 2] = ar * br + ai * bi;
        refData[(size_t) i * 2 + 1] = ai * br - ar * bi;
    }

    fft.performRealOnlyInverseTransform(refData.data());

    maxLagSamples = juce::jlimit(1, fftSize / 2 - 2, (int) (maxLagMs * 0.001f * newSampleRate));

    auto magnitudeAt = [this] (int index)
    {
        return magnitudeAtSigned(index);
    };

    auto findBest = [this, &magnitudeAt] (float polarity, int& index)
    {
        auto best = -std::numeric_limits<float>::max();

        for (int lag = 0; lag <= maxLagSamples; ++lag)
        {
            const auto value = magnitudeAt(lag) * polarity;

            if (value > best)
            {
                best = value;
                index = lag;
            }

            const auto wrapped = magnitudeAt(fftSize - lag) * polarity;

            if (wrapped > best)
            {
                best = wrapped;
                index = fftSize - lag;
            }
        }

        return best;
    };

    int bestIndex = 0;
    const auto bestValue = findBest(1.0f, bestIndex);
    const auto polarity = bestValue >= 0.0f ? 1.0f : -1.0f;

    if (polarity < 0.0f)
        findBest(polarity, bestIndex);

    const auto before = magnitudeAtSigned(bestIndex - 1) * polarity;
    const auto centre = magnitudeAtSigned(bestIndex) * polarity;
    const auto after = magnitudeAtSigned(bestIndex + 1) * polarity;

    auto refinement = 0.0f;
    const auto denominator = before - 2.0f * centre + after;

    if (std::abs(denominator) > 1.0e-20f)
        refinement = juce::jlimit(-1.0f, 1.0f, 0.5f * (before - after) / denominator);

    const auto signedIndex = bestIndex > fftSize / 2 ? bestIndex - fftSize : bestIndex;
    const auto delay = -(float) signedIndex - refinement;

    lastDelaySamples = juce::jlimit(-(float) (fftSize / 2), (float) (fftSize / 2), delay);
    lastDelayMs = delay / newSampleRate * 1000.0f;

    return lastDelaySamples;
}

float DelayFinder::magnitudeAtSigned(int index) const
{
    if (index < 0)
        index += fftSize;

    if (index >= fftSize)
        index -= fftSize;

    return refData[(size_t) juce::jlimit(0, fftSize - 1, index)];
}
}
