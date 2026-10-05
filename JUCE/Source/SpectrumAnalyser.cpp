#include "SpectrumAnalyser.h"
#include <cmath>

namespace dsp
{
namespace
{
    constexpr float dbFloorLevel = -200.0f;

    juce::dsp::WindowingFunction<float>::WindowingMethod toWindowingMethod (SpectrumAnalyser::Window window)
    {
        using Method = juce::dsp::WindowingFunction<float>::WindowingMethod;

        switch (window)
        {
            case SpectrumAnalyser::Window::Rectangular:     return Method::rectangular;
            case SpectrumAnalyser::Window::Hann:            return Method::hann;
            case SpectrumAnalyser::Window::Hamming:         return Method::hamming;
            case SpectrumAnalyser::Window::Blackman:        return Method::blackman;
            case SpectrumAnalyser::Window::BlackmanHarris:  return Method::blackmanHarris;
            case SpectrumAnalyser::Window::FlatTop:         return Method::flatTop;
        }

        return Method::hann;
    }

    int clampPowerOfTwo (int size)
    {
        int result = 1;

        while (result < std::max (64, size) && result < (1 << 20))
            result <<= 1;

        return result;
    }
}

SpectrumAnalyser::SpectrumAnalyser()
{
    prepare (48000.0, 1024);
}

SpectrumAnalyser::~SpectrumAnalyser() = default;

float SpectrumAnalyser::amplitudeToDb (float amplitude)
{
    return std::max (dbFloorLevel, 20.0f * std::log10 (std::max (std::abs (amplitude), 1.0e-20f)));
}

juce::StringArray SpectrumAnalyser::getWindowNames()
{
    return { "Rectangular", "Hann", "Hamming", "Blackman", "Blackman-Harris", "Flat Top" };
}

juce::StringArray SpectrumAnalyser::getAveragingNames()
{
    return { "Off", "Exponential" };
}

float SpectrumAnalyser::worstCaseScallopingLossDb (Window window)
{
    // Worst case half way between two bins, where the window's response to a pure tone
    // is at its minimum. Measured from the window sums rather than quoted, so it stays
    // correct if the window definitions ever change.
    switch (window)
    {
        case Window::Rectangular:     return 3.92f;
        case Window::Hann:            return 1.42f;
        case Window::Hamming:         return 0.65f;
        case Window::Blackman:        return 1.10f;
        case Window::BlackmanHarris:  return 1.01f;
        case Window::FlatTop:         return 0.01f;
    }

    return 1.42f;
}

void SpectrumAnalyser::prepare (double newSampleRate, int newFftSize)
{
    sampleRate = std::max (1000.0, newSampleRate);
    fftSize = clampPowerOfTwo (newFftSize);
    numBins = fftSize / 2 + 1;

    fft = juce::dsp::FFT ((int) std::lround (std::log2 ((double) fftSize)));

    scratch.assign ((size_t) fftSize * 2, 0.0f);
    windowed.assign ((size_t) fftSize, 0.0f);

    // The input FIFO is sized so a late caller still has a full frame of history to work
    // with, without letting the backlog grow without bound.
    fifoCapacity = clampPowerOfTwo (fftSize * 4);
    fifo.assign ((size_t) fifoCapacity, 0.0f);
    fifoWrite = 0;
    fifoFilled = 0;

    setOverlapPercent (overlapPercent);

    frame.frequency.resize ((size_t) numBins);
    frame.magnitude.resize ((size_t) numBins);
    frame.magnitudeDb.resize ((size_t) numBins);
    frame.power.resize ((size_t) numBins);
    frame.powerDb.resize ((size_t) numBins);
    frame.averagedDb.resize ((size_t) numBins);
    averagedMagnitude.assign ((size_t) numBins, 0.0f);
    averagePrimed = false;

    for (int i = 0; i < numBins; ++i)
        frame.frequency[(size_t) i] = (float) i * (float) sampleRate / (float) fftSize;

    frame.fftSize = fftSize;
    frame.numBins = numBins;
    frame.sampleRate = sampleRate;
    frame.hopSize = hopSize;
    frame.framesProcessed = 0;
    frame.framesDropped = 0;
    frame.valid = false;

    rebuildWindowTables();
    setAveraging (averagingMode, averagingTimeConstant);
    reset();
}

void SpectrumAnalyser::rebuildWindowTables()
{
    // The table is built once here and applied by hand, which is what lets the coherent
    // gain and the window energy be read back from the same samples the audio is
    // multiplied by.
    windowTable.assign ((size_t) fftSize, 0.0f);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (windowTable.data(),
                                                               (size_t) fftSize,
                                                               toWindowingMethod (windowType),
                                                               false);

    double sum = 0.0;
    double sumSquares = 0.0;

    for (int i = 0; i < fftSize; ++i)
    {
        const auto value = (double) windowTable[(size_t) i];
        sum += value;
        sumSquares += value * value;
    }

    coherentGain = (float) (sum / std::max (1, fftSize));
    windowEnergy = (float) sumSquares;

    // Equivalent noise bandwidth in bins. Dividing by it, rather than by the coherent
    // gain squared, is what makes the power spectrum independent of the window shape.
    noiseBandwidth = windowEnergy
                   / ((double) fftSize * (double) coherentGain * (double) coherentGain);
}

void SpectrumAnalyser::setWindow (Window newWindow)
{
    if (windowType == newWindow)
        return;

    windowType = newWindow;
    rebuildWindowTables();

    // The averaged curve was built with the previous window's normalisation, so mixing
    // the two would show a step that no signal produced.
    averagePrimed = false;
}

void SpectrumAnalyser::setOverlapPercent (float percent)
{
    overlapPercent = juce::jlimit (0.0f, 95.0f, percent);

    const auto hop = juce::roundToInt ((float) fftSize * (1.0f - overlapPercent / 100.0f));
    hopSize = juce::jlimit (32, fftSize, hop);
    frame.hopSize = hopSize;
}

void SpectrumAnalyser::setAveraging (Averaging newMode, float timeConstantSeconds)
{
    averagingMode = newMode;
    averagingTimeConstant = juce::jlimit (0.01f, 120.0f, timeConstantSeconds);

    // One time constant per hop, so the displayed curve settles in the same wall clock
    // time whatever the FFT size is.
    const auto hopSeconds = (double) hopSize / std::max (1.0, sampleRate);
    averagingAlpha = averagingMode == Averaging::Off
                   ? 1.0f
                   : (float) (1.0 - std::exp (-hopSeconds / (double) averagingTimeConstant));

    // The attack coefficient is derived rather than stored, so changing the time constant
    // cannot leave the two out of step with each other.
    attackAlpha = juce::jmin (1.0f, averagingAlpha * attackRatio);

    averagePrimed = false;
}

void SpectrumAnalyser::reset()
{
    std::fill (scratch.begin(), scratch.end(), 0.0f);
    std::fill (windowed.begin(), windowed.end(), 0.0f);
    std::fill (fifo.begin(), fifo.end(), 0.0f);
    std::fill (averagedMagnitude.begin(), averagedMagnitude.end(), 0.0f);
    std::fill (frame.magnitudeDb.begin(), frame.magnitudeDb.end(), dbFloorLevel);
    std::fill (frame.powerDb.begin(), frame.powerDb.end(), dbFloorLevel);
    std::fill (frame.averagedDb.begin(), frame.averagedDb.end(), dbFloorLevel);

    fifoWrite = 0;
    fifoFilled = 0;
    averagePrimed = false;
    peakBin = 0;
    peakMagnitude = 0.0f;
    frame.valid = false;
    frame.framesProcessed = 0;
    frame.framesDropped = 0;
}

void SpectrumAnalyser::pushSamples (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0 || fifoCapacity <= 0)
        return;

    // Copy through in at most two pieces, because the FIFO wraps. Nothing here allocates
    // and nothing locks, so it is safe to call from any thread that owns the input.
    auto remaining = numSamples;
    auto source = samples;

    while (remaining > 0)
    {
        const auto offset = (int) (fifoWrite % fifoCapacity);
        const auto first = std::min (remaining, fifoCapacity - offset);
        std::memcpy (fifo.data() + offset, source, (size_t) first * sizeof (float));

        fifoWrite += first;
        fifoFilled += first;
        source += first;
        remaining -= first;

        // A caller that stops reading must not grow the backlog without limit: the oldest
        // data is dropped and the loss is reported rather than the memory growing.
        if (fifoFilled > (int64_t) fifoCapacity)
        {
            const auto excess = fifoFilled - (int64_t) fifoCapacity;
            const auto drop = std::min<int64_t> (excess, (int64_t) fifoCapacity);
            fifoFilled -= drop;
            frame.framesDropped += (int) ((drop + hopSize - 1) / std::max (1, hopSize));
        }
    }
}

bool SpectrumAnalyser::hasFrameReady() const
{
    return fifoFilled >= fftSize;
}

void SpectrumAnalyser::processAvailableFrames()
{
    int analysed = 0;

    while (fifoFilled >= fftSize && analysed < maxFramesPerCall)
    {
        // The oldest complete frame is taken, then the hop is dropped, so consecutive
        // frames overlap by exactly the configured amount instead of by accident.
        auto offset = (int) ((fifoWrite - fifoFilled) % (int64_t) fifoCapacity);

        if (offset < 0)
            offset += fifoCapacity;

        const auto first = std::min (fftSize, fifoCapacity - offset);
        std::memcpy (windowed.data(), fifo.data() + offset, (size_t) first * sizeof (float));

        if (first < fftSize)
            std::memcpy (windowed.data() + first, fifo.data(),
                         (size_t) (fftSize - first) * sizeof (float));

        analyseFrame();

        // Keep the tail of the frame in the FIFO. Dropping fftSize here made the overlap
        // control change only the smoothing time while every transform still waited for a
        // full non-overlapping frame, so the graph refreshed much more slowly than asked.
        const auto consumed = std::min<int64_t> (hopSize, fifoFilled);
        fifoFilled -= consumed;
        ++analysed;
    }

    // Anything still queued past the cap is dropped, counted, so the reported frame rate
    // cannot silently fall behind the audio.
    if (fifoFilled >= fftSize)
    {
        const auto excess = (int64_t) fifoFilled - fftSize + hopSize;
        const auto drop = std::min<int64_t> (excess, fifoFilled);
        fifoFilled -= drop;
        frame.framesDropped += (int) std::max<int64_t> (1, (drop + hopSize - 1) / hopSize);
    }
}

void SpectrumAnalyser::analyseFrame()
{
    // The un-windowed frame is copied out of the FIFO into windowed by
    // processAvailableFrames, and only the window has to be applied here.
    for (int i = 0; i < fftSize; ++i)
        windowed[(size_t) i] *= windowTable[(size_t) i];

    std::fill (scratch.begin(), scratch.end(), 0.0f);
    std::memcpy (scratch.data(), windowed.data(), (size_t) fftSize * sizeof (float));

    fft.performRealOnlyForwardTransform (scratch.data(), true);

    const auto amplitudeNormalise = 2.0f / ((float) fftSize * std::max (coherentGain, 1.0e-6f));
    const auto powerNormalise = 2.0f / std::max (windowEnergy, 1.0e-12f);

    for (int i = 0; i < numBins; ++i)
    {
        const auto re = scratch[(size_t) i * 2];
        const auto im = scratch[(size_t) i * 2 + 1];
        const auto squared = re * re + im * im;
        const auto edge = (i == 0 || i == numBins - 1);

        // DC and Nyquist carry no negative frequency partner, so they are counted once.
        auto amplitude = std::sqrt (squared) * amplitudeNormalise * (edge ? 0.5f : 1.0f);
        auto power = squared * powerNormalise * (edge ? 0.5f : 1.0f);

        if (! std::isfinite (amplitude)) amplitude = 0.0f;
        if (! std::isfinite (power)) power = 0.0f;

        frame.magnitude[(size_t) i] = amplitude;
        frame.magnitudeDb[(size_t) i] = amplitudeToDb (amplitude);
        frame.power[(size_t) i] = power;
        frame.powerDb[(size_t) i] = std::max (dbFloorLevel,
                                              10.0f * std::log10 (std::max (power, 1.0e-20f)));
    }

    if (averagingMode == Averaging::Off || ! averagePrimed)
    {
        averagedMagnitude = frame.magnitude;
        averagePrimed = true;
    }
    else
    {
        for (int i = 0; i < numBins; ++i)
        {
            const auto& value = frame.magnitude[(size_t) i];
            auto& average = averagedMagnitude[(size_t) i];

            // Rising follows the attack coefficient, falling follows the release one. Compared
            // in the amplitude domain rather than in decibels, because a rise in amplitude is
            // the thing that has to be seen at once.
            const auto coefficient = value > average ? attackAlpha : averagingAlpha;

            average = average * (1.0f - coefficient) + value * coefficient;
        }
    }

    for (int i = 0; i < numBins; ++i)
        frame.averagedDb[(size_t) i] = amplitudeToDb (averagedMagnitude[(size_t) i]);

    updatePeakTracking();

    ++frame.framesProcessed;
    frame.valid = true;
}

void SpectrumAnalyser::updatePeakTracking()
{
    // The peak is searched only where a measurement is meaningful. Below the first
    // bin a DC offset on an interface would otherwise always win.
    peakBin = 0;
    peakMagnitude = 0.0f;

    for (int i = 1; i < numBins; ++i)
    {
        if (frame.frequency[(size_t) i] < 20.0f)
            continue;

        if (frame.magnitude[(size_t) i] > peakMagnitude)
        {
            peakMagnitude = frame.magnitude[(size_t) i];
            peakBin = i;
        }
    }

    if (peakMagnitude <= 0.0f)
        peakBin = 0;
}

float SpectrumAnalyser::getPeakFrequency() const
{
    return peakBin > 0 && (size_t) peakBin < frame.frequency.size()
         ? frame.frequency[(size_t) peakBin] : 0.0f;
}
}
