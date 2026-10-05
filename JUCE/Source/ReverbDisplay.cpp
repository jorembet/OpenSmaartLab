#include "ReverbDisplay.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff383838);
    const juce::Colour gridBoldColour = juce::Colour(0xff555555);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
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

void ReverbDisplay::setWaterfall(const ImpulseResponse::Waterfall& newWaterfall)
{
    juce::ScopedLock lock(dataLock);
    waterfallData = newWaterfall;
    repaint();
}

void ReverbDisplay::setBandRt60(const std::vector<ImpulseResponse::BandRt60>& newBandRt60)
{
    juce::ScopedLock lock(dataLock);
    bandRt60Data = newBandRt60;
    repaint();
}

void ReverbDisplay::setReverbResult(const dsp::ReverbAnalyser::Result& newResult)
{
    juce::ScopedLock lock(dataLock);
    reverbResult = newResult;
    repaint();
}

void ReverbDisplay::clear()
{
    juce::ScopedLock lock(dataLock);

    acoustics = ImpulseResponse::Acoustics();
    waterfallData = ImpulseResponse::Waterfall();
    bandRt60Data.clear();
    reverbResult = {};
    repaint();
}

ReverbDisplay::Regions ReverbDisplay::computeRegions() const
{
    Regions regions;

    auto area = getLocalBounds().toFloat().reduced(14.0f);
    const auto parametersHeight = 196.0f;
    const auto tableHeight = 272.0f;
    const auto gap = 10.0f;

    area.removeFromBottom(parametersHeight);
    area.removeFromBottom(tableHeight + gap);
    regions.reverbTable = getLocalBounds().toFloat().reduced(14.0f)
                              .removeFromBottom(parametersHeight + gap + tableHeight + gap);

    area.removeFromBottom(0.0f);
    const auto plotsHeight = area.getHeight();
    const auto topHeight = std::max(40.0f, plotsHeight * 0.5f - 6.0f);

    auto leftColumn = area.removeFromLeft(area.getWidth() * 0.55f);

    regions.energy = leftColumn.removeFromTop(topHeight);
    regions.decay = leftColumn;

    auto rightColumn = area;
    const auto rightTopHeight = std::max(40.0f, rightColumn.getHeight() * 0.55f);

    regions.waterfall = rightColumn.removeFromTop(rightTopHeight);
    regions.bandRt60 = rightColumn;

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
    drawWaterfall(g, regions.waterfall);
    drawBandRt60(g, regions.bandRt60);
    drawReverbTable(g, regions.reverbTable);
    drawParameters(g, regions.parameters);

    if (reverbResult.bands.empty() && ! acoustics.valid)
    {
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(juce::Font(20.0f));
        g.drawText("Belum ada data reverberasi", getLocalBounds().toFloat(),
                   juce::Justification::centred);
    }
}

void ReverbDisplay::drawPanel(juce::Graphics& g, const juce::Rectangle<float>& area,
                               const juce::String& title, const juce::String& subtitle) const
{
    g.setColour(plotColour);
    g.fillRoundedRectangle(area, 6.0f);

    g.setFont(juce::Font(16.0f));
    g.setColour(textColour);
    g.drawText(title, area.reduced(12.0f).removeFromTop(20), juce::Justification::left);

    if (subtitle.isNotEmpty())
    {
        g.setFont(juce::Font(14.0f));
        g.setColour(mutedColour);
        auto subArea = area.reduced(12.0f);
        subArea.removeFromTop(20.0f);
        g.drawText(subtitle, subArea.removeFromTop(16), juce::Justification::left);
    }
}

void ReverbDisplay::resized()
{
    repaint();
}

void ReverbDisplay::drawEnergy(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Pantulan setelah bunyi berhenti (Impulse Response)",
              "Gelombang biru = sisa suara seiring waktu; puncak = pantulan kuat");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(44.0f).withTrimmedLeft(44.0f);

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
    drawPanel(g, bounds, "Peluruhan Gema Ruangan (Schroeder Decay)",
              "Garis oranye turun = suara meredam; garis ungu = acuan kecepatan meredam");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(44.0f).withTrimmedLeft(44.0f);

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

void ReverbDisplay::drawWaterfall(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Waterfall (CSD)",
              "Tiap baris = spektrum sesaat; warna terang = energi besar");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(44.0f).withTrimmedLeft(8.0f);

    if (area.getWidth() < 20.0f || area.getHeight() < 20.0f || !waterfallData.valid
        || waterfallData.framesDb.empty())
        return;

    const auto rows = (int) waterfallData.framesDb.size();
    const auto cols = (int) waterfallData.framesDb.front().size();
    const auto rowHeight = area.getHeight() / (float) rows;
    const auto colWidth = area.getWidth() / (float) cols;

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const auto level = juce::jlimit(0.0f, 1.0f,
                                            (waterfallData.framesDb[(size_t) r][(size_t) c] + 60.0f) / 60.0f);
            g.setColour(juce::Colour::fromHSV(0.66f - level * 0.66f, 0.85f, 0.15f + level * 0.85f, 1.0f));
            g.fillRect(area.getX() + c * colWidth, area.getY() + r * rowHeight,
                       colWidth + 0.5f, rowHeight + 0.5f);
        }
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(10.0f));
    const auto lastTime = waterfallData.times.empty() ? 0.0f : waterfallData.times.back();
    g.drawText("0 s", area.getX(), area.getY() - 14.0f, 30.0f, 12.0f, juce::Justification::left);
    g.drawText(juce::String(lastTime, 1) + " s", area.getRight() - 34.0f, area.getBottom() + 2.0f,
               34.0f, 12.0f, juce::Justification::right);
}

void ReverbDisplay::drawBandRt60(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "RT60 per Oktaf",
              "Batang tinggi = frekuensi itu bergaung lama; lebar = perlu peredam");

    const auto area = bounds.reduced(12.0f).withTrimmedTop(44.0f).withTrimmedLeft(36.0f);

    if (area.getWidth() < 20.0f || area.getHeight() < 20.0f || bandRt60Data.empty())
        return;

    auto maxRt60 = 0.5f;

    for (const auto& b : bandRt60Data)
        if (b.valid)
            maxRt60 = std::max(maxRt60, b.rt60);

    const auto barWidth = area.getWidth() / (float) bandRt60Data.size();

    g.setColour(gridColour);

    for (int i = 0; i <= 4; ++i)
    {
        const auto y = area.getBottom() - (float) i / 4.0f * area.getHeight();
        g.fillRect(area.getX(), y, area.getWidth(), 1.0f);
        g.setColour(mutedColour);
        g.setFont(juce::Font(10.0f));
        g.drawText(juce::String(maxRt60 * (float) i / 4.0f, 1) + " s", area.getX() - 34.0f,
                   y - 7.0f, 30.0f, 14.0f, juce::Justification::centredRight);
        g.setColour(gridColour);
    }

    for (size_t i = 0; i < bandRt60Data.size(); ++i)
    {
        const auto& b = bandRt60Data[i];

        if (!b.valid)
            continue;

        const auto height = juce::jlimit(0.0f, 1.0f, b.rt60 / maxRt60) * area.getHeight();
        g.setColour(energyColour);
        g.fillRect(area.getX() + i * barWidth + 1.0f, area.getBottom() - height,
                   std::max(1.0f, barWidth - 2.0f), height);

        g.setColour(mutedColour);
        g.setFont(juce::Font(9.0f));
        g.drawText(b.frequency >= 1000.0f ? juce::String(b.frequency / 1000.0f, 0) + "k"
                                          : juce::String((int) b.frequency),
                   area.getX() + i * barWidth, area.getBottom() + 2.0f, barWidth, 12.0f,
                   juce::Justification::centred);
    }
}

void ReverbDisplay::drawReverbTable(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    const auto qualityText = dsp::ReverbAnalyser::qualityToString (reverbResult.quality);

    juce::String subtitle = "EDT / T20 / T30 each fitted over its own range and quoted at 60 dB";

    if (! qualityText.isEmpty())
        subtitle += "   |   broadband: " + qualityText
                  + "   |   range " + juce::String (reverbResult.decayRangeDb, 1) + " dB";

    drawPanel (g, bounds, "Waktu Gema per Frekuensi (EDT / T20 / T30 / RT60)", subtitle);

    auto area = bounds.reduced(12.0f).withTrimmedTop(46.0f);

    if (area.getHeight() < 40.0f || area.getWidth() < 120.0f)
        return;

    // The broadband figure sits at the top of the table as its own row, so the number a room is
    // judged by is visible without having to read it off a plot or work out which band is which.
    struct Row
    {
        juce::String frequency, edt, t20, t30, rt60;
        dsp::ReverbAnalyser::Quality quality;
        bool broadband;
    };

    juce::Array<Row> rows;

    if (reverbResult.bands.empty())
    {
        rows.add ({ "broadband",
                    juce::String (reverbResult.edt, 2),
                    juce::String (reverbResult.t20, 2),
                    juce::String (reverbResult.t30, 2),
                    juce::String (reverbResult.rt60, 2),
                    reverbResult.quality, true });
    }
    else
    {
        rows.add ({ "broadband",
                    juce::String (reverbResult.edt, 2),
                    juce::String (reverbResult.t20, 2),
                    juce::String (reverbResult.t30, 2),
                    juce::String (reverbResult.rt60, 2),
                    reverbResult.quality, true });

        for (const auto& band : reverbResult.bands)
        {
            const auto centre = band.frequency >= 1000.0f
                              ? juce::String (band.frequency / 1000.0f, band.frequency < 2000.0f ? 2 : 1) + "k"
                              : juce::String (band.frequency, 0);

            const auto time = [&band] (float value)
            {
                return band.valid ? juce::String (value, 2) : juce::String ("-");
            };

            rows.add ({ centre, time (band.edt), time (band.t20), time (band.t30),
                        time (band.rt60), band.quality, false });
        }
    }

    const auto headerHeight = 18.0f;
    const auto rowHeight = std::max (12.0f, (area.getHeight() - headerHeight) / (float) rows.size());

    const float fractions[] = { 0.20f, 0.15f, 0.15f, 0.15f, 0.15f, 0.20f };

    const juce::StringArray headers { "Frekuensi", "EDT", "T20", "T30", "RT60", "Status" };

    auto headerArea = area.removeFromTop(headerHeight);
    float x = 0.0f;

    g.setFont (juce::Font (13.0f, juce::Font::bold));

    for (int column = 0; column < headers.size(); ++column)
    {
        const auto width = area.getWidth() * fractions[column];
        g.setColour (mutedColour);
        g.drawText (headers[column],
                    juce::Rectangle<float> (headerArea.getX() + x, headerArea.getY(),
                                            width, headerArea.getHeight()).reduced(2.0f),
                    column == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
        x += width;
    }

    for (int row = 0; row < rows.size(); ++row)
    {
        const auto& entry = rows.getReference (row);
        auto cell = area.removeFromTop(rowHeight);

        if (! (row % 2))
        {
            g.setColour (plotColour.withAlpha (0.35f));
            g.fillRect (cell);
        }

        const auto state = dsp::ReverbAnalyser::qualityToString (entry.quality);

        const auto colour = entry.quality == dsp::ReverbAnalyser::Quality::Valid
                          ? textColour
                          : juce::Colour (0xffd0, 0x7a, 0x3c);

        const juce::StringArray values { entry.frequency, entry.edt, entry.t20,
                                         entry.t30, entry.rt60, state };

        g.setFont (juce::Font (12.5f, entry.broadband ? juce::Font::bold : juce::Font::plain));
        g.setColour (colour);

        x = 0.0f;

        for (int column = 0; column < values.size(); ++column)
        {
            const auto width = cell.getWidth() * fractions[column];
            g.drawText (values[column],
                        juce::Rectangle<float> (cell.getX() + x, cell.getY(),
                                                width, cell.getHeight()).reduced(2.0f),
                        column == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
            x += width;
        }
    }
}

void ReverbDisplay::drawParameters(juce::Graphics& g, const juce::Rectangle<float>& bounds) const
{
    drawPanel(g, bounds, "Parameter Akustik",
              "Angka-angka untuk menilai karakter ruangan, diringkas di bawah ini");

    const auto duration = acoustics.time.empty() ? 0.0f : acoustics.time.back();

    const juce::StringArray entries
    {
        "EDT   " + juce::String(acoustics.edt, 2) + " s|waktu gema mulai meredam",
        "T30   " + juce::String(acoustics.t30, 2) + " s|waktu gema standar ruangan",
        "RT60  " + juce::String(acoustics.rt60, 2) + " s|waktu sampai gema nyaris hilang",
        "C80   " + juce::String(acoustics.c80, 1) + " dB|kejelasan untuk musik",
        "C50   " + juce::String(acoustics.c50, 1) + " dB|kejelasan untuk bicara",
        "D50   " + juce::String(acoustics.d50, 1) + " dB|seberapa jelas kata terdengar",
        "Ts    " + juce::String(acoustics.centreTime * 1000.0f, 1) + " ms|pusat waktu energi suara",
        "r     " + juce::String(acoustics.correlation, 3) + "|kualitas pengukuran (dekat 1 = bagus)",
        "Arrival " + juce::String(acoustics.directArrivalMs, 2) + " ms|jeda suara langsung tiba",
        "Durasi " + juce::String(duration, 2) + " s|panjang rekaman",
        "Sample " + juce::String((int) sampleRate) + " Hz|laju sampling audio"
    };

    // The table stops above the verdict strip, so the last row and the conclusion cannot
    // be printed on top of each other.
    const auto textArea = bounds.reduced(14.0f).withTrimmedTop(44.0f).withTrimmedBottom(24.0f);
    const auto columns = 2;
    const auto rows = 6;
    const auto columnWidth = textArea.getWidth() / (float) columns;
    const auto rowHeight = textArea.getHeight() / (float) rows;

    for (int i = 0; i < entries.size(); ++i)
    {
        const auto column = i / rows;
        const auto row = i % rows;

        const auto cell = juce::Rectangle<float>(textArea.getX() + (float) column * columnWidth,
                                                  textArea.getY() + (float) row * rowHeight,
                                                  columnWidth - 8.0f, rowHeight);

        const auto parts = juce::StringArray::fromTokens (entries[i], "|", "");

        g.setFont(juce::Font(15.0f));
        g.setColour(textColour);
        g.drawText(parts[0], cell.withTrimmedBottom(rowHeight * 0.5f), juce::Justification::centredLeft);

        if (parts.size() > 1)
        {
            g.setFont(juce::Font(13.0f));
            g.setColour(mutedColour);
            g.drawText(parts[1], cell.withTrimmedTop(rowHeight * 0.5f), juce::Justification::centredLeft);
        }
    }

// The verdict gets its own strip under the table. Drawn over the cells it lands on
    // the last row and is unreadable, which is the one line the panel exists to deliver.
    const auto verdictArea = bounds.reduced(14.0f).removeFromBottom(22.0f);

    if (acoustics.valid && acoustics.rt60 > 0.0f)
    {
        juce::String verdict;

        if (acoustics.rt60 < 0.3f)
            verdict = "Ruangan terasa sangat kering/mati - gema hampir tidak ada";
        else if (acoustics.rt60 < 0.8f)
            verdict = "Ruangan terasa cukup kering - cocok untuk bicara";
        else if (acoustics.rt60 < 1.5f)
            verdict = "Ruangan terasa hidup sedang - cocok untuk musik kecil";
        else if (acoustics.rt60 < 2.5f)
            verdict = "Ruangan terasa bergema - seperti aula";
        else
            verdict = "Ruangan sangat bergema - perlu peredam";

        g.setFont(juce::Font(16.0f).boldened());
        g.setColour(decayColour);

        // Right aligned, so it reads as a conclusion on the edge of the panel rather than
        // as another entry in the table it sits under.
        g.drawText(verdict, verdictArea, juce::Justification::centredRight);
    }
}
