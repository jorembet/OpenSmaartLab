#include "AdvancedMeasurements.h"

namespace dsp
{
namespace
{
    /** Amplitude of a line from its power, which is how every figure here is presented. */
    float amplitudeOf (double power)
    {
        return power <= 0.0 ? 0.0f : (float) std::sqrt (2.0 * power);
    }

    /** Decibels of a power, with the floor stated rather than left to run away. */
    float powerToDb (double power)
    {
        return (float) (10.0 * std::log10 (std::max (1.0e-30, power)));
    }
}

juce::StringArray ImdAnalyser::getStandardNames()
{
    return { "SMPTE IMD (rasio 3:2)", "CCIF / twin tone (rasio 2:1)" };
}

juce::String ImdAnalyser::standardName (Standard standard)
{
    return standard == Standard::Ccif ? getStandardNames()[1] : getStandardNames()[0];
}

void ImdAnalyser::driveFrequencies (Standard standard, float lowHz, float& firstHz, float& secondHz)
{
    firstHz = lowHz;

    // SMPTE's ratio is what keeps the products clear of the tones; CCIF's octave ratio puts
    // them on top of the lower tone's harmonics, which is why it cannot separate them.
    secondHz = standard == Standard::Ccif ? lowHz * 2.0f : lowHz * 1.5f;
}

ImdAnalyser::ImdAnalyser()
{
    meter.prepare (48000.0, 32768);
}

void ImdAnalyser::prepare (double sampleRate, int fftSize)
{
    meter.prepare (sampleRate, fftSize);
}

void ImdAnalyser::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.lowToneHz = juce::jlimit (20.0f, 20000.0f / 1.5f, settings.lowToneHz);
    settings.amplitude = juce::jlimit (0.0f, 0.9f, settings.amplitude);
}

float ImdAnalyser::imdRatio (double drivePower, const std::vector<double>& productPowers)
{
    if (drivePower <= 1.0e-20)
        return 0.0f;

    double products = 0.0;

    for (auto power : productPowers)
        products += std::max (0.0, power);

    return products <= 0.0 ? 0.0f : (float) std::sqrt (products / drivePower);
}

juce::String ImdAnalyser::percentToString (float percent)
{
    if (! std::isfinite (percent))
        return "--";

    if (percent >= 1.0f)
        return juce::String (percent, 2) + " %";

    if (percent >= 0.01f)
        return juce::String (percent, 3) + " %";

    return juce::String (percent, 5) + " %";
}

juce::String ImdAnalyser::levelToString (float db)
{
    if (! std::isfinite (db) || db <= dbFloor + 0.01f)
        return "--";

    return juce::String (db, 1) + " dB";
}

ImdAnalyser::Result ImdAnalyser::analyse (const float* samples, int numSamples)
{
    Result result;

    if (! meter.analyse (samples, numSamples))
        return result;

    driveFrequencies (settings.standard, settings.lowToneHz,
                      result.firstHz, result.secondHz);

    const auto firstPower = meter.linePowerNear (result.firstHz, 2.0 * meter.binWidth());
    const auto secondPower = meter.linePowerNear (result.secondHz, 2.0 * meter.binWidth());

    result.firstAmplitude = amplitudeOf (firstPower);
    result.secondAmplitude = amplitudeOf (secondPower);

    // The drive is a root sum of squares of the two amplitudes. Summing them instead would
    // read three decibels high for an equal pair, and that error would land in the numerator's
    // denominator and quietly flatter every intermodulation figure.
    const auto drivePower = firstPower + secondPower;

    if (drivePower <= 1.0e-20)
        return result;

    const auto relativeTo = powerToDb (drivePower);

    // The products, in the order the standard names them.
    struct Definition
    {
        const char* expression;
        double frequency;
    };

    std::vector<Definition> definitions;

    if (settings.standard == Standard::Smpte)
    {
        // Second order lands at 2f1-f2 = 0.5 f1 and 2f2-f1 = 2 f1. Third order adds 3f2-2f1 at
        // 2.5 f1. The fourth order 4f1-3f2 falls at minus 0.5 f1, which is the same magnitude as
        // the second order product and lands in the same band, so it is already counted.
        definitions = { { "2f1-f2", 0.5 * result.firstHz },
                        { "2f2-f1", 2.0 * result.firstHz },
                        { "3f2-2f1", 2.5 * result.firstHz } };
    }
    else
    {
        // With f2 = 2 f1 the second order difference lands on zero and every other product
        // coincides with a harmonic of the lower tone, so what is measured is the harmonics
        // together with whatever products happen to be at those frequencies. Said plainly
        // rather than presented as a separable measurement.
        definitions = { { "3f1-f2", 1.0 * result.firstHz },
                        { "4f1-f2", 2.0 * result.firstHz },
                        { "3f2-2f1", 4.0 * result.firstHz },
                        { "4f2-3f1", 5.0 * result.firstHz } };
    }

    std::vector<double> productPowers;

    for (const auto& definition : definitions)
    {
        if (definition.frequency <= 0.0 || definition.frequency >= meter.getSampleRate() * 0.5)
            continue;

        // Near rather than exact, because these are derived from a fundamental that was itself
        // located rather than given, and the error in it is multiplied by the product's order.
        const auto power = meter.linePowerNear (definition.frequency, 3.0 * meter.binWidth());
        productPowers.push_back (power);

        Product product;
        product.expression = definition.expression;
        product.frequency = (float) definition.frequency;
        product.amplitude = amplitudeOf (power);
        product.relativeDb = powerToDb (power) - relativeTo;
        result.products.push_back (product);
    }

    const auto ratio = imdRatio (drivePower, productPowers);

    result.imdPercent = ratio * 100.0f;
    result.imdDb = ratio > 0.0f ? 20.0f * std::log10 ((double) ratio) : dbFloor;
    result.valid = true;
    return result;
}

// ---------------------------------------------------------------------------

CrosstalkAnalyser::CrosstalkAnalyser()
{
    meter.prepare (48000.0, 32768);
}

void CrosstalkAnalyser::prepare (double sampleRate, int fftSize)
{
    meter.prepare (sampleRate, fftSize);
}

void CrosstalkAnalyser::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.frequency = juce::jlimit (20.0f, 20000.0f, settings.frequency);
    settings.amplitude = juce::jlimit (0.0f, 1.0f, settings.amplitude);
    settings.floorDb = juce::jlimit (-160.0f, 0.0f, settings.floorDb);
}

CrosstalkAnalyser::Result CrosstalkAnalyser::measure (const float* driven, const float* receiving,
                                                       int numSamples)
{
    Result result;

    result.frequency = settings.frequency;

    // Each channel is transformed on its own. A single transform in which both were read would
    // give a spectrum in which the drive and the leakage are already summed, and the figure
    // would be neither of them.
    if (! meter.analyse (driven, numSamples))
        return result;

    const auto drivePower = meter.linePowerNear (settings.frequency, 2.0 * meter.binWidth());

    if (! meter.analyse (receiving, numSamples))
        return result;

    const auto receivePower = meter.linePowerNear (settings.frequency, 2.0 * meter.binWidth());

    const auto driveDb = powerToDb (drivePower);
    const auto receiveDb = powerToDb (receivePower);

    result.driveLevelDb = driveDb;
    result.receiveLevelDb = receiveDb;

    // A receiving channel sitting at or below the noise floor is reported relative to the floor
    // rather than as an infinitely quiet channel, because an infinite number would be a claim
    // about the world that was never measured.
    const auto floor = (double) settings.floorDb;

    result.leakageDb = receiveDb <= floor ? (float) (floor - driveDb)
                                         : (float) (receiveDb - driveDb);
    result.leakagePercent = (float) (std::pow (10.0, (double) result.leakageDb / 20.0) * 100.0);

    // A reading without a drive in it is not a leakage figure, so it is refused rather than
    // divided into.
    result.valid = drivePower > 0.0 && receiveDb > floor;
    return result;
}

juce::String CrosstalkAnalyser::levelToString (float db)
{
    if (! std::isfinite (db) || db <= -300.0f || db >= 0.0f)
        return "--";

    return juce::String (db, 1) + " dB";
}

// ---------------------------------------------------------------------------

PolarityDetector::PolarityDetector()
{
    settings.threshold = 0.3f;
    settings.maxLagSamples = 64;
}

void PolarityDetector::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.threshold = juce::jlimit (0.05f, 0.99f, settings.threshold);
    settings.maxLagSamples = juce::jlimit (0, 4096, settings.maxLagSamples);
}

juce::String PolarityDetector::verdictToString (Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::Normal:   return "NORMAL";
        case Verdict::Inverted: return "INVERTED";
        default:                return "UNKNOWN";
    }
}

PolarityDetector::Report PolarityDetector::detect (const float* reference,
                                                   const float* measurement,
                                                   int numSamples) const
{
    Report report;
    report.threshold = settings.threshold;

    if (reference == nullptr || measurement == nullptr)
        return report;

    const auto usable = std::min (numSamples, 4096);

    if (usable < 64)
        return report;

    // Means removed first, or a constant offset in either signal would count as agreement at
    // every lag and the peak would stop meaning anything.
    double referenceMean = 0.0, measurementMean = 0.0;

    for (int i = 0; i < usable; ++i)
    {
        referenceMean += reference[i];
        measurementMean += measurement[i];
    }

    referenceMean /= usable;
    measurementMean /= usable;

    double referencePower = 0.0, measurementPower = 0.0;
    double bestCorrelation = 0.0;
    int bestLag = 0;

    for (int lag = -settings.maxLagSamples; lag <= settings.maxLagSamples; ++lag)
    {
        double cross = 0.0, refPower = 0.0, measPower = 0.0;

        for (int i = 0; i < usable; ++i)
        {
            const auto r = (double) reference[i] - referenceMean;
            const auto m = (double) measurement[std::min (usable - 1, std::max (0, i + lag))]
                         - measurementMean;

            cross += r * m;
            refPower += r * r;
            measPower += m * m;
        }

        const auto denominator = std::sqrt (std::max (1.0e-30, refPower * measPower));

        if (denominator <= 1.0e-15)
            continue;

        const auto correlation = cross / denominator;

        // The largest magnitude, not the largest value: an inverted channel correlates just as
        // strongly as a correct one and only the sign tells them apart. Choosing on magnitude and
        // reading the sign afterwards is what lets one search report either.
        if (std::abs (correlation) > std::abs (bestCorrelation))
        {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }

    report.correlation = (float) bestCorrelation;
    report.lagSamples = bestLag;

    // Below the threshold the two signals have nothing in common, and calling that a wiring
    // fault would send someone looking for a polarity problem they do not have.
    if (std::abs ((float) bestCorrelation) < settings.threshold)
        return report;

    report.confident = true;
    report.verdict = bestCorrelation < 0.0 ? Verdict::Inverted : Verdict::Normal;
    return report;
}
}