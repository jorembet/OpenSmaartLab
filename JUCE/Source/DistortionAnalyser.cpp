#include "DistortionAnalyser.h"
#include <cmath>

namespace dsp
{
namespace
{
    /** Whether a bin carries both halves of its frequency, and so counts twice.

        The same convention the shared level calculation uses, kept identical on purpose: a
        distortion figure and a level figure that were not on one scale would be impossible to
        add together, and the noise figure here is exactly a total minus two pieces of it. */
    inline bool isInteriorBin (int bin, int numBins)
    {
        return bin != 0 && bin != numBins - 1;
    }
}

DistortionAnalyser::DistortionAnalyser()
{
    Settings defaults;
    settings = defaults;
    analyser.prepare (48000.0f, 16384);
}

void DistortionAnalyser::prepare (double sampleRate, int fftSize)
{
    analyser.prepare ((float) sampleRate, fftSize);
}

void DistortionAnalyser::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.searchLowHz = juce::jmax (1.0f, settings.searchLowHz);
    settings.searchHighHz = juce::jmax (settings.searchLowHz + 1.0f, settings.searchHighHz);
    settings.maxHarmonic = juce::jlimit (1, 50, settings.maxHarmonic);
    settings.halfWidthBins = juce::jlimit (1, 8, settings.halfWidthBins);
    settings.averaging = juce::jlimit (1, 512, settings.averaging);
}

float DistortionAnalyser::thdRatio (const std::vector<double>& harmonicPowers,
                                    double fundamentalPower)
{
    // A ratio against a silent fundamental is not a number, and dividing by it would produce
    // an infinity that then propagates through every other figure derived from it.
    if (fundamentalPower <= 1.0e-20)
        return 0.0f;

    double distortion = 0.0;

    for (auto power : harmonicPowers)
        distortion += std::max (0.0, power);

    return distortion <= 0.0 ? 0.0f
                             : (float) std::sqrt (distortion / fundamentalPower);
}

juce::String DistortionAnalyser::percentToString (float percent)
{
    if (! std::isfinite (percent))
        return "--";

    if (percent >= 10.0f)
        return juce::String (percent, 1) + " %";

    if (percent >= 0.1f)
        return juce::String (percent, 2) + " %";

    // Below a hundredth of a percent the digits that matter are past the second, and a reading
    // printed to two places would show a floor instead of the measurement. The last case is
    // written out in full rather than given a fixed number of places, because at this size the
    // first significant figure can be anywhere.
    if (percent >= 0.01f)
        return juce::String (percent, 3) + " %";

    if (percent >= 1.0e-4f)
        return juce::String (percent, 5) + " %";

    return juce::String (percent, 8) + " %";
}

juce::String DistortionAnalyser::levelToString (float db)
{
    // The floor stands for "nothing there", not for a level of minus two hundred decibels, and
    // printing it as a number would put a figure on screen that means nothing.
    if (! std::isfinite (db) || db <= dsp::dbFloor + 0.01f)
        return juce::String ("--");

    return juce::String (db, 1) + " dB";
}

double DistortionAnalyser::bandPower (const dsp::Spectrum& spec, int centreBin) const
{
    if (spec.data.empty())
        return 0.0;

    const auto numBins = (int) spec.data.size();
    const auto low = juce::jmax (1, centreBin - settings.halfWidthBins);
    const auto high = juce::jmin (numBins - 2, centreBin + settings.halfWidthBins);

    double total = 0.0;

    for (int i = low; i <= high; ++i)
    {
        const auto power = (double) std::norm (spec.data[(size_t) i]);

        if (isInteriorBin (i, numBins))
            total += 2.0 * power;
        else
            total += power;
    }

    // Divided by the transform's length squared and by the window's mean square, which is the
    // scale a level reading is on. The window factor is not optional: a Hann window passes
    // three eighths of the power, so leaving it out puts every amplitude, and every ratio built
    // on them, exactly 4.26 dB low. That error is indistinguishable from a microphone that is a
    // little worse than it is, which is why it is removed rather than absorbed.
    const auto windowScale = std::max (1.0e-6f, analyser.getWindowMeanSquare());

    return total / ((double) spec.fftSize * (double) spec.fftSize * (double) windowScale);
}

int DistortionAnalyser::findPeakNear (const dsp::Spectrum& spec, double centreBin,
                                      double searchRadius) const
{
    if (spec.data.empty())
        return 0;

    const auto numBins = (int) spec.data.size();
    const auto centre = (int) std::lround (centreBin);
    const auto low = juce::jmax (1, (int) std::floor (centreBin - searchRadius));
    const auto high = juce::jmin (numBins - 2, (int) std::ceil (centreBin + searchRadius));

    int best = juce::jlimit (1, numBins - 2, centre);

    for (int i = low; i <= high; ++i)
        if (std::norm (spec.data[(size_t) i]) > std::norm (spec.data[(size_t) best]))
            best = i;

    return best;
}

DistortionAnalyser::Result DistortionAnalyser::analyse (const float* samples, int numSamples) const
{
    Result result;

    if (samples == nullptr || numSamples < analyser.getFftSize())
        return result;

    analyser.analyse (samples, spectrum);

    if (! spectrum.valid || spectrum.data.empty())
        return result;

    const auto numBins = (int) spectrum.data.size();
    result.numBins = numBins;

    if (spectrum.sampleRate <= 0.0f || analyser.getFftSize() <= 0)
        return result;

    const auto binHz = spectrum.sampleRate / (double) analyser.getFftSize();
    const auto nyquist = spectrum.sampleRate * 0.5;

    // The fundamental: the strongest thing inside the search range, taken as a whole main lobe
    // rather than as one bin, so a tone between bins is not measured low.
    const auto lowBin = juce::jmax (1, (int) std::ceil (settings.searchLowHz / binHz));
    const auto highBin = juce::jmin (numBins - 2, (int) std::floor (settings.searchHighHz / binHz));

    if (highBin <= lowBin)
        return result;

    int fundamentalBin = lowBin;
    auto bestPower = -1.0;

    for (int i = lowBin; i <= highBin; ++i)
    {
        const auto power = (double) std::norm (spectrum.data[(size_t) i]);

        if (power > bestPower)
        {
            bestPower = power;
            fundamentalBin = i;
        }
    }

    // Refined to a fraction of a bin by fitting a parabola through the peak and its
    // neighbours, in decibels. A third of a bin is enough to place every harmonic correctly,
    // and going further than the peak's own shape supports would only add noise.
    auto fractionalBin = (double) fundamentalBin;

    if (fundamentalBin > lowBin && fundamentalBin < highBin)
    {
        const auto leftDb = dsp::db10 ((float) std::norm (spectrum.data[(size_t) (fundamentalBin - 1)]));
        const auto midDb = dsp::db10 ((float) std::norm (spectrum.data[(size_t) fundamentalBin]));
        const auto rightDb = dsp::db10 ((float) std::norm (spectrum.data[(size_t) (fundamentalBin + 1)]));

        const auto denominator = leftDb - 2.0f * midDb + rightDb;

        if (std::abs (denominator) > 1.0e-6f)
        {
            const auto offset = juce::jlimit (-0.5f, 0.5f,
                                              0.5f * (leftDb - rightDb) / denominator);
            fractionalBin += (double) offset;
        }
    }

    result.fundamentalHz = (float) (fractionalBin * binHz);

    const auto fundamentalPower = bandPower (spectrum, fundamentalBin);
    result.fundamentalLevelDb = (float) (10.0 * std::log10 (std::max (1.0e-30, fundamentalPower)));

    // Harmonics, placed by multiplying the fundamental's own fractional bin. A harmonic of a
    // coherent tone sits at exactly n times the fundamental, so its offset within its bin is n
    // times the fundamental's offset, and using that instead of rounding each one separately
    // keeps a tenth of a bin of accuracy all the way up to the tenth harmonic.
    harmonicPowers.clear();

    Harmonic h1;
    h1.order = 1;
    h1.frequency = result.fundamentalHz;
    h1.amplitude = powerToAmplitude (fundamentalPower);
    h1.levelDb = result.fundamentalLevelDb;
    h1.relativeDb = 0.0f;
    h1.valid = true;
    result.harmonics.push_back (h1);

    double harmonicTotal = 0.0;
    double coveredBins = 0.0;

    for (int order = 2; order <= settings.maxHarmonic; ++order)
    {
        Harmonic harmonic;
        harmonic.order = order;

        const auto centreBin = fractionalBin * (double) order;
        const auto frequency = (float) (centreBin * binHz);

        harmonic.frequency = frequency;

        if (frequency >= nyquist || centreBin >= (double) (numBins - 2))
        {
            harmonic.valid = false;
            result.harmonics.push_back (harmonic);
            continue;
        }

        // The search widens with the order, because the uncertainty in the fundamental is
        // multiplied by it. Three bins either side of the predicted place is enough to absorb a
        // tenth of a bin of fundamental error ten times over, and narrow enough that a
        // neighbouring harmonic cannot be mistaken for this one.
        const auto radius = 3.0 + (double) order * 0.25;
        const auto peakBin = findPeakNear (spectrum, centreBin, radius);

        const auto power = bandPower (spectrum, peakBin);

        harmonicPowers.push_back (power);
        harmonicTotal += power;
        coveredBins += (double) (2 * settings.halfWidthBins + 1);

        harmonic.amplitude = powerToAmplitude (power);
        harmonic.levelDb = (float) (10.0 * std::log10 (std::max (1.0e-20, power)));
        harmonic.relativeDb = harmonic.levelDb - result.fundamentalLevelDb;
        harmonic.valid = true;
        result.harmonics.push_back (harmonic);
    }

    // The total, so that what is left over is what is neither the fundamental nor a harmonic.
    // DC is left out because an audio interface sitting a few millivolts off zero is a property
    // of the converter and not of the signal.
    double totalPower = 0.0;

    for (int i = 1; i < numBins - 1; ++i)
        totalPower += 2.0 * (double) std::norm (spectrum.data[(size_t) i]);

    const auto windowScale = std::max (1.0e-6f, analyser.getWindowMeanSquare());

    totalPower /= ((double) analyser.getFftSize() * (double) analyser.getFftSize()
                   * (double) windowScale);

    const auto noisePower = std::max (0.0, totalPower - fundamentalPower - harmonicTotal);

    result.noiseLevelDb = (float) (10.0 * std::log10 (std::max (1.0e-30, noisePower)));
    result.analysedFraction = totalPower > 0.0 ? coveredBins / (double) (numBins - 2) : 0.0;

    // Nothing published may leave here as an infinity or a not-a-number. Whatever the input
    // was, a figure that cannot be expressed as a number is refused rather than handed on,
    // because a reading of that kind looks like a number on the panel and is believed.
    {
        const bool finite = std::isfinite (result.fundamentalHz)
                         && std::isfinite (result.fundamentalLevelDb)
                         && std::isfinite (result.noiseLevelDb);

        if (! finite)
        {
            result.valid = false;
            result.harmonics.clear();
            return result;
        }
    }

    const auto thd = thdRatio (harmonicPowers, fundamentalPower);

    result.thdPercent = thd * 100.0f;
    result.thdDb = thd > 0.0f ? 20.0f * std::log10 ((double) thd) : dbFloor;

    // The same ratio, with the noise added to the harmonics. Built as one vector rather than
    // by adding two, because the noise is a single number standing for everything that was not
    // a harmonic and it has to go into the sum on the same footing as they do.
    harmonicPowers.push_back (noisePower);

    const auto thdPlusN = thdRatio (harmonicPowers, fundamentalPower);

    harmonicPowers.pop_back();

    result.thdPlusNPercent = thdPlusN * 100.0f;
    result.thdPlusNDb = thdPlusN > 0.0f ? 20.0f * std::log10 ((double) thdPlusN) : dbFloor;

    // SINAD is the signal against everything that is not the signal. Because the noise is taken
    // as whatever the harmonics did not account for, it is exactly the negative of THD+N, and
    // the two are computed separately rather than one from the other so that a mistake in
    // either shows up as the two disagreeing.
    const auto remainder = std::max (1.0e-30, totalPower - fundamentalPower);

    // Floored the same way the noise level is. Without it, a signal with nothing but a
    // fundamental divides by zero and SINAD comes back as a not-a-number, which is what digital
    // silence does. It is the cleanest input there is and it has to come back as a very large
    // number rather than as nonsense.
    result.sinadDb = (float) (10.0 * std::log10 (fundamentalPower / std::max (1.0e-30, remainder)));

    // Checked again now the derived figures exist, since this is the first point at which all
    // of them are known.
    if (! std::isfinite (result.thdPercent) || ! std::isfinite (result.thdDb)
        || ! std::isfinite (result.thdPlusNPercent) || ! std::isfinite (result.thdPlusNDb)
        || ! std::isfinite (result.sinadDb))
    {
        result.valid = false;
        result.harmonics.clear();
        result.thdPercent = result.thdDb = 0.0f;
        result.thdPlusNPercent = result.thdPlusNDb = 0.0f;
        result.sinadDb = 0.0f;
    }
    result.valid = true;
    return result;
}
}