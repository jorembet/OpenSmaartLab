#include "ReverbAnalyser.h"

namespace dsp
{
namespace
{
    /** Least squares over the points of a decay curve that lie inside a decibel window.

        Returned as a slope in decibels a second, which is negative for a decaying room, plus
        how well the points followed it. The correlation travels with the slope because a slope
        on its own is exactly the thing that looks trustworthy when it should not be. */
    struct Fit
    {
        float slope = 0.0f;
        float correlation = 0.0f;
        float startTime = 0.0f;
        float endTime = 0.0f;
        int points = 0;
        float rangeDb = 0.0f;
        bool valid = false;
    };

    Fit fitRange (const std::vector<float>& time, const std::vector<float>& decayDb,
                  float upperDb, float lowerDb)
    {
        Fit fit;

        double sumT = 0.0, sumD = 0.0;
        int count = 0;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            // The window is taken with the lower level included and the upper one excluded, so
            // a point exactly on a boundary is counted once rather than twice.
            if (decayDb[i] >= upperDb || decayDb[i] < lowerDb)
                continue;

            sumT += time[i];
            sumD += decayDb[i];
            ++count;
        }

        if (count < 8)
            return fit;

        const auto meanT = sumT / count;
        const auto meanD = sumD / count;

        double numerator = 0.0, denominator = 0.0;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            if (decayDb[i] >= upperDb || decayDb[i] < lowerDb)
                continue;

            const auto dt = (double) time[i] - meanT;
            numerator += dt * ((double) decayDb[i] - meanD);
            denominator += dt * dt;
        }

        if (denominator <= 1.0e-12)
            return fit;

        fit.slope = (float) (numerator / denominator);

        // Correlation, so a bent line can be told from a straight one.
        double sumTT = 0.0, sumDD = 0.0, sumTD = 0.0;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            if (decayDb[i] >= upperDb || decayDb[i] < lowerDb)
                continue;

            const auto dt = (double) time[i] - meanT;
            const auto dd = (double) decayDb[i] - meanD;
            sumTT += dt * dt;
            sumDD += dd * dd;
            sumTD += dt * dd;
        }

        const auto correlationDenominator = std::sqrt (sumTT * sumDD);
        fit.correlation = correlationDenominator > 1.0e-12
                        ? (float) (sumTD / correlationDenominator) : 0.0f;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            if (decayDb[i] >= upperDb || decayDb[i] < lowerDb)
                continue;

            if (fit.startTime == 0.0f && fit.endTime == 0.0f)
                fit.startTime = time[i];

            fit.endTime = time[i];
        }

        fit.points = count;
        fit.rangeDb = std::abs (upperDb - lowerDb);
        fit.valid = true;
        return fit;
    }

    /** How far the curve actually fell over its whole length. */
    float reachedRange (const std::vector<float>& decayDb)
    {
        auto lowest = 0.0f;

        for (auto value : decayDb)
            lowest = std::min (lowest, value);

        return std::abs (lowest);
    }

    /** The slope just past the range that was fitted, used to spot the noise floor.

        Taken over the decibels immediately beyond the T30 window rather than over the last
        quarter of the record, because that is the question actually being asked: does the
        decay keep going where the answer came from, or has it stopped there? Measuring over
        time instead asked a different question, and answered it wrongly for a room recorded
        much longer than its reverberation needs: the far tail of such a record is thousands of
        decibels down and past what a float can resolve, so it looks flat for reasons that have
        nothing to do with the room. */
    float slopeBeyond (const std::vector<float>& time, const std::vector<float>& decayDb,
                       float fromDb, float toDb)
    {
        double sumT = 0.0, sumD = 0.0;
        int count = 0;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            if (decayDb[i] >= fromDb || decayDb[i] < toDb)
                continue;

            sumT += time[i];
            sumD += decayDb[i];
            ++count;
        }

        if (count < 8)
            return 0.0f;

        const auto meanT = sumT / count;
        const auto meanD = sumD / count;

        double numerator = 0.0, denominator = 0.0;

        for (size_t i = 0; i < time.size() && i < decayDb.size(); ++i)
        {
            if (decayDb[i] >= fromDb || decayDb[i] < toDb)
                continue;

            const auto dt = (double) time[i] - meanT;
            numerator += dt * ((double) decayDb[i] - meanD);
            denominator += dt * dt;
        }

        if (denominator <= 1.0e-12)
            return 0.0f;

        return (float) (numerator / denominator);
    }
}

ReverbAnalyser::ReverbAnalyser()
{
    Settings defaults;
    settings = defaults;
}

juce::StringArray ReverbAnalyser::getResolutionNames()
{
    return { "1 oktaf", "1/3 oktaf" };
}

juce::String ReverbAnalyser::qualityToString (Quality quality)
{
    switch (quality)
    {
        case Quality::Valid:               return "VALID";
        case Quality::Noisy:               return "NOISY";
        default:                            return "INSUFFICIENT DECAY";
    }
}

float ReverbAnalyser::extrapolatedToSixtyDb (float slopeDbPerSecond)
{
    // A slope of zero is a flat curve, which is not a very fast room, it is a measurement that
    // did not fall. Reporting a minute of reverberation for it would be an invention.
    if (! std::isfinite (slopeDbPerSecond) || slopeDbPerSecond >= -1.0e-4f)
        return 0.0f;

    return juce::jlimit (0.0f, 120.0f, -60.0f / slopeDbPerSecond);
}

void ReverbAnalyser::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.lowFrequency = juce::jmax (20.0f, settings.lowFrequency);
    settings.highFrequency = juce::jmax (settings.lowFrequency * 1.5f, settings.highFrequency);
    settings.minCorrelation = juce::jlimit (0.0f, 0.9999f, settings.minCorrelation);
    settings.maxTailFlattening = juce::jmax (1.0f, settings.maxTailFlattening);
}

void ReverbAnalyser::bandEdges (Resolution resolution, float centre, float& low, float& high)
{
    const auto ratio = resolution == Resolution::ThirdOctave ? std::pow (2.0f, 1.0f / 6.0f)
                                                             : std::pow (2.0f, 0.5f);

    low = centre / ratio;
    high = centre * ratio;
}

int ReverbAnalyser::bandIndexFor (Resolution resolution, float frequency, float lowFrequency)
{
    const auto ratio = resolution == Resolution::ThirdOctave ? std::pow (2.0f, 1.0f / 6.0f)
                                                             : std::pow (2.0f, 0.5f);
    return (int) std::lround (std::log ((double) frequency / lowFrequency) / std::log ((double) ratio));
}

ReverbAnalyser::Curve ReverbAnalyser::schroederCurve (const float* ir, int numSamples,
                                                     double sampleRate, int startSample)
{
    Curve curve;

    const auto length = numSamples - startSample;

    if (length < 256 || sampleRate <= 0.0)
        return curve;

    double totalEnergy = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto value = (double) ir[startSample + i];
        totalEnergy += value * value;
    }

    if (totalEnergy <= 1.0e-20)
        return curve;

    curve.time.resize ((size_t) length);
    curve.decayDb.resize ((size_t) length);

    // The integral is taken backwards, which is the whole of the method: the energy still
    // present in the tail from here on, accumulated from the far end towards the start. Done
    // forwards it would be the energy already passed and would rise instead of falling.
    double tail = 0.0;

    for (int i = length - 1; i >= 0; --i)
    {
        const auto value = (double) ir[startSample + i];
        tail += value * value;

        curve.time[(size_t) i] = (float) i / (float) sampleRate;

        // Deliberately not clamped at the general dbFloor. A Schroeder curve in a quiet room
        // runs far below anything a level reading would show, and clamping it there would make
        // the tail flat at the floor, which is indistinguishable from the response having run
        // into the converter's own noise. That is a false reading of the measurement, produced
        // by the instrument rather than by the room.
        curve.decayDb[(size_t) i] = (float) (10.0 * std::log10 (std::max (1.0e-30,
                                                                         tail / totalEnergy)));
    }

    curve.valid = true;
    return curve;
}

ReverbAnalyser::Band ReverbAnalyser::measureCurve (const Curve& curve) const
{
    Band band;

    if (! curve.valid || curve.time.size() < 256)
        return band;

    band.decayRangeDb = reachedRange (curve.decayDb);

    // The four ranges, each fitted over its own span and each extrapolated to sixty decibels by
    // the same rule. They differ only in the range they were fitted over, which is the point:
    // a room that is not a single exponential gives three different answers, and the spread
    // between them is the useful information.
    const auto edtFit = fitRange (curve.time, curve.decayDb, 0.0f, -10.0f);
    const auto t20Fit = fitRange (curve.time, curve.decayDb, -5.0f, -25.0f);
    const auto t30Fit = fitRange (curve.time, curve.decayDb, -5.0f, -35.0f);
    const auto rtFit = fitRange (curve.time, curve.decayDb, -5.0f, -60.0f);

    // Without the deepest range there is nothing to report, whatever the shallower fits say:
    // a curve that stops at twenty decibels cannot support a thirty five decibel answer, and
    // extrapolating one to it would be arithmetic rather than measurement.
    if (! t30Fit.valid)
    {
        band.quality = Quality::InsufficientDecay;
        return band;
    }

    if (band.decayRangeDb < 35.0f)
    {
        band.quality = Quality::InsufficientDecay;
        return band;
    }

    // The tail test. Compared against the T30 slope, because that is the range the reported
    // time comes from, and compared as a ratio because both are negative and a subtraction
    // would read backwards.
    const auto tail = slopeBeyond (curve.time, curve.decayDb, -35.0f, -50.0f);

    // The correlation is between the curve and time, so for a decay it is near minus one. It
    // is the strength of the fit that matters and not its sign, and comparing the signed value
    // against a positive threshold would reject every correctly decaying curve as noise.
    if (tail >= 0.0f
        || std::abs (tail) > std::abs (t30Fit.slope) * settings.maxTailFlattening
        || std::abs (t30Fit.correlation) < settings.minCorrelation)
    {
        band.quality = Quality::Noisy;
        band.correlation = t30Fit.correlation;
        return band;
    }

    band.edt = edtFit.valid ? extrapolatedToSixtyDb (edtFit.slope) : 0.0f;
    band.t20 = extrapolatedToSixtyDb (t20Fit.slope);
    band.t30 = extrapolatedToSixtyDb (t30Fit.slope);

    // RT60 is fitted over the widest range the response actually supports, which for a real
    // room is close to the whole decay. It falls back to the T30 figure when the response was
    // too short to reach sixty decibels, and says so through the quality rather than by
    // quietly reporting the same number twice.
    band.rt60 = rtFit.valid && band.decayRangeDb >= 60.0f
              ? extrapolatedToSixtyDb (rtFit.slope) : band.t30;

    band.correlation = t30Fit.correlation;
    band.quality = Quality::Valid;
    band.valid = true;
    return band;
}

std::vector<float> ReverbAnalyser::bandFiltered (const float* ir, int numSamples,
                                                 double sampleRate, float lowHz, float highHz)
{
    bandBuffer.clear();

    if (ir == nullptr || numSamples < 256 || sampleRate <= 0.0)
        return bandBuffer;

    // The transform size has to be the next power of two at or above the input length.
    //
    // Rounding the logarithm to the nearest integer looks equivalent and is not: for a response
    // of 182400 samples it gives 17 and a size of 131072, so the transform read only the first
    // 131072 samples and every band was measured from a truncated and mis-sized spectrum. The
    // broadband figure was still correct because it never goes through here, which is the kind
    // of split that makes a bug look like a bad room instead of a bad instrument.
    auto order = 0;
    while ((1 << order) < std::max (256, numSamples))
        ++order;

    const auto size = 1 << order;

    if ((int) scratch.size() != size * 2)
        scratch.assign ((size_t) size * 2, 0.0f);

    std::fill (scratch.begin(), scratch.end(), 0.0f);
    std::copy (ir, ir + numSamples, scratch.begin());

    juce::dsp::FFT forward (order);
    forward.performRealOnlyForwardTransform (scratch.data(), true);

    const auto bins = size / 2 + 1;
    const auto binHz = (float) sampleRate / (float) size;

    // The band edges are tapered rather than cut square.
    //
    // A brick wall in the frequency domain is an ideal filter, and an ideal filter rings. Its
    // impulse response is a sinc that keeps going for the whole record, and that ringing is a
    // slow decay added to the signal, so a band measured through one reported a reverberation
    // time of ten seconds for a room built to decay in eight tenths of a second. The taper
    // costs a little of the band's edges and removes the ringing, which is the right trade for
    // a number that is supposed to describe the room.
    const auto taperWidth = 0.12f * (highHz - lowHz);

    for (int i = 0; i < bins; ++i)
    {
        const auto frequency = (float) i * binHz;

        float gain = 0.0f;

        if (frequency >= lowHz && frequency < highHz)
        {
            // Raised cosine at each edge, so the gain and its slope are both continuous and the
            // filtered response does not start and stop with a step.
            const auto intoLow = std::max (0.0f, frequency - lowHz);
            const auto fromHigh = std::max (0.0f, highHz - frequency);

            gain = std::min (1.0f, intoLow / std::max (1.0f, taperWidth));
            gain = std::min (gain, fromHigh / std::max (1.0f, taperWidth));

            gain = 0.5f * (1.0f - std::cos (juce::jlimit (0.0f, 1.0f, gain)
                                             * juce::MathConstants<float>::pi));
        }

        scratch[(size_t) (i * 2)] *= gain;
        scratch[(size_t) (i * 2 + 1)] *= gain;
    }

    // The bins past Nyquist are the mirror of the ones below it, and have to be filled in with
    // the conjugate or the inverse transform returns a complex result.
    for (int i = 1; i < bins - 1; ++i)
    {
        const auto index = size - i;
        scratch[(size_t) (index * 2)] = scratch[(size_t) (i * 2)];
        scratch[(size_t) (index * 2 + 1)] = -scratch[(size_t) (i * 2 + 1)];
    }

    juce::dsp::FFT inverse (order);
    inverse.performRealOnlyInverseTransform (scratch.data());

    bandBuffer.assign (scratch.begin(), scratch.begin() + numSamples);
    return bandBuffer;
}

ReverbAnalyser::Result ReverbAnalyser::analyse (const float* ir, int numSamples, double sampleRate)
{
    Result result;

    if (ir == nullptr || numSamples < 256 || sampleRate <= 0.0)
        return result;

    // The direct sound and the few milliseconds after it are not decay and are not fitted over.
    // The energy rising into the direct arrival would otherwise dominate the first part of the
    // curve and steepen the slope into a shorter reverberation time than the room has.
    auto peakIndex = 0;
    auto peakValue = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto value = std::abs (ir[i]);

        if (value > peakValue)
        {
            peakValue = value;
            peakIndex = i;
        }
    }

    if (peakValue <= 0.0f)
        return result;

    const auto start = peakIndex + (int) (0.002 * sampleRate);

    if (start >= numSamples - 256)
        return result;

    result.directArrivalMs = (float) peakIndex / (float) sampleRate * 1000.0f;

    result.time.clear();
    result.decayDb.clear();

    const auto broadband = schroederCurve (ir, numSamples, sampleRate, start);

    if (! broadband.valid)
        return result;

    result.time = broadband.time;
    result.decayDb = broadband.decayDb;

    const auto broadbandBand = measureCurve (broadband);

    result.edt = broadbandBand.edt;
    result.t20 = broadbandBand.t20;
    result.t30 = broadbandBand.t30;
    result.rt60 = broadbandBand.rt60;
    result.correlation = broadbandBand.correlation;
    result.decayRangeDb = broadbandBand.decayRangeDb;
    result.quality = broadbandBand.quality;

    // Validity follows the quality. Setting this true on the strength of having an impulse
    // response to look at let a refused measurement report itself as valid with a quality
    // beside it that said the opposite, which is worse than either answer alone.
    result.valid = broadbandBand.quality == Quality::Valid;

    // Per band, over the bands the settings ask for. The centres are the standard series, so
    // the numbers can be compared with a table printed anywhere else.
    const auto ratio = settings.resolution == Resolution::ThirdOctave
                     ? std::pow (2.0f, 1.0f / 6.0f) : std::pow (2.0f, 0.5f);

    for (auto frequency = settings.lowFrequency; frequency <= settings.highFrequency * 1.001f;
         frequency *= ratio)
    {
        float low = 0.0f, high = 0.0f;
        bandEdges (settings.resolution, frequency, low, high);

        // A band whose top edge is above the response's own bandwidth would be reporting the
        // measurement's limit rather than the room's decay.
        const auto topUsable = std::min ((float) (sampleRate * 0.45), (float) (numSamples - start));

        if (low >= topUsable)
            continue;

        high = std::min (high, topUsable);

        const auto filtered = bandFiltered (ir, numSamples, sampleRate, low, high);

        if ((int) filtered.size() < 256)
            continue;

        bandScratch = schroederCurve (filtered.data(), (int) filtered.size(), sampleRate, 0);

        auto band = measureCurve (bandScratch);
        band.frequency = frequency;

        if (band.valid)
            ++result.bandsValid;

        result.bands.push_back (band);
    }

    return result;
}
}