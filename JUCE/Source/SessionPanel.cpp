#include "SessionPanel.h"

namespace
{
    static const juce::Colour backgroundColour (0xff141a21);
    static const juce::Colour panelColour (0xff1d252e);
    static const juce::Colour textColour (0xffe6edf3);
    static const juce::Colour mutedColour (0xff8b98a5);
    static const juce::Colour referenceColour (0xff8b98a5);
    static const juce::Colour measurementColour (0xff5aa9e6);
    static const juce::Colour differenceColour (0xffd07a3c);
}

SessionPanel::SessionPanel (AudioEngine& engineToUse) : engine (engineToUse)
{
    lastFolder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                     .getChildFile ("OpenSmaartLab");

    const auto kindNames = TraceStore::getKindNames();

    for (int i = 0; i < kindNames.size(); ++i)
        traceSelector.addItem (kindNames[i], i + 1);

    traceSelector.setSelectedId ((int) TraceStore::Kind::Measurement + 1);
    traceSelector.onChange = [this] { refresh(); };

    const auto sources = TraceStore::getSourceNames();

    for (int i = 0; i < sources.size(); ++i)
        sourceSelector.addItem (sources[i], i + 1);

    sourceSelector.setSelectedId ((int) TraceStore::Source::Spectrum + 1);
    sourceSelector.onChange = [this] { refresh(); };

    const auto kinds = MeasurementExporter::getKindNames();

    for (int i = 0; i < kinds.size(); ++i)
        exportKindSelector.addItem (kinds[i], i + 1);

    exportKindSelector.setSelectedId ((int) MeasurementExporter::Kind::Fft + 1);

    for (auto* component : { static_cast<juce::Component*> (&freezeButton),
                             static_cast<juce::Component*> (&averageButton),
                             static_cast<juce::Component*> (&differenceButton),
                             static_cast<juce::Component*> (&compareButton),
                             static_cast<juce::Component*> (&clearButton),
                             static_cast<juce::Component*> (&saveSessionButton),
                             static_cast<juce::Component*> (&loadSessionButton),
                             static_cast<juce::Component*> (&exportButton),
                             static_cast<juce::Component*> (&exportTableButton),
                             static_cast<juce::Component*> (&traceSelector),
                             static_cast<juce::Component*> (&sourceSelector),
                             static_cast<juce::Component*> (&exportKindSelector),
                             static_cast<juce::Component*> (&plot) })
        addAndMakeVisible (component);

    traceInfoLabel.setFont (juce::Font (13.5f));
    traceInfoLabel.setColour (juce::Label::textColourId, textColour);

    compareInfoLabel.setFont (juce::Font (13.5f));
    compareInfoLabel.setColour (juce::Label::textColourId, textColour);

    sessionInfoLabel.setFont (juce::Font (13.0f));
    sessionInfoLabel.setColour (juce::Label::textColourId, mutedColour);

    notes.setMultiLine (true, true);
    notes.setTextToShowWhenEmpty ("Catatan sesi: ruangan, posisi mikrofon, kondisi pengukuran",
                                  mutedColour);
    notes.setFont (juce::Font (13.0f));
    notes.setColour (juce::TextEditor::backgroundColourId, panelColour);
    addAndMakeVisible (notes);

    freezeButton.onClick = [this] { freeze(); };
    averageButton.onClick = [this] { average(); };
    differenceButton.onClick = [this] { computeDifference(); };
    compareButton.onClick = [this] { compare(); };
    clearButton.onClick = [this] { clearKind(); };
    saveSessionButton.onClick = [this] { saveSession(); };
    loadSessionButton.onClick = [this] { loadSession(); };
    exportButton.onClick = [this] { exportCurve(); };
    exportTableButton.onClick = [this] { exportTable(); };

    freezeButton.setColour (juce::TextButton::buttonColourId, measurementColour.withAlpha (0.18f));
    differenceButton.setColour (juce::TextButton::buttonColourId, differenceColour.withAlpha (0.18f));

    session.notes.clear();
    syncSettings();
    refresh();
}

SessionPanel::~SessionPanel() = default;

void SessionPanel::syncSettings()
{
    session.sampleRate = engine.getSampleRate();
    session.bufferSize = engine.getBufferSize();
    session.inputDevice = engine.getInputDeviceNames().isEmpty()
                        ? juce::String() : session.inputDevice;
    session.notes = notes.getText();
}

void SessionPanel::captureLive()
{
    // Only the live curves that have something to copy. Called on the message thread from the
    // same place the analysers are read, so what is frozen is what was on screen when the
    // button was pressed and not whatever the next frame produced.
    std::vector<float> left, right;
    engine.getLatestBlock (engine.getBufferSize(), left, right);

    juce::ignoreUnused (left, right);

    notes.setText (session.notes, false);
}

void SessionPanel::freeze()
{
    const auto kind = (TraceStore::Kind) (traceSelector.getSelectedId() - 1);

    if (kind == TraceStore::Kind::Live || kind == TraceStore::Kind::Difference)
    {
        traceInfoLabel.setText ("Live dan Difference tidak bisa di-freeze", juce::dontSendNotification);
        return;
    }

    const auto frozen = traces.freeze (kind);
    traceInfoLabel.setText (frozen ? "Trace beku: " + TraceStore::kindName (kind)
                                   : juce::String ("Tidak ada trace live untuk dibekukan"), juce::dontSendNotification);
    refresh();
}

void SessionPanel::average()
{
    const auto folded = traces.averageInto (TraceStore::Kind::Average);
    traceInfoLabel.setText ("Average sekarang memegang " + juce::String (folded) + " trace", juce::dontSendNotification);
    refresh();
}

void SessionPanel::computeDifference()
{
    const auto kind = (TraceStore::Kind) (traceSelector.getSelectedId() - 1);

    const auto reference = kind == TraceStore::Kind::Measurement
                        ? TraceStore::Kind::Reference : TraceStore::Kind::Memory;

    if (traces.computeDifference (reference, kind))
        traceInfoLabel.setText ("Difference: " + TraceStore::kindName (reference) + " - "
                                + TraceStore::kindName (kind), juce::dontSendNotification);
    else
        traceInfoLabel.setText ("Difference butuh dua trace dengan panjang yang sama", juce::dontSendNotification);

    refresh();
}

void SessionPanel::compare()
{
    const auto sourceForPlot = (TraceStore::Source) (sourceSelector.getSelectedId() - 1);

    plot.reference = traces.get (TraceStore::Kind::Reference, sourceForPlot).y;
    plot.measurement = traces.get (TraceStore::Kind::Measurement, sourceForPlot).y;
    plot.difference = traces.get (TraceStore::Kind::Difference, sourceForPlot).y;

    const auto source = (TraceStore::Source) (sourceSelector.getSelectedId() - 1);

    float rmsDb = 0.0f, peakDb = 0.0f, frequency = 0.0f;

    const auto& reference = traces.get (TraceStore::Kind::Reference, source);
    const auto& measurement = traces.get (TraceStore::Kind::Measurement, source);

    if (! TraceStore::rmsDifference (reference, measurement, rmsDb)
        || ! TraceStore::peakDifference (reference, measurement, peakDb, frequency))
    {
        compareInfoLabel.setText ("Butuh Reference dan Measurement untuk sumber ini", juce::dontSendNotification);
        plot.difference.clear();
        plot.repaint();
        return;
    }

    compareInfoLabel.setText ("Selisih rata-rata " + juce::String (rmsDb, 2) + " dB"
                              + "   |   terbesar " + juce::String (peakDb, 2) + " dB pada "
                              + juce::String (frequency, 1) + " Hz", juce::dontSendNotification);

    plot.repaint();
}

void SessionPanel::clearKind()
{
    traces.clear ((TraceStore::Kind) (traceSelector.getSelectedId() - 1));
    traceInfoLabel.setText ({}, juce::dontSendNotification);
    refresh();
}

void SessionPanel::saveSession()
{
    session.notes = notes.getText();

    auto* chooser = new juce::FileChooser ("Simpan session", lastFolder, "*.osls",
                                           juce::FileBrowserComponent::saveMode
                                             | juce::FileBrowserComponent::canSelectFiles);

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        delete chooser;

        if (chosen == juce::File{})
            return;

        lastFolder = chosen.getParentDirectory();

        juce::String error;

        if (session.save (chosen, &error))
            traceInfoLabel.setText ("Session tersimpan: " + chosen.getFileName(), juce::dontSendNotification);
        else
            traceInfoLabel.setText ("Session gagal disimpan: " + error, juce::dontSendNotification);

        refresh();
    });
}

void SessionPanel::loadSession()
{
    auto* chooser = new juce::FileChooser ("Buka session", lastFolder, "*.osls",
                                           juce::FileBrowserComponent::openMode
                                             | juce::FileBrowserComponent::canSelectFiles);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        delete chooser;

        if (chosen == juce::File{})
            return;

        lastFolder = chosen.getParentDirectory();

        Session loaded;
        juce::String error;

        // Loaded into a separate session first. If the file is refused, the session on screen
        // is left exactly as it was rather than emptied by a failed load.
        if (! loaded.load (chosen, &error))
        {
            traceInfoLabel.setText ("Session gagal dibuka: " + error, juce::dontSendNotification);
            return;
        }

        session.notes = loaded.notes;
        session.inputDevice = loaded.inputDevice;
        session.outputDevice = loaded.outputDevice;
        session.sampleRate = loaded.sampleRate;
        session.bufferSize = loaded.bufferSize;
        session.fftSize = loaded.fftSize;
        session.thirdOctave = loaded.thirdOctave;
        session.calibration = loaded.calibration;

        traces.clearAll();

        for (int kind = 0; kind < TraceStore::numKinds; ++kind)
            for (int source = 0; source < TraceStore::numSources; ++source)
                traces.setStored ((TraceStore::Kind) kind, (TraceStore::Source) source,
                                  loaded.traces.get ((TraceStore::Kind) kind,
                                                      (TraceStore::Source) source));

        traces.setAveragesFolded (TraceStore::Kind::Average,
                                  loaded.traces.getAveragesFolded (TraceStore::Kind::Average));
        notes.setText (session.notes, false);

        traceInfoLabel.setText ("Session dibuka: " + chosen.getFileName(), juce::dontSendNotification);
        refresh();
    });
}

void SessionPanel::exportCurve()
{
    const auto kind = (MeasurementExporter::Kind) (exportKindSelector.getSelectedId() - 1);
    const auto source = (TraceStore::Source) (sourceSelector.getSelectedId() - 1);

    MeasurementExporter::Curve curve;
    curve.columnName = MeasurementExporter::kindName (kind);
    curve.xName = "Frequency";
    curve.xUnit = "Hz";

    switch (kind)
    {
        case MeasurementExporter::Kind::Phase:
            curve.unit = "deg";
            curve.x = traces.get (TraceStore::Kind::Measurement, source).x;
            curve.y = traces.get (TraceStore::Kind::Measurement, source).y;
            break;

        case MeasurementExporter::Kind::Rt60:
        {
            curve.unit = "s";
            curve.xName = "Band";
            curve.xUnit = "";

            const auto& result = session.traces.get (TraceStore::Kind::Measurement, source);

            for (std::size_t i = 0; i < result.x.size(); ++i)
            {
                curve.x.push_back (result.x[i]);
                curve.y.push_back (result.y[i]);
                curve.xLabels.push_back (juce::String (result.x[i], 0));
            }

            break;
        }

        default:
            curve.unit = "dB";
            curve.x = traces.get (TraceStore::Kind::Measurement, source).x;
            curve.y = traces.get (TraceStore::Kind::Measurement, source).y;
            break;
    }

    if (curve.x.empty())
    {
        traceInfoLabel.setText ("Tidak ada trace untuk diekspor", juce::dontSendNotification);
        return;
    }

    auto* chooser = new juce::FileChooser ("Export curve", lastFolder,
                                           "*.csv;*.json;*.svg",
                                           juce::FileBrowserComponent::saveMode
                                             | juce::FileBrowserComponent::canSelectFiles);

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, chooser, curve] (const juce::FileChooser& fc)
    {
        auto chosen = fc.getResult();
        delete chooser;

        if (chosen == juce::File{})
            return;

        lastFolder = chosen.getParentDirectory();

        auto target = chosen;

        if (target.getFileExtension().isEmpty())
            target = target.getChildFile (target.getFileName() + ".csv");

        juce::String error;
        const auto extension = target.getFileExtension().toLowerCase();

        bool ok = false;

        if (extension == ".svg")
            ok = exporter.exportSvg (target, curve, curve.columnName, &error);
        else if (extension == ".json")
            ok = exporter.exportJson (chosen, curve, &error);
        else
            ok = exporter.exportCsv (chosen, curve, &error);

        traceInfoLabel.setText (ok ? "Curve diekspor: " + chosen.getFileName()
                                   : "Export gagal: " + error, juce::dontSendNotification);

        refresh();
    });
}

void SessionPanel::exportTable()
{
    MeasurementExporter::Table table;
    table.title = "OpenSmaartLab trace";
    table.columns = { "Trace", "Source", "Points" };

    for (int kind = 0; kind < TraceStore::numKinds; ++kind)
    {
        for (int source = 0; source < TraceStore::numSources; ++source)
        {
            const auto& trace = traces.get ((TraceStore::Kind) kind, (TraceStore::Source) source);

            if (! trace.valid)
                continue;

            table.rows.push_back ({ TraceStore::kindName ((TraceStore::Kind) kind),
                                   TraceStore::sourceName ((TraceStore::Source) source),
                                   juce::String ((int) trace.size()) });
        }
    }

    if (table.rows.empty())
    {
        traceInfoLabel.setText ("Tidak ada trace untuk diekspor", juce::dontSendNotification);
        return;
    }

    auto* chooser = new juce::FileChooser ("Export tabel", lastFolder, "*.csv;*.json;*.svg",
                                           juce::FileBrowserComponent::saveMode
                                             | juce::FileBrowserComponent::canSelectFiles);

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, chooser, table] (const juce::FileChooser& fc)
    {
        auto chosen = fc.getResult();
        delete chooser;

        if (chosen == juce::File{})
            return;

        lastFolder = chosen.getParentDirectory();

        auto target = chosen;

        if (target.getFileExtension().isEmpty())
            target = target.getChildFile (target.getFileName() + ".csv");

        juce::String error;
        const auto extension = target.getFileExtension().toLowerCase();

        bool ok = false;

        if (extension == ".svg")
            ok = exporter.exportSvg (target, table, &error);
        else if (extension == ".json")
            ok = exporter.exportJson (target, table, &error);
        else
            ok = exporter.exportCsv (target, table, &error);

        traceInfoLabel.setText (ok ? "Tabel diekspor: " + target.getFileName()
                                   : "Export gagal: " + error, juce::dontSendNotification);

        refresh();
    });
}

juce::String SessionPanel::describeTraces() const
{
    juce::String text;

    for (int kind = 0; kind < TraceStore::numKinds; ++kind)
    {
        const auto sources = traces.sourcesFor ((TraceStore::Kind) kind);

        if (sources.empty())
            continue;

        text << TraceStore::kindName ((TraceStore::Kind) kind) << ": ";

        for (int i = 0; i < sources.size(); ++i)
        {
            if (i > 0)
                text << ", ";

            text << TraceStore::sourceName (sources[(size_t) i]);
        }

        text << "   ";
    }

    return text.isEmpty() ? juce::String ("Belum ada trace") : text.trim();
}

juce::String SessionPanel::describeSession() const
{
    return session.describe();
}

void SessionPanel::refresh()
{
    // The trace store is copied across rather than shared, so a session saved from here holds
    // the curves as they are at that moment and not whatever they become afterwards.
    for (int kind = 0; kind < TraceStore::numKinds; ++kind)
        for (int source = 0; source < TraceStore::numSources; ++source)
            session.traces.setStored ((TraceStore::Kind) kind, (TraceStore::Source) source,
                                      traces.get ((TraceStore::Kind) kind, (TraceStore::Source) source));

    session.traces.setAveragesFolded (TraceStore::Kind::Average,
                                      traces.getAveragesFolded (TraceStore::Kind::Average));

    traceInfoLabel.setText (traceInfoLabel.getText().isNotEmpty() ? traceInfoLabel.getText()
                                                                   : describeTraces(), juce::dontSendNotification);
    sessionInfoLabel.setText (describeSession(), juce::dontSendNotification);
    compare();
}

void SessionPanel::CurvePlot::paint(juce::Graphics& g)
{
    g.fillAll (panelColour);

    if (measurement.empty() && reference.empty())
    {
        g.setColour (mutedColour);
        g.setFont (juce::Font (14.0f));
        g.drawText ("Bekukan sebuah trace untuk melihat kurvanya di sini",
                    getLocalBounds().toFloat(), juce::Justification::centred);
        return;
    }

    const auto count = std::max (measurement.size(), reference.size());

    if (count < 2)
        return;

    // The vertical scale comes from the data rather than being fixed, so a curve that moves
    // within a few decibels is still readable. Without a little headroom a flat line draws on
    // the frame and looks like clipping.
    float lowest = 0.0f, highest = -200.0f;
    bool first = true;

    for (const auto* series : { &reference, &measurement, &difference })
    {
        for (const auto value : *series)
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
    }

    if (first)
        return;

    const auto headroom = std::max (1.0f, (highest - lowest) * 0.1f);
    lowest -= headroom;
    highest += headroom;

    const auto span = std::max (1.0e-3f, highest - lowest);
    const auto area = getLocalBounds().toFloat().reduced (14.0f);

    const auto drawSeries = [&g, &area, lowest, highest, span, count] (const std::vector<float>& series,
                                                                      juce::Colour colour)
    {
        if (series.size() < 2)
            return;

        juce::Path path;
        bool started = false;

        for (std::size_t i = 0; i < series.size(); ++i)
        {
            if (! std::isfinite (series[i]))
                continue;

            const auto x = area.getX() + area.getWidth() * (float) i / (float) (count - 1);
            const auto y = area.getY() + area.getHeight()
                                   * juce::jlimit (0.0f, 1.0f, (highest - series[i]) / span);

            if (started)
                path.lineTo (x, y);
            else
            {
                path.startNewSubPath (x, y);
                started = true;
            }
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));
    };

    drawSeries (reference, referenceColour);
    drawSeries (measurement, measurementColour);
    drawSeries (difference, differenceColour);

    g.setFont (juce::Font (11.0f));
    g.setColour (referenceColour);
    g.drawText ("Reference", area.getX(), 4.0f, 90.0f, 16.0f, juce::Justification::centredLeft);
    g.setColour (measurementColour);
    g.drawText ("Measurement", area.getX() + 96.0f, 4.0f, 100.0f, 16.0f,
                juce::Justification::centredLeft);
    g.setColour (differenceColour);
    g.drawText ("Difference", area.getX() + 202.0f, 4.0f, 100.0f, 16.0f,
                juce::Justification::centredLeft);
}

void SessionPanel::paint(juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void SessionPanel::resized()
{
    auto area = getLocalBounds().toFloat().reduced (14.0f);

    auto topRow = area.removeFromTop (30.0f);

    traceSelector.setBounds (topRow.removeFromLeft (140.0f).toNearestInt());
    topRow.removeFromLeft (8.0f);
    sourceSelector.setBounds (topRow.removeFromLeft (150.0f).toNearestInt());

    topRow.removeFromLeft (12.0f);
    freezeButton.setBounds (topRow.removeFromLeft (90.0f).toNearestInt());
    topRow.removeFromLeft (6.0f);
    averageButton.setBounds (topRow.removeFromLeft (90.0f).toNearestInt());
    topRow.removeFromLeft (6.0f);
    differenceButton.setBounds (topRow.removeFromLeft (110.0f).toNearestInt());
    topRow.removeFromLeft (6.0f);
    compareButton.setBounds (topRow.removeFromLeft (90.0f).toNearestInt());
    topRow.removeFromLeft (6.0f);
    clearButton.setBounds (topRow.removeFromLeft (80.0f).toNearestInt());

    area.removeFromTop (8.0f);

    auto sessionRow = area.removeFromTop (30.0f);

    saveSessionButton.setBounds (sessionRow.removeFromLeft (140.0f).toNearestInt());
    sessionRow.removeFromLeft (8.0f);
    loadSessionButton.setBounds (sessionRow.removeFromLeft (140.0f).toNearestInt());

    sessionRow.removeFromLeft (12.0f);
    exportKindSelector.setBounds (sessionRow.removeFromLeft (140.0f).toNearestInt());
    sessionRow.removeFromLeft (8.0f);
    exportButton.setBounds (sessionRow.removeFromLeft (120.0f).toNearestInt());
    sessionRow.removeFromLeft (6.0f);
    exportTableButton.setBounds (sessionRow.removeFromLeft (120.0f).toNearestInt());

    area.removeFromTop (6.0f);
    traceInfoLabel.setBounds (area.removeFromTop (20.0f).toNearestInt());
    compareInfoLabel.setBounds (area.removeFromTop (20.0f).toNearestInt());
    sessionInfoLabel.setBounds (area.removeFromTop (20.0f).toNearestInt());
    area.removeFromTop (6.0f);

    auto lower = area.removeFromBottom (110.0f);
    auto plotColumn = area.removeFromLeft (area.getWidth() * 0.5f);

    plot.setBounds (plotColumn.toNearestInt());
    notes.setBounds (lower.toNearestInt());
}