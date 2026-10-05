#include "LevelMeter.h"
#include <cmath>

namespace dsp
{
namespace
{
    constexpr float levelFloorDb = -200.0f;
    constexpr float tinyLevel = 1.0e-10f;
}

LevelMeter::LevelMeter()
{
    prepare (48000.0);
}

LevelMeter::~LevelMeter() = default;

float LevelMeter::amplitudeToDb (float amplitude)
{
    return std::max (levelFloorDb, 20.0f * std::log10 (std::max (std::abs (amplitude), tinyLevel)));
}

float LevelMeter::crestFactor (float peak, float rms)
{
    if (rms <= tinyLevel)
        return 0.0f;

    return std::max (0.0f, 20.0f * std::log10 (std::max (peak, tinyLevel) / rms));
}

void LevelMeter::prepare (double newSampleRate)
{
    sampleRate = std::max (1000.0, newSampleRate);
    setIntegrationSeconds (integrationSeconds);
    reset();
}

void LevelMeter::setIntegrationSeconds (float seconds)
{
    integrationSeconds = juce::jlimit (0.005f, 10.0f, seconds);

    const auto samples = (int) std::lround ((double) integrationSeconds * sampleRate);
    windowSamples = std::max (16, samples);

    // The window is a power of two so the index wraps with a mask instead of a division,
    // and it is only reallocated when it actually has to grow.
    if ((int) window.size() != windowSamples)
        window.assign ((size_t) windowSamples, 0.0f);

    holdSeconds = std::max (0.05, (double) peakHoldSeconds);
    reset();
}

void LevelMeter::setPeakHoldSeconds (float seconds)
{
    peakHoldSeconds = juce::jlimit (0.05f, 30.0f, seconds);
    holdSeconds = peakHoldSeconds;
}

void LevelMeter::setPeakDecayDbPerSecond (float dbPerSecond)
{
    peakDecayDbPerSecond = juce::jlimit (1.0f, 200.0f, dbPerSecond);
}

void LevelMeter::reset()
{
    std::fill (window.begin(), window.end(), 0.0f);
    windowWrite = 0;
    windowFilled = 0;
    windowEnergy = 0.0;

    blockEnergy = 0.0;
    blockCount = 0;
    blockPeak = 0.0;
    blockMinimum = 0.0;
    blockMaximum = 0.0;
    blockHasData = false;

    holdPeak = 0.0;
    samplesSinceHold = 0.0;

    readings = Readings();
}

void LevelMeter::process (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0 || window.empty())
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        // A sample that is not a number, or is an infinity, carries no level, and letting one
        // into the running sum made the peak read an infinity rather than a very loud signal.
        // Counting it as silence keeps the meter reading something that can be looked at.
        const auto value = std::isfinite ((double) samples[i]) ? (double) samples[i] : 0.0;
        const auto outgoing = window[(size_t) windowWrite];

        // The running sum is what makes a windowed RMS cost one add and one subtract per
        // sample instead of a second pass over the window.
        windowEnergy += value * value - outgoing * outgoing;
        window[(size_t) windowWrite] = (float) value;

        if (windowWrite == windowSamples - 1)
            windowWrite = 0;
        else
            ++windowWrite;

        if (windowFilled < windowSamples)
            ++windowFilled;

        blockEnergy += value * value;
        blockPeak = std::max (blockPeak, std::abs (value));
        if (! blockHasData)
        {
            blockMinimum = value;
            blockMaximum = value;
        }
        else
        {
            blockMinimum = std::min (blockMinimum, value);
            blockMaximum = std::max (blockMaximum, value);
        }
        ++blockCount;
        blockHasData = true;
    }

    updateHold();
    publish();
}

void LevelMeter::updateHold()
{
    const auto peak = std::max (blockPeak, 0.0);

    if (peak >= holdPeak)
    {
        holdPeak = peak;
        samplesSinceHold = 0.0;
        return;
    }

    // The held peak only starts falling after the hold time, so a single loud transient
    // stays readable for long enough to be noticed and not longer.
    samplesSinceHold += (double) blockCount;

    if (samplesSinceHold < holdSeconds * sampleRate)
        return;

    // The fall is proportional to the time this block covered, not to the total time the
    // hold has been decaying, so the decay rate means the same thing whatever block size
    // the device is running at.
    const auto blockSeconds = (float) ((double) blockCount / sampleRate);
    const auto decayDb = peakDecayDbPerSecond * blockSeconds;
    holdPeak *= std::pow (10.0f, -decayDb / 20.0f);

    if (samplesSinceHold > holdSeconds * sampleRate * 2.0)
        samplesSinceHold = holdSeconds * sampleRate * 2.0;
}

void LevelMeter::publish()
{
    const auto divisor = (double) std::max (1, windowFilled);
    const auto rms = std::sqrt (std::max (0.0, windowEnergy) / divisor);
    const auto peak = std::max (blockPeak, 0.0);
    const auto range = std::max (0.0, blockMaximum - blockMinimum);
    const auto blockRms = blockCount > 0
                        ? std::sqrt (std::max (0.0, blockEnergy) / (double) blockCount)
                        : 0.0;

    readings.rmsLinear = (float) rms;
    readings.peakLinear = (float) peak;
    readings.rmsDbfs = amplitudeToDb ((float) rms);
    readings.peakDbfs = amplitudeToDb ((float) peak);
    readings.peakToPeakDbfs = amplitudeToDb ((float) range);
    readings.crestFactorDb = crestFactor ((float) peak, (float) rms);
    readings.peakHoldDbfs = amplitudeToDb ((float) holdPeak);
    readings.blockRmsDbfs = amplitudeToDb ((float) blockRms);
    readings.valid = blockHasData;

    // The block figures are per call, so they start again on the next one.
    blockEnergy = 0.0;
    blockCount = 0;
    blockPeak = 0.0;
    blockMinimum = 0.0;
    blockMaximum = 0.0;
    blockHasData = false;
}
}
