#include "DistortionDisplay.h"

namespace
{
    const juce::Colour backgroundColour (0xff1d1d1d);
    const juce::Colour panelColour (0xff262626);
    const juce::Colour mutedColour (0xff9a9a9a);
    const juce::Colour textColour (0xffe8e8e8);
    const juce::Colour goodColour (0xff43a047);
    const juce::Colour warnColour (0xffffb300);
    const juce::Colour badColour (0xffe53935);
    const juce::Colour fundamentalColour (0xff64b5f6);

    struct Column
    {
        const char* title;
        int width;
    };

    constexpr Column columns[] =
    {
        { "H", 34 }, { "Frequency", 110 }, { "Amplitude", 110 },
        { "Relative dB", 100 }, { "Level", 90 }
    };

    constexpr int rowHeight = 22;
}

DistortionDisplay::DistortionDisplay()
{
    setOpaque (true);
}

void DistortionDisplay::setResult (const dsp::DistortionAnalyser::Result& newResult)
{
    result = newResult;
    repaint();
}

void DistortionDisplay::setSearchRange (float lowHz, float highHz)
{
    // Held on the component so the caller can read the summary without keeping its own copy of
    // the range the measurement was made over.
    juce::ignoreUnused (lowHz, highHz);
}

void DistortionDisplay::setMaxHarmonic (int order)
{
    juce::ignoreUnused (order);
}

juce::String DistortionDisplay::getSummary() const
{
    if (! result.valid)
        return "Belum ada pengukuran distorsi";

    return "Fundamental " + juce::String (result.fundamentalHz, 1) + " Hz"
           + "   THD " + dsp::DistortionAnalyser::percentToString (result.thdPercent)
           + "   THD+N " + dsp::DistortionAnalyser::percentToString (result.thdPlusNPercent)
           + "   SINAD " + dsp::DistortionAnalyser::levelToString (result.sinadDb);
}

juce::Colour DistortionDisplay::figureColour (float db) const
{
    // Thresholds in the range a measurement of a working amplifier actually lands in, rather
    // than round numbers that would paint everything the same colour.
    if (db > -60.0f)
        return goodColour;

    if (db > -90.0f)
        return warnColour;

    return badColour;
}

void DistortionDisplay::drawFigures (juce::Graphics& g, const juce::Rectangle<int>& area) const
{
    struct Figure
    {
        const char* label;
        juce::String value;
        juce::Colour colour;
    };

    const auto thdColour = figureColour (result.thdDb);
    const auto thdNColour = figureColour (result.thdPlusNDb);

    const Figure figures[] =
    {
        { "THD",   result.valid ? dsp::DistortionAnalyser::percentToString (result.thdPercent)
                                : juce::String ("--"), thdColour },
        { "THD dB", result.valid ? dsp::DistortionAnalyser::levelToString (result.thdDb)
                                 : juce::String ("--"), thdColour },
        { "THD+N", result.valid ? dsp::DistortionAnalyser::percentToString (result.thdPlusNPercent)
                                : juce::String ("--"), thdNColour },
        { "SINAD", result.valid ? dsp::DistortionAnalyser::levelToString (result.sinadDb)
                                : juce::String ("--"), thdNColour },
        { "Fundamental", result.valid ? juce::String (result.fundamentalHz, 1) + " Hz"
                                      : juce::String ("--"), mutedColour },
        { "Noise", result.valid ? dsp::DistortionAnalyser::levelToString (result.noiseLevelDb)
                                : juce::String ("--"), mutedColour }
    };

    auto cellWidth = area.getWidth() / 3;
    auto rowHeight = juce::jmax (52, area.getHeight() / 2);

    for (int i = 0; i < 6; ++i)
    {
        // Placed by hand rather than by a row helper, so the two rows share one grid without
        // the display depending on a rectangle layout that is not available here.
        auto cell = juce::Rectangle<int> (
            area.getX() + cellWidth * (i % 3), area.getY() + rowHeight * (i / 3),
            cellWidth - 12, rowHeight - 10);

        g.setColour (panelColour);
        g.fillRoundedRectangle (cell.toFloat(), 4.0f);

        g.setColour (mutedColour);
        g.setFont (juce::Font (12.0f));
        g.drawText (figures[i].label, cell.removeFromTop (18), juce::Justification::centredLeft,
                    false);

        g.setColour (figures[i].colour);
        g.setFont (juce::Font (19.0f, juce::Font::bold));
        g.drawText (figures[i].value, cell, juce::Justification::centredLeft, false);
    }
}

void DistortionDisplay::drawTable (juce::Graphics& g, const juce::Rectangle<int>& area) const
{
    int x = area.getX();
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (mutedColour);

    for (const auto& column : columns)
    {
        g.drawText (column.title, x, area.getY(), column.width, 20,
                    juce::Justification::centredLeft, false);
        x += column.width;
    }

    if (! result.valid)
    {
        g.setColour (mutedColour);
        g.setFont (juce::Font (14.0f));
        g.drawText ("Belum ada pengukuran", area.withTrimmedTop (24), juce::Justification::centred,
                    false);
        return;
    }

    g.setFont (juce::Font (13.0f));

    auto y = area.getY() + 22;

    for (const auto& harmonic : result.harmonics)
    {
        // A harmonic whose order puts it past Nyquist has no line in the spectrum at all, and
        // showing a row of zeros for it would read as a measurement rather than as an absence.
        if (! harmonic.valid)
            continue;

        if (y + rowHeight > area.getBottom())
            break;

        const auto isFundamental = harmonic.order == 1;
        const auto relative = harmonic.relativeDb;

        g.setColour (isFundamental ? fundamentalColour
                                   : (relative < -60.0f ? mutedColour : textColour));

        const juce::String cells[5] =
        {
            juce::String ("H") + juce::String (harmonic.order),
            juce::String (harmonic.frequency, 1) + " Hz",
            juce::String (harmonic.amplitude, 5),
            juce::String (relative, 1),
            juce::String (harmonic.levelDb, 1) + " dB"
        };

        x = area.getX();

        for (int c = 0; c < 5; ++c)
        {
            g.drawText (cells[(size_t) c], x, y, columns[c].width, rowHeight,
                        juce::Justification::centredLeft, false);
            x += columns[c].width;
        }

        // A bar behind each row, so the fall through the harmonic series can be read as a shape
        // rather than read as eight separate numbers.
        if (! isFundamental && relative < 0.0f && relative > -80.0f)
        {
            const auto width = juce::jlimit (0.0f, 160.0f, (relative + 80.0f) * 2.0f);
            g.setColour (fundamentalColour.withAlpha (0.25f));
            g.fillRect ((float) x, (float) (y + 4), width, (float) (rowHeight - 8));
        }

        y += rowHeight;
    }
}

void DistortionDisplay::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto area = getLocalBounds().reduced (14, 14);

    drawFigures (g, area.removeFromTop (juce::jmin (140, area.getHeight() / 3)));
    area.removeFromTop (10);

    g.setColour (panelColour);
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    drawTable (g, area.reduced (12, 8));
}

void DistortionDisplay::resized()
{
    // Everything is laid out from the bounds in paint, so there is nothing to do here. Kept as
    // an override because the base class declares it pure and a display that silently does not
    // resize is worse than one that says it has nothing to do.
}
