#include "OfflinePanel.h"
#include <thread>

namespace
{
    static const auto backgroundColour = juce::Colour (0xff141a21);
    static const auto panelColour = juce::Colour (0xff1d252e);
    static const auto textColour = juce::Colour (0xffe6edf3);
    static const auto mutedColour = juce::Colour (0xff8b98a5);
    static const auto accentColour = juce::Colour (0xff5aa9e6);
    static const auto warnColour = juce::Colour (0xffd07a3c);

    /** The largest fft the panel offers.

        Sixty five thousand and thirty six is the largest power of two a length of audio can be
        transformed at, and past sixteen thousand the extra resolution costs time without adding
        anything a measurement can use: a bin narrower than the length of the window buys no
        information about a signal that is not stationary over the whole window. */
    int fftSizeForIndex (int index)
    {
        static const int sizes[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

        index = juce::jlimit (0, (int) (sizeof (sizes) / sizeof (sizes[0])) - 1, index);
        return sizes[index];
    }
}

OfflinePanel::OfflinePanel()
{
    addAndMakeVisible (openButton);
    addAndMakeVisible (analyseButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (fftSelector);
    addAndMakeVisible (resolutionSelector);
    addAndMakeVisible (fileLabel);
    addAndMakeVisible (summaryLabel);
    addAndMakeVisible (reportEditor);
    addAndMakeVisible (spectrumPlot);
    addAndMakeVisible (notes);


    for (int i = 0; i < 7; ++i)
        fftSelector.addItem (juce::String (fftSizeForIndex (i)), i + 1);

    fftSelector.setSelectedId (5); // 16384

    resolutionSelector.addItem ("1/1 Oktaf", 1);
    resolutionSelector.addItem ("1/3 Oktaf", 2);
    resolutionSelector.setSelectedId (1);

    fileLabel.setFont (juce::Font (15.0f, juce::Font::bold));
    fileLabel.setColour (juce::Label::textColourId, textColour);

    summaryLabel.setFont (juce::Font (13.5f));
    summaryLabel.setColour (juce::Label::textColourId, mutedColour);

    reportEditor.setMultiLine (true, true);
    reportEditor.setReadOnly (true);
    reportEditor.setCaretVisible (false);
    reportEditor.setFont (juce::Font (13.0f));
    reportEditor.setTextToShowWhenEmpty ("Hasil analisis akan muncul di sini", mutedColour);
    reportEditor.setColour (juce::TextEditor::backgroundColourId, panelColour);
    reportEditor.setColour (juce::TextEditor::textColourId, textColour);
    reportEditor.applyColourToAllText (textColour, true);

    notes.setMultiLine (true, true);
    notes.setTextToShowWhenEmpty ("Catatan untuk file ini", mutedColour);
    notes.setFont (juce::Font (13.0f));
    notes.setColour (juce::TextEditor::backgroundColourId, panelColour);

    openButton.onClick = [this] { chooseFile(); };
    analyseButton.onClick = [this] { runAnalysis(); };
    clearButton.onClick = [this] { clear(); };

    analyseButton.setEnabled (false);
    refresh();
}

OfflinePanel::~OfflinePanel() = default;

void OfflinePanel::setCalibration (const MicrophoneCalibration& profile)
{
    calibration = profile;
}

void OfflinePanel::setFftSize (int size)
{
    for (int i = 0; i < 7; ++i)
        if (fftSizeForIndex (i) == size)
        {
            fftSelector.setSelectedId (i + 1);
            return;
        }
}

void OfflinePanel::setResolution (bool thirdOctave)
{
    resolutionSelector.setSelectedId (thirdOctave ? 2 : 1);
}

void OfflinePanel::chooseFile()
{
    auto* chooser = new juce::FileChooser ("Buka file WAV", juce::File(),
                                          "*.wav;*.wave;*.aif;*.aiff;*.flac",
                                          juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectFiles);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        delete chooser;

        if (file == juce::File{})
            return;

        // Checked here rather than after the analysis so an unusable file is refused before
        // anything expensive is run over it.
        if (! OfflineAnalysis::isSupportedFile (file))
        {
            currentFile = juce::File();
            fileLabel.setText ("File itu bukan audio yang bisa dibaca", juce::dontSendNotification);
            analyseButton.setEnabled (false);
            return;
        }

        currentFile = file;
        fileLabel.setText (currentFile.getFileName(), juce::dontSendNotification);
        analyseButton.setEnabled (true);
        refresh();
    });
}

void OfflinePanel::runAnalysis()
{
    if (currentFile == juce::File() || busy)
        return;

    busy = true;
    analyseButton.setEnabled (false);
    refresh();

    // Everything that reads a component is read here, on the message thread, and the result
    // handed to the worker as values. Reading a control from the worker would be a race with
    // the message thread painting it, and a component read from two threads is undefined
    // however briefly it happens.
    OfflineAnalysis::Settings settings;
    settings.fftSize = fftSizeForIndex (juce::jmax (0, fftSelector.getSelectedId() - 1));
    settings.resolution = resolutionSelector.getSelectedId() == 2
                        ? dsp::RtaAnalyser::Resolution::ThirdOctave
                        : dsp::RtaAnalyser::Resolution::OneOctave;
    settings.calibration = calibration;
    settings.calibration.setEnabled (true);

    const auto file = currentFile;

    // The worker gets its own analyser. Running it on the panel's member would mean two threads
    // inside one set of FFT buffers if the button were ever pressed twice, and the guard above
    // is a courtesy rather than the thing keeping it safe.
    auto worker = std::make_shared<OfflineAnalysis>();

    // Started on a detached thread and not waited for, because waiting would block the message
    // thread and stop the window repainting, which is the whole reason for going off-thread.
    std::thread ([this, worker, file, settings]
    {
        auto analysed = worker->analyseFile (file, settings);

        juce::MessageManager::getInstance()->callAsync ([this, analysed]
        {
            result = analysed;
            haveResult = analysed.valid;
            busy = false;
            analyseButton.setEnabled (currentFile != juce::File());
            refresh();
        });
    }).detach();
}

void OfflinePanel::clear()
{
    currentFile = juce::File();
    result = OfflineAnalysis::Result();
    haveResult = false;
    reportEditor.clear();
    spectrumPlot.frequency.clear();
    spectrumPlot.magnitudeDb.clear();
    notes.clear();
    fileLabel.setText ("Belum ada file", juce::dontSendNotification);
    analyseButton.setEnabled (false);
    refresh();
}

void OfflinePanel::refresh()
{
    if (! haveResult)
    {
        summaryLabel.setText (busy ? "Menganalisis..." : "Buka sebuah file WAV untuk memulai", juce::dontSendNotification);
        return;
    }

    summaryLabel.setText ("Puncak " + juce::String (result.peakFrequency, 1) + " Hz pada "
                          + juce::String (result.peakMagnitudeDb, 1) + " dB"
                          + "   |   rata-rata " + juce::String (result.averageLevelDb, 1) + " dB"
                          + "   |   noise floor " + juce::String (result.noiseFloorDb, 1) + " dB", juce::dontSendNotification);

    juce::String text;

    for (const auto& entry : result.getReports())
    {
        const auto name = OfflineAnalysis::measurementName (entry.first);
        const auto& entryReport = entry.second;

        text << name << "\n";

        if (entryReport.valid)
        {
            for (const auto& line : entryReport.lines)
                text << "    " << line << "\n";
        }
        else
        {
            text << "    tidak diukur: " << entryReport.skippedReason << "\n";
        }

        text << "\n";
    }

    reportEditor.setText (text, false);

    spectrumPlot.frequency = result.frequency;
    spectrumPlot.magnitudeDb = result.magnitudeDb;
    spectrumPlot.averageLevelDb = result.averageLevelDb;
    spectrumPlot.noiseFloorDb = result.noiseFloorDb;
    spectrumPlot.repaint();
}

void OfflinePanel::paint(juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void OfflinePanel::SpectrumPlot::paint(juce::Graphics& g)
{
    g.fillAll (panelColour);

    if (frequency.size() < 4 || magnitudeDb.size() < 4)
    {
        g.setColour (mutedColour);
        g.setFont (juce::Font (14.0f));
        g.drawText ("Belum ada spektrum", getLocalBounds().toFloat(), juce::Justification::centred);
        return;
    }

    auto area = getLocalBounds().toFloat().reduced (10.0f).withTrimmedLeft (44.0f)
                    .withTrimmedBottom (20.0f);

    const auto topDb = 0.0f;
    const auto bottomDb = -120.0f;

    g.setColour (textColour.withAlpha (0.7f));
    g.setFont (juce::Font (11.0f));

    for (int db = 0; db >= -120; db -= 30)
    {
        const auto y = area.getY() + area.getHeight()
                               * (topDb - (float) db) / (topDb - bottomDb);

        g.setColour (mutedColour.withAlpha (0.25f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());

        g.setColour (mutedColour);
        g.drawText (juce::String (db), area.getX() - 42.0f, y - 8.0f, 38.0f, 16.0f,
                    juce::Justification::centredRight);
    }

    juce::Path curve;

    const auto binHz = frequency.size() > 1 ? frequency[1] - frequency[0] : 1.0f;

    for (size_t bin = 0; bin < magnitudeDb.size() && bin < frequency.size(); ++bin)
    {
        const auto hz = frequency[bin];
        const auto x = area.getX() + area.getWidth()
                              * juce::jlimit (0.0f, 1.0f,
                                              std::log10 (std::max (hz, 10.0f) / 10.0f)
                                                  / std::log10 (20000.0f / 10.0f));

        const auto db = juce::jlimit (bottomDb, topDb, magnitudeDb[bin]);
        const auto y = area.getY() + area.getHeight() * (topDb - db) / (topDb - bottomDb);

        if (bin == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    g.setColour (accentColour);
    g.strokePath (curve, juce::PathStrokeType (1.4f));

    juce::ignoreUnused (binHz);
}

void OfflinePanel::resized()
{
    auto area = getLocalBounds().toFloat().reduced (14.0f);

    auto topRow = area.removeFromTop (30.0f);

    openButton.setBounds (topRow.removeFromLeft (120.0f).toNearestInt());
    topRow.removeFromLeft (8.0f);
    analyseButton.setBounds (topRow.removeFromLeft (160.0f).toNearestInt());
    topRow.removeFromLeft (8.0f);
    clearButton.setBounds (topRow.removeFromLeft (110.0f).toNearestInt());
    topRow.removeFromLeft (12.0f);
    fftSelector.setBounds (topRow.removeFromLeft (130.0f).toNearestInt());
    topRow.removeFromLeft (8.0f);
    resolutionSelector.setBounds (topRow.removeFromLeft (130.0f).toNearestInt());

    area.removeFromTop (6.0f);
    fileLabel.setBounds (area.removeFromTop (22.0f).toNearestInt());
    summaryLabel.setBounds (area.removeFromTop (20.0f).toNearestInt());
    area.removeFromTop (6.0f);

    auto lower = area.removeFromBottom (110.0f);
    auto plotColumn = area.removeFromLeft (area.getWidth() * 0.42f).reduced (0.0f, 0.0f);

    spectrumPlot.setBounds (plotColumn.toNearestInt().toNearestInt());
    reportEditor.setBounds (area.reduced (6.0f, 0.0f).toNearestInt().toNearestInt());

    notes.setBounds (lower.reduced (0.0f, 20.0f).toNearestInt().toNearestInt());
}