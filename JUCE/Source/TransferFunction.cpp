#include "TransferFunction.h"
#include <algorithm>

TransferFunction::TransferFunction()
{
    prepare(48000.0f, 16384);
}

TransferFunction::~TransferFunction() = default;

void TransferFunction::prepare(float newSampleRate, int newFftSize)
{
    sampleRate = std::max(1000.0f, newSampleRate);
    fftSize = std::max(256, newFftSize);

    analyser.prepare(sampleRate, fftSize);
    delayFinder.prepare(fftSize);

    fftSize = analyser.getFftSize();
    inverseTransform = juce::dsp::FFT(juce::roundToInt(std::log2((double) fftSize)));

    reset();
}

void TransferFunction::reset()
{
    averagedRefPower.clear();
    averagedMeasPower.clear();
    averagedCross.clear();
    initialised = false;
    delayFinder.reset();
    delaySamples = 0.0f;
    delayMs = 0.0f;
    frameCounter = 0;
}

void TransferFunction::setAveraging(int numAverages)
{
    averages = juce::jlimit(1, 512, numAverages);
    smoothingAlpha = averages <= 1 ? 0.0f : (float) (averages - 1) / (float) averages;
    reset();
}

void TransferFunction::setDelayCompensation(bool shouldApply)
{
    delayCompensationEnabled = shouldApply;
}

void TransferFunction::setAutomaticDelay(bool shouldTrack, float newMaxLagMs)
{
    automaticDelayEnabled = shouldTrack;
    maxLagMs = juce::jlimit(1.0f, 2000.0f, newMaxLagMs);
}

void TransferFunction::setManualDelay(float newDelaySamples)
{
    delaySamples = newDelaySamples;
    delayMs = delaySamples / sampleRate * 1000.0f;
}

TransferFunction::Result TransferFunction::process(const float* ref,
                                                    const float* meas,
                                                    int numSamples)
{
    Result result;

    result.fftSize = analyser.getFftSize();
    result.sampleRate = sampleRate;
    result.averages = averages;
    result.delaySamples = delaySamples;
    result.delayMs = delayMs;

    if (ref == nullptr || meas == nullptr || numSamples < analyser.getFftSize())
    {
        result.valid = false;
        return result;
    }

    analyser.analyse(ref, refSpectrum);
    analyser.analyse(meas, measSpectrum);

    if (!refSpectrum.valid || !measSpectrum.valid)
    {
        result.valid = false;
        return result;
    }

    const auto bins = refSpectrum.data.size();
    const auto firstFrame = averagedRefPower.size() != bins;

    if (firstFrame)
    {
        averagedRefPower.assign(bins, 0.0f);
        averagedMeasPower.assign(bins, 0.0f);
        averagedCross.assign(bins, dsp::Complex(0.0f, 0.0f));
    }

    if (delayCompensationEnabled
        && ((automaticDelayEnabled && (frameCounter % 8) == 0)
            || (!automaticDelayEnabled && frameCounter == 0)))
    {
        delaySamples = delayFinder.analyse(ref, meas, sampleRate, maxLagMs);
        delayMs = delaySamples / sampleRate * 1000.0f;
    }

    const auto previousWeight = firstFrame ? 0.0f : smoothingAlpha;
    const auto currentWeight = 1.0f - previousWeight;

    for (size_t i = 0; i < bins; ++i)
    {
        const auto refValue = refSpectrum.data[i];
        const auto measValue = measSpectrum.data[i];

        averagedRefPower[i] = averagedRefPower[i] * previousWeight + std::norm(refValue) * currentWeight;
        averagedMeasPower[i] = averagedMeasPower[i] * previousWeight + std::norm(measValue) * currentWeight;
        averagedCross[i] = averagedCross[i] * previousWeight + (std::conj(refValue) * measValue) * currentWeight;
    }

    initialised = true;
    ++frameCounter;

    const auto binRotation = 2.0f * dsp::pi * delaySamples / (float) fftSize;

    result.freq = refSpectrum.freq;
    result.magnitudeDb.resize(bins);
    result.phaseDeg.resize(bins);
    result.coherence.resize(bins);
    result.refMagnitudeDb = refSpectrum.magnitudeDb;
    result.measMagnitudeDb = measSpectrum.magnitudeDb;

    double coherenceSum = 0.0;
    int coherenceCount = 0;

    for (size_t i = 0; i < bins; ++i)
    {
        const auto compensated = delayCompensationEnabled
                               ? averagedCross[i] * std::exp(dsp::Complex(0.0f, binRotation * (float) i))
                               : averagedCross[i];

        const auto refPower = std::max(averagedRefPower[i], 1.0e-30f);
        const auto measPower = std::max(averagedMeasPower[i], 1.0e-30f);

        const auto magnitude = std::abs(compensated) / refPower;
        const auto phase = std::atan2(compensated.imag(), compensated.real()) * dsp::radToDeg;
        const auto coherence = std::min(1.0f, std::norm(compensated) / (refPower * measPower));

        result.magnitudeDb[i] = dsp::db20(magnitude);
        result.phaseDeg[i] = phase;
        result.coherence[i] = coherence;

        if (refSpectrum.freq[i] >= 100.0f)
        {
            coherenceSum += coherence;
            ++coherenceCount;
        }
    }

    dsp::unwrapPhase(result.magnitudeDb, result.phaseDeg);

    result.averageCoherence = coherenceCount > 0 ? (float) (coherenceSum / coherenceCount) : 0.0f;
    result.delaySamples = delaySamples;
    result.delayMs = delayMs;

    buildImpulseResponse(result);

    result.valid = true;
    return result;
}

void TransferFunction::buildImpulseResponse(Result& result)
{
    const auto bins = averagedCross.size();

    if (bins == 0 || !initialised)
        return;

    impulseScratch.assign((size_t) fftSize * 2, 0.0f);

    const auto binRotation = 2.0f * dsp::pi * delaySamples / (float) fftSize;

    for (size_t i = 0; i < bins; ++i)
    {
        const auto refPower = std::max(averagedRefPower[i], 1.0e-30f);
        const auto rotation = delayCompensationEnabled
                            ? std::exp(dsp::Complex(0.0f, binRotation * (float) i))
                            : dsp::Complex(1.0f, 0.0f);
        const auto response = (averagedCross[i] * rotation) / refPower;

        impulseScratch[i * 2] = response.real();
        impulseScratch[i * 2 + 1] = response.imag();
    }

    for (size_t i = 1; i < (size_t) fftSize / 2; ++i)
    {
        const auto index = (size_t) fftSize - i;
        impulseScratch[index * 2] = impulseScratch[i * 2];
        impulseScratch[index * 2 + 1] = -impulseScratch[i * 2 + 1];
    }

    inverseTransform.performRealOnlyInverseTransform(impulseScratch.data());

    result.impulseResponse.resize((size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
        result.impulseResponse[(size_t) i] = impulseScratch[(size_t) i];

    int peakIndex = 0;
    float peakValue = 0.0f;

    for (int i = 0; i < fftSize; ++i)
    {
        const auto value = std::abs(result.impulseResponse[(size_t) i]);

        if (value > peakValue)
        {
            peakValue = value;
            peakIndex = i;
        }
    }

    if (peakValue > 0.0f && peakIndex > 0)
        std::rotate(result.impulseResponse.begin(),
                    result.impulseResponse.begin() + peakIndex,
                    result.impulseResponse.end());
}
