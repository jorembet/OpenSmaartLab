#include "EqDesigner.h"
#include <cmath>

namespace dsp
{
namespace
{
    constexpr float levelFloorDb = -200.0f;

    /** Resamples a measured response onto the design grid by picking the nearest bin.

        A log grid is right for judging a correction, but a measurement arrives on a linear
        FFT axis, so every grid point has to be matched to a bin. Nearest bin rather than
        interpolation: interpolating a spectrum between bins invents a level that was never
        measured, and the fit would then be aiming at an invention. */
    std::vector<float> resampleToGrid (const std::vector<float>& frequencies,
                                       const std::vector<float>& measuredDb,
                                       const std::vector<char>& validBins,
                                       const std::vector<float>& grid,
                                       std::vector<char>& gridValid)
    {
        std::vector<float> result (grid.size(), levelFloorDb);
        gridValid.assign (grid.size(), 0);

        if (frequencies.empty() || frequencies.size() != measuredDb.size() || grid.empty())
            return result;

        size_t bin = 0;

        for (size_t i = 0; i < grid.size(); ++i)
        {
            while (bin + 1 < frequencies.size() && frequencies[bin + 1] < grid[i])
                ++bin;

            // The closest of the bin at or below the point and the one above it.
            if (bin + 1 < frequencies.size()
                 && std::abs (frequencies[bin + 1] - grid[i]) < std::abs (frequencies[bin] - grid[i]))
                ++bin;

            if (frequencies[bin] <= 0.0f)
                continue;

            const auto valid = validBins.empty() || bin >= validBins.size() || validBins[bin] != 0;

            if (! valid)
                continue;

            result[i] = measuredDb[bin];
            gridValid[i] = 1;
        }

        return result;
    }
}

juce::StringArray TargetCurve::getKindNames()
{
    return { "Datar (0 dB)", "Datar + tilt halus", "House Harman", "Datar + tilt kuat" };
}

juce::String TargetCurve::kindToString (Kind kind)
{
    const auto names = getKindNames();
    const auto index = (int) kind;
    return index >= 0 && index < names.size() ? names[index] : juce::String();
}

float TargetCurve::levelAt (float frequency) const
{
    if (frequency <= 0.0f)
        return 0.0f;

    switch (kind)
    {
        case Kind::Flat:
            return 0.0f;

        case Kind::FlatWithTilt:
        {
            // A gentle shelf: flat to 1 kHz, then falling slowly towards -3 dB at 20 kHz.
            // Below 1 kHz nothing is asked for, because a measurement at the listening
            // position almost always carries less bass than an anechoic target would.
            if (frequency <= 1000.0f)
                return 0.0f;

            const auto fraction = (std::log10 (frequency / 1000.0f) / std::log10 (20.0f));
            return -3.0f * (float) fraction;
        }

        case Kind::Harman:
        {
            // A small presence lift with a rolled off low end. This is close to the curve a
            // well behaved speaker system is usually judged against.
            auto level = 0.0f;

            if (frequency > 1000.0f && frequency < 10000.0f)
            {
                // Peaking near 2.5 kHz: the region where a room takes the most energy off.
                const auto octavesFromCentre = std::log2 (frequency / 2500.0f);
                level += 1.5f * (float) std::exp (-0.5 * octavesFromCentre * octavesFromCentre);
            }

            if (frequency < 120.0f)
                level -= 6.0f * (float) (std::log2 (120.0f / frequency) / std::log2 (6.0f));

            if (frequency > 8000.0f)
                level -= 3.0f * (float) ((std::log2 (frequency / 8000.0f)) / std::log2 (2.5f));

            return level;
        }

        case Kind::FlatWithStrongTilt:
        {
            // For a bright room where the top end is the problem: a shelf from 2 kHz down
            // to -6 dB at 20 kHz.
            if (frequency <= 2000.0f)
                return 0.0f;

            const auto fraction = (std::log10 (frequency / 2000.0f) / std::log10 (10.0f));
            return -6.0f * (float) fraction;
        }
    }

    return 0.0f;
}

EqDesigner::EqDesigner()
{
    settings.designPoints = std::max (16, settings.designPoints);
}

void EqDesigner::setSettings (const Settings& newSettings)
{
    Settings clamped = newSettings;

    clamped.maxFilters = juce::jlimit (0, 32, clamped.maxFilters);
    clamped.maxFilterGainDb = juce::jlimit (0.5f, 24.0f, clamped.maxFilterGainDb);
    clamped.lowFrequency = juce::jlimit (10.0f, 2000.0f, clamped.lowFrequency);
    clamped.highFrequency = juce::jlimit (clamped.lowFrequency * 2.0f, 20000.0f,
                                          clamped.highFrequency);
    clamped.designPoints = juce::jlimit (16, 512, clamped.designPoints);
    clamped.toleranceDb = juce::jlimit (0.05f, 6.0f, clamped.toleranceDb);

    settings = clamped;
}

void EqDesigner::setTarget (const TargetCurve& newTarget)
{
    target = newTarget;
}

bool EqDesigner::isInDesignRange (float frequency, const Settings& settings)
{
    return frequency >= settings.lowFrequency && frequency <= settings.highFrequency;
}

std::vector<float> EqDesigner::designGrid (const Settings& settings)
{
    std::vector<float> grid;
    grid.reserve ((size_t) std::max (16, settings.designPoints));

    const auto points = std::max (16, settings.designPoints);

    for (int i = 0; i < points; ++i)
    {
        const auto fraction = (double) i / (double) (points - 1);
        grid.push_back ((float) (settings.lowFrequency
                                 * std::pow (settings.highFrequency / settings.lowFrequency, fraction)));
    }

    return grid;
}

float EqDesigner::peakingResponseDb (const PeakingFilter& filter, float frequency,
                                     double sampleRate)
{
    if (! filter.enabled || sampleRate <= 0.0f || frequency <= 0.0f)
        return 0.0f;

    // A peaking filter cannot be centred at or above Nyquist, and asking for one there
    // produces a division by zero rather than a filter.
    const auto nyquist = (float) (sampleRate * 0.5);
    const auto centre = juce::jlimit (10.0f, nyquist * 0.95f, filter.frequency);
    const auto gain = juce::jlimit (-24.0f, 24.0f, filter.gainDb);
    const auto q = juce::jlimit (0.1f, 20.0f, filter.q);

    const auto amplitude = std::pow (10.0f, gain / 40.0f);
    const auto w0 = 2.0f * juce::MathConstants<float>::pi * centre / (float) sampleRate;
    const auto alpha = std::sin (w0) / (2.0f * q);

    const auto b0 = 1.0f + alpha * amplitude;
    const auto b1 = -2.0f * std::cos (w0);
    const auto b2 = 1.0f - alpha * amplitude;
    const auto a0 = 1.0f + alpha / amplitude;
    const auto a1 = -2.0f * std::cos (w0);
    const auto a2 = 1.0f - alpha / amplitude;

    const auto w = 2.0f * juce::MathConstants<float>::pi * frequency / (float) sampleRate;
    const auto cosW = std::cos (w);
    const auto cos2W = std::cos (2.0f * w);
    const auto sinW = std::sin (w);
    const auto sin2W = std::sin (2.0f * w);

    const auto numeratorReal = b0 + b1 * cosW + b2 * cos2W;
    const auto numeratorImag = b1 * sinW + b2 * sin2W;
    const auto denominatorReal = a0 + a1 * cosW + a2 * cos2W;
    const auto denominatorImag = a1 * sinW + a2 * sin2W;

    const auto numerator = std::sqrt (numeratorReal * numeratorReal + numeratorImag * numeratorImag);
    const auto denominator = std::sqrt (denominatorReal * denominatorReal + denominatorImag * denominatorImag);

    if (denominator < 1.0e-20f)
        return 0.0f;

    return juce::jlimit (-36.0f, 36.0f, 20.0f * std::log10 (std::max (1.0e-20f, numerator / denominator)));
}

std::vector<float> EqDesigner::filterResponseDb (const std::vector<PeakingFilter>& filters,
                                                 const std::vector<float>& frequencies,
                                                 double sampleRate)
{
    std::vector<float> response (frequencies.size(), 0.0f);

    for (size_t i = 0; i < frequencies.size(); ++i)
    {
        auto total = 0.0f;

        for (const auto& filter : filters)
            total += peakingResponseDb (filter, frequencies[i], sampleRate);

        response[i] = total;
    }

    return response;
}

std::vector<float> EqDesigner::targetResponseDb (const TargetCurve& target,
                                                  const std::vector<float>& frequencies)
{
    std::vector<float> response (frequencies.size(), 0.0f);

    for (size_t i = 0; i < frequencies.size(); ++i)
        response[i] = target.levelAt (frequencies[i]);

    return response;
}

float EqDesigner::worstErrorDb (const std::vector<PeakingFilter>& filters,
                                const std::vector<float>& grid,
                                const std::vector<float>& measuredDb,
                                const std::vector<float>& targetDb,
                                const std::vector<int>& usable,
                                double sampleRate)
{
    auto worst = 0.0f;

    for (auto index : usable)
    {
        auto error = measuredDb[(size_t) index] - targetDb[(size_t) index];

        for (const auto& filter : filters)
            error += peakingResponseDb (filter, grid[(size_t) index], sampleRate);

        worst = std::max (worst, std::abs (error));
    }

    return worst;
}

float EqDesigner::rmsErrorDb (const std::vector<PeakingFilter>& filters,
                              const std::vector<float>& grid,
                              const std::vector<float>& measuredDb,
                              const std::vector<float>& targetDb,
                              const std::vector<int>& usable,
                              double sampleRate)
{
    if (usable.empty())
        return 0.0f;

    double sumSquares = 0.0;

    for (auto index : usable)
    {
        auto error = measuredDb[(size_t) index] - targetDb[(size_t) index];

        for (const auto& filter : filters)
            error += peakingResponseDb (filter, grid[(size_t) index], sampleRate);

        sumSquares += (double) error * (double) error;
    }

    return (float) std::sqrt (sumSquares / (double) usable.size());
}

EqDesigner::Result EqDesigner::design (const std::vector<float>& frequencies,
                                       const std::vector<float>& measuredDb,
                                       double sampleRate,
                                       const std::vector<char>& validBins) const
{
    Result result;

    if (frequencies.size() < 4 || frequencies.size() != measuredDb.size())
        return result;

    // The rate the filters will actually run at, clamped to something a biquad can be built
    // for. A fit at an impossible rate would produce filters that cannot be implemented.
    const auto rate = juce::jlimit (8000.0, 192000.0, sampleRate);

    const auto grid = designGrid (settings);

    std::vector<char> gridValid;
    const auto measured = resampleToGrid (frequencies, measuredDb, validBins, grid, gridValid);
    const auto wanted = targetResponseDb (target, grid);

    // The fit only works on points that were actually measured. A band the reference never
    // excited has no level, and aiming a filter at it would be fitting to noise.
    std::vector<int> usable;

    for (size_t i = 0; i < grid.size(); ++i)
        if (gridValid[i] != 0)
            usable.push_back ((int) i);

    result.pointsUsed = (int) usable.size();

    if (usable.size() < 8)
        return result;

    result.initialDb = worstErrorDb (result.filters, grid, measured, wanted, usable, rate);

    // Signed error at one grid point: what the measurement is, plus what the filters chosen
    // so far would do, minus what the target asks for.
    const auto errorAt = [&] (size_t index, const std::vector<PeakingFilter>& filters)
    {
        auto error = measured[index] - wanted[index];

        for (const auto& filter : filters)
            error += peakingResponseDb (filter, grid[index], rate);

        return error;
    };

    for (int step = 0; step < settings.maxFilters; ++step)
    {
        // Find the frequency that is furthest from where the target wants it.
        size_t worstIndex = 0;
        auto worstValue = 0.0f;

        for (auto index : usable)
        {
            const auto error = errorAt ((size_t) index, result.filters);

            if (std::abs (error) > std::abs (worstValue))
            {
                worstValue = error;
                worstIndex = (size_t) index;
            }
        }

        if (std::abs (worstValue) <= settings.toleranceDb)
            break;

        // The gain is a fraction of the error on purpose. Correcting the whole error in one
        // filter overshoots, because a single filter cannot be shaped to match a curve it
        // is standing in for, and the next step then has to undo the overshoot. A fraction
        // converges instead of oscillating.
        PeakingFilter filter;
        filter.frequency = grid[worstIndex];
        filter.gainDb = juce::jlimit (-settings.maxFilterGainDb, settings.maxFilterGainDb,
                                      -worstValue * 0.7f);

        // Q follows how tightly the design grid is spaced: a deviation one grid step wide is
        // corrected by a filter one grid step wide. Without this the fit either smears a
        // narrow dip across the whole range or rings on a single point.
        const auto spanOctaves = std::log2 ((double) settings.highFrequency
                                            / (double) settings.lowFrequency);
        const auto stepOctaves = spanOctaves / (double) std::max (16, settings.designPoints - 1);
        filter.q = juce::jlimit (0.7f, 8.0f, (float) (1.0 / std::pow (2.0, stepOctaves * 2.0)));

        // The nominal gain is tried first and halved while it fails to improve the worst
        // error. A filter placed on the wrong side of a steep slope overshoots easily, and
        // accepting it would leave the fit worse off than the filter before it.
        auto accepted = false;
        auto gain = filter.gainDb;

        for (int attempt = 0; attempt < 5; ++attempt)
        {
            auto trial = result.filters;
            trial.push_back (filter);
            trial.back().gainDb = gain;

            if (worstErrorDb (trial, grid, measured, wanted, usable, rate)
                 < std::abs (worstValue) - 0.01f)
            {
                result.filters = trial;
                accepted = true;
                break;
            }

            gain *= 0.5f;
        }

        if (! accepted)
        {
            // No gain helped: either the error is already within tolerance, or this many
            // filters cannot reach it. Another filter would be a guess, so the fit stops and
            // reports what is left rather than padding the filter count.
            break;
        }
    }

    result.residualDb = worstErrorDb (result.filters, grid, measured, wanted, usable, rate);
    result.rmsResidualDb = rmsErrorDb (result.filters, grid, measured, wanted, usable, rate);
    result.valid = true;
    return result;
}

juce::StringArray EqDesigner::getExportFormats()
{
    return { "AutoEq ( teks )", "JSON", "REW Filter" };
}

juce::String EqDesigner::exportToText (const std::vector<PeakingFilter>& filters, double sampleRate)
{
    juce::String text;

    text << "# OpenSmaartLab auto-EQ\n";
    text << "# " << filters.size() << " peaking filter";
    text << (filters.size() == 1 ? "" : "s") << " pada " << juce::String (sampleRate, 0) << " Hz\n";
    text << "# Terapkan sebagai gain yang berlawanan dengan respons yang diukur.\n\n";

    for (size_t i = 0; i < filters.size(); ++i)
    {
        const auto& filter = filters[i];
        text << "Filter " << juce::String ((int) i + 1) << "  "
             << (filter.enabled ? "aktif" : "nonaktif") << "\n";
        text << "  Frekuensi : " << juce::String (filter.frequency, 1) << " Hz\n";
        text << "  Q         : " << juce::String (filter.q, 3) << "\n";
        text << "  Gain      : " << juce::String (filter.gainDb, 2) << " dB\n\n";
    }

    return text;
}

juce::String EqDesigner::exportToRew (const std::vector<PeakingFilter>& filters)
{
    // The REW filter file format, so a result can be loaded into the tool people already
    // have rather than being retyped.
    juce::String text;
    text << "FilterSettings file\n";
    text << "Version: 1\n";
    text << "Filters: " << filters.size() << "\n";

    for (const auto& filter : filters)
    {
        text << (filter.enabled ? "PK" : "//PK") << " "
             << juce::String (filter.frequency, 6) << " "
             << juce::String (filter.gainDb, 6) << " "
             << juce::String (filter.q, 6) << "\n";
    }

    return text;
}

juce::String EqDesigner::exportToJson (const std::vector<PeakingFilter>& filters, double sampleRate)
{
    juce::String text;
    text << "{\n";
    text << "  \"tool\": \"OpenSmaartLab\",\n";
    text << "  \"filterType\": \"peaking\",\n";
    text << "  \"sampleRate\": " << juce::String (sampleRate, 0) << ",\n";
    text << "  \"filters\": [\n";

    for (size_t i = 0; i < filters.size(); ++i)
    {
        const auto& filter = filters[i];
        text << "    { \"frequency\": " << juce::String (filter.frequency, 2)
             << ", \"gainDb\": " << juce::String (filter.gainDb, 3)
             << ", \"q\": " << juce::String (filter.q, 4)
             << ", \"enabled\": " << (filter.enabled ? "true" : "false") << " }";

        if (i + 1 < filters.size())
            text << ",";

        text << "\n";
    }

    text << "  ]\n}\n";
    return text;
}
}
