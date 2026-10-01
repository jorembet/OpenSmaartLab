#include "ReverbDisplay.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff111318);
    const juce::Colour panelColour = juce::Colour(0xff1b1f27);
    const juce::Colour gridColour = juce::Colour(0xff232833);
    const juce::Colour gridBoldColour = juce::Colour(0xff39404f);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff8892a0);
    const juce::Colour decayColour = juce::Colour(0xffffb74d);
    const juce::Colour energyColour = juce::Colour(0xff4fc3f7);
    const juce::Colour fitColour = juce::Colour(0xffee82ee);

    constexpr float plotTopDb = 0.0f;
    constexpr float plotBottomDb = -60.0f;

    float levelToFraction(float levelDb)
    {
        return juce::jlimit(0.0f, 1.0f, (levelDb - plotBottomDb) / (plotTopDb - plotBottomDb));
    }

    float fractionToLevel(float fraction)
    {
        return plotBottomDb + fraction * (plotTopDb - plotBottomDb);
    }
}

ReverbDisplay::ReverbDisplay() = default;
ReverbDisplay::~ReverbDisplay() = default;

void ReverbDisplay::setAcoustics(const ImpulseResponse::Acoustics& newAcoustics,
                                 float newSampleRate)
{
    juce::ScopedLock lock(dataLock);

    acoustics = newAcoustics;
    sampleRate = std::max(1000.0f, newSampleRate);

    repaint();
}

void ReverbDisplay::clear()
{
    juce::ScopedLock lock(dataLock);

    acoustics = ImpulseResponse::Acoustics();
    repaint();
}

ReverbDisplay::Regions ReverbDisplay::computeRegions() const
{
    Regions regions;

    auto area = getLocalBounds().toFloat().reduced(14.0f);
    const auto parametersHeight = 140.0f;

    area.removeFromBottom(parametersHeight);
    const auto plotsHeight = area.getHeight();
    const auto topHeight = std::max(40.0f, plotsHeight * 0.5f - 6.0f);

    regions.energy = area.removeFromTop(topHeight);
    regions.decay = area;
    regions.parameters = getLocalBounds().toFloat().reduced(14.0f).removeFromBottom(parametersHeight);

    return regions;
}

void ReverbDisplay::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    juce::ScopedLock lock(dataLock);

    const auto regions = computeRegions();

    drawEnergy(g, regions.energy);
    drawDecay(g, regions.decay);
    drawParameters(g, regions.parameters);

    if (!acoustics.valid)
    {
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(juce::Font(20.0f));
        g.drawText("Belum ada data reverberasi", getLocalBounds().toFloat(),
                   juce::Justification::centred);
    }
}

void ReverbDisplay::drawPanel(juce::Graphics& g, const juce::Rectangle<float>& area,
                              const juce::String& title) const
{
    g.setColour(panelColour);
    g.fillRoundedRectangle(area, 6.0f);

    g.setFont(juce::Font(14.0f));
    g.setColour(mutedColour);
    g.drawText(title, area.reduced(12.0f).removeFromTop(20), juce::Justification::left);
}

void ReverbDisplay::resized()
{
    repaint();
}

void ReverbDisplay::drawEnergy(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Impulse Response (Energy Time Curve)");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(26.0f).withTrimmedLeft(44.0f);

    if (area.getWidth() < 20.0f || area.getHeight() < 20.0f)
        return;

    g.setColour(gridColour);

    for (int db = 0; db >= -60; db -= 10)
    {
        const auto y = area.getBottom() - levelToFraction((float) db) * area.getHeight();
        g.fillRect(area.getX(), y, area.getWidth(), 1.0f);

        if (db % 20 == 0)
        {
            g.setColour(mutedColour);
            g.setFont(juce::Font(11.0f));
            g.drawText(juce::String(db), area.getX() - 40.0f, y - 8.0f, 36.0f, 16.0f,
                       juce::Justification::centredRight);
        }
    }

    if (!acoustics.valid || acoustics.energyDb.size() < 2)
        return;

    const auto maxTime = std::max(acoustics.time.back(), 1.0e-6f);
    juce::Path path;

    for (size_t i = 0; i < acoustics.energyDb.size(); ++i)
    {
        const auto x = area.getX() + acoustics.time[i] / maxTime * area.getWidth();
        const auto y = area.getBottom() - levelToFraction(acoustics.energyDb[i]) * area.getHeight();

        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(energyColour);
    g.strokePath(path, juce::PathStrokeType(1.0f));

    const auto seconds = (int) std::ceil(maxTime);

    for (int step = 0; step <= seconds; ++step)
    {
        const auto x = area.getX() + (float) step / maxTime * area.getWidth();

        g.setColour(step % 5 == 0 ? gridBoldColour : gridColour);
        g.fillRect(x, area.getY(), 1.0f, area.getHeight());

        g.setColour(mutedColour);
        g.setFont(juce::Font(11.0f));
        g.drawText(juce::String(step) + "s", x - 16.0f, area.getBottom() + 2.0f, 32.0f, 14.0f,
                   juce::Justification::centred);
    }
}

void ReverbDisplay::drawDecay(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Schroeder Decay dengan garis fit T30");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(26.0f).withTrimmedLeft(44.0f);

    if (area.getWidth() < 20.0f || area.getHeight() < 20.0f)
        return;

    g.setColour(gridColour);

    for (int db = 0; db >= -60; db -= 10)
    {
        const auto y = area.getBottom() - levelToFraction((float) db) * area.getHeight();
        g.fillRect(area.getX(), y, area.getWidth(), 1.0f);

        if (db % 20 == 0)
        {
            g.setColour(mutedColour);
            g.setFont(juce::Font(11.0f));
            g.drawText(juce::String(db), area.getX() - 40.0f, y - 8.0f, 36.0f, 16.0f,
                       juce::Justification::centredRight);
        }
    }

    if (!acoustics.valid || acoustics.decayDb.size() < 2)
        return;

    const auto maxTime = std::max(acoustics.time.back(), 1.0e-6f);

    if (acoustics.rt60 > 0.0f)
    {
        const auto slope = -60.0f / acoustics.rt60;
        juce::Path fit;
        fit.startNewSubPath(area.getX(), area.getY());
        fit.lineTo(area.getRight(), area.getY() + slope * maxTime / 60.0f * area.getHeight());

        g.setColour(fitColour.withAlpha(0.7f));
        g.strokePath(fit, juce::PathStrokeType(2.0f));
    }

    juce::Path path;

    for (size_t i = 0; i < acoustics.decayDb.size(); ++i)
    {
        const auto x = area.getX() + acoustics.time[i] / maxTime * area.getWidth();
        const auto y = area.getBottom() - levelToFraction(acoustics.decayDb[i]) * area.getHeight();

        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(decayColour);
    g.strokePath(path, juce::PathStrokeType(1.5f));

    const auto seconds = (int) std::ceil(maxTime);

    for (int step = 0; step <= seconds; ++step)
    {
        const auto x = area.getX() + (float) step / maxTime * area.getWidth();

        g.setColour(step % 5 == 0 ? gridBoldColour : gridColour);
        g.fillRect(x, area.getY(), 1.0f, area.getHeight());

        g.setColour(mutedColour);
        g.setFont(juce::Font(11.0f));
        g.drawText(juce::String(step) + "s", x - 16.0f, area.getBottom() + 2.0f, 32.0f, 14.0f,
                   juce::Justification::centred);
    }
}

void ReverbDisplay::drawParameters(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Parameter Akustik (broadband)");

    const auto duration = acoustics.time.empty() ? 0.0f : acoustics.time.back();

    const juce::StringArray entries
    {
        "EDT      : " + juce::String(acoustics.edt, 2) + " s",
        "T20      : " + juce::String(acoustics.t20, 2) + " s",
        "T30      : " + juce::String(acoustics.t30, 2) + " s",
        "RT60     : " + juce::String(acoustics.rt60, 2) + " s",
        "C50      : " + juce::String(acoustics.c50, 1) + " dB",
        "C80      : " + juce::String(acoustics.c80, 1) + " dB",
        "D50      : " + juce::String(acoustics.d50, 1) + " dB",
        "Ts       : " + juce::String(acoustics.centreTime * 1000.0f, 1) + " ms",
        "r        : " + juce::String(acoustics.correlation, 3),
        "Arrival  : " + juce::String(acoustics.directArrivalMs, 2) + " ms",
        "Durasi   : " + juce::String(duration, 2) + " s",
        "Sample   : " + juce::String((int) sampleRate) + " Hz"
    };

    const auto textArea = bounds.reduced(14.0f).withTrimmedTop(26.0f);
    const auto columns = 3;
    const auto rows = 4;
    const auto columnWidth = textArea.getWidth() / (float) columns;
    const auto rowHeight = textArea.getHeight() / (float) rows;

    g.setFont(juce::Font(14.0f));

    for (int i = 0; i < entries.size(); ++i)
    {
        const auto column = i / rows;
        const auto row = i % rows;

        const auto cell = juce::Rectangle<float>(textArea.getX() + (float) column * columnWidth,
                                                 textArea.getY() + (float) row * rowHeight,
                                                 columnWidth - 8.0f, rowHeight);

        g.setColour(column == 0 ? textColour : mutedColour);
        g.drawText(entries[i], cell, juce::Justification::centredLeft);
    }
}
