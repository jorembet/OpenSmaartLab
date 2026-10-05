#include "RecorderPanel.h"

namespace
{
    static const auto backgroundColour = juce::Colour (0xff141a21);
    static const auto panelColour = juce::Colour (0xff1d252e);
    static const auto textColour = juce::Colour (0xffe6edf3);
    static const auto mutedColour = juce::Colour (0xff8b98a5);
    static const auto recordColour = juce::Colour (0xffd04545);
    static const auto goodColour = juce::Colour (0xff4fb477);

    juce::String describeState (WavRecorder::State state)
    {
        switch (state)
        {
            case WavRecorder::State::Recording: return "Merekam";
            case WavRecorder::State::Paused:    return "Dijeda";
            case WavRecorder::State::Stopped:   return "Berhenti, belum disimpan";
            case WavRecorder::State::Idle:      break;
        }

        return "Idle";
    }
}

RecorderPanel::RecorderPanel (AudioEngine& engineToUse) : engine (engineToUse)
{
    addAndMakeVisible (recordButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (pauseButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (locationButton);
    addAndMakeVisible (depthSelector);
    addAndMakeVisible (rateSelector);
    addAndMakeVisible (stateLabel);
    addAndMakeVisible (levelLabel);
    addAndMakeVisible (detailLabel);
    addAndMakeVisible (adviceLabel);

    const auto depths = WavRecorder::getBitDepthNames();

    for (int i = 0; i < depths.size(); ++i)
        depthSelector.addItem (depths[i], i + 1);

    // Twenty four bit is the default. Sixteen bit is fine for a loud signal and has no headroom
    // for a quiet one, and float is for files that will be processed again.
    depthSelector.setSelectedId (2);

    const auto rates = WavRecorder::getSupportedSampleRates();

    for (int i = 0; i < rates.size(); ++i)
        rateSelector.addItem (rates[i] + " Hz", i + 1);

    rateSelector.setSelectedId (2);


    recordButton.setColour (juce::TextButton::buttonColourId, recordColour.withAlpha (0.22f));
    recordButton.setColour (juce::TextButton::textColourOffId, recordColour);

    saveButton.setColour (juce::TextButton::buttonColourId, goodColour.withAlpha (0.18f));
    saveButton.setColour (juce::TextButton::textColourOffId, goodColour);

    stateLabel.setFont (juce::Font (17.0f, juce::Font::bold));
    stateLabel.setColour (juce::Label::textColourId, textColour);
    stateLabel.setJustificationType (juce::Justification::centredLeft);

    levelLabel.setFont (juce::Font (15.0f));
    levelLabel.setColour (juce::Label::textColourId, mutedColour);
    levelLabel.setJustificationType (juce::Justification::centredRight);

    detailLabel.setFont (juce::Font (14.0f));
    detailLabel.setColour (juce::Label::textColourId, textColour);

    adviceLabel.setFont (juce::Font (13.0f));
    adviceLabel.setColour (juce::Label::textColourId, mutedColour);

    recordButton.onClick = [this] { start(); };
    stopButton.onClick = [this] { stop(); };
    pauseButton.onClick = [this] { pauseOrResume(); };
    saveButton.onClick = [this] { save(); };
    locationButton.onClick = [this] { chooseLocation(); };

    chosenFolder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("OpenSmaartLab");

    updateButtonStates();
    refresh();

    // The recorder's file is written from here rather than from a timer on the whole window, so
    // a recording keeps being written while other views are being repainted.
    startTimerHz (20);
}

RecorderPanel::~RecorderPanel()
{
    stopTimer();
    stopAndRelease();
}

void RecorderPanel::stopAndRelease()
{
    engine.setRecorder (nullptr);

    if (recorder.isOpen())
        recorder.discard();
}

void RecorderPanel::setSampleRate (double rate)
{
    if (rate > 0.0)
    {
        sampleRate = rate;

        // Preselect the rate the device is actually running at, since recording at a rate the
        // hardware is not using would resample the file on the way in.
        const auto rates = WavRecorder::getSupportedSampleRates();

        for (int i = 0; i < rates.size(); ++i)
            if (std::abs (rates[i].getDoubleValue() - rate) < 1.0)
                rateSelector.setSelectedId (i + 1);
    }
}

void RecorderPanel::chooseLocation()
{
    auto* chooser = new juce::FileChooser ("Folder untuk rekaman", chosenFolder, "",
                                          juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
                          [this, chooser] (const juce::FileChooser& fc)
    {
        const auto result = fc.getResult();
        delete chooser;

        if (result == juce::File{} || ! result.isDirectory())
            return;

        chosenFolder = result;
        refresh();
    });
}

void RecorderPanel::start()
{
    if (recorder.isOpen())
        return;

    if (! engine.isRunning())
    {
        adviceLabel.setText ("Tekan Mulai dulu: rekaman mengambil audio dari perangkat yang sedang berjalan.", juce::dontSendNotification);
        return;
    }

    if (! chosenFolder.exists())
    {
        if (auto result = chosenFolder.createDirectory(); result.failed())
        {
            adviceLabel.setText ("Folder rekaman tidak bisa dibuat: " + result.getErrorMessage(), juce::dontSendNotification);
            return;
        }
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    const auto target = chosenFolder.getChildFile ("osl_" + stamp + ".wav");

    const auto depth = (WavRecorder::BitDepth) (depthSelector.getSelectedId() - 1);
    // Recorded at the device's own rate. Writing the file at a rate the hardware is not running
    // would put a conversion in the middle of a measurement, and a measurement with a resampler
    // in it is not the same measurement as the live one.
    juce::String error;

    if (! recorder.startRecording (target, engine.getSampleRate(), engine.getCapturedChannels(),
                                  depth, &error))
    {
        adviceLabel.setText ("Rekaman tidak bisa dimulai: " + error, juce::dontSendNotification);
        return;
    }

    engine.setRecorder (&recorder);
    armed = true;

    adviceLabel.setText ({}, juce::dontSendNotification);
    updateButtonStates();
    refresh();
}

void RecorderPanel::pauseOrResume()
{
    if (recorder.isRecording())
    {
        recorder.pause();
        adviceLabel.setText ("Dijeda. Audio yang masuk selama jeda dibuang, dan rekaman dilanjutkan "
                             "dari titik itu tanpa menyisipkan senyap.", juce::dontSendNotification);
    }
    else if (recorder.isPaused())
    {
        recorder.resume();
        adviceLabel.setText ({}, juce::dontSendNotification);
    }

    updateButtonStates();
    refresh();
}

void RecorderPanel::stop()
{
    if (! recorder.isOpen())
        return;

    recorder.stop();
    engine.setRecorder (nullptr);
    armed = false;

    updateButtonStates();
    refresh();
}

void RecorderPanel::save()
{
    if (! recorder.isOpen())
        return;

    const auto dropped = recorder.getDroppedSamples();

    recorder.stop();
    engine.setRecorder (nullptr);
    armed = false;

    juce::String error;

    if (! recorder.save())
        error = recorder.getLastError();

    lastSaved = recorder.getTargetFile();
    updateButtonStates();
    refresh();

    if (error.isNotEmpty())
    {
        adviceLabel.setText ("Rekaman gagal disimpan: " + error, juce::dontSendNotification);
        return;
    }

    adviceLabel.setText ("Tersimpan: " + lastSaved.getFullPathName(), juce::dontSendNotification);

    if (dropped > 0)
        adviceLabel.setText (adviceLabel.getText() + "   (" + juce::String (dropped)
                             + " sampel hilang karena buffer penuh: pesan thread terlalu sibuk)", juce::dontSendNotification);
}

void RecorderPanel::timerCallback()
{
    if (recorder.isOpen())
        recorder.pump();

    if (armed)
    {
        std::vector<float> left, right;
        engine.getLatestBlock (1024, left, right);

        float peak = 0.0f;

        for (const auto& value : left)
            peak = std::max (peak, std::abs (value));

        levelLabel.setText (peak > 0.0f ? juce::String (20.0f * std::log10 (peak), 1) + " dBFS"
                                         : juce::String ("-inf dBFS"), juce::dontSendNotification);
    }
    else if (levelLabel.getText() != "-inf dBFS")
    {
        levelLabel.setText ("-inf dBFS", juce::dontSendNotification);
    }

    refresh();
}

juce::String RecorderPanel::describeDuration() const
{
    const auto seconds = recorder.getRecordedSeconds();

    const auto minutes = (int) (seconds / 60.0);
    const auto remainder = seconds - (double) minutes * 60.0;

    return juce::String (minutes) + ":" + juce::String (remainder, 2).paddedLeft ('0', 5);
}

void RecorderPanel::refresh()
{
    stateLabel.setText (describeState (recorder.getState())
                          + "   " + describeDuration()
                          + "   " + juce::String ((int) recorder.getSampleRate()) + " Hz / "
                          + WavRecorder::getBitDepthNames()[(int) recorder.getBitDepth()], juce::dontSendNotification);

    juce::String detail = chosenFolder.getFullPathName();

    if (recorder.getRecordedSamples() > 0)
        detail += "   |   " + juce::String (recorder.getRecordedSamples()) + " sampel";

    if (recorder.getDroppedSamples() > 0)
        detail += "   |   " + juce::String (recorder.getDroppedSamples()) + " hilang";

    detailLabel.setText (detail, juce::dontSendNotification);
    repaint();
}

void RecorderPanel::updateButtonStates()
{
    const auto state = recorder.getState();

    recordButton.setEnabled (state == WavRecorder::State::Idle);
    stopButton.setEnabled (state == WavRecorder::State::Recording || state == WavRecorder::State::Paused);
    pauseButton.setEnabled (state == WavRecorder::State::Recording || state == WavRecorder::State::Paused);
    saveButton.setEnabled (state == WavRecorder::State::Stopped);
    depthSelector.setEnabled (state == WavRecorder::State::Idle);
    rateSelector.setEnabled (state == WavRecorder::State::Idle);

    pauseButton.setButtonText (state == WavRecorder::State::Paused ? "LANJUT" : "PAUSE");

    if (state == WavRecorder::State::Recording)
        recordButton.setColour (juce::TextButton::buttonColourId, recordColour);
    else
        recordButton.setColour (juce::TextButton::buttonColourId, recordColour.withAlpha (0.22f));
}

void RecorderPanel::paint(juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto area = getLocalBounds().toFloat().reduced (16.0f);

    auto card = juce::Rectangle<float> (area.getX(), area.getY(), area.getWidth(), area.getHeight() - 96.0f);

    g.setColour (panelColour);
    g.fillRoundedRectangle (card, 8.0f);

    g.setColour (textColour);
    g.setFont (juce::Font (19.0f, juce::Font::bold));
    g.drawText ("Rekaman WAV", card.reduced (16.0f).removeFromTop (26.0f),
                juce::Justification::centredLeft);

    g.setFont (juce::Font (13.5f));
    g.setColour (mutedColour);
    g.drawText ("16-bit, 24-bit, atau 32-bit float pada 44.1, 48, 96, atau 192 kHz. Audio diambil "
                "dari perangkat yang sedang berjalan, pada laju samplel sendiri.",
                card.reduced (16.0f).removeFromTop (34.0f), juce::Justification::topLeft);

    auto adviceArea = getLocalBounds().toFloat().reduced (16.0f).removeFromBottom (76.0f);

    g.setColour (panelColour.withAlpha (0.7f));
    g.fillRoundedRectangle (adviceArea, 6.0f);

    g.setColour (adviceLabel.getText().isNotEmpty() ? textColour : mutedColour);
    g.setFont (juce::Font (13.0f));
    g.drawText (adviceLabel.getText().isNotEmpty() ? adviceLabel.getText()
                                                   : juce::String ("Pilih folder, lalu tekan REC. "
                                                                     "Tekan SAVE untuk menulis file."),
                adviceArea.reduced (12.0f), juce::Justification::centredLeft);
}

void RecorderPanel::resized()
{
    auto area = getLocalBounds().reduced (16.0f);

    auto adviceArea = area.removeFromBottom (76.0f);
    auto card = area.removeFromBottom (area.getHeight() - 96.0f).reduced (16.0f);

    stateLabel.setBounds (card.removeFromTop (28.0f));
    card.removeFromTop (4.0f);
    card.removeFromTop (32.0f);

    auto buttonRow = card.removeFromTop (34.0f);

    recordButton.setBounds (buttonRow.removeFromLeft (110.0f));
    buttonRow.removeFromLeft (8.0f);
    stopButton.setBounds (buttonRow.removeFromLeft (110.0f));
    buttonRow.removeFromLeft (8.0f);
    pauseButton.setBounds (buttonRow.removeFromLeft (110.0f));
    buttonRow.removeFromLeft (8.0f);
    saveButton.setBounds (buttonRow.removeFromLeft (110.0f));

    levelLabel.setBounds (buttonRow.removeFromRight (120.0f));

    card.removeFromTop (10.0f);

    auto selectorRow = card.removeFromTop (28.0f);

    depthSelector.setBounds (selectorRow.removeFromLeft (150.0f));
    selectorRow.removeFromLeft (10.0f);
    rateSelector.setBounds (selectorRow.removeFromLeft (150.0f));

    locationButton.setBounds (selectorRow.removeFromLeft (200.0f));

    detailLabel.setBounds (card.removeFromTop (20.0f));

    juce::ignoreUnused (adviceArea);
}
