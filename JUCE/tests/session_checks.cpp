/* Checks for traces, sessions and exports.

   The acceptance this file exists for is that a measurement can be taken, saved, the application
   closed, and the measurement picked up again with everything that gave it meaning still
   attached: the device it came from, the settings that shaped it, the calibration that made its
   levels real, and the curves themselves. Every check below is one of the things that would be
   silently lost if that failed. */

#include "TraceStore.h"
#include "Session.h"
#include "Session.h"
#include "OfflineAnalysis.h"
#include "WavRecorder.h"
#include <iostream>

namespace
{
    int failures = 0;
    int checks = 0;

    void report (bool condition, const juce::String& message, const juce::String& detail = {})
    {
        ++checks;

        if (condition)
        {
            std::cout << "PASS: " << message << "\n";
            return;
        }

        ++failures;
        std::cout << "FAIL: " << message << "  [" << detail << "]\n";
    }

    /** The names on one line, for a failure message that has to show what was actually there. */
    juce::String listText (const juce::StringArray& items)
    {
        juce::String text;

        for (int i = 0; i < items.size(); ++i)
        {
            if (i > 0)
                text << ", ";

            text << items[i];
        }

        return text;
    }

    juce::File scratchFile (const juce::String& name)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("opensmaartlab_session_" + name);
    }

    /** A slope falling with frequency, which is what a real response looks like. */
    void slopingCurve (int points, std::vector<float>& x, std::vector<float>& y)
    {
        x.resize ((size_t) points);
        y.resize ((size_t) points);

        for (int i = 0; i < points; ++i)
        {
            x[(size_t) i] = (float) i * 10.0f;
            y[(size_t) i] = -20.0f - (float) i * 0.25f;
        }
    }
}

// Test 1: the six traces exist, and freezing one copies the live curve without disturbing it.
void testTraces()
{
    TraceStore store;

    report (TraceStore::getKindNames().size() == 6, "there must be six traces",
            juce::String (TraceStore::getKindNames().size()));

    const auto names = listText (TraceStore::getKindNames());

    for (const auto& expected : { "Live", "Reference", "Measurement", "Average", "Memory", "Difference" })
        report (names.contains (expected), juce::String ("the list must include ") + expected, names);

    std::vector<float> x, y;
    slopingCurve (64, x, y);

    store.setLive (TraceStore::Source::Spectrum, x, y);
    report (store.has (TraceStore::Kind::Live, TraceStore::Source::Spectrum),
            "a live curve must be held as live");

    report (store.freeze (TraceStore::Kind::Measurement), "freezing a live curve must work");
    report (store.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
            "the frozen curve must be held");

    report (store.get (TraceStore::Kind::Live, TraceStore::Source::Spectrum).y == y,
            "freezing must not disturb the live curve");

    // Live is the one trace that cannot be frozen into, because freezing it into itself is a
    // copy of nothing and would report a measurement that was never taken.
    report (! store.freeze (TraceStore::Kind::Live), "the live trace cannot be frozen");
    report (! store.freeze (TraceStore::Kind::Difference),
            "the difference trace cannot be frozen, it is computed");

    report (store.has (TraceStore::Kind::Measurement, TraceStore::Source::Rta) == false,
            "a source with no live curve must not appear as frozen");

    // A curve of a different length cannot be compared with one already stored, so it is
    // refused rather than silently replacing it.
    std::vector<float> shorter (32, 0.0f);
    store.setLive (TraceStore::Source::Spectrum, shorter, shorter);
    report (store.get (TraceStore::Kind::Live, TraceStore::Source::Spectrum).y.size() == 64,
            "a live curve of a different length must be refused",
            juce::String (store.get (TraceStore::Kind::Live, TraceStore::Source::Spectrum).y.size()));

    std::vector<float> otherX, otherY;
    slopingCurve (64, otherX, otherY);

    for (std::size_t i = 0; i < otherY.size(); ++i)
        otherY[i] += 3.0f;

    store.setLive (TraceStore::Source::Spectrum, otherX, otherY);
    report (store.freeze (TraceStore::Kind::Reference), "a reference must be freezable");

    report (store.computeDifference(),
            "a difference between a reference and a measurement must be computable");

    const auto& difference = store.get (TraceStore::Kind::Difference, TraceStore::Source::Spectrum);

    report (difference.valid, "the computed difference must be held");
    report (std::abs (difference.y[10] - 3.0f) < 1.0e-4f,
            "the difference must be the reference above the measurement",
            juce::String (difference.y[10], 5));

    // Comparing two curves, which is what the compare action asks for.
    float rmsDb = 0.0f, peakDb = 0.0f, peakHz = 0.0f;

    report (TraceStore::rmsDifference (store.get (TraceStore::Kind::Reference, TraceStore::Source::Spectrum),
                                       store.get (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
                                       rmsDb),
            "two curves of the same length must be comparable");
    report (std::abs (rmsDb - 3.0f) < 1.0e-3f, "the rms difference must be the offset",
            juce::String (rmsDb, 5));

    report (TraceStore::peakDifference (store.get (TraceStore::Kind::Reference, TraceStore::Source::Spectrum),
                                        store.get (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
                                        peakDb, peakHz),
            "the peak difference must be findable");
    report (std::abs (peakDb - 3.0f) < 1.0e-3f, "the peak difference must be the offset",
            juce::String (peakDb, 5));

    // Curves of different lengths cannot be compared bin for bin, and must say so.
    {
        TraceStore::Trace shortTrace;
        shortTrace.x.assign (8, 1.0f);
        shortTrace.y.assign (8, 1.0f);
        shortTrace.valid = true;

        report (! TraceStore::rmsDifference (store.get (TraceStore::Kind::Reference,
                                                        TraceStore::Source::Spectrum),
                                             shortTrace, rmsDb),
                "curves of different lengths must not be compared");
    }

    store.clear (TraceStore::Kind::Measurement);
    report (! store.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
            "clearing a trace must empty it");

    store.clearAll();
    report (store.sourcesFor (TraceStore::Kind::Reference).empty(),
            "clearing everything must empty everything");
}

// Test 2: the average must be a running mean of what was folded into it, not the last curve.
void testAveraging()
{
    TraceStore store;

    std::vector<float> x(16), y(16);

    for (int i = 0; i < 16; ++i)
    {
        x[(size_t) i] = (float) i;
        y[(size_t) i] = 0.0f;
    }

    store.setLive (TraceStore::Source::Spectrum, x, y);
    report (store.averageInto (TraceStore::Kind::Average) == 1, "the first fold must count as one",
            juce::String (store.getAveragesFolded (TraceStore::Kind::Average)));

    report (std::abs (store.get (TraceStore::Kind::Average, TraceStore::Source::Spectrum).y[0]) < 1.0e-5f,
            "one curve folded in must average to itself");

    // A second curve at ten decibels: the mean of zero and ten is five, not ten.
    for (auto& value : y)
        value = 10.0f;

    store.setLive (TraceStore::Source::Spectrum, x, y);
    report (store.averageInto (TraceStore::Kind::Average) == 2, "the second fold must count as two");

    const auto averaged = store.get (TraceStore::Kind::Average, TraceStore::Source::Spectrum).y[0];
    report (std::abs (averaged - 5.0f) < 1.0e-4f,
            "the average must be the mean of every curve folded in, not the last one",
            juce::String (averaged, 5));

    // A third at twenty: the mean of zero, ten and twenty is ten.
    for (auto& value : y)
        value = 20.0f;

    store.setLive (TraceStore::Source::Spectrum, x, y);
    store.averageInto (TraceStore::Kind::Average);

    const auto averagedThree = store.get (TraceStore::Kind::Average, TraceStore::Source::Spectrum).y[0];
    report (std::abs (averagedThree - 10.0f) < 1.0e-4f,
            "the running mean must carry every curve folded into it",
            juce::String (averagedThree, 5));

    report (! store.averageInto (TraceStore::Kind::Measurement),
            "only the average trace can be averaged into");

    store.resetAverage();
    report (store.getAveragesFolded (TraceStore::Kind::Average) == 0,
            "resetting the average must clear its count");
    report (! store.has (TraceStore::Kind::Average, TraceStore::Source::Spectrum),
            "resetting the average must clear its curve");
}

// Test 3: memory must survive a clear, because that is the whole point of it.
void testMemorySurvives()
{
    TraceStore store;

    std::vector<float> x, y;
    slopingCurve (32, x, y);

    store.setLive (TraceStore::Source::Rta, x, y);
    store.freeze (TraceStore::Kind::Memory);

    for (int i = 0; i < 16; ++i)
        y[(size_t) i] = 40.0f;

    store.setLive (TraceStore::Source::Rta, x, y);
    store.freeze (TraceStore::Kind::Measurement);

    report (store.has (TraceStore::Kind::Memory, TraceStore::Source::Rta),
            "memory must be held");

    store.clearAll();

    report (! store.has (TraceStore::Kind::Memory, TraceStore::Source::Rta),
            "clearing everything does clear memory, which is why a session is the way to keep it");

    // Built again after the clear, because freezing an empty live trace would leave memory empty
    // and the comparison below would have nothing to subtract.
    store.setLive (TraceStore::Source::Rta, x, y);
    store.freeze (TraceStore::Kind::Memory);

    for (int i = 0; i < 32; ++i)
        y[(size_t) i] = 40.0f;

    store.setLive (TraceStore::Source::Rta, x, y);
    store.freeze (TraceStore::Kind::Measurement);

    report (store.has (TraceStore::Kind::Memory, TraceStore::Source::Rta),
            "memory must be freezable again after a clear");

    // The difference of a curve held in memory against a fresh reading is the comparison a
    // session exists for: has this changed since last time.
    report (store.computeDifference (TraceStore::Kind::Memory, TraceStore::Kind::Measurement),
            "memory and measurement must be comparable");

    report (store.has (TraceStore::Kind::Difference, TraceStore::Source::Rta),
            "comparing memory against a measurement must produce a difference");
}

// Test 4: a session must survive being written, closed and read back, with every part of it.
void testSessionRoundTrip()
{
    auto file = scratchFile ("roundtrip.osls");
    file.deleteFile();

    Session saved;
    saved.inputDevice = "Universal Audio API";
    saved.outputDevice = "Built-in Output";
    saved.outputPulseSink = "system";
    saved.measurementChannel = 1;
    saved.referenceChannel = 0;
    saved.sampleRate = 96000.0;
    saved.bufferSize = 256;
    saved.fftSize = 32768;
    saved.averaging = 8;
    saved.averagingSeconds = 2.5f;
    saved.window = 4;
    saved.overlapPercent = 75.0f;
    saved.thirdOctave = true;
    saved.rtaLowHz = 25.0f;
    saved.rtaHighHz = 16000.0f;
    saved.automaticDelay = false;
    saved.delayCompensation = false;
    saved.temperatureC = 27.5f;
    saved.notes = "Room 2, floor 3, mic on a stand at 1.2 m.";

    // Kept aside because the session below is emptied to prove that everything after this comes
    // from the file. Comparing against the session that was saved would compare it against
    // nothing.
    const auto expectedNotes = saved.notes;

    std::vector<float> savedDifference;

    saved.calibration = MicrophoneCalibration::daytonImm6c();
    saved.calibration.calibrateAgainst (94.0f, -12.0f);
    saved.calibration.setCalibrationDate ("2026-02-11");

    const auto expectedSensitivity = saved.calibration.getSensitivityDb();

    std::vector<float> x, y;
    slopingCurve (48, x, y);

    saved.traces.setLive (TraceStore::Source::Spectrum, x, y);
    saved.traces.freeze (TraceStore::Kind::Measurement);
    saved.traces.freeze (TraceStore::Kind::Reference);

    for (std::size_t i = 0; i < y.size(); ++i)
        y[i] += 2.5f;

    saved.traces.setLive (TraceStore::Source::Spectrum, x, y);
    saved.traces.freeze (TraceStore::Kind::Measurement);
    saved.traces.averageInto (TraceStore::Kind::Average);
    saved.traces.computeDifference();
    savedDifference = saved.traces.get (TraceStore::Kind::Difference,
                                        TraceStore::Source::Spectrum).y;

    report (saved.save (file), "a session must be savable");

    // Everything above is thrown away, which is what closing the application does. What comes
    // back is read from the file alone.
    saved.clear();

    report (! saved.traces.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
            "clearing the session must clear its traces");
    report (saved.notes.isEmpty(), "clearing the session must clear its notes");

    Session loaded;
    juce::String error;

    report (loaded.load (file, &error), "a saved session must be loadable", error);

    report (loaded.inputDevice == "Universal Audio API", "the input device must survive",
            loaded.inputDevice);
    report (loaded.outputDevice == "Built-in Output", "the output device must survive",
            loaded.outputDevice);
    report (loaded.outputPulseSink == "system", "the output sink must survive");
    report (loaded.sampleRate == 96000.0, "the sample rate must survive",
            juce::String (loaded.sampleRate));
    report (loaded.bufferSize == 256, "the buffer size must survive", juce::String (loaded.bufferSize));
    report (loaded.measurementChannel == 1, "the channel assignment must survive");
    report (loaded.referenceChannel == 0, "the channel assignment must survive both ways");

    report (loaded.fftSize == 32768, "the fft size must survive", juce::String (loaded.fftSize));
    report (loaded.averaging == 8, "the averaging must survive", juce::String (loaded.averaging));
    report (std::abs (loaded.averagingSeconds - 2.5f) < 1.0e-4f, "the averaging time must survive",
            juce::String (loaded.averagingSeconds, 4));
    report (loaded.window == 4, "the window must survive", juce::String (loaded.window));
    report (std::abs (loaded.overlapPercent - 75.0f) < 1.0e-4f, "the overlap must survive",
            juce::String (loaded.overlapPercent, 3));
    report (loaded.thirdOctave, "the resolution must survive");
    report (std::abs (loaded.rtaLowHz - 25.0f) < 1.0e-3f, "the range low must survive",
            juce::String (loaded.rtaLowHz, 2));
    report (std::abs (loaded.rtaHighHz - 16000.0f) < 1.0e-1f, "the range high must survive",
            juce::String (loaded.rtaHighHz, 1));
    report (! loaded.automaticDelay, "the delay setting must survive");
    report (std::abs (loaded.temperatureC - 27.5f) < 1.0e-3f, "the temperature must survive",
            juce::String (loaded.temperatureC, 3));

    report (loaded.calibration.isEnabled(), "the calibration must survive being enabled");
    report (loaded.calibration.hasMeasuredSensitivity(),
            "the calibration sensitivity must survive");
    report (std::abs (loaded.calibration.getSensitivityDb() - expectedSensitivity) < 1.0e-3f,
            "the calibration figure must survive exactly",
            juce::String (loaded.calibration.getSensitivityDb(), 3));
    report (loaded.calibration.getCalibrationDate() == "2026-02-11",
            "the calibration date must survive", loaded.calibration.getCalibrationDate());

    report (loaded.notes == expectedNotes, "the notes must survive", loaded.notes);

    report (loaded.traces.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum),
            "a frozen measurement must survive");
    report (loaded.traces.has (TraceStore::Kind::Reference, TraceStore::Source::Spectrum),
            "a reference must survive");
    report (loaded.traces.has (TraceStore::Kind::Average, TraceStore::Source::Spectrum),
            "an average must survive");
    report (loaded.traces.has (TraceStore::Kind::Difference, TraceStore::Source::Spectrum),
            "a difference must survive");

    report (loaded.traces.getAveragesFolded (TraceStore::Kind::Average) == 1,
            "the number of traces the average holds must survive",
            juce::String (loaded.traces.getAveragesFolded (TraceStore::Kind::Average)));

    // The curve itself, not merely its presence. A trace that came back empty or shortened
    // would look saved and be useless.
    if (loaded.traces.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum))
    {
        const auto& restored = loaded.traces.get (TraceStore::Kind::Measurement,
                                                  TraceStore::Source::Spectrum);

        report (restored.x.size() == 48, "a restored curve must keep its length",
                juce::String (restored.x.size()));

        report (restored.isFinite(), "a restored curve must hold finite values only");

        float worst = 0.0f;

        for (std::size_t i = 0; i < restored.y.size(); ++i)
            worst = std::max (worst, std::abs (restored.y[i] - y[i]));

        report (worst < 1.0e-5f,
                "a restored curve must hold the values that went in, to the last digit",
                juce::String (worst, 9));
    }

    // The session must be usable after loading, not merely present: the difference has to be
    // recomputable and match what it was.
    if (loaded.traces.has (TraceStore::Kind::Reference, TraceStore::Source::Spectrum)
        && loaded.traces.has (TraceStore::Kind::Measurement, TraceStore::Source::Spectrum))
    {
        report (loaded.traces.computeDifference(),
                "a loaded session must still be able to have its difference recomputed");

        const auto& restoredDifference = loaded.traces.get (TraceStore::Kind::Difference,
                                                            TraceStore::Source::Spectrum);

        // Compared against the value the saved session produced, kept aside before it was
        // emptied, and not against the session object itself.
        if (! savedDifference.empty()
            && restoredDifference.y.size() == savedDifference.size())
        {
            float worst = 0.0f;

            for (std::size_t i = 0; i < restoredDifference.y.size(); ++i)
                worst = std::max (worst, std::abs (restoredDifference.y[i] - savedDifference[i]));

            report (worst < 1.0e-4f, "the recomputed difference must match the saved one",
                    juce::String (worst, 9));
        }
    }

    report (loaded.describe().isNotEmpty(), "a loaded session must describe itself");

    file.deleteFile();
}

// Test 5: a session file that is damaged, wrong, or from a newer build must be refused.
void testSessionRefusals()
{
    juce::String error;

    {
        Session session;
        report (! session.load (scratchFile ("nothing_here.osls"), &error),
                "a session file that does not exist must be refused");
        report (error.isNotEmpty(), "refusing a missing file must say why");
    }

    {
        auto file = scratchFile ("garbage.osls");
        file.replaceWithText ("this is not a session, it is just some text");

        Session session;
        report (! session.load (file, &error), "a file that is not a session must be refused",
                error);

        file.deleteFile();
    }

    {
        // A session from a newer format must be refused whole, not read in part. Half reading
        // one produces a session that looks complete and has lost whatever the reader did not
        // know about, which is worse than refusing.
        auto file = scratchFile ("future.osls");

        juce::ValueTree root ("OpenSmaartLabSession");
        root.setProperty ("version", Session::formatVersion + 5, nullptr);
        root.setProperty ("notes", "from the future", nullptr);
        root.createXml()->writeToFile (file, {}, "UTF-8", 0);

        Session session;
        const auto refused = ! session.load (file, &error);

        report (refused, "a session from a newer format must be refused", error);
        report (error.contains ("newer"), "refusing a newer format must say why", error);

        file.deleteFile();
    }

    {
        // Settings that came out of range are put back in range rather than trusted. A
        // transform size that is not a power of two would fail inside the FFT, and a negative
        // buffer size would fail inside the device, so a file that carried one has to be
        // corrected rather than obeyed.
        auto file = scratchFile ("hostile.osls");

        juce::ValueTree root ("OpenSmaartLabSession");
        root.setProperty ("version", Session::formatVersion, nullptr);

        juce::ValueTree settings ("settings");
        settings.setProperty ("fftSize", -5, nullptr);
        settings.setProperty ("bufferSize", -1, nullptr);
        settings.setProperty ("temperatureC", 900.0f, nullptr);
        settings.setProperty ("averagingSeconds", 0.0f, nullptr);
        root.appendChild (settings, nullptr);
        root.createXml()->writeToFile (file, {}, "UTF-8", 0);

        Session session;
        report (session.load (file, &error), "a session with impossible settings must still load",
                error);

        report (session.fftSize >= 256 && session.fftSize <= 65536,
                "an impossible fft size must be put back in range", juce::String (session.fftSize));
        report (session.bufferSize > 0, "an impossible buffer size must be put back in range",
                juce::String (session.bufferSize));
        report (session.temperatureC >= -40.0f && session.temperatureC <= 60.0f,
                "an impossible temperature must be put back in range",
                juce::String (session.temperatureC, 1));
        report (session.averagingSeconds > 0.0f, "an impossible averaging time must be put in range",
                juce::String (session.averagingSeconds, 4));

        file.deleteFile();
    }

    {
        auto file = scratchFile ("unwritable.osls");
        Session session;

        report (! session.save (juce::File ("/this/path/does/not/exist/session.osls"), &error),
                "a session cannot be saved somewhere that does not exist");
        report (error.isNotEmpty(), "refusing to save must say why");

        juce::ignoreUnused (file);
    }
}

// Test 6: every export the panel offers must actually produce a file with the readings in it.
void testExports()
{
    MeasurementExporter exporter;

    report (MeasurementExporter::getKindNames().size() == 12,
            "every measurement must be exportable",
            juce::String (MeasurementExporter::getKindNames().size()));

    const auto names = listText (MeasurementExporter::getKindNames());

    for (const auto& expected : { "FFT", "RTA", "Magnitude", "Phase", "Coherence",
                                  "Impulse", "ETC", "RT60", "THD", "THD+N", "Noise", "SPL" })
        report (names.contains (expected),
                juce::String ("the export list must include ") + expected, names);

    MeasurementExporter::Curve curve;
    curve.columnName = "Magnitude";
    curve.unit = "dB";
    curve.xName = "Frequency";
    curve.xUnit = "Hz";

    std::vector<float> x, y;
    slopingCurve (128, x, y);
    curve.x = x;
    curve.y = y;

    // Csv.
    {
        auto file = scratchFile ("curve.csv");
        file.deleteFile();

        juce::String error;
        report (exporter.exportCsv (file, curve, &error), "a curve must export as csv", error);
        report (file.existsAsFile(), "the csv must exist");

        const auto text = file.loadFileAsString();

        report (text.contains ("Frequency"), "the csv must name its x axis", text.substring (0, 120));
        report (text.contains ("Magnitude"), "the csv must name its column");
        report (text.contains ("dB"), "the csv must state its units");
        report (text.contains ("Hz"), "the csv must state its x units");

        // Every point must be there, or the export is a picture of the measurement rather than
        // the measurement.
        auto lines = juce::StringArray::fromTokens (text, "\n", "");
        int dataLines = 0;

        for (const auto& line : lines)
            if (line.contains (",") && ! line.startsWith ("#"))
                ++dataLines;

        report (dataLines == 129, "the csv must hold every point plus its header",
                juce::String (dataLines));

        report (text.contains (juce::String (y[64], 6)),
                "the csv must hold the values that went in");

        file.deleteFile();
    }

    // Json.
    {
        auto file = scratchFile ("curve.json");
        file.deleteFile();

        report (exporter.exportJson (file, curve), "a curve must export as json");

        const auto text = file.loadFileAsString();
        report (text.startsWith ("{"), "the json must be a json object", text.substring (0, 40));
        report (text.contains ("\"unit\": \"dB\""), "the json must keep the units");
        report (text.contains ("\"points\""), "the json must hold its points");

        file.deleteFile();
    }

    // A curve whose x axis is labels rather than numbers, which is how a reverberation table is
    // written out. The header has to say so or the file reads as a list of meaningless floats.
    {
        MeasurementExporter::Curve bands;
        bands.columnName = "RT60";
        bands.unit = "s";
        bands.xName = "Band";
        bands.xUnit = "";
        bands.x = { 125.0f, 250.0f, 500.0f };
        bands.y = { 0.82f, 0.79f, 0.81f };
        bands.xLabels = { "125", "250", "500" };

        auto file = scratchFile ("bands.csv");
        file.deleteFile();

        report (exporter.exportCsv (file, bands), "a labelled curve must export as csv");

        const auto text = file.loadFileAsString();
        report (text.contains ("Band"), "the csv must name its labelled axis");
        report (text.contains ("125") && text.contains ("0.82"),
                "the csv must hold the band names and values");

        file.deleteFile();
    }

    // Tables.
    {
        MeasurementExporter::Table table;
        table.title = "THD";
        table.columns = { "Order", "Frequency", "Level" };
        table.units = { "", "Hz", "dB" };
        table.rows.push_back ({ "1", "1000.0", "-3.0" });
        table.rows.push_back ({ "2", "2000.0", "-42.1" });

        auto csvFile = scratchFile ("table.csv");
        csvFile.deleteFile();
        report (exporter.exportCsv (csvFile, table), "a table must export as csv");

        const auto csvText = csvFile.loadFileAsString();
        report (csvText.contains ("THD"), "the table csv must carry its title");
        report (csvText.contains ("-42.1"), "the table csv must hold its values");
        report (csvText.contains ("dB"), "the table csv must state its units");
        csvFile.deleteFile();

        auto jsonFile = scratchFile ("table.json");
        jsonFile.deleteFile();
        report (exporter.exportJson (jsonFile, table), "a table must export as json");
        report (jsonFile.loadFileAsString().contains ("\"rows\""),
                "the table json must hold its rows");
        jsonFile.deleteFile();

        auto svgFile = scratchFile ("table.svg");
        svgFile.deleteFile();
        report (exporter.exportSvg (svgFile, table), "a table must export as svg");
        report (svgFile.loadFileAsString().startsWith ("<?xml"),
                "the svg must be an xml document");
        svgFile.deleteFile();
    }

    // Svg of a curve.
    {
        auto file = scratchFile ("curve.svg");
        file.deleteFile();

        juce::String error;
        report (exporter.exportSvg (file, curve, "Magnitude", &error),
                "a curve must export as svg", error);

        const auto text = file.loadFileAsString();
        report (text.contains ("<svg"), "the svg must be an svg");
        report (text.contains ("polyline"), "the svg must actually draw the curve");
        report (text.contains ("Magnitude"), "the svg must be titled with what it shows");

        file.deleteFile();
    }

    // Wav, mono and stereo, at each depth.
    {
        std::vector<float> samples (4800);

        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                               * (double) i / 64.0);

        std::vector<float> second (samples.size());

        for (std::size_t i = 0; i < second.size(); ++i)
            second[i] = samples[i] * 0.5f;

        for (int depth = 0; depth < 3; ++depth)
        {
            const auto bitDepth = (WavRecorder::BitDepth) depth;
            const auto name = WavRecorder::getBitDepthNames()[depth];

            auto file = scratchFile ("export_" + juce::String (depth) + ".wav");
            file.deleteFile();

            juce::String error;
            report (exporter.exportWav (file, samples, 48000.0, 1, bitDepth, &error),
                    ("samples must export as a " + name + " wav").toRawUTF8(), error);
            report (file.existsAsFile(), "the exported wav must exist");

            file.deleteFile();
        }

        auto stereoFile = scratchFile ("export_stereo.wav");
        stereoFile.deleteFile();

        report (exporter.exportWav (stereoFile, samples, second, 48000.0),
                "two channels must export as a stereo wav");

        // Read it back through the offline reader, which is the path a user would take to get
        // an exported file back into the application.
        std::vector<float> readA, readB;
        double readRate = 0.0;
        int readChannels = 0;

        report (OfflineAnalysis::readFile (stereoFile, readA, readB, readRate, readChannels),
                "an exported wav must be readable again");
        report (readChannels == 2, "an exported stereo wav must come back with two channels",
                juce::String (readChannels));
        report (readA.size() == samples.size(), "an exported wav must keep every sample",
                juce::String (readA.size()));

        stereoFile.deleteFile();
    }

    // Refusals. The signal is rebuilt here rather than taken from the block above, which is a
    // scope of its own.
    std::vector<float> refusalSamples (480, 0.1f);

    {
        juce::String error;

        MeasurementExporter::Curve empty;
        report (! exporter.exportCsv (scratchFile ("empty.csv"), empty, &error),
                "an empty curve must be refused");
        report (error.isNotEmpty(), "refusing an empty curve must say why");

        report (! exporter.exportWav (scratchFile ("empty.wav"), {}, 48000.0, 1,
                                      WavRecorder::BitDepth::Float32, &error),
                "an empty signal must be refused as a wav");

        std::vector<float> shorterSignal (240, 0.1f);

        report (! exporter.exportWav (scratchFile ("mismatch.wav"), refusalSamples, shorterSignal,
                                      48000.0),
                "two channels of different lengths must be refused as a wav");
    }

    juce::ignoreUnused (x);
}

int main()
{
    std::cout << "--- traces ---\n";
    testTraces();

    std::cout << "--- averaging ---\n";
    testAveraging();

    std::cout << "--- memory survives ---\n";
    testMemorySurvives();

    std::cout << "--- session round trip ---\n";
    testSessionRoundTrip();

    std::cout << "--- session refusals ---\n";
    testSessionRefusals();

    std::cout << "--- exports ---\n";
    testExports();

    std::cout << "\n" << (checks - failures) << " of " << checks << " checks passed\n";

    if (failures > 0)
        std::cout << failures << " check(s) failed\n";

    return failures == 0 ? 0 : 1;
}