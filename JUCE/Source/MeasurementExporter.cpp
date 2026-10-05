#include "Session.h"
#include "WavRecorder.h"

namespace
{
    const char* kindNames[] =
    {
        "FFT", "RTA", "Magnitude", "Phase", "Coherence",
        "Impulse", "ETC", "RT60", "THD", "THD+N", "Noise", "SPL"
    };

    const char* kindExtensions[] =
    {
        "csv", "csv", "csv", "csv", "csv",
        "wav", "csv", "csv", "csv", "csv", "csv", "csv"
    };

    /** Enough digits to come back the same number.

        A curve exported to csv and read back has to be the curve that was measured. Six
        decimals is more than any measurement here is meaningful to and fewer than a
        spreadsheet will drop. */
    juce::String numberToText (float value)
    {
        if (! std::isfinite (value))
            return "";

        return juce::String (value, 6);
    }

    /** A csv field, quoted when it holds something that would break the row apart. */
    juce::String csvField (const juce::String& text)
    {
        if (! (text.contains (",") || text.contains ("\"") || text.contains ("\n")))
            return text;

        return "\"" + text.replace ("\"", "\"\"") + "\"";
    }
}

MeasurementExporter::MeasurementExporter() = default;

juce::StringArray MeasurementExporter::getKindNames()
{
    juce::StringArray names;

    for (const auto* name : kindNames)
        names.add (name);

    return names;
}

juce::String MeasurementExporter::kindName (Kind kind)
{
    const auto index = (int) kind;
    return index >= 0 && index < numKinds ? juce::String (kindNames[index]) : juce::String();
}

juce::String MeasurementExporter::extensionFor (Kind kind)
{
    const auto index = (int) kind;
    return index >= 0 && index < numKinds ? juce::String (kindExtensions[index]) : juce::String ("csv");
}

juce::String MeasurementExporter::makeHeader() const
{
    return "OpenSmaartLab measurement export";
}

bool MeasurementExporter::exportCsv (const juce::File& file, const Curve& curve,
                                     juce::String* errorOut) const
{
    if (curve.x.size() != curve.y.size() || curve.x.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no curve to write out";

        return false;
    }

    juce::String text;

    // Everything needed to read the file without anyone having to remember what produced it.
    text << "# " << makeHeader() << "\n";
    text << "# curve: " << curve.columnName << "\n";
    text << "# " << curve.xName << " (" << curve.xUnit << "), " << curve.columnName
         << " (" << curve.unit << ")\n";
    text << "# points: " << curve.x.size() << "\n\n";

    text << csvField (curve.xName + " " + curve.xUnit) << ","
         << csvField (curve.columnName + " " + curve.unit) << "\n";

    for (std::size_t i = 0; i < curve.x.size(); ++i)
    {
        const auto x = i < curve.xLabels.size() ? curve.xLabels[i] : numberToText (curve.x[i]);
        text << csvField (x) << "," << numberToText (curve.y[i]) << "\n";
    }

    if (! file.replaceWithText (text))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}

bool MeasurementExporter::exportJson (const juce::File& file, const Curve& curve,
                                      juce::String* errorOut) const
{
    if (curve.x.size() != curve.y.size() || curve.x.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no curve to write out";

        return false;
    }

    juce::String text;
    text << "{\n";
    text << "  \"generator\": \"" << makeHeader() << "\",\n";
    text << "  \"curve\": \"" << curve.columnName << "\",\n";
    text << "  \"x\": {\"name\": \"" << curve.xName << "\", \"unit\": \"" << curve.xUnit << "\"},\n";
    text << "  \"y\": {\"name\": \"" << curve.columnName << "\", \"unit\": \"" << curve.unit << "\"},\n";
    text << "  \"points\": [\n";

    for (std::size_t i = 0; i < curve.x.size(); ++i)
    {
        text << "    {\"x\": " << numberToText (curve.x[i])
             << ", \"y\": " << numberToText (curve.y[i]);

        if (i < curve.xLabels.size())
            text << ", \"label\": \"" << curve.xLabels[i] << "\"";

        text << "}" << (i + 1 < curve.x.size() ? "," : "") << "\n";
    }

    text << "  ]\n}\n";

    if (! file.replaceWithText (text))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}

bool MeasurementExporter::exportCsv (const juce::File& file, const Table& table,
                                     juce::String* errorOut) const
{
    if (table.columns.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no table to write out";

        return false;
    }

    juce::String text;
    text << "# " << makeHeader() << "\n";

    if (table.title.isNotEmpty())
        text << "# " << table.title << "\n";

    text << "# rows: " << table.rows.size() << "\n\n";

    for (std::size_t column = 0; column < table.columns.size(); ++column)
    {
        const auto unit = column < table.units.size() ? table.units[column] : juce::String();
        text << csvField (table.columns[column] + (unit.isNotEmpty() ? " " + unit : juce::String()));

        if (column + 1 < table.columns.size())
            text << ",";
    }

    text << "\n";

    for (const auto& row : table.rows)
    {
        for (std::size_t column = 0; column < row.size(); ++column)
        {
            text << csvField (row[column]);

            if (column + 1 < row.size())
                text << ",";
        }

        text << "\n";
    }

    if (! file.replaceWithText (text))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}

bool MeasurementExporter::exportJson (const juce::File& file, const Table& table,
                                      juce::String* errorOut) const
{
    if (table.columns.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no table to write out";

        return false;
    }

    juce::String text;
    text << "{\n";
    text << "  \"generator\": \"" << makeHeader() << "\",\n";
    text << "  \"title\": \"" << table.title << "\",\n";
    text << "  \"columns\": [";

    for (std::size_t column = 0; column < table.columns.size(); ++column)
    {
        text << "\"" << table.columns[column] << "\"";

        if (column < table.units.size() && ! table.units[column].isEmpty())
            text << " (" << table.units[column] << ")\"";

        text << (column + 1 < table.columns.size() ? ", " : "");
    }

    text << "],\n  \"rows\": [\n";

    for (std::size_t row = 0; row < table.rows.size(); ++row)
    {
        text << "    [";

        for (std::size_t column = 0; column < table.rows[row].size(); ++column)
            text << "\"" << table.rows[row][column] << "\""
                 << (column + 1 < table.rows[row].size() ? ", " : "");

        text << "]" << (row + 1 < table.rows.size() ? "," : "") << "\n";
    }

    text << "  ]\n}\n";

    if (! file.replaceWithText (text))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}

bool MeasurementExporter::exportWav (const juce::File& file, const std::vector<float>& samples,
                                     double sampleRate, int numChannels,
                                     WavRecorder::BitDepth bitDepth, juce::String* errorOut) const
{
    return exportWav (file, samples,
                      std::vector<float> ((size_t) numChannels > 1 ? samples.size() : 0),
                      sampleRate, bitDepth, errorOut);
}

bool MeasurementExporter::exportWav (const juce::File& file, const std::vector<float>& left,
                                     const std::vector<float>& right, double sampleRate,
                                     WavRecorder::BitDepth bitDepth, juce::String* errorOut) const
{
    if (left.empty() || sampleRate <= 0.0)
    {
        if (errorOut != nullptr)
            *errorOut = "There are no samples to write out";

        return false;
    }

    const auto numChannels = right.empty() ? 1 : 2;

    if (numChannels == 2 && right.size() != left.size())
    {
        if (errorOut != nullptr)
            *errorOut = "The two channels are different lengths";

        return false;
    }

    // Written through the same recorder the application records with, so an exported file is
    // the same kind of file as a recorded one and is opened by the same code.
    WavRecorder recorder;

    if (! recorder.startRecording (file, sampleRate, numChannels, bitDepth, errorOut))
        return false;

    constexpr int blockSize = 8192;
    size_t position = 0;

    while (position < left.size())
    {
        const auto count = (int) std::min<size_t> (blockSize, left.size() - position);

        const float* planes[2] = { left.data() + position, nullptr };

        if (numChannels == 2)
            planes[1] = right.data() + position;

        recorder.pushFromAudioThread (planes, count);
        recorder.pump();
        position += (size_t) count;
    }

    if (! recorder.save())
    {
        if (errorOut != nullptr)
            *errorOut = recorder.getLastError().isNotEmpty()
                      ? recorder.getLastError()
                      : juce::String ("Could not write " + file.getFullPathName());

        return false;
    }

    return true;
}

bool MeasurementExporter::exportSvg (const juce::File& file, const Curve& curve,
                                     const juce::String& title, juce::String* errorOut) const
{
    if (curve.x.size() != curve.y.size() || curve.x.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no curve to draw";

        return false;
    }

    // A fixed page rather than one sized to the data, so two exports of the same measurement
    // come out the same size and can be laid side by side in a report.
    constexpr int width = 1000;
    constexpr int height = 620;
    constexpr int left = 90, right = 30, top = 70, bottom = 70;

    const auto plotWidth = width - left - right;
    const auto plotHeight = height - top - bottom;

    float lowest = 0.0f, highest = 0.0f;
    bool first = true;

    for (const auto value : curve.y)
    {
        if (! std::isfinite (value))
            continue;

        if (first)
        {
            lowest = highest = value;
            first = false;
        }
        else
        {
            lowest = std::min (lowest, value);
            highest = std::max (highest, value);
        }
    }

    if (first)
    {
        if (errorOut != nullptr)
            *errorOut = "That curve holds no finite values to draw";

        return false;
    }

    // Headroom so the curve never touches the frame, and a range that cannot be zero.
    lowest -= std::max (1.0f, (highest - lowest) * 0.08f);
    highest += std::max (1.0f, (highest - lowest) * 0.08f);

    const auto span = std::max (1.0e-6f, highest - lowest);
    const auto xLow = curve.x.front();
    const auto xHigh = curve.x.back();
    const auto xRange = std::max (1.0e-6f, xHigh - xLow);

    juce::String svg;
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
    svg << "  <rect width=\"" << width << "\" height=\"" << height << "\" fill=\"#141a21\"/>\n";
    svg << "  <text x=\"" << left << "\" y=\"34\" fill=\"#e6edf3\" font-family=\"sans-serif\" "
           "font-size=\"20\">" << juce::String (title.isNotEmpty() ? title : curve.columnName)
        << "</text>\n";
    svg << "  <text x=\"" << left << "\" y=\"54\" fill=\"#8b98a5\" font-family=\"sans-serif\" "
           "font-size=\"13\">" << curve.columnName << " (" << curve.unit << ") against "
        << curve.xName << " (" << curve.xUnit << ")</text>\n";

    // Grid and axes, so a printout can be read without the application.
    for (int step = 0; step <= 10; ++step)
    {
        const auto y = top + plotHeight * step / 10;
        const auto db = highest - span * step / 10.0f;

        svg << "  <line x1=\"" << left << "\" y1=\"" << y << "\" x2=\"" << (left + plotWidth)
            << "\" y2=\"" << y << "\" stroke=\"#2a343f\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << (left - 8) << "\" y=\"" << (y + 4) << "\" fill=\"#8b98a5\" "
               "font-family=\"sans-serif\" font-size=\"11\" text-anchor=\"end\">"
            << juce::String ((int) std::round (db)) << "</text>\n";
    }

    for (int step = 0; step <= 5; ++step)
    {
        const auto x = left + plotWidth * step / 5;
        const auto value = xLow + xRange * step / 5.0f;

        svg << "  <text x=\"" << x << "\" y=\"" << (top + plotHeight + 22)
            << "\" fill=\"#8b98a5\" font-family=\"sans-serif\" font-size=\"11\" "
               "text-anchor=\"middle\">"
            << (std::abs (value) >= 1000.0f ? juce::String (value / 1000.0f, 1) + "k"
                                            : juce::String (value, 0))
            << "</text>\n";
    }

    // The curve, as one polyline. Points that are not finite are skipped rather than written
    // as zero, which would draw a dive to the floor that was never in the measurement.
    svg << "  <polyline fill=\"none\" stroke=\"#5aa9e6\" stroke-width=\"1.6\" points=\"";

    for (std::size_t i = 0; i < curve.y.size() && i < curve.x.size(); ++i)
    {
        if (! std::isfinite (curve.y[i]) || ! std::isfinite (curve.x[i]))
            continue;

        const auto x = left + plotWidth * juce::jlimit (0.0f, 1.0f, (curve.x[i] - xLow) / xRange);
        const auto y = top + plotHeight
                           * juce::jlimit (0.0f, 1.0f, (highest - curve.y[i]) / span);

        svg << juce::String (x, 1) << "," << juce::String (y, 1) << " ";
    }

    svg << "\"/>\n</svg>\n";

    if (! file.replaceWithText (svg))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}

bool MeasurementExporter::exportSvg (const juce::File& file, const Table& table,
                                     juce::String* errorOut) const
{
    if (table.columns.empty())
    {
        if (errorOut != nullptr)
            *errorOut = "There is no table to draw";

        return false;
    }

    constexpr int rowHeight = 26;
    constexpr int margin = 30;
    const auto headerHeight = table.title.isNotEmpty() ? rowHeight * 2 : rowHeight;
    const auto height = headerHeight + rowHeight * (int) (table.rows.size() + 1) + margin * 2;

    constexpr int columnWidth = 190;

    auto widest = columnWidth;

    for (const auto& column : table.columns)
        widest = std::max (widest, 40 + (int) column.length() * 8);

    const auto width = margin * 2 + widest * (int) table.columns.size();

    juce::String svg;
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
    svg << "  <rect width=\"" << width << "\" height=\"" << height << "\" fill=\"#141a21\"/>\n";

    int y = margin + rowHeight;

    if (table.title.isNotEmpty())
    {
        svg << "  <text x=\"" << margin << "\" y=\"" << y << "\" fill=\"#e6edf3\" "
               "font-family=\"sans-serif\" font-size=\"18\">" << table.title << "</text>\n";
        y += rowHeight;
    }

    int x = margin;

    for (std::size_t column = 0; column < table.columns.size(); ++column)
    {
        const auto unit = column < table.units.size() ? table.units[column] : juce::String();

        svg << "  <text x=\"" << x << "\" y=\"" << y << "\" fill=\"#8b98a5\" "
               "font-family=\"sans-serif\" font-size=\"13\">"
            << table.columns[column] << (unit.isNotEmpty() ? " (" + unit + ")" : juce::String())
            << "</text>\n";

        x += widest;
    }

    y += 6;
    svg << "  <line x1=\"" << margin << "\" y1=\"" << y << "\" x2=\"" << (width - margin)
        << "\" y2=\"" << y << "\" stroke=\"#2a343f\"/>\n";

    for (const auto& row : table.rows)
    {
        y += rowHeight;
        x = margin;

        for (std::size_t column = 0; column < row.size(); ++column)
        {
            svg << "  <text x=\"" << x << "\" y=\"" << y << "\" fill=\"#e6edf3\" "
                   "font-family=\"sans-serif\" font-size=\"13\">"
                << row[column].replace ("&", "&amp;").replace ("<", "&lt;") << "</text>\n";

            x += widest;
        }
    }

    svg << "</svg>\n";

    if (! file.replaceWithText (svg))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write " + file.getFullPathName();

        return false;
    }

    return true;
}