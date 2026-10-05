#include "RtaAnalyser.h"
#include "DSP.h"
#include <cmath>

namespace dsp
{
namespace
{
    constexpr float levelFloorDb = -200.0f;
}

RtaAnalyser::RtaAnalyser()
{
    prepare (48000.0, 2048);
}

juce::StringArray RtaAnalyser::getResolutionNames()
{
    return { "1/1 Oktaf", "1/3 Oktaf", "1/6 Oktaf", "1/12 Oktaf", "1/24 Oktaf" };
}

juce::String RtaAnalyser::resolutionToString (Resolution resolution)
{
    switch (resolution)
    {
        case Resolution::OneOctave:         return "1/1";
        case Resolution::ThirdOctave:       return "1/3";
        case Resolution::SixthOctave:       return "1/6";
        case Resolution::TwelfthOctave:     return "1/12";
        case Resolution::TwentyFourthOctave: return "1/24";
    }

    return "1/3";
}

float RtaAnalyser::bandEdgeLow (const std::vector<float>& centres, size_t index)
{
    // Band edges are the geometric midpoints between neighbouring centres, which is what
    // makes the bands tile the axis with no gap and no overlap. An arithmetic midpoint
    // would leave a band wider than it should be at the top of the range, where the
    // centres are far apart, and swallow part of its neighbour.
    if (index == 0)
        return 0.0f;

    return std::sqrt (centres[index - 1] * centres[index]);
}

float RtaAnalyser::bandEdgeHigh (const std::vector<float>& centres, size_t index)
{
    if (index + 1 >= centres.size())
        return 0.0f;

    return std::sqrt (centres[index] * centres[index + 1]);
}

void RtaAnalyser::prepare (double newSampleRate, int newFftSize)
{
    sampleRate = std::max (1000.0, newSampleRate);
    fftSize = std::max (256, newFftSize);

    // Every time constant is expressed against the frame rate the analyser actually
    // produces, so changing the FFT size does not silently change how fast the display
    // settles.
    const auto hopSeconds = (double) std::max (32, fftSize / 2) / sampleRate;
    frameSeconds = std::max (1.0 / sampleRate, hopSeconds);

    rebuildBands();
    setAveraging (averagingMode, averagingTimeConstant);
    setSmoothingSeconds (smoothingSeconds);
    setPeakHoldSeconds (peakHoldSeconds);
    setPeakDecayDbPerSecond (peakDecayDbPerSecond);
    setMinMaxWindowSeconds (minMaxWindowSeconds);
    reset();
}

void RtaAnalyser::rebuildBands()
{
    // The centres come from the shared octave band table, which already snaps them to the
    // nominal values the standards use, so the RTA and the RTA/Delay view quote the same
    // frequencies for the same band.
    bandFrequencies = octaveBandFrequencies ((int) resolution, rangeLow, rangeHigh);

    bands.assign (bandFrequencies.size(), Band());
    averagedDb.assign (bandFrequencies.size(), levelFloorDb);
    smoothedDb.assign (bandFrequencies.size(), levelFloorDb);
    samplesSinceHold.assign (bandFrequencies.size(), 0.0);
    samplesSinceMinMaxReset.assign (bandFrequencies.size(), 0.0);

    for (size_t i = 0; i < bandFrequencies.size(); ++i)
        bands[i].frequency = bandFrequencies[i];

    primed = false;
}

void RtaAnalyser::setResolution (Resolution newResolution)
{
    if (resolution == newResolution)
        return;

    resolution = newResolution;
    rebuildBands();
    reset();
}

void RtaAnalyser::setFrequencyRange (float lowFrequency, float highFrequency)
{
    const auto low = juce::jlimit (10.0f, 2000.0f, lowFrequency);
    const auto high = juce::jlimit (low * 2.0f, 20000.0f, highFrequency);

    if (std::abs (rangeLow - low) < 0.01f && std::abs (rangeHigh - high) < 0.01f)
        return;

    rangeLow = low;
    rangeHigh = high;
    rebuildBands();
    reset();
}

void RtaAnalyser::setAveraging (SpectrumAnalyser::Averaging mode, float timeConstantSeconds)
{
    averagingMode = mode;
    averagingTimeConstant = juce::jlimit (0.01f, 120.0f, timeConstantSeconds);

    averagingAlpha = averagingMode == SpectrumAnalyser::Averaging::Off
                   ? 1.0f
                   : (float) (1.0 - std::exp (-frameSeconds / (double) averagingTimeConstant));

    primed = false;
    attackAlpha = juce::jmin (1.0f, averagingAlpha * attackRatio);
}

void RtaAnalyser::setIntegration (Integration newIntegration)
{
    if (integration == newIntegration)
        return;

    integration = newIntegration;
    reset();
}

void RtaAnalyser::setSmoothingSeconds (float seconds)
{
    smoothingSeconds = juce::jlimit (0.0f, 10.0f, seconds);
    smoothingAlpha = smoothingSeconds <= 0.0f
                   ? 1.0f
                   : (float) (1.0 - std::exp (-frameSeconds / (double) smoothingSeconds));
}

void RtaAnalyser::setPeakHoldSeconds (float seconds)
{
    peakHoldSeconds = juce::jlimit (0.0f, 60.0f, seconds);
}

void RtaAnalyser::setPeakDecayDbPerSecond (float dbPerSecond)
{
    peakDecayDbPerSecond = juce::jlimit (0.0f, 200.0f, dbPerSecond);
}

void RtaAnalyser::setMinMaxWindowSeconds (float seconds)
{
    minMaxWindowSeconds = juce::jlimit (0.1f, 600.0f, seconds);
}

void RtaAnalyser::reset()
{
    std::fill (averagedDb.begin(), averagedDb.end(), levelFloorDb);
    std::fill (smoothedDb.begin(), smoothedDb.end(), levelFloorDb);
    std::fill (samplesSinceHold.begin(), samplesSinceHold.end(), 0.0);
    std::fill (samplesSinceMinMaxReset.begin(), samplesSinceMinMaxReset.end(), 0.0);

    for (auto& band : bands)
    {
        band.levelDb = levelFloorDb;
        band.peakHoldDb = levelFloorDb;
        band.minDb = levelFloorDb;
        band.maxDb = levelFloorDb;
    }

    primed = false;
    framesProcessed = 0;
}

void RtaAnalyser::clearPeakHold()
{
    for (auto& band : bands)
        band.peakHoldDb = levelFloorDb;

    std::fill (samplesSinceHold.begin(), samplesSinceHold.end(), 0.0);
}

void RtaAnalyser::process (const SpectrumAnalyser::Frame& frame)
{
    if (bands.empty() || frame.frequency.size() < 2
         || frame.magnitude.size() != frame.frequency.size())
        return;

    // A frame built for a different FFT size has different bins at different frequencies,
    // so integrating it into this band table would attribute energy to the wrong bands.
    // It is refused outright; the caller has to re-prepare for the size it is using.
    if (frame.fftSize != fftSize)
        return;

    // A frame from a different FFT size would map onto the bands wrongly, so it is
    // refused rather than integrated into a table built for another resolution.
    if (frame.fftSize != fftSize)
    {
        prepare (frame.sampleRate, frame.fftSize);
        bands.clear();
        rebuildBands();
    }

        // The band table has to follow the spectrum that is arriving, or the bands would
        // be integrated against the wrong FFT size. prepare() reallocates, so it is only
        // called when the size actually differs.
        if (frame.fftSize != fftSize)
            return;

        const auto& frequencies = frame.frequency;
        const auto& magnitudes = frame.magnitude;

    // The bins are walked once, in order, and each one is handed to the band it falls in.
    // Taking the single bin nearest the centre instead would make a band level depend on
    // where a tone happened to land between bins, which is the difference between a
    // measurement and a picture of one.
    size_t bin = 0;

    for (size_t band = 0; band < bands.size(); ++band)
    {
        auto low = bandEdgeLow (bandFrequencies, band);
        auto high = bandEdgeHigh (bandFrequencies, band);

        // The outermost bands reach out to the edges of the configured range rather than
        // to a midpoint, so the first and last band are not half a band too narrow.
        if (band == 0)
            low = rangeLow * 0.5f;

        if (band + 1 >= bands.size())
            high = rangeHigh * 2.0f;

        while (bin < frequencies.size() && frequencies[bin] < low)
            ++bin;

        // Each bin is weighted by the part of it that falls inside this band. A bin that
        // straddles the band edge belongs partly to each neighbour, and giving it wholly to
        // one of them would make the level depend on where the edges happen to land
        // between bins.
        double sumSquares = 0.0;
        double totalWeight = 0.0;

        while (bin < frequencies.size() && frequencies[bin] < high)
        {
            const auto value = magnitudes[bin];

            if (value > 0.0f)
            {
                const auto binLow = frequencies[bin];
                const auto binHigh = bin + 1 < frequencies.size() ? frequencies[bin + 1]
                                                                   : binLow * 2.0f;

                // The fraction of this bin's width that the band covers, never more than
                // the whole bin. A bin narrower than the band counts fully, and a bin
                // straddling an edge is shared with the neighbour.
                const auto overlap = std::max (0.0f, std::min (binHigh, high) - std::max (binLow, low));
                const auto width = std::max (1.0e-6f, binHigh - binLow);
                const auto weight = std::min (1.0f, overlap / width);

                if (weight > 0.0f)
                {
                    sumSquares += (double) value * (double) value * (double) weight;
                    totalWeight += (double) weight;
                }
            }

            ++bin;
        }

        // The bin count is reported as the weight actually integrated, so a band narrower
        // than a bin shows a fraction rather than a whole one.
        const auto count = (int) std::lround (totalWeight);

        if (count == 0)
        {
            // Nothing was captured in this band, so it has no level. Reporting the
            // previous one would leave a stale reading on screen after the signal stopped.
            bands[band].levelDb = levelFloorDb;
            bands[band].binCount = 0;
            bands[band].resolved = false;
            averagedDb[band] = levelFloorDb;
            smoothedDb[band] = levelFloorDb;
            continue;
        }

        // Summing gives the band energy, which is the acoustic band level: a tone inside the
        // band reads its own amplitude, and pink noise shows the staircase it really is,
        // rising with the band width.
        //
        // Averaging divides by the weight integrated rather than by a bin count, which is
        // what makes a flat spectrum read flat across bands of very different widths. A tone
        // spread over a wide band then reads below its own amplitude, because the tone's
        // energy is divided by every bin the band happens to cover. That is the honest
        // result of asking for a per bin level, and it is why both modes are offered.
        const auto integrated = integration == Integration::Sum
                              ? std::sqrt (sumSquares)
                              : std::sqrt (sumSquares / std::max (1.0, totalWeight));
        const auto levelDb = std::max (levelFloorDb,
                                       20.0f * std::log10 ((float) std::max (integrated, 1.0e-20)));

        auto& target = bands[band];
        target.binCount = count;

        // Every band reports how much of the spectrum it actually saw. A band narrower
        // than one bin is marked unresolved rather than given a number that belongs to a
        // wider range than the band claims.
        const auto bandWidth = high - low;
        const auto binWidth = (float) frame.sampleRate / (float) frame.fftSize;
        target.resolved = binWidth <= bandWidth;

        if (! primed)
        {
            averagedDb[band] = levelDb;
            smoothedDb[band] = levelDb;
            target.minDb = levelDb;
            target.maxDb = levelDb;
            target.peakHoldDb = levelDb;
        }
        else
        {
            // The same asymmetry the spectrum average uses, so a band level and the curve
            // drawn from the same frame rise and fall together instead of one lagging.
            const auto coefficient = levelDb > averagedDb[band] ? attackAlpha : averagingAlpha;

            averagedDb[band] = averagedDb[band] * (1.0f - coefficient)
                             + levelDb * coefficient;
            smoothedDb[band] = smoothedDb[band] * (1.0f - smoothingAlpha)
                             + averagedDb[band] * smoothingAlpha;

            target.minDb = std::min (target.minDb, smoothedDb[band]);
            target.maxDb = std::max (target.maxDb, smoothedDb[band]);

            // Counted in frames, so the window lasts the configured time whatever the
            // FFT size is.
            samplesSinceMinMaxReset[band] += 1.0;

            if (samplesSinceMinMaxReset[band] > minMaxWindowSeconds / (float) std::max (1.0e-6, frameSeconds))
            {
                // The window has passed, so the extremes start again from what is on
                // screen rather than being kept for ever.
                target.minDb = smoothedDb[band];
                target.maxDb = smoothedDb[band];
                samplesSinceMinMaxReset[band] = 0.0;
            }
        }

        target.levelDb = smoothedDb[band];

        // Silence must pull the band down, not freeze it at its last reading.
        if (target.levelDb < levelFloorDb + 1.0f)
        {
            target.levelDb = levelFloorDb;
            target.minDb = levelFloorDb;
            target.maxDb = levelFloorDb;
            target.peakHoldDb = levelFloorDb;
        }
    }

    primed = true;
    ++framesProcessed;
    updateHolds();
}

void RtaAnalyser::updateHolds()
{
    // The hold is counted in frames, so the time it lasts is the configured time whatever
    // the FFT size is.
    const auto holdFrames = peakHoldSeconds / (float) std::max (1.0e-6, frameSeconds);

    for (size_t band = 0; band < bands.size(); ++band)
    {
        auto& target = bands[band];

        // A hold is a hold: it remembers the highest level the band has reached and does not
        // care whether that level is still on screen. Letting a quiet frame reset it would
        // be the opposite of what holding is for.
        if (target.levelDb >= target.peakHoldDb)
        {
            target.peakHoldDb = target.levelDb;
            samplesSinceHold[band] = 0.0;
            continue;
        }

        samplesSinceHold[band] += 1.0;

        if (holdFrames <= 0.0f || samplesSinceHold[band] < holdFrames)
            continue;

        // The hold falls at a fixed rate in dB per second, which is what a hardware peak
        // meter does, rather than sliding towards whatever the band is sitting at now.
        target.peakHoldDb -= peakDecayDbPerSecond * (float) frameSeconds;
        target.peakHoldDb = std::max (target.peakHoldDb, target.levelDb);

        // The clock restarts once the fall has begun, so the decay rate is the rate the
        // hold is actually falling at and not a single burst that then stops.
        if (target.peakHoldDb > target.levelDb)
            samplesSinceHold[band] = 0.0;
    }
}

float RtaAnalyser::getLowestBandFrequency() const
{
    return bandFrequencies.empty() ? 0.0f : bandFrequencies.front();
}

float RtaAnalyser::getHighestBandFrequency() const
{
    return bandFrequencies.empty() ? 0.0f : bandFrequencies.back();
}

int RtaAnalyser::getUnresolvedBandCount() const
{
    auto count = 0;

    for (const auto& band : bands)
        if (! band.resolved)
            ++count;

    return count;
}
}
