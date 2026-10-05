#include "SplAnalyser.h"
#include "DSP.h"

namespace dsp
{
namespace
{
    /** Corner frequencies of the A and C weightings, in hertz, from IEC 61672.

        The two curves share every corner except the lowest: A rolls off below 1 kHz where C
        stays flat, and both fall away above it. Stated once here so the two cannot drift apart
        by a typo. */
    constexpr double aLow = 20.598997;
    constexpr double aMid = 107.65265;
    constexpr double aHigh = 737.86223;
    constexpr double aTop = 12194.217;

    constexpr double cLow = 20.597;

    /** The A and C weighting magnitude, relative to nothing: both come out as an absolute
        level, and the caller differences them against their own value at 1 kHz.

        The two curves are not one formula with a corner swapped. They differ in the power of
        frequency in the numerator, and that is the whole of the difference between them: A
        falls away four orders of magnitude across the audio band while C falls by about three
        decibels per decade, so writing them as one shape with a parameter would have made C
        roll off like A everywhere below a kilohertz, which is the one thing C does not do.
    */
    float ratingDb (SplAnalyser::Weighting weighting, double frequency)
    {
        const auto f2 = frequency * frequency;
        const auto top = aTop * aTop;
        const auto low = aLow * aLow;

        double ratio = 0.0;

        if (weighting == SplAnalyser::Weighting::C)
        {
            // C is the flat-ish curve: one power of frequency in the numerator and no resonant
            // pair in the denominator, which is what leaves it level at 100 Hz.
            ratio = (top * f2) / ((f2 + low) * (f2 + top));
        }
        else if (weighting == SplAnalyser::Weighting::A)
        {
            // A carries two powers of frequency and the 107.7 / 737.9 Hz pair, which is what
            // gives it its slope through the middle of the band.
            const auto mid = aMid * aMid;
            const auto high = aHigh * aHigh;

            ratio = (top * f2 * f2)
                  / ((f2 + low) * std::sqrt ((f2 + mid) * (f2 + high)) * (f2 + top));
        }
        else
        {
            return 0.0f;
        }

        return (float) (20.0 * std::log10 (std::max (1.0e-30, ratio)));
    }
}

SplAnalyser::SplAnalyser()
{
    prepare (48000.0);
}

double SplAnalyser::timeConstant (TimeWeighting timeWeighting)
{
    switch (timeWeighting)
    {
        case TimeWeighting::Fast:     return 0.125;
        case TimeWeighting::Slow:     return 1.0;
        case TimeWeighting::Impulse: return 0.035;
    }

    return 0.125;
}

float SplAnalyser::weightingGainDbAt (Weighting weighting, float frequency)
{
    if (weighting == Weighting::Z || frequency <= 0.0f)
        return 0.0f;

    // Referenced to 1 kHz, where both A and C are defined to be exactly zero, so a reading at
    // 1 kHz comes out unchanged whatever weighting is selected. That is what makes the curve
    // checkable: the definition and the implementation have to agree there.
    return ratingDb (weighting, (double) frequency) - ratingDb (weighting, 1000.0);
}

void SplAnalyser::prepare (double newSampleRate)
{
    sampleRate = juce::jmax (1000.0, newSampleRate);

    blockAnalyser.prepare (sampleRate, blockSize);

    setTimeWeighting (timeWeighting);
    reset();
}

void SplAnalyser::setCalibration (const MicrophoneCalibration& profile)
{
    // Sampled onto this analyser's own bins. A profile with no curve leaves the readings
    // uncorrected rather than corrected by an invented flat response, which is the honest
    // outcome for a microphone whose response nobody has measured.
    if (! profile.hasFrequencyResponse())
    {
        blockAnalyser.setBinCorrectionDb ({});
        return;
    }

    std::vector<float> correction ((size_t) blockAnalyser.getNumBins(), 0.0f);
    const auto binWidth = sampleRate / (double) blockSize;

    for (int i = 0; i < blockAnalyser.getNumBins(); ++i)
        correction[(size_t) i] = profile.correctionDb ((float) (i * binWidth));

    blockAnalyser.setBinCorrectionDb (std::move (correction));
}

void SplAnalyser::setTimeWeighting (TimeWeighting newTimeWeighting)
{
    timeWeighting = newTimeWeighting;

    // Per block, from the block duration, so the response does not change with the device
    // buffer size the user can change while the meter runs.
    const auto blockDuration = (double) blockSize / sampleRate;
    alpha = std::exp (-blockDuration / timeConstant (newTimeWeighting));
}

void SplAnalyser::reset()
{
    for (auto& energy : timeEnergy)
        energy = 0.0;

    for (auto& peak : peakLevel)
        peak = dsp::dbFloor;

    pending = 0;
    blockCount = 0;
    started = false;
    resetLeq();
}

void SplAnalyser::resetPeak()
{
    for (auto& peak : peakLevel)
        peak = dsp::dbFloor;
}

void SplAnalyser::resetLeq()
{
    for (auto& energy : leqEnergy)
        energy = 0.0;

    integratedSamples = 0;
    blockCount = 0;
    leqBlocks = 0;
}

void SplAnalyser::processBlock (const float* block) noexcept
{
    // Each weighting is asked for separately. That costs one transform per weighting, which at
    // this block size is a few hundred a second and not worth the complexity of sharing one.
    for (int w = 0; w < numWeightings; ++w)
    {
        const auto rating = w == 1 ? 'A' : (w == 2 ? 'C' : 'Z');
        const auto db = blockAnalyser.levelDb (block, rating);

        if (! (db > dsp::dbFloor + 1.0f))
            continue;

        // Energy, not decibels. Averaging dB values is not the same as averaging sound energy,
        // and the gap widens with the spread of the levels being averaged: two passages 40 dB
        // apart come out 7 dB apart under an arithmetic mean of decibels and 37 dB apart when
        // the energy is averaged, which is the whole reason an equivalent level exists.
        const auto power = std::pow (10.0, (double) db / 10.0);

        if (! started)
            timeEnergy[(size_t) w] = power;
        else
            timeEnergy[(size_t) w] = alpha * timeEnergy[(size_t) w] + (1.0 - alpha) * power;

        leqEnergy[(size_t) w] += power;

        const auto level = (float) (10.0 * std::log10 (std::max (1.0e-30,
                                                               timeEnergy[(size_t) w])));

        if (level > peakLevel[(size_t) w])
            peakLevel[(size_t) w] = level;
    }

    started = true;
    integratedSamples += blockSize;
    ++blockCount;

    // Counted once per block, not once per weighting. Counting inside the loop made the
    // average divide by three times as many blocks as it had terms, which is five decibels low
    // and identically so for every weighting, which is the worst kind of error to notice.
    leqBlocks += 1;
}

void SplAnalyser::process (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        timeDomain[(size_t) pending] = samples[i];
        ++pending;

        if (pending < blockSize)
            continue;

        processBlock (timeDomain.data());
        pending = 0;
    }
}

float SplAnalyser::getLevel (Weighting weighting) const
{
    return (float) (10.0 * std::log10 (std::max (1.0e-30,
                                                timeEnergy[(size_t) indexOf (weighting)])));
}

float SplAnalyser::getLeq (Weighting weighting) const
{
    // Divided by the number of blocks, not by the number of samples. Each block contributes a
    // mean square already, so averaging those means dividing by how many there were. Dividing
    // by the sample count instead divides by the block length a second time, which is thirty
    // decibels at a block of 1024 and would put every equivalent level a fixed and completely
    // invisible amount wrong.
    if (leqBlocks <= 0)
        return dsp::dbFloor;

    const auto& energy = leqEnergy[(size_t) indexOf (weighting)];

    if (energy <= 0.0)
        return dsp::dbFloor;

    return (float) (10.0 * std::log10 (std::max (1.0e-30, energy / (double) leqBlocks)));
}

float SplAnalyser::getPeak (Weighting weighting) const
{
    return peakLevel[(size_t) indexOf (weighting)];
}

juce::StringArray SplAnalyser::getWeightingNames()
{
    return { "Z (flat)", "A", "C" };
}

juce::StringArray SplAnalyser::getTimeWeightingNames()
{
    return { "Fast (125 ms)", "Slow (1 s)", "Impulse (35 ms)" };
}
}