#include "MicrophoneCalibration.h"

namespace
{
    // "Dayton Audio iMM-6c" published figures:
    //   sensitivity 10 mV/Pa into 1 kOhm, i.e. -40 dBV re 1 V/Pa
    //   frequency response 18 Hz - 20 kHz, +/-0.5 dB after factory calibration
    //   self noise 70 dB(A), so the equivalent noise level is 94 - 70 = 24 dB SPL
    //
    // The factory curve is calibrated per serial number, so no response correction
    // is assumed here: loading the unit's own CSV is what flattens the response.
    constexpr float imm6cMvPerPascal = 10.0f;

    float splAtFullScale()
    {
        // 0 dBFS is taken as 1 V. SPL at full scale is 94 dB plus the ratio between
        // 1 V and the mic's rated output for 1 Pa.
        return 94.0f + juce::jmax (0.0f, 20.0f * std::log10 (1000.0f / imm6cMvPerPascal));
    }
}

MicrophoneCalibration::MicrophoneCalibration()
    : sensitivityDb (120.0f)
{
}

MicrophoneCalibration MicrophoneCalibration::daytonImm6c()
{
    MicrophoneCalibration calibration;
    calibration.modelName = "Dayton Audio iMM-6c";
    calibration.sensitivityDb = splAtFullScale();
    calibration.enabled = true;
    return calibration;
}

void MicrophoneCalibration::setCurve(std::vector<CurvePoint> newCurve)
{
    std::sort (newCurve.begin(), newCurve.end(),
               [] (const CurvePoint& a, const CurvePoint& b) { return a.frequency < b.frequency; });

    curve = std::move (newCurve);
    responseDb.clear();
    responseDb.reserve (curve.size());

    for (const auto& point : curve)
        responseDb.push_back (point.db);
}

namespace
{
    // Dayton's download tool is not the only producer of these files, so the loader
    // accepts any separator and any column order, but only once it has confirmed
    // which column actually holds the frequency.
    struct Column
    {
        juce::StringArray header;
        int frequencyColumn = 0;
        int dbColumn = 1;
        bool useHeader = false;
    };

    // Splits one data row. Quotes are removed first because a quoted value such as
    // "\"20\"" would otherwise leave an empty token between the two real columns.
    juce::StringArray tokeniseRow(const juce::String& line)
    {
auto body = line;

        // Quotes are stripped rather than treated as separators, otherwise a quoted
        // value such as "\"20\"" leaves an empty token between the two real columns.
        // juce::String::replace is const and returns a copy, so the result has to be
        // taken back.
        body = body.replace ("\"", "");
        body = body.replace ("'", "");

        juce::StringArray kept;

        for (const auto& token : juce::StringArray::fromTokens (body, ",;\t ", ""))
        {
            const auto trimmed = token.trim();

            if (trimmed.isNotEmpty())
                kept.add (trimmed);
        }

        return kept;
    }

    /** A row is a header only if a text label such as "Frequency" or "dB" appears in it.
        A plain numeric row is data, even when the first value would otherwise look
        like a column index, so "20,0.00" is never mistaken for a header.
    */
    bool looksLikeHeader(const juce::String& line)
    {
        if (line.containsIgnoreCase ("freq") || line.containsIgnoreCase ("response")
             || line.containsIgnoreCase ("sensitiv") || line.containsIgnoreCase ("magnitude"))
            return true;

        return line.toLowerCase().contains ("db");
    }

    Column detectColumns(const juce::StringArray& lines)
    {
        Column column;

        for (const auto& rawLine : lines)
        {
            const auto line = rawLine.trim();

            if (line.isEmpty() || line.startsWith ("#") || ! looksLikeHeader (line))
                continue;

            auto tokens = tokeniseRow (line);

            for (auto& token : tokens)
                token = token.trim();

            if (tokens.size() < 2)
                continue;

            for (int t = 0; t < tokens.size(); ++t)
            {
                const auto lowered = tokens[t].toLowerCase();

                if (lowered.contains ("freq") || lowered.contains ("hz"))
                    column.frequencyColumn = t;

                if (lowered.contains ("db") || lowered.contains ("response")
                     || lowered.contains ("sensitiv") || lowered.contains ("corr"))
                    column.dbColumn = t;
            }

            column.useHeader = true;
            break;
        }

        return column;
    }
}

bool MicrophoneCalibration::parseSensitivityHeader(const juce::String& line,
                                                   float& sensitivityDbOut)
{
    auto body = line;
    body = body.replace ("\"", "");
    body = body.replace ("'", "");
    body = body.trim();

    // Only an explicit header counts. A plain data row such as "20.00<tab>-1.2"
    // also holds a plausible number, so without this check the first data row would
    // be swallowed as a sensitivity.
    const auto lowered = body.toLowerCase();
    const auto isHeader = body.startsWith ("*") || lowered.contains ("sensitiv")
                        || lowered.contains ("dbv") || lowered.contains ("dbfs")
                        || (lowered.contains ("hz") && lowered.contains ("-"));

    if (! isHeader)
        return false;

    const auto tokens = juce::StringArray::fromTokens (body, ",;\t ", "");

    for (const auto& rawToken : tokens)
    {
        auto token = rawToken.trim();

        if (token.isEmpty())
            continue;

        // A token naming the reference frequency ("*1000Hz") is not the sensitivity.
        if (token.toLowerCase().contains ("hz"))
            continue;

        auto cleaned = token;
        cleaned.replace (",", ".");
        cleaned = cleaned.trim();

        // Keep the leading minus: "-38.5" is the sensitivity.
        cleaned = cleaned.trimStart();

        if (! cleaned.containsOnly ("0123456789.-+eE"))
            continue;

        const auto value = cleaned.getFloatValue();

        // A mic sensitivity sits far below full scale, roughly -20 to -60 dB.
        if (std::abs (value) < 5.0f || std::abs (value) > 90.0f)
            continue;

        sensitivityDbOut = 94.0f + std::abs (value);
        return true;
    }

    return false;
}

bool MicrophoneCalibration::loadFile(const juce::File& file)
{
    return file.existsAsFile() && loadCsv (file.loadFileAsString());
}

bool MicrophoneCalibration::loadCsv(const juce::String& text)
{
    // Dayton files are plain text but may use Windows or Unix line endings, and some
    // download steps wrap values in quotes, so the buffer is normalised first.
    auto body = text;

    // juce::String::replace returns a copy, so the result must be taken back.
    body = body.replace ("\r\n", "\n");
    body = body.replace ("\r", "\n");

    const auto lines = juce::StringArray::fromLines (body);
    const auto column = detectColumns (lines);
    auto sensitivityFromFile = 0.0f;
    auto haveSensitivity = false;
    std::vector<CurvePoint> parsed;

    for (const auto& rawLine : lines)
    {
        auto line = rawLine.trim();

        if (line.isEmpty() || line.startsWith ("#"))
            continue;

        // Dayton puts the sensitivity on its own starred line, e.g. "*1000Hz<tab>-38.5".
        // That value is a dBFS/dBV level rather than a response deviation, so it
        // must be kept out of the curve.
        if (! haveSensitivity)
        {
            float candidate = 0.0f;

            if (parseSensitivityHeader (line, candidate))
            {
                sensitivityFromFile = candidate;
                haveSensitivity = true;
                continue;
            }
        }

        if (line.startsWith ("*"))
            continue;

        if (column.useHeader && looksLikeHeader (line))
            continue;

        const auto tokens = tokeniseRow (line);
        const auto needed = std::max (column.frequencyColumn, column.dbColumn) + 1;

        if (tokens.size() < needed)
            continue;

        const auto frequencyToken = tokens[(size_t) column.frequencyColumn].trim();
        const auto dbToken = tokens[(size_t) column.dbColumn].trim();

        // Reject anything that is not plainly a number, so headers and notes
        // cannot be mistaken for calibration points.
        if (! frequencyToken.containsOnly ("0123456789.eE+-")
             || ! dbToken.containsOnly ("0123456789.eE+-"))
            continue;

        const auto frequency = frequencyToken.getFloatValue();
        const auto db = dbToken.getFloatValue();

        if (frequency <= 0.0f || frequency > 100000.0f)
            continue;

        // A correction curve is a small adjustment either way, not a level. Anything
        // beyond this range means the wrong column was read. Both signs are normal:
        // a mic response often dips as well as peaks.
        if (std::abs (db) > 60.0f)
            continue;

        parsed.push_back ({ frequency, db });
    }

    if (parsed.size() < 2)
        return false;

    setCurve (std::move (parsed));

    if (haveSensitivity)
    {
        sensitivityDb = sensitivityFromFile;
        measuredSensitivity = true;
    }

    return true;
}

float MicrophoneCalibration::correctionDb(float frequency) const
{
    if (curve.size() < 2)
        return 0.0f;

    const auto clamped = juce::jlimit (curve.front().frequency, curve.back().frequency, frequency);
    auto next = curve.begin();

    while (next != curve.end() && next->frequency < clamped)
        ++next;

    if (next == curve.begin())
        return next->db;

    if (next == curve.end())
        return curve.back().db;

    const auto previous = std::prev (next);
    const auto span = next->frequency - previous->frequency;

    if (span <= 0.0f)
        return previous->db;

    const auto fraction = (clamped - previous->frequency) / span;
    return previous->db + fraction * (next->db - previous->db);
}

void MicrophoneCalibration::calibrateAgainst (float referenceSplDb, float measuredDbfs,
                                              const juce::String& microphoneName)
{
    if (! std::isfinite (referenceSplDb) || ! std::isfinite (measuredDbfs))
        return;

    calibratorLevelDb = juce::jlimit (60.0f, 160.0f, referenceSplDb);
    calibratorReadingDb = measuredDbfs;

    // The whole correction, expressed the same way toSpl adds it: the difference between what
    // the calibrator was set to and what the chain actually read. Folding it into the
    // sensitivity rather than adding a second term keeps one number doing one job, and means
    // every path that converts to SPL picks the correction up without having to know how it
    // was arrived at.
    sensitivityDb = calibratorLevelDb - calibratorReadingDb - 3.0103f;
    measuredSensitivity = true;
    calibratedAgainstReference = true;
    enabled = true;

    // Stamped only when a name was supplied, so re-running the calibration on the same mic does
    // not wipe the model off it.
    if (microphoneName.isNotEmpty())
        modelName = microphoneName;

    // Stamped in ISO form with no time of day, because a calibration is a day's work and the
    // hour it was finished says nothing about whether it is still good. A fixed format also
    // means the date sorts correctly when profiles are listed.
    calibrationDate = juce::Time::getCurrentTime().toString (false, true, false, false);
}

juce::String MicrophoneCalibration::toJson() const
{
    juce::String text;
    text << "{\n";
    text << "  \"tool\": \"OpenSmaartLab\",\n";
    text << "  \"model\": \"" << modelName.replace ("\"", "'") << "\",\n";
    text << "  \"enabled\": " << (enabled ? "true" : "false") << ",\n";
    text << "  \"calibratedAgainstReference\": " << (calibratedAgainstReference ? "true" : "false") << ",\n";
    text << "  \"sensitivityDb\": " << juce::String (sensitivityDb, 4) << ",\n";
    text << "  \"inputTrimDb\": " << juce::String (inputTrimDb, 4) << ",\n";
    text << "  \"calibratorLevelDb\": " << juce::String (calibratorLevelDb, 2) << ",\n";
    text << "  \"calibratorReadingDb\": " << juce::String (calibratorReadingDb, 4) << ",\n";
    text << "  \"calibrationDate\": \"" << calibrationDate.replace ("\"", "'") << "\",\n";
    text << "  \"curve\": [\n";

    for (size_t i = 0; i < curve.size(); ++i)
    {
        text << "    { \"frequency\": " << juce::String (curve[i].frequency, 4)
             << ", \"db\": " << juce::String (curve[i].db, 4) << " }";

        if (i + 1 < curve.size())
            text << ",";

        text << "\n";
    }

    text << "  ]\n}\n";
    return text;
}

bool MicrophoneCalibration::fromJson (const juce::String& text)
{
    if (text.isEmpty())
        return false;

    const auto json = juce::JSON::parse (text);

    if (! json.isObject())
        return false;

    MicrophoneCalibration loaded;
    loaded.modelName = json["model"].toString();
    loaded.enabled = (bool) json["enabled"];
    loaded.calibratedAgainstReference = (bool) json["calibratedAgainstReference"];
    loaded.sensitivityDb = (float) json["sensitivityDb"];
    loaded.inputTrimDb = (float) json["inputTrimDb"];
    loaded.calibratorLevelDb = (float) (json["calibratorLevelDb"] == juce::var()
                                            ? 94.0 : (double) json["calibratorLevelDb"]);
    loaded.calibratorReadingDb = (float) json["calibratorReadingDb"];
    loaded.calibrationDate = json["calibrationDate"].toString();

    std::vector<CurvePoint> loadedCurve;
    const auto curve = json["curve"];

    if (curve.isArray())
    {
        for (int i = 0; i < curve.size(); ++i)
        {
            const auto point = curve[i];
            loadedCurve.push_back ({ (float) point["frequency"], (float) point["db"] });
        }
    }

    // Only a profile that names itself and carries a sensitivity is accepted. A file that
    // merely parses is not the same as a file that describes a microphone, and accepting the
    // first would let any stray settings file leave the meter reporting dB SPL from nothing.
    if (loaded.modelName.trim().isEmpty())
        return false;

    if (! std::isfinite ((double) loaded.sensitivityDb))
        return false;

    *this = loaded;
    measuredSensitivity = true;
    setCurve (std::move (loadedCurve));
    return true;
}

bool MicrophoneCalibration::saveToFile (const juce::File& file) const
{
    if (file == juce::File())
        return false;

    return file.replaceWithText (toJson());
}

juce::String MicrophoneCalibration::describe() const
{
    if (! hasCalibration())
        return "MIC NOT CALIBRATED";

    juce::String text = "MIC CALIBRATED";

    if (modelName.trim().isNotEmpty() && modelName != "Tanpa kalibrasi")
        text += " - " + modelName;

    text += "  (" + juce::String ((int) calibratorLevelDb) + " dB ref, "
            + juce::String (sensitivityDb, 1) + " dB sens";

    if (calibrationDate.isNotEmpty())
        text += ", " + calibrationDate;

    text += ")";
    return text;
}

float MicrophoneCalibration::toSpl(float dbfs) const
{
    if (! enabled)
        return dbfs;

    // dBFS of a full-scale sine is -3.01 dB, so the digital reading is shifted
    // before being referred to full scale.
    return dbfs + 3.0103f + sensitivityDb + inputTrimDb;
}

void MicrophoneCalibration::apply(std::vector<float>& dbfsTrace,
                                 const std::vector<float>& frequencies) const
{
    if (! enabled)
        return;

    for (size_t i = 0; i < dbfsTrace.size(); ++i)
    {
        const auto frequency = i < frequencies.size() ? frequencies[i] : 0.0f;
        dbfsTrace[i] = toSpl (dbfsTrace[i]) + correctionDb (frequency);
    }
}

juce::String MicrophoneCalibration::toString() const
{
    if (! enabled)
        return "Mic: tidak dikalibrasi (dBFS)";

    auto text = "Mic: " + modelName + " · " + juce::String (sensitivityDb, 1) + " dB SPL @ full scale";

    if (! responseDb.empty())
        text += " · kurva kalibrasi " + juce::String ((int) responseDb.size()) + " titik";
    else
        text += " · kurva respons dari serial number (belum dimuat)";

    if (! juce::isPositiveAndBelow (inputTrimDb, 0.0001f))
        text += " · trim " + juce::String (inputTrimDb, 1) + " dB";

    return text;
}