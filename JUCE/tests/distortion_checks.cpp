// Phase 10 checks: the distortion analyser.
//
// Every figure here is compared against a number worked out by hand, because "the software
// agrees with the software" is not a check. A fundamental with two harmonics of known amplitude
// has a THD that can be written down on paper:
//
//     THD = sqrt(V2^2 + V3^2 + ...) / V1
//
// and that is what the analyser has to reproduce. Getting the amplitudes wrong is the failure
// this catches, and it is easy to get wrong in ways that all still look plausible: taking one
// bin instead of the tone's main lobe, summing amplitudes instead of powers, or reporting the
// ratio against a fundamental that was itself measured low.
//
// The noise figures are checked by construction as well as by value. SINAD and THD+N are two
// ways of asking the same question from opposite ends, so they must be exact negatives of each
// other, and the tests hold them to that.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "DistortionAnalyser.h"
#include "DistortionDisplay.h"

namespace
{
    int failures = 0;

    void report (bool condition, const char* message, const juce::String& detail = {})
    {
        if (condition)
            return;

        ++failures;
        std::cout << "FAIL: " << message << "  [" << detail << "]" << std::endl;
    }

    void require (bool condition, const char* message)
    {
        report (condition, message, {});
    }

    constexpr double rate = 48000.0;
    constexpr int fftSize = 32768;

    class Noise
    {
    public:
        explicit Noise (uint32_t seed) : state (seed == 0u ? 0x9e3779b9u : seed) {}

        float next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return ((float) (state >> 8) * (1.0f / 8388608.0f)) - 1.0f;
        }

    private:
        uint32_t state;
    };

    /** A signal built from named partials, each with a peak amplitude.

        Built by adding sines rather than by filtering white noise, so the answer is known
        exactly and the test cannot be satisfied by a plausible-looking coincidence. */
    std::vector<float> partials (const std::vector<std::pair<double, float>>& components,
                                 double seconds = 1.5)
    {
        const auto count = (int) (seconds * rate);
        std::vector<float> samples ((size_t) count, 0.0f);

        for (int i = 0; i < count; ++i)
        {
            double value = 0.0;

            for (const auto& component : components)
                value += (double) component.second
                       * std::sin (2.0 * dsp::pi * component.first * (double) i / rate);

            samples[(size_t) i] = (float) value;
        }

        return samples;
    }

    void analyse (const std::vector<float>& samples, dsp::DistortionAnalyser::Result& result,
                  float searchLow = 20.0f, float searchHigh = 20000.0f)
    {
        dsp::DistortionAnalyser analyser;
        analyser.prepare (rate, fftSize);

        dsp::DistortionAnalyser::Settings settings;
        settings.searchLowHz = searchLow;
        settings.searchHighHz = searchHigh;
        settings.maxHarmonic = 10;
        settings.halfWidthBins = 2;
        analyser.setSettings (settings);

        result = analyser.analyse (samples.data(), (int) samples.size());
    }

    /** The hand calculation, written out so it can be read next to the software's answer. */
    float expectedThdPercent (double v1, const std::vector<double>& harmonics)
    {
        double sum = 0.0;

        for (auto v : harmonics)
            sum += v * v;

        return (float) (100.0 * std::sqrt (sum) / v1);
    }

    float expectedThdDb (double v1, const std::vector<double>& harmonics)
    {
        double sum = 0.0;

        for (auto v : harmonics)
            sum += v * v;

        return (float) (20.0 * std::log10 (std::sqrt (sum) / v1));
    }

    const dsp::DistortionAnalyser::Harmonic& harmonicAt (
        const dsp::DistortionAnalyser::Result& result, int order)
    {
        for (const auto& harmonic : result.harmonics)
            if (harmonic.order == order)
                return harmonic;

        return result.harmonics.front();
    }
}

int main()
{
    // Test 1: a fundamental with two harmonics of known amplitude.
    //
    // V1 = 1.0, V2 = 0.10, V3 = 0.05, so THD = sqrt(0.01 + 0.0025) / 1 = 0.1118034,
    // which is 11.1803 % or -19.0309 dB.
    {
        const auto samples = partials ({ { 1000.0, 1.0f }, { 2000.0, 0.10f },
                                         { 3000.0, 0.05f } });

        dsp::DistortionAnalyser::Result result;
        analyse (samples, result);

        require (result.valid, "a periodic signal must be measurable");

        report (std::abs (result.fundamentalHz - 1000.0f) < 3.0f,
                "the fundamental must be found at 1 kHz",
                juce::String (result.fundamentalHz, 2) + " Hz");

        // The fundamental's own amplitude, which everything else is relative to. A fundamental
        // measured low inflates nothing but deflates every ratio, so it is checked directly.
        report (std::abs (result.fundamentalLevelDb + 3.01f) < 0.5f,
                "a full scale fundamental must read -3.01 dBFS",
                juce::String (result.fundamentalLevelDb, 3) + " dB");

        report (std::abs (harmonicAt (result, 1).amplitude - 1.0f) < 0.02f,
                "the fundamental amplitude must be 1.0",
                juce::String (harmonicAt (result, 1).amplitude, 4));

        report (std::abs (harmonicAt (result, 2).amplitude - 0.10f) < 0.004f,
                "the second harmonic amplitude must be 0.10",
                juce::String (harmonicAt (result, 2).amplitude, 5));

        report (std::abs (harmonicAt (result, 3).amplitude - 0.05f) < 0.003f,
                "the third harmonic amplitude must be 0.05",
                juce::String (harmonicAt (result, 3).amplitude, 5));

        const auto expectedPercent = expectedThdPercent (1.0, { 0.10, 0.05 });
        const auto expectedDb = expectedThdDb (1.0, { 0.10, 0.05 });

        report (std::abs (result.thdPercent - expectedPercent) < 0.15f,
                "THD must match the hand calculation",
                juce::String (result.thdPercent, 4) + " % against "
                    + juce::String (expectedPercent, 4) + " %");

        report (std::abs (result.thdDb - expectedDb) < 0.15f,
                "THD in decibels must match the hand calculation",
                juce::String (result.thdDb, 4) + " dB against "
                    + juce::String (expectedDb, 4) + " dB");

        // A percentage and a decibel figure are two views of one number, so they have to agree
        // with each other. A pair that disagreed would mean one of them was computed from
        // something other than the other.
        report (std::abs (20.0f * std::log10 (result.thdPercent / 100.0f) - result.thdDb) < 0.05f,
                "the percentage and the decibel figure must be the same number",
                juce::String (result.thdPercent, 4) + " % against "
                    + juce::String (result.thdDb, 4) + " dB");
    }

    // Test 2: powers, not amplitudes. Two harmonics at the same level must add to a ratio of
    // sqrt(2) and not to 2. Getting this wrong is invisible when there is one harmonic and
    // doubles the answer when there are two of equal size, which is why it is pinned here.
    {
        const auto samples = partials ({ { 1000.0, 1.0f }, { 2000.0, 0.10f },
                                         { 3000.0, 0.10f } });

        dsp::DistortionAnalyser::Result result;
        analyse (samples, result);

        const auto expected = (float) (100.0 * std::sqrt (0.10 * 0.10 + 0.10 * 0.10) / 1.0);

        report (std::abs (result.thdPercent - expected) < 0.2f,
                "two equal harmonics must add as powers, giving sqrt(2) times one",
                juce::String (result.thdPercent, 4) + " % against "
                    + juce::String (expected, 4) + " %");
    }

    // Test 3: many harmonics, so the sum over the orders is exercised rather than two terms.
    {
        std::vector<double> amplitudes;
        std::vector<std::pair<double, float>> components { { 1000.0, 1.0f } };

        for (int order = 2; order <= 8; ++order)
        {
            const auto amplitude = 0.10 / (double) order;
            amplitudes.push_back (amplitude);
            components.push_back ({ 1000.0 * order, (float) amplitude });
        }

        const auto samples = partials (components);
        dsp::DistortionAnalyser::Result result;
        analyse (samples, result);

        const auto expected = expectedThdPercent (1.0, amplitudes);

        report (std::abs (result.thdPercent - expected) < 0.15f,
                "THD over seven harmonics must match the hand calculation",
                juce::String (result.thdPercent, 4) + " % against "
                    + juce::String (expected, 4) + " %");

        // Each harmonic must be found at its own frequency, which also means the fractional
        // bin placement works: at 8 kHz a tenth of a bin is 0.7 Hz and rounding would drift.
        for (int order = 2; order <= 8; ++order)
        {
            const auto& harmonic = harmonicAt (result, order);

            report (harmonic.valid && std::abs (harmonic.frequency - 1000.0 * order) < 3.0f,
                    ("harmonic " + juce::String (order) + " must be found at "
                         + juce::String (1000.0 * order, 0) + " Hz").toRawUTF8(),
                    juce::String (harmonic.frequency, 2) + " Hz");
        }
    }

    // Test 4: a pure tone has no harmonics to speak of. The noise floor of the analysis lands
    // in the harmonic slots, so this bounds how small a THD figure can honestly be reported.
    {
        const auto samples = partials ({ { 1000.0, 1.0f } });

        dsp::DistortionAnalyser::Result result;
        analyse (samples, result);

        require (result.valid, "a pure tone must still be measurable");

        report (result.thdPercent < 0.05f,
                "a pure tone must show a THD far below any real distortion",
                juce::String (result.thdPercent, 5) + " %");

        report (result.harmonics.size() >= 3,
                "the harmonic table must still be reported for a pure tone",
                juce::String ((int) result.harmonics.size()) + " rows");
    }

    // Test 5: harmonics well away from the fundamental, where the fractional bin matters most
    // and a rounding error would put the band in the wrong place.
    {
        const auto samples = partials ({ { 997.0, 1.0f }, { 2991.0, 0.05f } });

        dsp::DistortionAnalyser::Result result;
        analyse (samples, result, 500.0f, 5000.0f);

        report (std::abs (result.fundamentalHz - 997.0f) < 1.0f,
                "the fundamental must be located to a fraction of a hertz",
                juce::String (result.fundamentalHz, 3) + " Hz");

        report (std::abs (harmonicAt (result, 3).amplitude - 0.05f) < 0.004f,
                "a harmonic at three times a non integer frequency must still be measured right",
                juce::String (harmonicAt (result, 3).amplitude, 5));

        const auto expected = expectedThdPercent (1.0, { 0.05 });
        report (std::abs (result.thdPercent - expected) < 0.15f,
                "THD must be right when the fundamental is not bin centred",
                juce::String (result.thdPercent, 4) + " % against "
                    + juce::String (expected, 4) + " %");
    }

    // Test 6: THD+N and SINAD, and the identity between them.
    {
        const auto clean = partials ({ { 1000.0, 1.0f }, { 2000.0, 0.05f },
                                       { 3000.0, 0.02f } });
        const auto thdOnly = expectedThdPercent (1.0, { 0.05, 0.02 });

        dsp::DistortionAnalyser::Result cleanResult;
        analyse (clean, cleanResult);

        // The same signal with noise added, at a level chosen to be clearly above the
        // analysis floor so the difference cannot be confused with measurement scatter.
        auto noisy = clean;
        Noise noise (9876);

        for (auto& sample : noisy)
            sample += (float) (noise.next() * 0.05);

        dsp::DistortionAnalyser::Result noisyResult;
        analyse (noisy, noisyResult);

        require (noisyResult.valid, "a noisy signal must still be measurable");

        // The acceptance requirement: adding noise must move THD+N away from THD. If they
        // stayed together, the noise would be being reported as distortion or ignored, and
        // either way the two figures would be the same number wearing different names.
        report (noisyResult.thdPlusNPercent > cleanResult.thdPlusNPercent * 1.2f,
                "adding noise must raise THD+N well above THD",
                juce::String (cleanResult.thdPlusNPercent, 3) + " % clean against "
                    + juce::String (noisyResult.thdPlusNPercent, 3) + " % noisy");

        // Without noise the two must be very close, but they are not required to be equal and
        // the reason is worth stating. A window leaks: the fundamental's own sidelobes put a
        // little energy into bins that are neither the fundamental nor a harmonic, and that
        // energy is honestly part of the noise floor of the measurement. The generated signal
        // is also quantised, and a two sine sum is not exactly representable in floats.
        //
        // So the check is that they agree closely and that THD+N is never the smaller of the
        // two, which is the direction that would mean noise was being counted as distortion.
        report (cleanResult.thdPlusNPercent >= cleanResult.thdPercent - 1.0e-4f,
                "THD+N can never be below THD",
                juce::String (cleanResult.thdPlusNPercent, 4) + " against "
                    + juce::String (cleanResult.thdPercent, 4) + " %");

        report (std::abs (cleanResult.thdPlusNPercent - thdOnly) < 0.6f,
                "without noise, THD+N must be close to THD",
                juce::String (cleanResult.thdPlusNPercent, 4) + " against "
                    + juce::String (thdOnly, 4) + " %");

        report (std::abs (cleanResult.thdPlusNPercent - cleanResult.thdPercent) < 0.6f,
                "the excess of THD+N over THD must be window leakage, not something else",
                juce::String (cleanResult.thdPlusNPercent - cleanResult.thdPercent, 4)
                    + " % of excess");

        // SINAD and THD+N are the same measurement read from opposite ends: the signal against
        // everything that is not the signal, and its reciprocal. They must be exact negatives.
        report (std::abs (noisyResult.sinadDb + noisyResult.thdPlusNDb) < 0.05f,
                "SINAD must be the exact negative of THD+N",
                juce::String (noisyResult.sinadDb, 4) + " against "
                    + juce::String (-noisyResult.thdPlusNDb, 4) + " dB");

        report (std::abs (cleanResult.sinadDb + cleanResult.thdPlusNDb) < 0.05f,
                "SINAD must be the exact negative of THD+N for a clean signal too",
                juce::String (cleanResult.sinadDb, 4) + " against "
                    + juce::String (-cleanResult.thdPlusNDb, 4) + " dB");

        // The noise must have been separated rather than lumped in with the distortion, which
        // is what the harmonic table is for: the harmonics themselves must be unchanged by the
        // noise that was added.
        report (std::abs (harmonicAt (noisyResult, 2).amplitude
                          - harmonicAt (cleanResult, 2).amplitude) < 0.004f,
                "adding noise must not change the measured harmonics",
                juce::String (harmonicAt (noisyResult, 2).amplitude, 5) + " against "
                    + juce::String (harmonicAt (cleanResult, 2).amplitude, 5));

        report (noisyResult.noiseLevelDb > -40.0f,
                "the noise floor must be reported as a real level",
                juce::String (noisyResult.noiseLevelDb, 2) + " dB");

        // A noisier signal must give a worse SINAD, which is the direction the whole figure is
        // supposed to move in.
        auto noisier = clean;
        Noise other (13579);

        for (auto& sample : noisier)
            sample += (float) (other.next() * 0.30);

        dsp::DistortionAnalyser::Result noisierResult;
        analyse (noisier, noisierResult);

        report (noisierResult.sinadDb < noisyResult.sinadDb - 3.0f,
                "a noisier signal must report a worse SINAD",
                juce::String (noisierResult.sinadDb, 2) + " against "
                    + juce::String (noisyResult.sinadDb, 2) + " dB");
    }

    // Test 7: the harmonic table itself, which is what a reader judges the measurement by.
    {
        const auto samples = partials ({ { 1000.0, 1.0f }, { 2000.0, 0.10f },
                                         { 3000.0, 0.05f }, { 4000.0, 0.01f } });

        dsp::DistortionAnalyser::Result result;
        analyse (samples, result);

        report (result.harmonics.size() >= 5,
                 "the table must carry H1 through at least H4");

        report (result.harmonics.front().order == 1,
                "the first row must be H1",
                juce::String (result.harmonics.front().order));

        for (size_t i = 1; i < result.harmonics.size(); ++i)
            report (result.harmonics[i].order == result.harmonics[i - 1].order + 1,
                    "the harmonic orders must run consecutively",
                    juce::String (result.harmonics[i - 1].order) + " then "
                        + juce::String (result.harmonics[i].order));

        // The relative column is the whole reason for a table: a harmonic is read against the
        // fundamental, not against an absolute scale it shares with nothing.
        const auto& h2 = harmonicAt (result, 2);
        const auto& h3 = harmonicAt (result, 3);

        report (std::abs (h2.relativeDb + 20.0f) < 0.5f,
                "H2 at a tenth of the fundamental must read -20 dB relative",
                juce::String (h2.relativeDb, 3) + " dB");

        report (std::abs (h3.relativeDb + 26.02f) < 0.5f,
                "H3 at a twentieth of the fundamental must read -26 dB relative",
                juce::String (h3.relativeDb, 3) + " dB");

        report (std::abs (h2.relativeDb - h2.levelDb + result.fundamentalLevelDb) < 0.05f,
                "the relative column must be the level against the fundamental",
                juce::String (h2.relativeDb, 3) + " against "
                    + juce::String (h2.levelDb - result.fundamentalLevelDb, 3) + " dB");

        report (std::abs (harmonicAt (result, 1).relativeDb) < 1.0e-4f,
                "H1 must be 0 dB relative to itself",
                juce::String (harmonicAt (result, 1).relativeDb, 6) + " dB");

        report (result.analysedFraction > 0.0 && result.analysedFraction < 1.0,
                "the analysed fraction must be reported and must be a fraction",
                juce::String (result.analysedFraction, 4));
    }

    // Test 8: nothing may come out as a NaN or an infinity. These are the figures most likely
    // to, because each one is a ratio of two measurements that can both be zero: a silent
    // input, a fundamental outside the search range, a block shorter than the transform.
    {
        dsp::DistortionAnalyser::Result silent;
        analyse (std::vector<float> ((size_t) fftSize, 0.0f), silent);

        report (! silent.valid || silent.thdPercent == 0.0f,
                 "silence must not produce a distortion figure");

        report (std::isfinite (silent.thdPercent) && std::isfinite (silent.thdDb),
                "silence must not produce NaN or Inf");

        // A block too short to transform.
        dsp::DistortionAnalyser analyser;
        analyser.prepare (rate, fftSize);
        const auto tooShort = analyser.analyse (nullptr, fftSize - 1);

        require (! tooShort.valid, "a null or short block must be refused");

        // A signal whose fundamental is outside the search range.
        dsp::DistortionAnalyser::Result outOfRange;
        analyse (partials ({ { 50.0, 1.0f }, { 150.0, 0.1f } }), outOfRange, 500.0f, 5000.0f);

        report (std::isfinite (outOfRange.thdPercent),
                "a fundamental outside the search range must not produce NaN",
                juce::String (outOfRange.thdPercent));

        // And the arithmetic itself, called directly, must refuse rather than divide.
        report (dsp::DistortionAnalyser::thdRatio ({ 1.0, 2.0 }, 0.0) == 0.0f,
                 "THD against a silent fundamental must be refused rather than divided");

        report (dsp::DistortionAnalyser::thdRatio ({ 0.0, 0.0 }, 1.0) == 0.0f,
                 "THD with no harmonics must be zero");

        report (std::isfinite (dsp::DistortionAnalyser::thdRatio ({ 4.0, 9.0 }, 25.0)),
                 "the ratio itself must be finite for ordinary input");
    }

    // Test 9: the display formatting, because a reading printed at a fixed precision reports a
    // floor instead of a measurement once the value drops below it.
    {
        const auto small = dsp::DistortionAnalyser::percentToString (0.0004f);
        report (small != "0.000 %",
                "a very small distortion must not be printed as zero",
                small);

        const auto tiny = dsp::DistortionAnalyser::percentToString (0.0000012f);
        report (tiny != "0.000 %" && tiny != "0.0000 %",
                "a distortion far below the second decimal must still print a digit",
                tiny);

        const auto large = dsp::DistortionAnalyser::percentToString (42.13f);
        report (large.contains ("42.1"),
                "a large distortion must be printed to a sensible precision",
                large);

        report (dsp::DistortionAnalyser::levelToString (dsp::dbFloor) == "--",
                "an unavailable level must print as unavailable rather than as a number");

        report (dsp::DistortionAnalyser::levelToString (-19.03f) == "-19.0 dB",
                "an available level must print with its unit",
                dsp::DistortionAnalyser::levelToString (-19.03f));
    }

    // Test 10: the display itself. The figures are the product of this phase and the summary is
    // what a reader quotes, so both are checked to carry the numbers rather than a label.
    {
        DistortionDisplay display;

        display.setSize (900, 500);

        auto summary = display.getSummary();

        report (summary.contains ("Belum ada"),
                "with no measurement the summary must say so rather than show a figure",
                summary);

        // Painting an empty display must be harmless, because it happens on every resize before
        // any audio has arrived.
        {
            juce::Image image (juce::Image::ARGB, 900, 500, true);
            juce::Graphics g (image);
            display.paint (g);
        }

        const auto samples = partials ({ { 1000.0, 1.0f }, { 2000.0, 0.10f },
                                         { 3000.0, 0.05f } });

        dsp::DistortionAnalyser::Result measured;
        analyse (samples, measured);

        display.setResult (measured);
        summary = display.getSummary();

        report (summary.contains ("1000.0 Hz"),
                "the summary must name the fundamental it measured",
                summary);

        report (summary.contains (juce::String (measured.thdPercent, 1)),
                "the summary must carry the THD that was measured",
                summary);

        report (summary.contains ("THD+N") && summary.contains ("SINAD"),
                "the summary must carry both the noise figures",
                summary);

        // And painting a populated one must not fault on a harmonic whose order runs past the
        // end of the table, which is the case that catches a loop bound written for the
        // number of harmonics rather than for the rows.
        {
            juce::Image image (juce::Image::ARGB, 900, 500, true);
            juce::Graphics g (image);
            display.paint (g);
        }

        // An empty result must clear the display rather than leave the previous figures up,
        // which would be the more dangerous of the two mistakes: a stale distortion reading
        // looks exactly like a current one.
        display.setResult (dsp::DistortionAnalyser::Result());
        report (display.getSummary().contains ("Belum ada"),
                "clearing the measurement must clear the summary",
                display.getSummary());
    }

    if (failures == 0)
        std::cout << "PASS: THD, THD+N and SINAD match the hand calculations, and the noise is "
                     "separated from the distortion" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}