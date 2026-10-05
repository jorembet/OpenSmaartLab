// Phase 4 checks: the auto-EQ designer.
//
// The designer is judged by a round trip rather than by looking at the filters it produced.
// A known set of filters stands in for the room, their response becomes the measurement, and
// the fitted filters are then added back onto that same measurement. If the fit is correct
// the sum has to come back to the target, and that statement does not care how many filters
// were used, what Q they carry, or in what order they were placed. A solver that produced a
// plausible looking list while leaving the response wrong would still fail here.

#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>
#include <vector>

#include "EqDesigner.h"

namespace
{
    int failures = 0;

    void report (bool condition, const char* message, const juce::String& detail)
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

    /** Frequencies on the linear axis an FFT produces, so the designer is handed the shape
        of data it will actually receive rather than the log grid it works on. */
    std::vector<float> fftFrequencies (int fftSize = 16384)
    {
        std::vector<float> frequencies;

        for (int i = 1; i < fftSize / 2; ++i)
            frequencies.push_back ((float) (i * rate / fftSize));

        return frequencies;
    }

    /** The response of a known set of filters, standing in for how a room behaves. */
    std::vector<float> responseOf (const std::vector<dsp::PeakingFilter>& filters,
                                   const std::vector<float>& frequencies)
    {
        return dsp::EqDesigner::filterResponseDb (filters, frequencies, rate);
    }

    struct Band
    {
        float lowest = 0.0f;
        float highest = 0.0f;
        int bins = 0;
    };

    Band bandOf (const std::vector<float>& frequencies, const std::vector<float>& values,
                 double lowHz, double highHz)
    {
        Band band;
        band.lowest = 1.0e9f;
        band.highest = -1.0e9f;

        for (size_t i = 0; i < frequencies.size() && i < values.size(); ++i)
        {
            if (frequencies[i] < lowHz || frequencies[i] > highHz)
                continue;

            band.lowest = std::min (band.lowest, values[i]);
            band.highest = std::max (band.highest, values[i]);
            ++band.bins;
        }

        return band;
    }

    void configure (dsp::EqDesigner& designer, int maxFilters = 8)
    {
        dsp::EqDesigner::Settings settings;
        settings.maxFilters = maxFilters;
        settings.lowFrequency = 100.0f;
        settings.highFrequency = 12000.0f;
        designer.setSettings (settings);

        dsp::TargetCurve target;
        target.kind = dsp::TargetCurve::Kind::Flat;
        designer.setTarget (target);
    }
}

int main()
{
    const auto frequencies = fftFrequencies();

    // A room: a bass roll off and a presence dip, both cuts, which is the shape a correction
    // has to fill. Two well separated problems, so the fit has to place more than one filter.
    const std::vector<dsp::PeakingFilter> room
    {
        { 220.0f, -9.0f, 1.2f, true },
        { 2400.0f, -6.0f, 1.6f, true }
    };

    const auto roomResponse = responseOf (room, frequencies);

    std::vector<float> measurement (frequencies.size(), 0.0f);

    for (size_t i = 0; i < frequencies.size(); ++i)
        measurement[i] = roomResponse[i];

    // Test 1: a room response must be recognisable as needing correction, so the reported
    // starting error has to be of the order of the dips and not something rounded away.
    {
        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, measurement, rate);

        require (result.valid, "a measurable response must produce a valid result");
        report (result.pointsUsed > 8, "the design must use most of its grid",
                juce::String (result.pointsUsed) + " points");

        report (result.initialDb > 4.0f,
                "the uncorrected response must be reported as well off target",
                juce::String (result.initialDb, 2) + " dB");
    }

    // Test 2: the round trip. Adding the fitted filters onto the measurement has to bring the
    // response back to the target, and this is the check the whole designer exists for.
    {
        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, measurement, rate);

        require (result.filters.size() > 0, "a dipped response must produce filters");

        const auto correction = responseOf (result.filters, frequencies);

        std::vector<float> corrected (frequencies.size(), 0.0f);

        for (size_t i = 0; i < frequencies.size(); ++i)
            corrected[i] = measurement[i] + correction[i];

        const auto band = bandOf (frequencies, corrected, 100.0, 12000.0);

        report (std::abs (band.highest) < 1.5f,
                "the corrected response must not overshoot above the target",
                juce::String (band.highest, 3) + " dB");

        report (std::abs (band.lowest) < 1.5f,
                "the corrected response must not leave a dip below the target",
                juce::String (band.lowest, 3) + " dB");

        // The fit has to be a real improvement, stated as a number the reader can check.
        report (result.residualDb < result.initialDb - 3.0f,
                "fitting must improve the worst error by at least 3 dB",
                juce::String (result.initialDb, 2) + " dB down to "
                    + juce::String (result.residualDb, 2) + " dB");
    }

    // Test 3: more filters must not make the answer worse. A solver that kept stacking
    // filters regardless would reach a good residual by accident and then overshoot on the
    // way, so the round trip is repeated with a larger budget.
    {
        dsp::EqDesigner designer;
        configure (designer, 16);

        const auto result = designer.design (frequencies, measurement, rate);
        const auto correction = responseOf (result.filters, frequencies);

        std::vector<float> corrected (frequencies.size(), 0.0f);

        for (size_t i = 0; i < frequencies.size(); ++i)
            corrected[i] = measurement[i] + correction[i];

        const auto band = bandOf (frequencies, corrected, 100.0, 12000.0);

        report (std::abs (band.lowest) < 1.5f && std::abs (band.highest) < 1.5f,
                "a larger filter budget must still land on the target",
                juce::String (band.lowest, 3) + " to " + juce::String (band.highest, 3) + " dB");
    }

    // Test 4: a response that is already flat must be left alone. A designer that always
    // returns its full budget would be applying correction to a system that does not need
    // any, and every one of those filters is a risk the owner did not ask to take.
    {
        std::vector<float> flat (frequencies.size(), 0.0f);

        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, flat, rate);

        require (result.valid, "a flat response must still produce a valid result");
        report (result.filters.empty(),
                "a response already on target must need no filters",
                juce::String ((int) result.filters.size()) + " filters");

        report (result.residualDb < 0.5f,
                "a flat response must measure as already corrected",
                juce::String (result.residualDb, 3) + " dB");
    }

    // Test 5: every filter handed to the user has to be buildable. A centre at or above
    // Nyquist, a gain past what a filter can express, or a Q of zero cannot be turned into
    // audio, and a correction that cannot be implemented is worse than no correction because
    // it looks finished.
    {
        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, measurement, rate);

        for (const auto& filter : result.filters)
        {
            report (filter.frequency > 0.0f && filter.frequency < (float) (rate * 0.5),
                    "a filter must sit below Nyquist",
                    juce::String (filter.frequency, 1) + " Hz");

            report (std::abs (filter.gainDb) <= 24.0f,
                    "a filter gain must stay inside what a filter can express",
                    juce::String (filter.gainDb, 2) + " dB");

            report (filter.q >= 0.1f && filter.q <= 20.0f,
                    "a filter Q must stay inside the buildable range",
                    juce::String (filter.q, 3));

            require (filter.enabled, "a fitted filter must arrive enabled");
        }
    }

    // Test 6: the gain limit must be honoured. Asked to correct more than one filter may give,
    // the designer has to stop at the limit and report the error it could not remove, rather
    // than quietly exceeding it or pretending the response was fixed.
    {
        std::vector<float> deepDip (frequencies.size(), 0.0f);

        for (size_t i = 0; i < frequencies.size(); ++i)
            deepDip[i] = -20.0f;

        dsp::EqDesigner designer;
        dsp::EqDesigner::Settings settings;
        settings.maxFilters = 4;
        settings.maxFilterGainDb = 6.0f;
        designer.setSettings (settings);

        dsp::TargetCurve target;
        designer.setTarget (target);

        const auto result = designer.design (frequencies, deepDip, rate);

        for (const auto& filter : result.filters)
            report (std::abs (filter.gainDb) <= 6.0f + 0.001f,
                    "no filter may exceed the gain limit",
                    juce::String (filter.gainDb, 2) + " dB");

        // A 20 dB dip cannot be closed by four filters limited to 6 dB, and saying so is the
        // honest outcome. A residual near zero here would mean the limit was not applied.
        report (result.residualDb > 5.0f,
                "an unreachable correction must report the error it left",
                juce::String (result.residualDb, 2) + " dB");
    }

    // Test 7: the exports have to be usable by the tools people actually have. A filter set
    // that only exists inside this application is a result nobody can act on, so the formats
    // are checked for the fields those tools require rather than for being non empty.
    {
        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, measurement, rate);
        require (! result.filters.empty(), "the export checks need filters to export");

        const auto text = dsp::EqDesigner::exportToText (result.filters, rate);
        report (text.contains ("Peaking") || text.contains ("peaking")
                    || text.contains ("Frekuensi"),
                "the text export must name the filter parameters",
                text.substring (0, 60));
        require (text.contains (juce::String (result.filters.size())),
                "the text export must state how many filters it carries");

        const auto json = dsp::EqDesigner::exportToJson (result.filters, rate);
        require (json.contains ("\"filters\""), "the JSON export must carry a filters field");
        require (json.contains ("\"frequency\""), "the JSON export must carry a frequency field");
        require (json.contains ("\"gainDb\""), "the JSON export must carry a gain field");
        require (json.contains ("\"q\""), "the JSON export must carry a Q field");
        report (json.contains ("\"sampleRate\": 48000"),
                "the JSON export must record the rate it was fitted for",
                json.substring (0, 120));

        const auto rew = dsp::EqDesigner::exportToRew (result.filters);
        report (rew.startsWith ("FilterSettings file"),
                "the REW export must start with the header that tool expects",
                rew.substring (0, 40));

        // One PK line per enabled filter, each with three numbers, which is all REW needs.
        auto pkLines = 0;

        for (auto line : juce::StringArray::fromTokens (rew, "\n", ""))
            if (line.startsWith ("PK "))
                ++pkLines;

        report (pkLines == (int) result.filters.size(),
                "the REW export must carry one line per filter",
                juce::String (pkLines) + " lines for "
                    + juce::String ((int) result.filters.size()) + " filters");
    }

    // Test 8: the targets must actually differ, and in the direction their names promise. A
    // tilt that fell with frequency would turn every correction into a mistake, so the
    // shapes are checked directly rather than through the fit.
    {
        dsp::TargetCurve flat;
        flat.kind = dsp::TargetCurve::Kind::Flat;
        require (std::abs (flat.levelAt (1000.0f)) < 0.01f,
                 "a flat target must be 0 dB everywhere");
        require (std::abs (flat.levelAt (20.0f)) < 0.01f,
                 "a flat target must be 0 dB at the bottom too");

        dsp::TargetCurve tilt;
        tilt.kind = dsp::TargetCurve::Kind::FlatWithTilt;
        report (tilt.levelAt (1000.0f) >= -0.01f,
                "the gentle tilt must not cut below its pivot",
                juce::String (tilt.levelAt (1000.0f), 2) + " dB at 1 kHz");
        report (tilt.levelAt (20000.0f) < -2.0f,
                "the gentle tilt must fall by the top octave",
                juce::String (tilt.levelAt (20000.0f), 2) + " dB at 20 kHz");

        dsp::TargetCurve strong;
        strong.kind = dsp::TargetCurve::Kind::FlatWithStrongTilt;
        report (strong.levelAt (20000.0f) < tilt.levelAt (20000.0f),
                "the strong tilt must fall further than the gentle one",
                juce::String (strong.levelAt (20000.0f), 2) + " against "
                    + juce::String (tilt.levelAt (20000.0f), 2) + " dB");
        report (strong.levelAt (2000.0f) >= -0.01f,
                "the strong tilt must not cut below its pivot",
                juce::String (strong.levelAt (2000.0f), 2) + " dB at 2 kHz");

        dsp::TargetCurve harman;
        harman.kind = dsp::TargetCurve::Kind::Harman;
        report (harman.levelAt (2500.0f) > flat.levelAt (2500.0f),
                "the house curve must lift the presence it is named for",
                juce::String (harman.levelAt (2500.0f), 2) + " dB at 2.5 kHz");
        report (harman.levelAt (50.0f) < harman.levelAt (500.0f),
                "the house curve must roll off the bottom",
                juce::String (harman.levelAt (50.0f), 2) + " against "
                    + juce::String (harman.levelAt (500.0f), 2) + " dB");

        require (dsp::TargetCurve::getKindNames().size() == 4,
                 "every target kind must have a name to show");
    }

    // Test 9: a measurement the reference never excited must not be fitted. The design grid
    // covers the whole band, and a band limited measurement leaves part of it with no level
    // at all. Chasing that part would hand the user filters for a frequency nothing proved.
    {
        std::vector<float> bandLimited (frequencies.size(), 0.0f);

        for (size_t i = 0; i < frequencies.size(); ++i)
            if (frequencies[i] < 2000.0f)
                bandLimited[i] = roomResponse[i];

        dsp::EqDesigner designer;
        configure (designer);

        const auto result = designer.design (frequencies, bandLimited, rate);

        require (result.valid, "a band limited response must still produce a valid result");

        for (const auto& filter : result.filters)
            report (filter.frequency <= 2100.0f,
                    "no filter may be placed where the measurement had no signal",
                    juce::String (filter.frequency, 1) + " Hz");
    }

    if (failures == 0)
        std::cout << "PASS: the auto-EQ designer converges, respects its limits, and exports "
                     "usable filter sets" << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}