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
    averagedCrossRaw.clear();
    initialised = false;
    averagesInvalid = true;
    compensatedDelaySamples = 0.0f;
    delayFinder.reset();
    delaySamples = 0.0f;
    delayMs = 0.0f;
    delayTrusted = false;
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

void TransferFunction::setBlankingRangeDb(float rangeDb)
{
    blankingRangeDb = juce::jlimit(10.0f, 120.0f, rangeDb);
}

float TransferFunction::findDelay(const float* ref, const float* meas)
{
    if (ref == nullptr || meas == nullptr)
        return delaySamples;

    trackDelay(ref, meas);

    // Every frame averaged so far was compensated with a different delay, so the averages
    // start again and the whole measurement shares one compensation.
    averagesInvalid = true;

    return delaySamples;
}

void TransferFunction::trackDelay(const float* ref, const float* meas)
{
    const auto found = delayFinder.analyse(ref, meas, sampleRate, getMaxReachableDelayMs());

    // The strongest point of the correlation is only believable if the aligned pair
    // really looks like one signal. Otherwise the peak belongs to the noise and rotating
    // the average by it would wreck the phase while leaving magnitude and coherence, which
    // do not care about phase, looking perfect.
    const auto confidence = dsp::delayConfidence(ref, meas, (int) fftSize, juce::roundToInt(found));
    delayTrusted = confidence >= delayConfidenceThreshold;

    if (! delayTrusted)
        return;

    delaySamples = found;
    delayMs = delaySamples / sampleRate * 1000.0f;
}

void TransferFunction::setManualDelay(float newDelaySamples)
{
    delaySamples = newDelaySamples;
    delayMs = delaySamples / sampleRate * 1000.0f;

    // The averages held so far were built with the previous compensation.
    averagesInvalid = true;
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

    // Broadband RMS and peak of the two blocks, taken before anything can reject the frame:
    // they describe the signal that arrived, not the measurement. A microphone with signal
    // and a silent reference still has a level worth reading, and the peak is sampled
    // separately from the running total because RMS squares and averages, so one loud
    // transient would be invisible in it.
    {
        double refSquare = 0.0, measSquare = 0.0, refPeak = 0.0, measPeak = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto r = (double) ref[i], m = (double) meas[i];
            refSquare += r * r; measSquare += m * m;
            refPeak = std::max (refPeak, std::abs (r));
            measPeak = std::max (measPeak, std::abs (m));
        }

        result.refRmsDb = dsp::db10 ((float) (refSquare / numSamples));
        result.refPeakDb = dsp::db20 ((float) refPeak);
        result.measRmsDb = dsp::db10 ((float) (measSquare / numSamples));
        result.measPeakDb = dsp::db20 ((float) measPeak);
    }

    analyser.analyse(ref, refSpectrum);
    analyser.analyse(meas, measSpectrum);

    if (!refSpectrum.valid || !measSpectrum.valid)
    {
        result.valid = false;
        return result;
    }

    const auto bins = refSpectrum.data.size();

    // The loop delay moves as soon as the speaker and the microphone run off two different
    // device clocks, which is the normal case: the generator is the reference and its
    // samples are played by the output device while the microphone captures on the input
    // device. So the delay is tracked on every frame and each frame is compensated before
    // it joins the average. Rotating the finished average instead would leave every frame
    // that was recorded with a different delay mis-compensated, and the coherence would
    // fall further the longer it averaged.
    if (delayCompensationEnabled && (automaticDelayEnabled || frameCounter == 0))
        trackDelay(ref, meas);

    // A search that moved the compensation by more than the jitter restarts the average,
    // because the frames held so far line up with a delay that is no longer the one being
    // applied. A search that found the same delay again leaves the average alone.
    if (std::abs (delaySamples - compensatedDelaySamples) > delayJitterSamples)
    {
        compensatedDelaySamples = delaySamples;
        averagesInvalid = true;
    }

    const auto firstFrame = averagesInvalid || averagedRefPower.size() != bins;

    if (firstFrame)
    {
        averagedRefPower.assign(bins, 0.0f);
        averagedMeasPower.assign(bins, 0.0f);
        averagedCross.assign(bins, dsp::Complex(0.0f, 0.0f));
        averagedCrossRaw.assign(bins, dsp::Complex(0.0f, 0.0f));
        compensatedDelaySamples = delaySamples;
        averagesInvalid = false;
    }

    const auto previousWeight = firstFrame ? 0.0f : smoothingAlpha;
    const auto currentWeight = 1.0f - previousWeight;
    const auto binRotation = 2.0f * dsp::pi * delaySamples / (float) fftSize;

    for (size_t i = 0; i < bins; ++i)
    {
        const auto refValue = refSpectrum.data[i];
        const auto measValue = measSpectrum.data[i];

        averagedRefPower[i] = averagedRefPower[i] * previousWeight + std::norm(refValue) * currentWeight;
        averagedMeasPower[i] = averagedMeasPower[i] * previousWeight + std::norm(measValue) * currentWeight;

        const auto rotation = delayCompensationEnabled
                            ? std::exp (dsp::Complex(0.0f, binRotation * (float) i))
                            : dsp::Complex(1.0f, 0.0f);

        const auto cross = std::conj(refValue) * measValue;

        averagedCross[i] = averagedCross[i] * previousWeight + cross * rotation * currentWeight;
        averagedCrossRaw[i] = averagedCrossRaw[i] * previousWeight + cross * currentWeight;
    }

    initialised = true;
    ++frameCounter;

    result.freq = refSpectrum.freq;
    result.magnitudeDb.resize(bins);
    result.phaseDeg.resize(bins);
    result.phaseDegWrapped.resize(bins);
    result.coherence.resize(bins);
    result.refMagnitudeDb = refSpectrum.magnitudeDb;
    result.measMagnitudeDb = measSpectrum.magnitudeDb;

    // Bins whose reference sits far below the strongest bin carry no measurement. Their
    // coherence would be the ratio of two near-zero powers, which reads as a confident
    // 1.0 and their magnitude as a huge gain, so they are blanked rather than divided.
    auto peakReferencePower = 0.0f;

    for (size_t i = 1; i < bins; ++i)
        peakReferencePower = std::max(peakReferencePower, averagedRefPower[i]);

    const auto blankingPower = peakReferencePower * std::pow (10.0f, -blankingRangeDb / 10.0f);

    result.binValid.resize(bins);
    result.peakReferenceDb = 10.0f * std::log10 (std::max(peakReferencePower, 1.0e-30f));

    double coherenceWeighted = 0.0;
    double coherenceWeight = 0.0;
    auto coherenceCount = 0;

    for (size_t i = 0; i < bins; ++i)
    {
        const auto refPower = averagedRefPower[i];
        const auto measPower = std::max(averagedMeasPower[i], 1.0e-30f);
        const auto& compensated = averagedCross[i];
        const auto magnitude = dsp::db20 (std::abs (compensated) / std::max(refPower, 1.0e-30f));

        // A loud reference is only half the condition. Where the measurement itself has
        // nothing left, the cross spectrum is the reference multiplied by noise, and its
        // magnitude lands far below the bottom of the pane while its phase draws a full,
        // confident curve across a band that was never measured. Coherence cannot catch it,
        // because it is a ratio and stays finite. So a bin also has to carry signal of its
        // own to be shown at all.
        //
        // A bin that carries signal but correlates poorly is a different case, and it stays
        // on screen: its magnitude is low by a known amount and its phase is spread out, and
        // both of those are facts about the measurement. The coherence pane and the validity
        // banner are what report that the band cannot be trusted, so blanking the phase here
        // would hide the evidence instead of the conclusion.
        const auto measurable = i > 0 && refPower > blankingPower && refPower > 1.0e-30f
                             && magnitude > measurementFloorDb;

        if (! measurable)
        {
            result.binValid[i] = 0;
            result.magnitudeDb[i] = dsp::dbFloor;
            result.phaseDeg[i] = 0.0f;
            result.phaseDegWrapped[i] = 0.0f;
            result.coherence[i] = 0.0f;
            ++result.blankedBins;
            continue;
        }

        const auto coherence = juce::jlimit (0.0f, 1.0f, std::norm (compensated) / (refPower * measPower));

        result.binValid[i] = 1;
        result.magnitudeDb[i] = magnitude;

        // Folded here, before the unwrap below rewrites the series, so both forms come from
        // the one atan2 rather than one being recovered from the other afterwards.
        result.phaseDegWrapped[i] = dsp::wrapDeg (
            std::atan2 (compensated.imag(), compensated.real()) * dsp::radToDeg);
        result.phaseDeg[i] = result.phaseDegWrapped[i];
        result.coherence[i] = coherence;
        ++result.validBins;

        // The average is weighted by reference power: a flat mean would count a 20 Hz wide
        // bin at the bottom the same as a wide one at the top and hide where the
        // measurement actually holds.
        if (refSpectrum.freq[i] >= 100.0f)
        {
            coherenceWeighted += (double) result.coherence[i] * (double) refPower;
            coherenceWeight += (double) refPower;
            ++coherenceCount;
        }
    }

    dsp::unwrapPhase(result.magnitudeDb, result.phaseDeg);

    // A bin whose atan2 landed on a non-finite value would poison the whole unwrap, because
    // the unwrap carries its predecessor forward. Anything that is not a real angle is put
    // back to zero here rather than being allowed to spread along the series.
    for (auto& phase : result.phaseDeg)
        if (! std::isfinite (phase))
            phase = 0.0f;

    for (auto& phase : result.phaseDegWrapped)
        if (! std::isfinite (phase))
            phase = 0.0f;

    result.phaseDegSmoothed = phaseSmoothingBins > 0
                            ? dsp::smoothUnwrappedPhase (result.phaseDeg, result.binValid,
                                                         phaseSmoothingBins)
                            : result.phaseDeg;

    // The second opinion on the delay. It is fitted on the smoothed curve because a straight
    // line survives the moving average unchanged while a noisy one does not, so smoothing can
    // tighten this estimate but never moves it away from the delay that produced the line.
    {
        // Fitted on the unrotated phase. The compensated phase has had its slope taken out
        // by design, so fitting it would report a delay of zero for every measurement and
        // agree with nothing. The unrotated series is unwrapped with the same magnitude
        // gating, because a bin carrying no signal has no phase to unwrap through.
        std::vector<float> rawPhase (bins);

        for (size_t i = 0; i < bins; ++i)
            rawPhase[i] = result.binValid[i] != 0
                        ? std::atan2 (averagedCrossRaw[i].imag(),
                                      averagedCrossRaw[i].real()) * dsp::radToDeg
                        : 0.0f;

        dsp::unwrapPhase (result.magnitudeDb, rawPhase);
        const auto rawSmoothed = dsp::smoothUnwrappedPhase (rawPhase, result.binValid,
                                                            juce::jmax (1, phaseSmoothingBins));

        const auto slope = dsp::estimatePhaseSlopeDelay (
            result.freq, rawSmoothed, result.coherence, result.binValid, sampleRate);

        result.phaseSlopeValid = slope.valid;
        result.phaseSlopeDelaySamples = slope.delaySamples;
        result.phaseSlopeDelayMs = slope.delayMs;
        result.phaseSlopeDegPerHz = slope.slopeDegPerHz;
        result.phaseSlopeScatterDeg = slope.scatterDeg;
        result.phaseSlopeBins = slope.binsUsed;
    }

    // Agreement is reported rather than averaged into one number. Two methods that differ are
    // saying one of them latched onto something, and splitting the difference would hide the
    // only interesting thing in the measurement.
    result.delayMethodsAgree = result.phaseSlopeValid && delayTrusted
                            && std::abs (result.phaseSlopeDelayMs - delayMs) < 0.1f;

    result.averageCoherence = coherenceWeight > 0.0
                            ? (float) (coherenceWeighted / coherenceWeight)
                            : 0.0f;

    // No measurable bin at all means the reference is silent, which must not be reported
    // as a perfect measurement.
    if (result.validBins == 0 || coherenceCount == 0)
        result.averageCoherence = 0.0f;
    result.delaySamples = delaySamples;
    result.delayMs = delayMs;
    result.delayTrusted = delayTrusted;
    result.temperatureC = temperatureC;
    result.speedOfSound = dsp::speedOfSoundMetresPerSecond (temperatureC);

    // Only a delay that was actually believed becomes a distance. Publishing a length from a
    // correlation peak that failed its own confidence check would put a confident number on
    // screen for a measurement that in fact found nothing.
    result.delayDistanceM = delayTrusted
                          ? dsp::distanceFromDelayMetres (delaySamples, sampleRate, temperatureC)
                          : 0.0f;

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

    // The same threshold the bins were judged by, so a bin that was not measured does not
    // contribute to the impulse at all.
    auto peakReferencePower = 0.0f;

    for (size_t i = 0; i < bins; ++i)
        peakReferencePower = std::max (peakReferencePower, averagedRefPower[i]);

    const auto blankingPower = peakReferencePower * std::pow (10.0f, -blankingRangeDb / 10.0f);

    // Bin 0 is left out on purpose. It carries the DC term, which for audio is an offset
    // rather than a signal, and dividing its near-zero power by a floor produces a response
    // thousands of times larger than anything real. Left in, it put its peak at sample 0 for
    // every measurement and hid the arrival the impulse exists to show.
    for (size_t i = 1; i < bins; ++i)
    {
        if (averagedRefPower[i] <= blankingPower || averagedRefPower[i] <= 1.0e-30f)
            continue;

        const auto refPower = averagedRefPower[i];
        const auto response = averagedCross[i] / refPower;

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

    // Recorded before the rotation, because after it the peak is at sample 0 by construction
    // and the arrival this impulse was measured to show would be gone.
    result.impulsePeakIndex = peakIndex;
    result.impulsePeakValue = peakValue;

    if (peakValue > 0.0f && peakIndex > 0)
        std::rotate(result.impulseResponse.begin(),
                    result.impulseResponse.begin() + peakIndex,
                    result.impulseResponse.end());
}
