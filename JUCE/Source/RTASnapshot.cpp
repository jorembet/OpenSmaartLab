#include "RTASnapshot.h"

bool RTASnapshot::isUsable() const
{
    if (frequency.empty() || traces.empty())
        return false;

    // The draw path indexes traces by the frequency axis, so a mismatch would
    // read past the end of a vector.
    for (const auto& trace : traces)
        if (trace.values.size() != frequency.size())
            return false;

    for (const auto freq : frequency)
        if (freq <= 0.0f)
            return false;

    return true;
}

juce::String RTASnapshot::toCsv() const
{
    auto text = juce::String();

    text << "# OpenSmaartLab RTA snapshot\n"
         << "name=" << name << "\n"
         << "axis=" << axisLabel << "\n"
         << "calibration=" << calibrationText << "\n"
         << "top_db=" << topDb << "\n"
         << "bottom_db=" << bottomDb << "\n";

    auto header = juce::String ("frequency_hz");

    for (const auto& trace : traces)
        header << "," << trace.name.toLowerCase().replace (" ", "_") << "_db";

    text << header << "\n";

    for (size_t i = 0; i < frequency.size(); ++i)
    {
        auto line = juce::String (frequency[i], 3);

        for (const auto& trace : traces)
            line << "," << (i < trace.values.size() ? juce::String (trace.values[i], 3) : juce::String());

        text << line << "\n";
    }

    return text;
}

bool RTASnapshot::fromCsv(const juce::String& text, RTASnapshot& result)
{
    RTASnapshot loaded;
    auto headerSeen = false;

    for (const auto& rawLine : juce::StringArray::fromLines (text))
    {
        const auto line = rawLine.trim();

        if (line.isEmpty() || line.startsWith ("#"))
            continue;

        if (line.startsWith ("name="))        { loaded.name = line.substring (5).trim(); continue; }
        if (line.startsWith ("axis="))        { loaded.axisLabel = line.substring (5).trim(); continue; }
        if (line.startsWith ("calibration=")) { loaded.calibrationText = line.substring (12).trim(); continue; }
        if (line.startsWith ("top_db="))      { loaded.topDb = line.substring (7).trim().getFloatValue(); continue; }
        if (line.startsWith ("bottom_db="))   { loaded.bottomDb = line.substring (10).trim().getFloatValue(); continue; }

        if (line.startsWith ("frequency_hz"))
        {
            const auto columns = juce::StringArray::fromTokens (line, ",", "\"'");

            if (columns.size() < 2)
                return false;

            loaded.traces.resize (columns.size() - 1);

            for (size_t t = 0; t < loaded.traces.size(); ++t)
            {
                loaded.traces[t].name = columns[t + 1];
                loaded.traces[t].colour = juce::Colour (0xff4fc3f7).withRotatedHue ((float) t * 0.18f);
            }

            headerSeen = true;
            continue;
        }

        if (! headerSeen)
            continue;

        const auto tokens = juce::StringArray::fromTokens (line, ",", "\"'");

        if (tokens.size() < 2)
            continue;

        const auto freq = tokens[0].trim().getFloatValue();

        if (freq <= 0.0f)
            continue;

        loaded.frequency.push_back (freq);

        for (size_t t = 0; t < loaded.traces.size() && t + 1 < tokens.size(); ++t)
            loaded.traces[t].values.push_back (tokens[t + 1].trim().getFloatValue());
    }

    if (! loaded.isUsable())
        return false;

    if (loaded.name.isEmpty())
        loaded.name = "RTA";

    result = std::move (loaded);
    return true;
}

bool RTASnapshot::writeTo(const juce::File& file) const
{
    if (! isUsable())
        return false;

    juce::FileOutputStream stream (file);

    if (! stream.openedOk())
        return false;

    return stream.writeText (toCsv(), false, false, nullptr);
}

bool RTASnapshot::readFrom(const juce::File& file, RTASnapshot& result)
{
    if (! file.existsAsFile())
        return false;

    return fromCsv (file.loadFileAsString(), result);
}