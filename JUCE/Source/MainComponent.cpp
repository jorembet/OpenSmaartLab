#include "MainComponent.h"
#include "DSP.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour barColour = juce::Colour(0xff262626);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour runningColour = juce::Colour(0xff43a047);
    const juce::Colour stoppedColour = juce::Colour(0xffe53935);

    void applyInputGain (std::vector<float>& samples, float gain)
    {
        if (gain == 1.0f)
            return;

        for (auto& sample : samples)
            sample *= gain;
    }
}

MainComponent::MainComponent()
{
    setSize(1480, 920);
    pinkNoiseButton.onClick = [this] { playPinkNoise(); };
    pinkNoiseLevel.setRange(-60.0, -3.0, 0.5);
    pinkNoiseLevel.setValue(-24.0, juce::dontSendNotification);
    pinkNoiseLevel.setTextValueSuffix(" dBFS");
    pinkNoiseLevel.setSliderStyle(juce::Slider::LinearHorizontal);
    pinkNoiseLevel.setTextBoxStyle(juce::Slider::TextBoxRight, false, 85, 24);
    pinkNoiseLevel.setTooltip("Level pink noise yang dikirim ke perangkat Output / Putar");
    pinkNoiseLevel.onValueChange = [this] { generatorLevelSlider.setValue(pinkNoiseLevel.getValue()); };

    inputGainLabel.setFont (juce::Font (13.0f));
    inputGainLabel.setColour (juce::Label::textColourId, textColour);
    inputGainSlider.setRange (-60.0, 24.0, 0.5);
    inputGainSlider.setValue (0.0, juce::dontSendNotification);
    inputGainSlider.setTextValueSuffix (" dB");
    inputGainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    inputGainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
    inputGainSlider.setDoubleClickReturnValue (true, 0.0);
    inputGainSlider.setTooltip ("Mengatur volume input mikrofon; memakai gain hardware bila tersedia, jika tidak memakai trim digital.");
    inputGainSlider.onValueChange = [this]
    {
        const auto requestedDb = (float) inputGainSlider.getValue();
        if (hardwareInputGainActive)
        {
            const auto previous = audioEngine.getHardwareInputGainInfo().currentDb;
            if (audioEngine.setHardwareInputGainDb (requestedDb))
            {
                const auto applied = audioEngine.getHardwareInputGainInfo().currentDb;
                inputGainSlider.setValue (applied, juce::dontSendNotification);
                inputLevelPanel.setInputGainControl (
                    audioEngine.getHardwareInputGainInfo().minimumDb,
                    audioEngine.getHardwareInputGainInfo().maximumDb, applied, true);
                if (microphoneCalibration.hasCalibration())
                {
                    microphoneCalibration.setInputTrimDb (
                        microphoneCalibration.getInputTrimDb()
                        + previous - applied);
                    applyCalibrationToSplMeter();
                }
            }
            else
            {
                // A mixer can disappear or become inaccessible while the capture PCM stays
                // open. Keep the same control usable by falling back to the analysis trim.
                hardwareInputGainActive = false;
                softwareInputGainDb = requestedDb;
                inputGainLabel.setText ("Trim Mic", juce::dontSendNotification);
                inputGainSlider.setRange (-60.0, 24.0, 0.5);
                inputGainSlider.setValue (softwareInputGainDb, juce::dontSendNotification);
                inputLevelPanel.setInputGainControl (-60.0, 24.0, softwareInputGainDb, false);
                inputGainSlider.setTooltip ("Trim digital untuk analisis mikrofon; kontrol hardware tidak tersedia.");
                statusLabel.setText ("Gain hardware tidak tersedia; memakai trim digital",
                                     juce::dontSendNotification);
            }
        }
        else
        {
            softwareInputGainDb = requestedDb;
            inputLevelPanel.setInputGainControl (-60.0, 24.0,
                                                 softwareInputGainDb, false);
        }

        dspWorker.resetAveraging();
        transferFunction.reset();
        referenceFrames = 0;
    };
    inputLevelPanel.onInputGainChanged = [this] (float requestedDb)
    {
        inputGainSlider.setValue (requestedDb);
    };
    addAndMakeVisible(pinkNoiseButton);
    addAndMakeVisible(pinkNoiseLevel);
    addAndMakeVisible (inputGainLabel);
    addAndMakeVisible (inputGainSlider);
    addAndMakeVisible(pinkNoiseHint);
    addAndMakeVisible(startHintLabel);
    startHintLabel.setColour(juce::Label::textColourId, mutedColour);
    startHintLabel.setFont(juce::Font(13.0f));
    startHintLabel.setJustificationType(juce::Justification::centredLeft);

    audioEngine.onStatusMessage = [this] (const juce::String& text)
    {
        engineStatusMessage = text;
        statusLabel.setText(text, juce::dontSendNotification);
    };

    fftSizeSelector.addItem("1024", 1024);
    fftSizeSelector.addItem("2048", 2048);
    fftSizeSelector.addItem("4096", 4096);
    fftSizeSelector.addItem("8192", 8192);
    fftSizeSelector.addItem("16384", 16384);
    fftSizeSelector.addItem("32768", 32768);
    fftSizeSelector.addItem("65536", 65536);
    fftSizeSelector.setSelectedId(fftSize);
    fftSizeSelector.onChange = [this] { analysisSettingsChanged(); };

    averagingSelector.addItem("1", 1);
    averagingSelector.addItem("2", 2);
    averagingSelector.addItem("4", 4);
    averagingSelector.addItem("8", 8);
    averagingSelector.addItem("16", 16);
    averagingSelector.addItem("32", 32);
    averagingSelector.addItem("64", 64);
    averagingSelector.addItem("128", 128);
    averagingSelector.setSelectedId(8);
    averagingSelector.onChange = [this]
    {
        referenceFrames = 0;
        transferFunction.setAveraging(averagingSelector.getSelectedId());
    };

    // The temperatures a room is actually held at. 20 is the default because it is the value
    // the 343 m/s figure everybody quotes belongs to, and it is the one a measurement made
    // without a thermometer is implicitly assuming.
    temperatureSelector.addItem ("Suhu 15 C", 15);
    temperatureSelector.addItem ("Suhu 18 C", 18);
    temperatureSelector.addItem ("Suhu 20 C", 20);
    temperatureSelector.addItem ("Suhu 22 C", 22);
    temperatureSelector.addItem ("Suhu 25 C", 25);
    temperatureSelector.addItem ("Suhu 28 C", 28);
    temperatureSelector.addItem ("Suhu 30 C", 30);
    temperatureSelector.setSelectedId (20);
    temperatureSelector.setTooltip (" Suhu udara, untuk mengubah delay menjadi jarak");
    temperatureSelector.onChange = [this]
    {
        transferFunction.setTemperature ((float) temperatureSelector.getSelectedId());
        // The distance is derived from the temperature, so the readout has to be recomputed
        // even though nothing about the measurement itself has changed.
        referenceFrames = 0;
    };
    addAndMakeVisible (temperatureSelector);

    inputSelector.onChange = [this] { inputSelector.setTooltip(inputSelector.getText()); };
    outputSelector.onChange = [this] { outputSelector.setTooltip(outputSelector.getText()); };
sampleRateSelector.onChange = [this] { applyAudioSettingsToDevice(); };
bufferSizeSelector.onChange = [this] { applyAudioSettingsToDevice(); };

    startStopButton.onClick = [this] { startStopClicked(); };
    exportButton.onClick = [this] { exportCSVClicked(); };
    exportIrButton.onClick = [this] { exportIrClicked(); };
    calibrationButton.onClick = [this] { calibrationMenuClicked(); };
    splMeter.onCalibrationRequested = [this] { runCalibratorWorkflow(); };
    saveSnapshotButton.onClick = [this] { saveSnapshotClicked(); };
    loadSnapshotButton.onClick = [this] { loadSnapshotClicked(); };
    // Start is the one control that has to be found and pressed first, so it carries its own
    // colour state instead of the toolbar's flat buttons.
    startStopButton.setTooltip ("Mulai atau hentikan penangkapan audio");
    startStopButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    startStopButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible (startStopButton);
    addAndMakeVisible (calibrationButton);
    addAndMakeVisible (saveSnapshotButton);
    addAndMakeVisible (loadSnapshotButton);
    calibrationLabel.setFont (juce::Font (12.0f));
    calibrationLabel.setColour (juce::Label::textColourId, mutedColour);
    calibrationLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (calibrationLabel);
    calibrationButton.setTooltip ("Kelola profil mic: kalibrasi dengan acoustic calibrator, pilih model, "
                                  "muat kurva, atau matikan kalibrasi.");
    saveSnapshotButton.setTooltip ("Simpan hasil RTA sekarang ke folder Documents/OpenSmaartLab/RTA");
    loadSnapshotButton.setTooltip ("Muat kembali hasil RTA yang tersimpan, atau kembali ke RTA langsung");
    // Calibration stays off by default. Turning it on converts dBFS into dB SPL,
    // which lands far above the display range and pins every bar to the top, so it
    // must be an explicit choice from the Kalibrasi Mic menu.
    updateCalibrationLabel();
    applyCalibrationToSplMeter();

    // Measurement and reference get their own channel selector, so a microphone on the
    // left input and a loopback on the right is a normal wiring rather than a swap of a
    // fixed pair. Both feed the transfer function and the delay reading.
    // The prefix stays in the item text so the wiring is readable on a toolbar with no
    // room for separate captions.
    for (int channel = 0; channel < 2; ++channel)
    {
        measurementChannelSelector.addItem ("Mic: " + AudioEngine::channelName (channel), channel + 1);
        referenceChannelSelector.addItem ("Ref: " + AudioEngine::channelName (channel), channel + 1);
    }

    measurementChannelSelector.setSelectedId (1, juce::dontSendNotification);
    referenceChannelSelector.setSelectedId (2, juce::dontSendNotification);
    measurementChannelSelector.setTooltip ("Kanal input untuk pengukuran RTA dan transfer function");
    referenceChannelSelector.setTooltip ("Kanal input untuk sinyal referensi,misal loopback dari output amplifier");
    measurementChannelSelector.onChange = [this]
    {
        keepMeasurementAndReferenceDistinct();
        resetReferenceTracking();
    };
    referenceChannelSelector.onChange = [this]
    {
        keepMeasurementAndReferenceDistinct();
        resetReferenceTracking();
    };
    addAndMakeVisible(measurementChannelSelector);
    addAndMakeVisible(referenceChannelSelector);
    resetReferenceTracking();
    addAndMakeVisible(inputLabel);
    addAndMakeVisible(outputLabel);
    inputSelector.setTitle("Input / Rekam");
    outputSelector.setTitle("Output / Putar");
    addAndMakeVisible(inputSelector);
    addAndMakeVisible(outputSelector);
    addAndMakeVisible(sampleRateSelector);
    addAndMakeVisible(bufferSizeSelector);
    addAndMakeVisible(fftSizeSelector);
    addAndMakeVisible(averagingSelector);
    addAndMakeVisible(exportButton);
    addAndMakeVisible(exportIrButton);
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(delayLabel);
    addAndMakeVisible(coherenceLabel);

    generatorTypeSelector.addItemList(SignalGenerator::getTypeNames(), 1);
    generatorTypeSelector.setSelectedId(1);
    generatorTypeSelector.onChange = [this]
    {
        const auto name = generatorTypeSelector.getText();
        auto& generator = audioEngine.getGenerator();
        generator.setType(SignalGenerator::typeFromName(name));
        transferFunction.reset();
        updateGeneratorGeneratorControls();
        updateGeneratorInfo();
    };

    // A text box beside the slider, so an exact frequency can be typed instead of
    // hunted for with the mouse. This matters for measurement: 997 Hz matters more
    // than "about 1000 Hz" when lining up a sweep point.
    generatorFrequencyEditor.setTooltip ("Frekuensi dapat diketik langsung, mis. 997 atau 1000.5");
    generatorFrequencyEditor.setTextToShowWhenEmpty ("Hz", mutedColour);
    generatorFrequencyEditor.addListener (&frequencyEditorListener);
    generatorFrequencyEditor.setKeyboardType (juce::TextEditor::numericKeyboard);

    generatorLevelSlider.setRange(-60.0, 0.0, 0.5);
    generatorLevelSlider.setValue(-24.0);
    generatorLevelSlider.setTextValueSuffix(" dBFS");
    generatorLevelSlider.onValueChange = [this]
    {
        audioEngine.getGenerator().setLevelDb((float) generatorLevelSlider.getValue());
        pinkNoiseLevel.setValue(generatorLevelSlider.getValue(), juce::dontSendNotification);
        updateGeneratorInfo();
    };

generatorFrequencySlider.setRange(20.0, 20000.0, 0.1f);
    generatorFrequencySlider.setSkewFactorFromMidPoint(1000.0f);
    generatorFrequencySlider.setValue(1000.0);
    generatorFrequencySlider.setTextValueSuffix(" Hz");
    generatorFrequencySlider.onValueChange = [this]
    {
        const auto value = (float) generatorFrequencySlider.getValue();
        auto& generator = audioEngine.getGenerator();

        // The slider means a tone frequency for sine, and the upper band limit for
        // pink noise. Following the current type keeps one control useful for both.
        if (generator.getType() == SignalGenerator::Type::Pink)
        {
            generator.setBandLimits (generator.getBandLow(), value);
            syncGeneratorBandPreset();
        }
        else
        {
            generator.setFrequency (value);
        }

        if (! isEditingGeneratorFrequency())
            generatorFrequencyEditor.setText (juce::String (juce::roundToInt (value)),
                                             juce::dontSendNotification);
    };

    generatorSweepStartSlider.setRange(10.0, 2000.0, 1.0);
    generatorSweepStartSlider.setValue(20.0, juce::dontSendNotification);
    generatorSweepStartSlider.setTextValueSuffix(" Hz");
    generatorSweepStartSlider.onValueChange = [this]
    {
        audioEngine.getGenerator().setSweepRange((float) generatorSweepStartSlider.getValue(),
                                                (float) generatorSweepEndSlider.getValue(),
                                                (float) generatorSweepDurationSlider.getValue());
        updateGeneratorInfo();
    };

    generatorSweepEndSlider.setRange(2000.0, 20000.0, 10.0);
    generatorSweepEndSlider.setValue(20000.0, juce::dontSendNotification);
    generatorSweepEndSlider.setTextValueSuffix(" Hz");
    generatorSweepEndSlider.onValueChange = [this]
    {
        audioEngine.getGenerator().setSweepRange((float) generatorSweepStartSlider.getValue(),
                                                (float) generatorSweepEndSlider.getValue(),
                                                (float) generatorSweepDurationSlider.getValue());
        updateGeneratorInfo();
    };

    generatorSweepDurationSlider.setRange(5.0, 60.0, 5.0);

    // The interval is the snap, so a five second step puts the dial on the durations a sweep
    // is actually run at: 5, 10 and 20. A continuous dial looked more flexible and only ever
    // produced odd lengths like 12.5 s, which nobody sweeps with.
    generatorSweepDurationSlider.setValue(10.0, juce::dontSendNotification);
    generatorSweepDurationSlider.setTextValueSuffix(" s");
    generatorSweepDurationSlider.onValueChange = [this]
    {
        audioEngine.getGenerator().setSweepRange((float) generatorSweepStartSlider.getValue(),
                                                (float) generatorSweepEndSlider.getValue(),
                                                (float) generatorSweepDurationSlider.getValue());
        updateGeneratorInfo();
    };

    // A low-band slider for pink noise, so both ends of the band can be set. The
    // frequency control handles the upper end, so this only covers the lower.
    // Up to 2 kHz, so the tweeter preset fits: a 2 kHz floor is the highest band edge
    // any driver band needs, and the slider must be able to show it.
    generatorBandLowSlider.setRange(10.0, 2000.0, 1.0);
    generatorBandLowSlider.setSkewFactorFromMidPoint (50.0f);
    generatorBandLowSlider.setValue (audioEngine.getGenerator().getBandLow(),
                                     juce::dontSendNotification);
    generatorBandLowSlider.setTextValueSuffix (" Hz");
    generatorBandLowSlider.onValueChange = [this]
    {
        auto& generator = audioEngine.getGenerator();
        generator.setBandLimits ((float) generatorBandLowSlider.getValue(), generator.getBandHigh());
        syncGeneratorBandPreset();
        updateGeneratorInfo();
    };
    generatorBandLowLabel.setText ("Batas bawah pink noise", juce::dontSendNotification);
    generatorBandLowEditor.setKeyboardType (juce::TextEditor::numericKeyboard);
    generatorBandLowEditor.setTooltip ("Batas bawah pink noise dalam Hz");
    generatorBandLowEditor.setTextToShowWhenEmpty ("Hz", mutedColour);
    generatorBandLowEditor.addListener (&bandLowEditorListener);

    // Band per driver. Correcting a system happens one driver at a time, and coherence
    // measured across a whole speaker is a blend of a subwoofer and a tweeter, so the
    // driver bands are picked by name instead of by hunting for two limits.
    for (int index = 0; index < SignalGenerator::getBandPresets().size(); ++index)
        generatorBandPresetSelector.addItem (SignalGenerator::getBandPresets()[index].name,
                                             index + 1);
    generatorBandPresetSelector.addItem ("Bandingkat manual", 99);
    generatorBandPresetSelector.setTooltip ("Pilih band driver untuk pink noise");
    generatorBandPresetSelector.onChange = [this]
    {
        applyGeneratorBandPreset (generatorBandPresetSelector.getSelectedId() - 1);
    };
    syncGeneratorBandPreset();

    // Output side. A measurement wants the amplifier fed from one output while the other
    // stays silent, so the reference tap can come from the amplifier output without the
    // generator arriving straight into the interface input as well.
    generatorOutputRoutingSelector.addItem ("Kiri + Kanan", 1);
    generatorOutputRoutingSelector.addItem ("Kiri saja", 2);
    generatorOutputRoutingSelector.addItem ("Kanan saja", 3);
    generatorOutputRoutingSelector.setSelectedId (1, juce::dontSendNotification);
    generatorOutputRoutingSelector.setTooltip ("Kanal output mana yang diberi sinyal generator. "
                                               "Pilih satu sisi supaya kanal lain bisa dipakai untuk referensi.");
    generatorOutputRoutingSelector.onChange = [this]
    {
        audioEngine.setGeneratorRouting (currentGeneratorRouting());
        updateGeneratorInfo();
    };

    auto makeLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(14.0f));
        label.setColour(juce::Label::textColourId, mutedColour);
        label.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(label);
    };

    makeLabel(generatorTypeLabel, "Jenis Sinyal");
    makeLabel(generatorBandPresetLabel, "Band Driver");
    makeLabel(generatorOutputRoutingLabel, "Output Kanal");
    makeLabel(generatorLevelLabel, "Level");
    makeLabel(generatorFrequencyLabel, "Frekuensi");
    makeLabel(generatorSweepStartLabel, "Sweep Mulai");
    makeLabel(generatorSweepEndLabel, "Sweep Selesai");
    makeLabel(generatorSweepDurationLabel, "Durasi Sweep");

    generatorInfoLabel.setFont(juce::Font(14.0f));
    generatorInfoLabel.setColour(juce::Label::textColourId, textColour);
    generatorInfoLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(generatorInfoLabel);

    generatorButton.onClick = [this] { generatorToggled(); };

    // Mute is not the same as switching off. Stopping tears the session down and forgets the
    // waveform; muting keeps it running and only silences the output, so the level, the band
    // and the frequency are all still exactly where they were when it comes back.
    generatorMuteButton.setClickingTogglesState (true);
    generatorMuteButton.setTooltip ("Bisukan generator tanpa mengubah pengaturan");
    generatorMuteButton.onClick = [this]
    {
        audioEngine.getGenerator().setMuted (generatorMuteButton.getToggleState());

        // Same reason as switching the generator on or off: the average would otherwise carry
        // the level from before the mute, and coming back from a mute should show the new level
        // straight away rather than easing into it.
        dspWorker.resetAveraging();
    };
    addAndMakeVisible (generatorMuteButton);

    generatorPanel.addAndMakeVisible(generatorButton);
    generatorPanel.addAndMakeVisible(generatorTypeSelector);
    generatorPanel.addAndMakeVisible(generatorLevelSlider);
    generatorPanel.addAndMakeVisible(generatorFrequencySlider);
    generatorPanel.addAndMakeVisible(generatorFrequencyEditor);
    generatorPanel.addAndMakeVisible(generatorBandPresetSelector);
    generatorPanel.addAndMakeVisible(generatorOutputRoutingSelector);
    generatorPanel.addAndMakeVisible(generatorBandLowLabel);
    generatorPanel.addAndMakeVisible(generatorBandLowSlider);
    generatorPanel.addAndMakeVisible(generatorBandLowEditor);
    generatorPanel.addAndMakeVisible(generatorSweepStartSlider);
    generatorPanel.addAndMakeVisible(generatorSweepEndSlider);
    generatorPanel.addAndMakeVisible(generatorSweepDurationSlider);
    generatorPanel.addAndMakeVisible(generatorTypeLabel);
    generatorPanel.addAndMakeVisible(generatorBandPresetLabel);
    generatorPanel.addAndMakeVisible(generatorOutputRoutingLabel);
    generatorPanel.addAndMakeVisible(generatorLevelLabel);
    generatorPanel.addAndMakeVisible(generatorFrequencyLabel);
    generatorPanel.addAndMakeVisible(generatorSweepStartLabel);
    generatorPanel.addAndMakeVisible(generatorSweepEndLabel);
    generatorPanel.addAndMakeVisible(generatorSweepDurationLabel);
    generatorPanel.addAndMakeVisible(generatorInfoLabel);
    generatorPanel.addAndMakeVisible(generatorDisplay);
    generatorPanel.setInterceptsMouseClicks(false, true);

    generatorPanel.addComponentListener(this);
    addAndMakeVisible(tabs);
    tabs.addTab("RTA / Delay", backgroundColour, &fftDisplay, false);
    tabs.addTab("Transfer Function", backgroundColour, &transferFunctionDisplay, false);
    transferFunctionDisplay.onFindDelay = [this] { findDelayRequested = true; };
    tabs.addTab("Reverberation", backgroundColour, &reverbDisplay, false);
    tabs.addTab("Impedansi", backgroundColour, &impedanceDisplay, false);
    tabs.addTab("Input Level", backgroundColour, &inputLevelPanel, false);
    tabs.addTab("Spectrum", backgroundColour, &spectrumDisplay, false);
    tabs.addTab("Distorsi", backgroundColour, &distortionDisplay, false);
    tabs.addTab("SPL Meter", backgroundColour, &splMeter, false);
    tabs.addTab("Generator", backgroundColour, &generatorPanel, false);
    tabs.addTab("Rekam", backgroundColour, &recorderPanel, false);
    tabs.addTab("Offline", backgroundColour, &offlinePanel, false);
    tabs.addTab("Session & Export", backgroundColour, &sessionPanel, false);

    statusLabel.setFont(juce::Font(13.0f));
    statusLabel.setColour(juce::Label::textColourId, mutedColour);
    delayLabel.setFont(juce::Font(13.0f, juce::Font::bold));
    delayLabel.setColour(juce::Label::textColourId, textColour);
    delayLabel.setJustificationType(juce::Justification::centredRight);
    coherenceLabel.setFont(juce::Font(13.0f, juce::Font::bold));
    coherenceLabel.setColour(juce::Label::textColourId, textColour);
    coherenceLabel.setJustificationType(juce::Justification::centredRight);

    transferFunction.setAveraging(averagingSelector.getSelectedId());
    transferFunction.setAutomaticDelay(true);
    transferFunction.setDelayCompensation(true);

    audioEngine.getGenerator().setType(SignalGenerator::Type::Pink);
    audioEngine.getGenerator().setLevelDb(-24.0f);
    audioEngine.getGenerator().setFrequency(1000.0f);
    audioEngine.getGenerator().setSweepRange(20.0f, 20000.0f, 10.0f);

    // The measurement worker is prepared before any audio arrives, so its buffers exist
    // by the time the first captured block is handed over.
    dspWorker.setSampleRate(audioEngine.getSampleRate());
    dspWorker.setFftSize(spectrumDisplay.getFftSize());
    dspWorker.setWindow(spectrumDisplay.getWindowType());
    dspWorker.setOverlapPercent(spectrumDisplay.getOverlapPercent());
    dspWorker.setAveraging(spectrumDisplay.getAveragingMode(),
                           spectrumDisplay.getAveragingSeconds());
    dspWorker.start();

    audioEngine.onDeviceLost = [this]
    {
        // Only ever called from the message thread, from pollDeviceHealth().
        //
        // Everything that claims a session is running has to be cleared here, not just the
        // display. Leaving isRunning true while the device has gone leaves the button
        // offering to stop a session that no longer exists, so the next press is taken as a
        // stop: it resets stale state, the button returns to "Mulai", and from the reader's
        // side Mulai did nothing at all. Only the press after that would start anything.
        isRunning = false;
        generatorOn = false;
        audioEngine.getGenerator().setRunning(false);
        generatorDisplay.setRunning(false);
        generatorDisplay.clear();

        fftDisplay.setRunning(false);
        fftDisplay.setDelayAvailable(false);
        fftDisplay.pushData({}, {}, {}, {}, {}, {});
        delayLabel.setText("Delay --", juce::dontSendNotification);
        reverbDisplay.clear();
        splMeter.reset();

        startStopButton.setButtonText("Mulai");
        generatorButton.setButtonText("Nyalakan");
        updateGeneratorInfo();

        engineStatusMessage = "Perangkat terputus - pilih device lalu tekan Mulai";
        statusLabel.setText(engineStatusMessage, juce::dontSendNotification);
    };

    spectrumDisplay.onFftSizeChanged = [this] { spectrumControlsChanged(); };
    spectrumDisplay.onWindowChanged = [this] { spectrumControlsChanged(); };
    spectrumDisplay.onAveragingChanged = [this] { spectrumControlsChanged(); };

    // The band analyser shares the FFT size with the spectrum, so both views are built
    // from one transform and cannot disagree.
    //
    // Configured through rtaControlsChanged, which reads the octave selector on the display
    // rather than the hidden RTA view. That makes the control the user can actually reach the
    // control that decides the measurement, instead of the two starting out disagreeing and
    // only agreeing once something is changed.
    fftDisplay.onOctaveChanged = [this] { rtaControlsChanged(); };

    dspWorker.setRtaAveraging (dsp::SpectrumAnalyser::Averaging::Exponential, 0.5f);
    dspWorker.setRtaSmoothingSeconds (0.0f);
    dspWorker.setRtaPeakHoldSeconds (1.0f);
    dspWorker.setRtaPeakDecayDbPerSecond (20.0f);
    dspWorker.setRtaMinMaxWindowSeconds (5.0f);

    rtaControlsChanged();

    refreshDeviceSelectors();
    updateGeneratorInfo();
    updateStatus();
    updateGeneratorGeneratorControls();

    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    generatorPanel.removeComponentListener(this);
    stopTimer();
    dspWorker.stop();
    audioEngine.getGenerator().setRunning(false);
    audioEngine.stop();
}

void MainComponent::refreshDeviceSelectors()
{
    // Rebuilding the lists empties the combo boxes, so restoring a selection counts as a
    // change to them and the change callback arrives even though the reader touched
    // nothing. Left unguarded, that callback reopened the device, the reopen rebuilt the
    // lists again, and a running session restarted itself forever without the reader ever
    // touching a control. The flag makes a programmatic rebuild silent.
    const auto guard = juce::ScopedValueSetter<bool> (rebuildingSelectors, true);

    const auto inputs = audioEngine.getInputDeviceLabels();
    const auto outputs = audioEngine.getOutputDeviceLabels();

    const auto previousInput = inputSelector.getSelectedId();
    const auto previousOutput = outputSelector.getSelectedId();
    const auto previousRate = sampleRateSelector.getSelectedId();
    const auto previousBuffer = bufferSizeSelector.getSelectedId();

    inputSelector.clear();
    inputSelector.addItem("Input nonaktif", 1);

    for (int i = 0; i < inputs.size(); ++i)
        inputSelector.addItem(inputs[i], i + 2);

    outputSelector.clear();
    outputSelector.addItem("Output nonaktif", 1);

    for (int i = 0; i < outputs.size(); ++i)
        outputSelector.addItem(outputs[i], i + 2);

    // Restoring the previous choice is not a user edit, so it must not notify. Sending a
    // notification here told the rest of the app that the reader had just changed the sample
    // rate, which is what made applyAudioSettingsToDevice() reopen the device: this function
    // also runs inside the start path, so the notification was delivered after the device had
    // already opened and stopped the session that was starting. A rebuild that fires change
    // callbacks is a rebuild that renames the user's settings into events.
    inputSelector.setSelectedId(previousInput > 1 && previousInput <= inputs.size() + 1
                                    ? previousInput : juce::jmin(2, inputs.size() + 1),
                                juce::dontSendNotification);
    outputSelector.setSelectedId(previousOutput > 1 && previousOutput <= outputs.size() + 1
                                    ? previousOutput : juce::jmin(2, outputs.size() + 1),
                                juce::dontSendNotification);

    sampleRateSelector.clear();
    bufferSizeSelector.clear();

    juce::Array<double> rates;
    juce::Array<int> buffers;

    auto& types = audioEngine.getDeviceManager().getAvailableDeviceTypes();

    for (int i = 0; i < types.size() && rates.isEmpty(); ++i)
    {
        auto* type = types[i];

        if (type == nullptr)
            continue;

        for (const auto& deviceName : type->getDeviceNames(false))
        {
            if (auto* device = type->createDevice(deviceName, deviceName))
            {
                rates = device->getAvailableSampleRates();
                buffers = device->getAvailableBufferSizes();
                delete device;
                break;
            }

            if (auto* device = type->createDevice(deviceName, juce::String()))
            {
                rates = device->getAvailableSampleRates();
                buffers = device->getAvailableBufferSizes();
                delete device;
                break;
            }
        }
    }

    if (auto* device = audioEngine.getDeviceManager().getCurrentAudioDevice())
    {
        rates = device->getAvailableSampleRates();
        buffers = device->getAvailableBufferSizes();
    }

    if (rates.isEmpty())
        rates.add(48000.0);

    if (buffers.isEmpty())
    {
        buffers.add(256);
        buffers.add(512);
        buffers.add(1024);
        buffers.add(2048);
    }

    int rateId = 0;

    for (int i = 0; i < rates.size(); ++i)
    {
        sampleRateSelector.addItem(juce::String((int) rates[i]), i + 1);

        if (rateId == 0 && rates[i] >= 48000.0)
            rateId = i + 1;
    }

    if (rateId == 0 && rates.size() > 0)
        rateId = 1;

    sampleRateSelector.setSelectedId(previousRate > 0 && previousRate <= rates.size()
                                        ? previousRate : rateId,
                                    juce::dontSendNotification);

    int bufferId = 0;

    for (int i = 0; i < buffers.size(); ++i)
    {
        bufferSizeSelector.addItem(juce::String(buffers[i]), i + 1);

        if (bufferId == 0 && buffers[i] >= 1024)
            bufferId = i + 1;
    }

    if (bufferId == 0 && buffers.size() > 0)
        bufferId = 1;

    bufferSizeSelector.setSelectedId(previousBuffer > 0 && previousBuffer <= buffers.size()
                                        ? previousBuffer : bufferId,
                                    juce::dontSendNotification);
}

void MainComponent::analysisSettingsChanged()
{
    referenceFrames = 0;
    fftSize = fftSizeSelector.getSelectedId();
    fftSize = std::max(1024, fftSize);

    const auto rate = (float) audioEngine.getSampleRate();

    transferFunction.prepare(rate, fftSize);
    transferFunction.setAveraging(averagingSelector.getSelectedId());

    fftDisplay.clearPeakHold();
    reverbDisplay.clear();

    updateStatus();
}

void MainComponent::startStopClicked()
{
    // The button is the only toggle in the program, so it is the only place a session is
    // started or stopped on purpose. Everything else that needs the device reopened calls
    // restartAudio() instead, because reaching this function to "apply a setting" means the
    // outcome depends on what the session happened to be doing when the notification
    // arrived, which is how a running session used to stop itself the instant it opened.
    if (isRunning)
        stopAudio();
    else
        startAudio();
}

void MainComponent::stopAudio()
{
    audioEngine.stop();
    updateInputGainControl();
    audioEngine.getGenerator().setRunning(false);
    generatorOn = false;
    isRunning = false;

    fftDisplay.setDelayAvailable(false);
    delayLabel.setText("Delay --", juce::dontSendNotification);
    fftDisplay.setRunning(false);
    fftDisplay.pushData({}, {}, {}, {}, {}, {});
    reverbDisplay.clear();
    splMeter.reset();

    startStopButton.setButtonText("Mulai");
    startStopButton.setColour(juce::TextButton::buttonColourId, runningColour);
    generatorButton.setButtonText("Nyalakan");
    updateGeneratorInfo();
    updateStatus();
}

void MainComponent::startAudio()
{
    // The device lists are rebuilt on every start, so a device plugged in while the window was
    // open is selectable without a separate refresh control. The previous selection is
    // restored by refreshDeviceSelectors, so this cannot silently change which device is
    // being measured.
    audioEngine.scanDevices();
    refreshDeviceSelectors();

    const auto inputIndex = inputSelector.getSelectedId() - 2;
    const auto outputIndex = outputSelector.getSelectedId() - 2;

    const auto& inputNames = audioEngine.getInputDeviceNames();
    const auto& outputNames = audioEngine.getOutputDeviceNames();

    const auto inputName = juce::isPositiveAndBelow(inputIndex, inputNames.size())
                         ? inputNames[inputIndex] : juce::String();
    const auto outputName = juce::isPositiveAndBelow(outputIndex, outputNames.size())
                          ? outputNames[outputIndex] : juce::String();
    const auto rate = sampleRateSelector.getSelectedId() > 0
                    ? sampleRateSelector.getText().getDoubleValue() : 48000.0;
    const auto bufferSize = bufferSizeSelector.getSelectedId() > 0
                          ? bufferSizeSelector.getText().getIntValue() : 1024;

    if (inputName.isEmpty())
    {
        statusLabel.setText("Pilih microphone pada Input / Rekam", juce::dontSendNotification);
        return;
    }

    // Cleared per attempt rather than on success, so a second start that fails without the
    // engine reporting anything is still given the generic status line instead of inheriting
    // the reason the first attempt gave.
    engineStatusMessage.clear();

    appliedRateId = sampleRateSelector.getSelectedId();
    appliedBufferSizeId = bufferSizeSelector.getSelectedId();

    audioEngine.start(inputName, outputName, rate, bufferSize,
                      audioEngine.getOutputPulseSink(outputIndex));

    if (!audioEngine.isRunning())
    {
        startStopButton.setButtonText("Mulai");
        fftDisplay.setDelayAvailable(false);
        delayLabel.setText("Delay --", juce::dontSendNotification);
        fftDisplay.setRunning(false);

        // The engine has already written the reason onto the status line. Calling
        // updateStatus() here would replace it with the generic "pick a device" text on the
        // very same click, so the reader would be told their device failed to open and then,
        // immediately, that nothing had happened. The button also stays on "Mulai", which
        // makes a failed start look exactly like a click that was never registered. So the
        // reason is left on screen, and the generic text is only used when the engine had
        // nothing to say.
        if (engineStatusMessage.isEmpty())
            updateStatus();

        return;
    }

    // The device is open, so anything the engine said about the previous attempt is stale.
    engineStatusMessage.clear();
    updateInputGainControl();

    referenceFrames = 0;
    isRunning = true;
    startStopButton.setButtonText("Berhenti");
    startStopButton.setColour(juce::TextButton::buttonColourId, stoppedColour);

    splMeter.prepare((float) audioEngine.getSampleRate(), 2048);
    transferFunction.prepare((float) audioEngine.getSampleRate(), fftSize);

    // Sized from the transform the transfer function already uses, so the distortion figures
    // and the curve on the previous tab describe the same block of audio. A distortion figure
    // taken from a different length than the curve beside it would be answering a slightly
    // different question than the reader assumes.
    distortionAnalyser.prepare(audioEngine.getSampleRate(), transferFunction.getFftSize());

    imdAnalyser.prepare(audioEngine.getSampleRate(), transferFunction.getFftSize());
    crosstalkAnalyser.prepare(audioEngine.getSampleRate(), transferFunction.getFftSize());

    {
        dsp::DistortionAnalyser::Settings distortionSettings;
        distortionSettings.searchLowHz = 20.0f;
        distortionSettings.searchHighHz = 20000.0f;
        distortionSettings.maxHarmonic = 10;
        distortionAnalyser.setSettings(distortionSettings);
    }
    transferFunction.setAveraging(averagingSelector.getSelectedId());

    tabs.setCurrentTabIndex(0);
    fftDisplay.setDelayAvailable(false);
    delayLabel.setText("Delay --", juce::dontSendNotification);
    fftDisplay.setRunning(true);
    fftDisplay.clearPeakHold();

    updateStatus();
}

void MainComponent::updateInputGainControl()
{
    const auto hardware = audioEngine.getHardwareInputGainInfo();
    hardwareInputGainActive = hardware.available;

    if (hardware.available)
    {
        inputGainLabel.setText ("Gain Mic", juce::dontSendNotification);
        inputGainSlider.setRange (hardware.minimumDb, hardware.maximumDb, 0.5);
        inputGainSlider.setValue (hardware.currentDb, juce::dontSendNotification);
        inputGainSlider.setTooltip ("Mengatur gain capture hardware ALSA. Rentang "
                                    + juce::String (hardware.minimumDb, 1) + " sampai "
                                    + juce::String (hardware.maximumDb, 1) + " dB.");
        inputLevelPanel.setInputGainControl (hardware.minimumDb, hardware.maximumDb,
                                             hardware.currentDb, true);
        return;
    }

    inputGainLabel.setText ("Trim Mic", juce::dontSendNotification);
    inputGainSlider.setRange (-60.0, 24.0, 0.5);
    inputGainSlider.setValue (softwareInputGainDb, juce::dontSendNotification);
    inputGainSlider.setTooltip ("Trim digital untuk analisis mikrofon; tidak mengubah gain hardware atau rekaman mentah.");
    inputLevelPanel.setInputGainControl (-60.0, 24.0, softwareInputGainDb, false);
}

float MainComponent::analysisInputGainDb() const
{
    return hardwareInputGainActive ? 0.0f : softwareInputGainDb;
}

void MainComponent::restartAudio()
{
    // A setting that only takes effect when the device is reopened. Stopping first and
    // starting second makes the outcome independent of what the session was doing, so this
    // cannot turn a running session into a stopped one.
    if (! isRunning)
        return;
    stopAudio();
    startAudio();
}

void MainComponent::playPinkNoise()
{
    if (generatorOn)
    {
        generatorToggled();
        return;
    }
    if (outputSelector.getSelectedId() <= 1)
    {
        statusLabel.setText("Pilih speaker pada Output / Putar sebelum Play Pink Noise", juce::dontSendNotification);
        return;
    }
    generatorTypeSelector.setSelectedId(1, juce::sendNotificationSync);

    if (!isRunning)
        startStopClicked();
    if (isRunning)
    {
        generatorToggled();
        tabs.setCurrentTabIndex(0);
        fftDisplay.setMode(FFTDisplay::Mode::SingleChannel);
    }
}

void MainComponent::generatorToggled()
{
    if (!isRunning || !audioEngine.isRunning())
    {
        generatorOn = false;
        audioEngine.getGenerator().setRunning(false);
        generatorDisplay.setRunning(false);
        generatorDisplay.clear();
        generatorInfoLabel.setColour(juce::Label::textColourId, stoppedColour);
        generatorInfoLabel.setText("Audio belum berjalan - tekan Mulai dulu sebelum menyalakan generator",
                                  juce::dontSendNotification);
        generatorButton.setButtonText("Nyalakan");
        generatorButton.setColour(juce::TextButton::buttonColourId, stoppedColour);
        return;
    }

    if (!generatorOn)
    {
        auto* device = audioEngine.getDeviceManager().getCurrentAudioDevice();
        if (device == nullptr || device->getActiveOutputChannels().isZero())
        {
            statusLabel.setText("Output belum aktif: pilih speaker, hentikan audio lalu tekan Play Pink Noise", juce::dontSendNotification);
            return;
        }
    }
    generatorOn = !generatorOn;
    transferFunction.reset();

    // The averages have to go as well. They are exponential, so after the generator starts or
    // stops the display shows the old signal fading into the new one over several time
    // constants, which reads as the measurement taking a long time to come up when in fact it
    // is only the average remembering. Resetting is deterministic: it happens on this switch,
    // not on a level change, so it cannot retrigger while a measurement is under way.
    dspWorker.resetAveraging();

    referenceFrames = 0;
    fftDisplay.setDelayAvailable(false);
    fftDisplay.clearPeakHold();
    delayLabel.setText("Delay --", juce::dontSendNotification);

    audioEngine.getGenerator().setRunning(generatorOn);
    generatorDisplay.setRunning(generatorOn);
    generatorButton.setButtonText(generatorOn ? "Matikan" : "Nyalakan");
    generatorButton.setColour(juce::TextButton::buttonColourId,
                              generatorOn ? runningColour : stoppedColour);

    updateGeneratorInfo();
}

void MainComponent::updateGeneratorInfo()
{
    auto& generator = audioEngine.getGenerator();

    // The display has to follow the settings even while stopped, otherwise the axis and
    // band shading keep describing the previous configuration.
    generatorDisplay.setSignal(generatorTypeSelector.getText(), (float) generatorLevelSlider.getValue(),
                               generator.getSweepProgress(), generatorOutputLabel());
    generatorDisplay.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                          generator.getFrequency(),
                                          generator.getSweepStart(), generator.getSweepEnd());

    pinkNoiseButton.setButtonText(generatorOn ? "Stop Generator" : "Play Pink Noise");
    pinkNoiseButton.setColour(juce::TextButton::buttonColourId, generatorOn ? runningColour : barColour);
    fftDisplay.showGeneratorReference(generatorOn);

    if (!generatorOn)
    {
        generatorDisplay.clear();
        generatorInfoLabel.setColour(juce::Label::textColourId, mutedColour);
        generatorInfoLabel.setText("Generator mati", juce::dontSendNotification);
        return;
    }

    // While running, refresh the plot at once so a slider change is visible before the
    // next captured block arrives.
    updateGeneratorDisplay();

    generatorInfoLabel.setColour(juce::Label::textColourId, runningColour);
    generatorInfoLabel.setText("Generator aktif - " + generatorTypeSelector.getText()
                                   + "   level " + juce::String(generator.getLevelDb(), 1) + " dBFS",
                               juce::dontSendNotification);
}

void MainComponent::updateGeneratorDisplay()
{
    auto& generator = audioEngine.getGenerator();
    const auto sampleRate = (float) audioEngine.getSampleRate();

    generatorDisplay.setSignal(generatorTypeSelector.getText(), (float) generatorLevelSlider.getValue(),
                               generator.getSweepProgress(), generatorOutputLabel());
    generatorDisplay.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                          generator.getFrequency(),
                                          generator.getSweepStart(), generator.getSweepEnd());

    auto sampleCount = generatorDisplay.getRequiredSamples();
    auto waveformFrequency = 0.0f;

    switch (generator.getType())
    {
        case SignalGenerator::Type::Sine:
        case SignalGenerator::Type::Square:
        case SignalGenerator::Type::Triangle:
        case SignalGenerator::Type::Saw:
            waveformFrequency = generator.getFrequency();
            break;

        case SignalGenerator::Type::LogSweep:
            waveformFrequency = generator.getSweepStart()
                               * std::pow (generator.getSweepEnd() / generator.getSweepStart(),
                                           generator.getSweepProgress());
            break;

        default:
            break;
    }

    if (waveformFrequency > 0.0f)
        sampleCount = juce::jmax (sampleCount,
                                  juce::roundToInt (sampleRate * 3.0f / waveformFrequency));

    audioEngine.getGeneratorOutput (generatorOutputSamples, sampleCount);
    generatorDisplay.setSamples (generatorOutputSamples, sampleRate);
}

void MainComponent::updateStatus()
{
    if (isRunning)
    {
        auto* device = audioEngine.getDeviceManager().getCurrentAudioDevice();
        const auto inputChannels = device != nullptr
                                 ? device->getInputChannelNames().size() : 0;

        // A session can be running and still have nothing to measure. The engine opens the
        // device it was asked for and reports success, but a device with no capture channels
        // hands over silence, so every curve stayed flat and the button looked like it had
        // done nothing. Saying so here is the difference between a dead control and a
        // readable one, and it names the cause instead of asking the reader to guess.
        if (inputChannels == 0)
        {
            statusLabel.setColour (juce::Label::textColourId, stoppedColour);
            statusLabel.setText ("Device terbuka tapi tidak punya channel masuk - "
                                "pilih input lain", juce::dontSendNotification);
            return;
        }

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        statusLabel.setText("Running - " + inputSelector.getText()
                            + " -> " + outputSelector.getText()
                            + " @ " + juce::String((int) audioEngine.getSampleRate()) + " Hz",
                            juce::dontSendNotification);
        return;
    }

    statusLabel.setText("Berhenti - pilih device lalu tekan Mulai", juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    // Device health and the measurement worker are pumped even while stopped: a device
    // that disappeared has to be reported, and the worker has to be told to stand down.
    audioEngine.pollDeviceHealth();

    if (!isRunning)
    {
        dspWorker.setSpectrumEnabled(false);
        dspWorker.setRtaEnabled (false);
        return;
    }

    const auto spectrumVisible = spectrumDisplay.isVisible();
    const auto rtaVisible = rtaDisplay.isVisible();
    dspWorker.setSpectrumEnabled (spectrumVisible || rtaVisible);
    dspWorker.setRtaEnabled (rtaVisible);

    // Keep the worker's capture stream independent from the transfer-function FFT below:
    // a large transfer frame or missing reference must not hold back the Spectrum tab.
    pumpMeasurementWorker();

    // The generator plot reads the output ring directly. It must update even when the
    // microphone has not supplied a complete analysis frame or the input is silent, and
    // hidden generator tabs should not spend time building an FFT nobody can see.
    if (generatorOn && generatorDisplay.isShowing())
        updateGeneratorDisplay();

    auto& ref = analysisReference;
    auto& meas = analysisMeasurement;
    auto& left = analysisLeft;
    auto& right = analysisRight;
    auto& generated = analysisGenerated;
    // A sliding FFT window makes the graph update on every 30 Hz refresh. Reading 65536
    // samples just because the generator is on made it wait over a second between frames,
    // although the transfer analysis only uses its selected FFT size and delay search cannot
    // exceed half that size anyway.
    if (! audioEngine.readLatestFrame (transferFunction.getFftSize(), left, right, &generated))
        return;

    const auto channels = audioEngine.getCapturedChannels();
    if (channels == 0)
        return;

    // The engine hands the channels back in physical order, so the measurement is picked
    // out by its assignment. Both outputs have to come back the same length: downstream
    // code walks them together, and an empty one would read past the end of the other.
    dsp::assignMeasurementAndReference(measurementChannelIndex(), left, right, meas, ref);

    if (ref.size() < (size_t) transferFunction.getFftSize()
        || meas.size() != ref.size())
        return;

    const auto generatorType = audioEngine.getGenerator().getType();
    const bool broadbandGenerator = generatorOn && (generatorType == SignalGenerator::Type::Pink
                                                     || generatorType == SignalGenerator::Type::White);
    if (generatorOn)
    {
        ref = generated;

        // The reference for the measurement is the generator itself, but it only carries
        // the delay once the block is longer than the loop can possibly be.
        const auto keep = (size_t) transferFunction.getFftSize();
        if (ref.size() > keep)
        {
            ref.erase(ref.begin(), ref.end() - (ptrdiff_t) keep);
            meas.erase(meas.begin(), meas.end() - (ptrdiff_t) keep);
        }
    }

    // The delay search needs the raw pair, so the button sets a flag and the frame that
    // follows does the work with fresh data.
    if (findDelayRequested)
    {
        findDelayRequested = false;
        transferFunction.findDelay(ref.data(), meas.data());
    }

    applyInputGain (meas, juce::Decibels::decibelsToGain (analysisInputGainDb()));

    auto result = transferFunction.process(ref.data(), meas.data(), (int) ref.size());

    if (!result.valid)
        return;

    // TransferFunction already measures RMS and peak while it has each sample in hand;
    // reuse those readings instead of making four more passes over both audio frames.
    fftDisplay.setMeasuredLevels (result.measRmsDb, result.measPeakDb);
    fftDisplay.setReferenceLevels (result.refRmsDb, result.refPeakDb);

    const auto referenceLevel = result.refRmsDb;
    const auto measuredLevel = result.measRmsDb;

    const bool hasReference = (generatorOn || channels > 1) && referenceLevel > -80.0f && measuredLevel > -80.0f;
    referenceFrames = hasReference ? referenceFrames + 1 : 0;

    // The delay the phase and the impulse are compensated with is the one the transfer
    // function tracked on this frame, so the reading and the curves can never disagree. A
    // reading sitting on the limit of the search means the peak was never found, and a
    // generator that is not broadband carries no delay to find at all.
    const auto delayMeasured = transferFunction.isDelayTrusted()
                            && std::abs (result.delayMs) < transferFunction.getMaxReachableDelayMs() - 1.0f
                            && (! generatorOn || broadbandGenerator);
    const bool delayAvailable = generatorOn ? (hasReference && delayMeasured)
                             : referenceFrames >= 8 && hasReference && delayMeasured
                               && averagingSelector.getSelectedId() > 1
                               && result.averageCoherence >= 0.5f;
    fftDisplay.setDelayAvailable(delayAvailable);
    if (!hasReference)
    {
        result.magnitudeDb.clear();
        result.phaseDeg.clear();
        result.coherence.clear();
        result.impulseResponse.clear();
        reverbDisplay.clear();
        impedanceDisplay.clear();
    }
    // An unavailable delay is carried as NaN, so every reader has to agree on it. Masking
    // only lastResult left the Transfer Function sidebar showing a number the status line
    // was hiding.
    if (!delayAvailable)
        result.delayMs = std::numeric_limits<float>::quiet_NaN();

    lastResult = result;
    fftDisplay.setDelayMs(result.delayMs);
    fftDisplay.setAverageCoherence(result.averageCoherence);
    fftDisplay.pushData(result.freq, result.refMagnitudeDb, result.measMagnitudeDb,
                        result.magnitudeDb, result.phaseDeg, result.coherence, result.binValid);

    if (transferFunctionDisplay.isVisible())
        transferFunctionDisplay.pushData(result);

    // Measured on the measurement channel alone. Distortion belongs to whatever is being
    // measured, and folding in the reference as well would report the reference's own
    // distortion and the room's with it.
    if (distortionDisplay.isVisible())
    {
        distortionDisplay.setResult(distortionAnalyser.analyse(meas.data(), (int) meas.size()));

        // Intermodulation needs its own two tones, so it is only read when the generator is
        // actually producing them. Measuring it against whatever happens to be playing would
        // report the intermodulation of the wrong test, which is worse than reporting nothing.
        if (generatorOn && imdAnalyser.getSettings().standard == dsp::ImdAnalyser::Standard::Smpte
             && std::abs (audioEngine.getGenerator().getFrequency() - 1000.0f) < 400.0f)
        {
            const auto imd = imdAnalyser.analyse (meas.data(), (int) meas.size());

            statusLabel.setText ("IMD " + dsp::ImdAnalyser::percentToString (imd.imdPercent)
                                   + "  (" + dsp::ImdAnalyser::levelToString (imd.imdDb) + ")",
                                 juce::dontSendNotification);
        }

        // Polarity is judged from the correlation sign rather than the level, because an
        // inverted channel leaves the level, the spectrum and the coherence completely
        // unchanged. Only something comparing the waveform against its reference can see it.
        if (generatorOn)
        {
            const auto polarity = polarityDetector.detect (ref.data(), meas.data(),
                                                           (int) ref.size());

            if (polarity.confident)
            {
                const auto verdict = dsp::PolarityDetector::verdictToString (polarity.verdict);

                statusLabel.setColour (juce::Label::textColourId,
                                      polarity.verdict == dsp::PolarityDetector::Verdict::Inverted
                                          ? stoppedColour : runningColour);
                statusLabel.setText ("POLARITAS " + verdict + "  (korelasi "
                                         + juce::String (polarity.correlation, 3)
                                         + ", lag " + juce::String (polarity.lagSamples)
                                         + " sample)",
                                     juce::dontSendNotification);
            }
        }
    }

    if (impedanceDisplay.isVisible())
        impedanceDisplay.pushData(result);

    // The distance is shown next to the delay rather than instead of it, because a delay on
    // its own means nothing to a reader without the air it travelled through, and the
    // temperature that produced it is printed so the number can be held to account. It stays
    // blank while the delay is unproven: a confident length derived from a correlation peak
    // that failed its own confidence check would be the most misleading thing on the bar.
    juce::String delayText;

    if (delayAvailable)
    {
        delayText = (generatorOn ? "Total " : "Delay ") + juce::String (result.delayMs, 3) + " ms";

        if (result.delayDistanceM > 0.0f)
            delayText += "   " + juce::String (result.delayDistanceM, 2) + " m @ "
                       + juce::String ((int) result.temperatureC) + "\u00b0C";

        if (! result.delayMethodsAgree && result.phaseSlopeValid)
            delayText += "  (slope " + juce::String (result.phaseSlopeDelayMs, 2) + " ms)";
    }
    else
    {
        delayText = generatorOn ? "Total -- (menunggu mic)" : "Delay -- (perlu referensi)";
    }

    delayLabel.setText (delayText, juce::dontSendNotification);

    const auto averages = averagingSelector.getSelectedId();

    if (!hasReference)
        coherenceLabel.setText("RTA microphone", juce::dontSendNotification);
    else if (averages <= 1)
        coherenceLabel.setText("Coherence perlu averaging > 1", juce::dontSendNotification);
    else
        coherenceLabel.setText("Coherence " + juce::String(result.averageCoherence * 100.0f, 1)
                                   + " %   (avg " + juce::String(averages) + ")",
                               juce::dontSendNotification);

    if (generatorOn)
        statusLabel.setText("Output -> Mic | " + juce::String(measuredLevel, 1)
                            + " dBFS | Delay total termasuk latensi perangkat", juce::dontSendNotification);
    else if (!hasReference)
        statusLabel.setText("RTA Mic " + juce::String(measuredLevel, 1)
                            + " dBFS - delay perlu loopback referensi pada kanal lain",
                            juce::dontSendNotification);
    else if (referenceLevel < -80.0f)
        statusLabel.setText("Sinyal input terlalu kecil - naikkan gain atau gunakan loopback",
                            juce::dontSendNotification);
    else if (measuredLevel < -80.0f)
        statusLabel.setText("Sinyal channel 2 kosong - cek loopback atau cabling input 2",
                            juce::dontSendNotification);
    else
        statusLabel.setText("Ref " + juce::String(referenceLevel, 1) + " dBFS   Mic "
                            + juce::String(measuredLevel, 1) + " dBFS   "
                            + juce::String((int) transferFunction.getSampleRate()) + " Hz / "
                            + juce::String(transferFunction.getFftSize()) + " FFT",
                            juce::dontSendNotification);

    if (hasReference && ++reverbCounter >= 5)
    {
        reverbCounter = 0;

        const auto acoustics = ImpulseResponse::analyse(result.impulseResponse.data(),
                                                        (int) result.impulseResponse.size(),
                                                        result.sampleRate);
        reverbDisplay.setAcoustics(acoustics, result.sampleRate);

        // The table's EDT, T20, T30 and RT60 come from here rather than from the legacy figures
        // above. The legacy fit divides T20 by twenty and doubles T30, so its numbers disagree
        // with these by a factor of two for the same response; showing both side by side is
        // how that disagreement would be found, so the table is the one that is labelled.
        reverbDisplay.setReverbResult(reverbAnalyser.analyse(result.impulseResponse.data(),
                                                             (int) result.impulseResponse.size(),
                                                             result.sampleRate));

        const std::vector<float> bandCentres { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
                                               1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
        reverbDisplay.setBandRt60(ImpulseResponse::bandRt60(result.impulseResponse.data(),
                                                            (int) result.impulseResponse.size(),
                                                            result.sampleRate, bandCentres));
        reverbDisplay.setWaterfall(ImpulseResponse::waterfall(result.impulseResponse.data(),
                                                              (int) result.impulseResponse.size(),
                                                              result.sampleRate, bandCentres));
    }

}

void MainComponent::updateCalibrationLabel()
{
    calibrationLabel.setText (microphoneCalibration.toString(), juce::dontSendNotification);
    fftDisplay.setMicrophoneCalibration (microphoneCalibration);
    applyCalibrationToSplMeter();
}

void MainComponent::applyCalibrationToSplMeter()
{
    // The whole profile is handed over rather than a single offset, because the meter has to
    // know whether the profile was ever measured against a reference. Passing the number alone
    // would let it report a confident SPL from a profile that was merely selected.
    splMeter.setCalibration (microphoneCalibration);

    // The status line says which of the two states the readings are in. A reader looking at a
    // level in decibels has to be able to see whether anything is known about the microphone
    // behind it, and "94 dB" in a box does not say so.
    const auto description = microphoneCalibration.describe();

    if (microphoneCalibration.hasCalibration())
        statusLabel.setColour (juce::Label::textColourId, runningColour);
    else
        statusLabel.setColour (juce::Label::textColourId, stoppedColour);

    statusLabel.setText (description, juce::dontSendNotification);
}

void MainComponent::calibrationMenuClicked()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Tanpa kalibrasi (dBFS)", microphoneCalibration.getModelName() == "Tanpa kalibrasi");
    menu.addItem (2, "Dayton Audio iMM-6c",
                  microphoneCalibration.getModelName() == "Dayton Audio iMM-6c");
    menu.addSeparator();
    // The workflow an acoustic calibrator exists for: put the microphone in the calibrator,
    // set the box to the level it is marked with, and the correction is whatever this chain
    // read against it. Nothing here asks for a sensitivity figure, because the measurement is
    // the authority and a typed number would not be.
    menu.addItem (5, "Kalibrasi dengan acoustic calibrator...");
    menu.addItem (6, "Simpan profil kalibrasi ke file...");
    menu.addItem (3, "Muat kurva kalibrasi (TXT/CSV)...");
    menu.addItem (4, "Reset kalibrasi", microphoneCalibration.getModelName() == "Tanpa kalibrasi");

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (&calibrationButton),
                        [this] (int result)
                        {
                            if (result == 1)
                            {
                                microphoneCalibration = MicrophoneCalibration();
                            }
                            else if (result == 2)
                            {
                                microphoneCalibration = MicrophoneCalibration::daytonImm6c();

                                // SPL readings sit far above the default 0 to -120 dB
                                // window, so the range has to follow the calibration.
                                statusLabel.setText ("Kalibrasi aktif: level kini dB SPL, bukan dBFS. "
                                                     "Pilih rentang display yang sesuai bila grafik terpotong",
                                                     juce::dontSendNotification);
                            }
            else if (result == 5)
            {
                runCalibratorWorkflow();
            }
            else if (result == 6)
            {
                saveCalibrationProfile();
            }
                            else if (result == 3)
                            {
                                // Dayton's download tool saves the file as .txt, so both are offered.
                                auto* chooser = new juce::FileChooser ("Muat kurva kalibrasi mikrofon",
                                                                          juce::File::getCurrentWorkingDirectory(),
                                                                          "*.txt;*.csv;*.cal;*.dat");
                                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                                          | juce::FileBrowserComponent::canSelectFiles,
                                                      [this, chooser] (const juce::FileChooser& fc)
                                                      {
                                                          const auto file = fc.getResult();
                                                          delete chooser;

                                                          if (file == juce::File())
                                                              return;

                                                          if (! microphoneCalibration.loadFile (file))
                                                          {
                                                              statusLabel.setText ("Kurva kalibrasi gagal dibaca dari "
                                                                                   + file.getFileName()
                                                                                   + ": butuh minimal 2 baris \"frekuensi,dB\"",
                                                                                    juce::dontSendNotification);
                                                              return;
                                                          }

                                                          microphoneCalibration.setEnabled (true);
                                                          microphoneCalibration.setModelName (file.getFileNameWithoutExtension());

                                                          if (microphoneCalibration.hasMeasuredSensitivity())
                                                              statusLabel.setText ("Kurva dimuat: sensitivitas mikrofon "
                                                                                   + juce::String (microphoneCalibration.getSensitivityDb(), 1)
                                                                                   + " dB SPL dari file kalibrasi",
                                                                                    juce::dontSendNotification);
                                                          else
                                                              statusLabel.setText ("Kurva dimuat: "
                                                                                   + juce::String ((int) microphoneCalibration.getCurve().size())
                                                                                   + " titik dari " + file.getFileName()
                                                                                   + " (sensitivitas memakai nilai nominal, "
                                                                                   + "file tidak memuat spesifikasi sensitivitas)",
                                                                                    juce::dontSendNotification);
                                                          return;

                                                      });
                                return;
                            }
                            else if (result == 4)
                            {
                                microphoneCalibration = MicrophoneCalibration();
                            }
                            else
                            {
                                return;
                            }

                            updateCalibrationLabel();
                        });
}

juce::File MainComponent::snapshotsDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("OpenSmaartLab")
             .getChildFile ("RTA");
}

// Where the user last saved, remembered so the load list and the file chooser can
// start there rather than always falling back to the default folder.
juce::File MainComponent::lastSnapshotDirectory() const
{
    if (lastSnapshotFolder.isDirectory())
        return lastSnapshotFolder;

    // Nothing remembered yet: look where snapshots tend to end up, including a
    // folder named AUDIO in the home directory, which is a common choice.
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    for (const auto& name : { juce::String ("AUDIO"), juce::String ("RTA") })
    {
        const auto candidate = home.getChildFile (name);

        if (candidate.isDirectory()
             && ! candidate.findChildFiles (juce::File::findFiles, false, "*.rta.csv").isEmpty())
            return candidate;
    }

    return snapshotsDirectory();
}

juce::String MainComponent::sanitiseName(const juce::String& name)
{
    auto cleaned = name.trim();

    for (const auto character : juce::String ("\\/:*?\"<>|"))
        cleaned.replace (juce::String (character), "_");

    return cleaned.isEmpty() ? juce::String ("RTA") : cleaned;
}

void MainComponent::saveSnapshotClicked()
{
    if (! isRunning)
    {
        statusLabel.setText ("Jalankan pengukuran dulu sebelum menyimpan RTA", juce::dontSendNotification);
        return;
    }

    auto* chooser = new juce::FileChooser ("Simpan hasil RTA",
                                          snapshotsDirectory(),
                                          "*.rta.csv");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, chooser] (const juce::FileChooser& fc)
                         {
                             auto file = fc.getResult();
                             delete chooser;

                             if (file == juce::File())
                                 return;

                             if (! file.hasFileExtension (".csv"))
                                 file = file.withFileExtension (".rta.csv");

                             file.getParentDirectory().create();

                             // writeTo creates the parent directory itself, but doing it here as well means the
                             // failure is reported before any chooser work.
                             file.getParentDirectory().create();

                             if (! fftDisplay.writeSnapshotTo (file))
                             {
                                 statusLabel.setText ("Gagal menyimpan RTA ke " + file.getFullPathName(),
                                                      juce::dontSendNotification);
                                 return;
                             }

                             lastSnapshotFolder = file.getParentDirectory();
                             statusLabel.setText ("RTA disimpan: " + file.getFileName()
                                                  + "  di " + file.getParentDirectory().getFullPathName(),
                                                  juce::dontSendNotification);
                         });
}

// Snapshot files are listed from two places: the default folder, and the folder the
// user actually saved into. Saving goes through a file chooser, so files can land
// anywhere, and a list built from one hardcoded folder would miss them.
juce::Array<juce::File> MainComponent::collectSnapshots() const
{
    juce::Array<juce::File> found;

    const auto addFrom = [&found] (const juce::File& directory)
    {
        if (! directory.isDirectory())
            return;

        // Newest first, so the most recent measurement is the first thing offered.
        auto files = directory.findChildFiles (juce::File::findFiles, false, "*.rta.csv");
        struct NewestFirst
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                if (a.getLastModificationTime() == b.getLastModificationTime())
                    return 0;

                return a.getLastModificationTime() > b.getLastModificationTime() ? -1 : 1;
            }
        };

        NewestFirst newestFirst;
        files.sort (newestFirst);

        for (const auto& file : files)
            if (! found.contains (file))
                found.add (file);
    };

    addFrom (snapshotsDirectory());
    addFrom (lastSnapshotDirectory());

    return found;
}

void MainComponent::populateSnapshotMenu()
{
    snapshotMenu.clear();
    snapshotMenu.addItem (1, "Hapus tampilan RTA tersimpan");
    snapshotMenu.addItem (2, "Pilih folder lain...");
    snapshotMenu.addSeparator();

    snapshotFiles = collectSnapshots();

    if (snapshotFiles.isEmpty())
    {
        snapshotMenu.addItem (3, "(belum ada RTA tersimpan)", false);
        return;
    }

    for (int i = 0; i < snapshotFiles.size(); ++i)
    {
        const auto& file = snapshotFiles[i];
        auto label = file.getFileName();

        // Include the folder when it is not the default one, otherwise the list is
        // ambiguous once files from two directories are mixed.
        if (file.getParentDirectory() != snapshotsDirectory())
            label = file.getParentDirectory().getFileName() + "/" + label;

        snapshotMenu.addItem (10 + i, label);
    }
}

void MainComponent::loadSnapshotClicked()
{
    populateSnapshotMenu();

    snapshotMenu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetComponent (&loadSnapshotButton),
                                [this] (int result)
                                {
                                    if (result == 1)
                                    {
                                        fftDisplay.clearSnapshot();
                                        statusLabel.setText ("Kembali ke RTA langsung", juce::dontSendNotification);
                                        return;
                                    }

                                    if (result == 2)
                                    {
                                        chooseSnapshotFile();
                                        return;
                                    }

                                    if (result < 10 || result - 10 >= snapshotFiles.size())
                                        return;

                                    loadSnapshotFile (snapshotFiles[result - 10]);
                                });
}

void MainComponent::chooseSnapshotFile()
{
    // Start where the files were last saved, so repeat loads do not begin at Home.
    auto startDirectory = snapshotsDirectory();

    if (startDirectory.isDirectory())
        startDirectory = lastSnapshotDirectory();
    else if (lastSnapshotDirectory().isDirectory())
        startDirectory = lastSnapshotDirectory();

    auto* chooser = new juce::FileChooser ("Muat hasil RTA tersimpan", startDirectory,
                                          "*.rta.csv;*.csv");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this, chooser] (const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             delete chooser;

                             if (file == juce::File())
                                 return;

                             loadSnapshotFile (file);
                         });
}

void MainComponent::loadSnapshotFile(const juce::File& file)
{
    if (! file.existsAsFile())
    {
        statusLabel.setText ("File tidak ditemukan: " + file.getFullPathName(),
                             juce::dontSendNotification);
        return;
    }

    RTASnapshot loaded;

    if (! RTASnapshot::readFrom (file, loaded))
    {
        statusLabel.setText ("Gagal membaca " + file.getFileName()
                             + ": format bukan hasil simpan RTA",
                             juce::dontSendNotification);
        return;
    }

    fftDisplay.showSnapshot (loaded);
    lastSnapshotFolder = file.getParentDirectory();

    auto message = juce::String ("Memuat RTA: ") + loaded.name
                 + juce::String ("  (") + juce::String ((int) loaded.frequency.size()) + " titik, "
                 + juce::String ((int) loaded.traces.size()) + " kanal) dari "
                 + file.getFileName();

    if (loaded.calibrationText.isNotEmpty())
        message += "  [" + loaded.calibrationText + "]";

    statusLabel.setText (message, juce::dontSendNotification);
}

void MainComponent::exportCSVClicked()
{
    if (!isRunning)
        return;

    auto* chooser = new juce::FileChooser("Export CSV",
                                          juce::File::getCurrentWorkingDirectory(),
                                          "*.csv");

    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, chooser] (const juce::FileChooser& fc)
                         {
                             const juce::File file = fc.getResult();
                             delete chooser;

                             if (file == juce::File())
                                 return;

                             juce::FileOutputStream stream(file);

                             if (!stream.openedOk())
                                 return;

                             stream.writeText("# OpenSmaartLab - sample rate "
                                                  + juce::String((int) lastResult.sampleRate)
                                                  + " Hz, FFT " + juce::String(lastResult.fftSize)
                                                  + ", averaging " + juce::String(lastResult.averages)
                                                  + ", delay " + juce::String(lastResult.delayMs, 3)
                                                  + " ms\n",
                                              false, false, nullptr);

                             fftDisplay.writeCsv(stream);
                         });
}

void MainComponent::exportIrClicked()
{
    if (lastResult.impulseResponse.empty())
    {
        statusLabel.setText("Belum ada impuls response untuk diekspor", juce::dontSendNotification);
        return;
    }

    auto* chooser = new juce::FileChooser("Export IR WAV",
                                          juce::File::getCurrentWorkingDirectory(),
                                          "*.wav");

    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, chooser] (const juce::FileChooser& fc)
                         {
                             const juce::File file = fc.getResult();
                             delete chooser;

                             if (file == juce::File())
                                 return;

                             juce::WavAudioFormat wav;
                             std::unique_ptr<juce::AudioFormatWriter> writer(
                                 wav.createWriterFor(new juce::FileOutputStream(file),
                                                     lastResult.sampleRate, 1, 32, {}, 0));

                             if (writer != nullptr)
                             {
                                 auto* data = lastResult.impulseResponse.data();
                                 writer->writeFromAudioSampleBuffer(
                                     juce::AudioSampleBuffer(&data, 1, (int) lastResult.impulseResponse.size()), 0,
                                     (int) lastResult.impulseResponse.size());
                                 statusLabel.setText("IR disimpan: " + file.getFileName(),
                                                     juce::dontSendNotification);
                             }
                         });
}

void MainComponent::spectrumControlsChanged()
{
    dspWorker.setFftSize(spectrumDisplay.getFftSize());
    dspWorker.setWindow(spectrumDisplay.getWindowType());
    dspWorker.setOverlapPercent(spectrumDisplay.getOverlapPercent());
    dspWorker.setAveraging(spectrumDisplay.getAveragingMode(),
                           spectrumDisplay.getAveragingSeconds());
}

void MainComponent::rtaControlsChanged()
{
    dspWorker.setRtaResolution (rtaDisplay.getResolution());
    dspWorker.setRtaRange (rtaDisplay.getRangeLow(), rtaDisplay.getRangeHigh());

    // The octave selector on the display is what the user can reach now that the separate RTA
    // tab is gone, so it is what the analysis is configured from. Reading it here rather than
    // only on the change event means the two can never start out disagreeing.
    fftDisplay.onOctaveChanged = [this] { rtaControlsChanged(); };
}

void MainComponent::applyAudioSettingsToDevice()
{
    // A rebuild in progress is this class changing the controls to match the device, not the
    // reader changing a setting, so it must not reopen anything.
    if (rebuildingSelectors)
        return;

    // Nothing to reopen while the engine is not running: the values are read again by
    // the next start, so changing them here would only be noise on screen.
    if (! isRunning)
        return;

    // Reopening the device interrupts the stream, so it is only worth doing when a value
    // really moved. Without this, a notification carrying the value that is already applied
    // still drops the session for nothing.
    const auto rate = sampleRateSelector.getSelectedId();
    const auto bufferSize = bufferSizeSelector.getSelectedId();

    if (rate == appliedRateId && bufferSize == appliedBufferSizeId)
        return;

    appliedRateId = rate;
    appliedBufferSizeId = bufferSize;

    restartAudio();
}

void MainComponent::pumpMeasurementWorker()
{
    const auto needsWorkerSnapshot = inputLevelPanel.isVisible()
                                  || spectrumDisplay.isVisible()
                                  || rtaDisplay.isVisible();

    if (audioEngine.isRunning())
    {
        // Only the samples this reader has not seen, so the analyser frames a continuous
        // stream instead of the same window over and over.
        audioEngine.readNewSamples(workerLeft, workerRight, &workerGenerated);

        if (! workerLeft.empty())
        {
            dsp::assignMeasurementAndReference (measurementChannelIndex(), workerLeft,
                                                workerRight, analysisMeasurement,
                                                analysisReference);

            // Match the Transfer Function's routing: the played generator is the reference
            // when it is active, otherwise use the selected second input channel.
            if (generatorOn && workerGenerated.size() == analysisMeasurement.size())
                analysisReference = workerGenerated;

            const auto inputGainDb = analysisInputGainDb();
            if (analysisReference.size() == analysisMeasurement.size())
                splMeter.process (analysisReference.data(), analysisMeasurement.data(),
                                  (int) analysisMeasurement.size(), inputGainDb);

            applyInputGain (analysisMeasurement,
                            juce::Decibels::decibelsToGain (inputGainDb));
        }

        if (needsWorkerSnapshot && !workerLeft.empty())
            dspWorker.pushSamples(workerLeft.data(),
                                  workerRight.empty() ? nullptr : workerRight.data(),
                                  analysisMeasurement.data(),
                                  analysisReference.data(),
                                  workerGenerated.empty() ? nullptr : workerGenerated.data(),
                                  (int) analysisMeasurement.size());
    }

    if (! needsWorkerSnapshot)
        return;

    DspWorker::Snapshot snapshot;

    if (!dspWorker.getSnapshot(snapshot))
        return;

    if (inputLevelPanel.isVisible())
        inputLevelPanel.setSnapshot(snapshot);

    if (spectrumDisplay.isVisible() && snapshot.spectrumValid
         && !snapshot.spectrumDb.empty())
    {
        spectrumDisplay.setSpectrum(snapshot.spectrumFrequency, snapshot.spectrumDb,
                                    "FFT " + juce::String(snapshot.spectrumFftSize)
                                        + " | " + juce::String((int)snapshot.sampleRate) + " Hz");
    }

    if (rtaDisplay.isVisible() && snapshot.rtaValid && !snapshot.rtaBands.empty())
    {
        // The unresolved count is on screen because a band narrower than one FFT bin has
        // no level worth reading, and hiding that would make a wrong FFT size look fine.
        juce::String status = juce::String (snapshot.rtaResolution) + " oktaf, "
                            + juce::String (snapshot.rtaBands.size()) + " band, FFT "
                            + juce::String (snapshot.spectrumFftSize);

        if (snapshot.rtaUnresolvedBands > 0)
            status += " | " + juce::String (snapshot.rtaUnresolvedBands)
                      + " band lebih sempit dari 1 bin (merah)";

        rtaDisplay.setBands(snapshot.rtaBands, snapshot.rtaFrequencies, status);
    }
}

juce::Rectangle<int> MainComponent::topBarBounds() const
{
    return getLocalBounds().withTrimmedTop(8).withHeight(154).reduced(12, 0);
}

juce::Rectangle<int> MainComponent::statusBarBounds() const
{
    return getLocalBounds().removeFromBottom(26).reduced(12, 0);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto top = topBarBounds();
    g.setColour(barColour);
    g.fillRoundedRectangle(top.toFloat(), 6.0f);

    auto status = statusBarBounds();
    g.setColour(barColour);
    g.fillRoundedRectangle(status.toFloat(), 4.0f);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    auto status = statusBarBounds();
    auto top = topBarBounds();

    statusLabel.setBounds(status.removeFromLeft(560));
    status.removeFromLeft(10);
    delayLabel.setBounds(status.removeFromLeft(200));
    status.removeFromLeft(10);
    coherenceLabel.setBounds(status.removeFromLeft(220));

    auto controls = top.reduced(12, 12);

    auto devices = controls.removeFromTop(42);
    auto inputArea = devices.removeFromLeft((devices.getWidth() - 16) / 2);
    devices.removeFromLeft(16);
    inputLabel.setBounds(inputArea.removeFromLeft(100));
    inputSelector.setBounds(inputArea.reduced(0, 3));
    outputLabel.setBounds(devices.removeFromLeft(110));
    outputSelector.setBounds(devices.reduced(0, 3));
    // Start sits at the far right, next to Kalibrasi Mic: it is the one control that has to
    // be found and pressed first, and the far right corner is where the eye ends up after
    // reading the input and output pickers it depends on.
    auto startStopArea = controls.removeFromRight (160).reduced (0, 3);
    startStopButton.setBounds (startStopArea.withSizeKeepingCentre (startStopArea.getWidth(),
                                                                    startStopArea.getHeight()));
    controls.removeFromLeft (8);
    calibrationButton.setBounds (controls.removeFromRight (130).reduced (0, 3));
    controls.removeFromLeft (6);
    saveSnapshotButton.setBounds (controls.removeFromRight (110).reduced (0, 3));
    controls.removeFromLeft (6);
    loadSnapshotButton.setBounds (controls.removeFromRight (100).reduced (0, 3));
    controls.removeFromLeft (6);
    auto generatorRow = controls.removeFromBottom(34);
    const auto compactGeneratorRow = generatorRow.getWidth() < 1100;
    pinkNoiseButton.setBounds (generatorRow.removeFromLeft (compactGeneratorRow ? 160 : 180));
    generatorRow.removeFromLeft (compactGeneratorRow ? 10 : 12);
    pinkNoiseLevel.setBounds (generatorRow.removeFromLeft (compactGeneratorRow ? 200 : 250));
    generatorRow.removeFromLeft (8);
    inputGainLabel.setBounds (generatorRow.removeFromLeft (compactGeneratorRow ? 72 : 76));
    inputGainSlider.setBounds (generatorRow.removeFromLeft (compactGeneratorRow ? 190 : 210));
    generatorRow.removeFromLeft (compactGeneratorRow ? 8 : 12);
    // Taken from the right of the hint rather than from the crowded settings row above, which
    // is already within a few pixels of the window width.
    temperatureSelector.setBounds (generatorRow.removeFromRight (compactGeneratorRow ? 120 : 132));
    generatorRow.removeFromRight (compactGeneratorRow ? 8 : 10);
    pinkNoiseHint.setBounds(generatorRow);
    controls = controls.removeFromTop(40);
    measurementChannelSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    referenceChannelSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    sampleRateSelector.setBounds(controls.removeFromLeft(100));
    controls.removeFromLeft(8);
    bufferSizeSelector.setBounds(controls.removeFromLeft(100));
    controls.removeFromLeft(12);
    fftSizeSelector.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(8);
    averagingSelector.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(12);
    exportButton.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(8);
    exportIrButton.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(12);
    startHintLabel.setBounds(controls.removeFromLeft(430));

    tabs.setBounds(area.reduced(12, 0).withTrimmedTop(166).withTrimmedBottom(34));

    layoutGenerator();
}

void MainComponent::componentMovedOrResized(juce::Component& component, bool, bool wasResized)
{
    if (&component == &generatorPanel && wasResized)
        layoutGenerator();
}

// TextEditor hides its focus state, so the flag is tracked from the listener events
// that can change it rather than guessed at paint time.
bool MainComponent::isEditingGeneratorFrequency() const
{
    return frequencyEditorActive;
}

bool MainComponent::isEditingGeneratorBandLow() const
{
    return bandLowEditorActive;
}

void MainComponent::updateGeneratorBandLowEditor()
{
    if (isEditingGeneratorBandLow())
        return;

    const auto value = audioEngine.getGenerator().getBandLow();
    generatorBandLowEditor.setText (juce::String (value, std::fmod (value, 1.0f) > 0.05f ? 1 : 0),
                                    juce::dontSendNotification);
}

void MainComponent::FrequencyEditorListener::textEditorEscapeKeyPressed (juce::TextEditor& editor)
{
    component.frequencyEditorActive = false;
    component.updateGeneratorFrequencyEditor();

    if (&editor == &component.generatorBandLowEditor)
    {
        component.bandLowEditorActive = false;
        component.updateGeneratorBandLowEditor();
    }
}

void MainComponent::applyTypedBandLow()
{
    const auto typed = generatorBandLowEditor.getText().trim();

    if (typed.isEmpty())
    {
        updateGeneratorBandLowEditor();
        return;
    }

    const auto parsed = typed.getFloatValue();

    if (parsed < 10.0f)
    {
        statusLabel.setText ("Batas bawah minimal 10 Hz", juce::dontSendNotification);
        updateGeneratorBandLowEditor();
        return;
    }

    auto& generator = audioEngine.getGenerator();
    const auto clamped = juce::jmin (parsed, 2000.0f);

    generator.setBandLimits (clamped, generator.getBandHigh());
    generatorBandLowSlider.setValue ((double) generator.getBandLow(), juce::dontSendNotification);
    updateGeneratorBandLowEditor();
    syncGeneratorBandPreset();
    updateGeneratorInfo();

    statusLabel.setText ("Pink noise: " + juce::String (generator.getBandLow(), 1)
                         + " Hz - " + juce::String (generator.getBandHigh(), 1) + " Hz",
                         juce::dontSendNotification);
}

void MainComponent::FrequencyEditorListener::textEditorReturnKeyPressed (juce::TextEditor& editor)
{
    if (&editor == &component.generatorBandLowEditor)
    {
        component.bandLowEditorActive = false;
        component.applyTypedBandLow();
        return;
    }

    component.frequencyEditorActive = false;
    component.applyTypedFrequency();
}

void MainComponent::FrequencyEditorListener::textEditorFocusLost (juce::TextEditor& editor)
{
    if (&editor == &component.generatorBandLowEditor)
    {
        component.bandLowEditorActive = false;
        component.updateGeneratorBandLowEditor();
        return;
    }

    component.frequencyEditorActive = false;
    component.applyTypedFrequency();
}

void MainComponent::applyTypedFrequency()
{
    const auto typed = generatorFrequencyEditor.getText().trim();

    if (typed.isEmpty())
    {
        updateGeneratorFrequencyEditor();
        return;
    }

    const auto parsed = typed.getFloatValue();
    auto& generator = audioEngine.getGenerator();
    const auto pink = generator.getType() == SignalGenerator::Type::Pink;

    // Pink noise is limited to a band, so the accepted range is narrower than a single
    // tone and the low end cannot go below the existing band floor.
    const auto low = pink ? 100.0f : 1.0f;
    const auto high = 20000.0f;

    if (parsed < low)
    {
        statusLabel.setText ("Frekuensi minimal " + juce::String ((int) low) + " Hz",
                             juce::dontSendNotification);
        updateGeneratorFrequencyEditor();
        return;
    }

    const auto clamped = juce::jlimit (low, high, parsed);

    if (pink)
    {
        generator.setBandLimits (generator.getBandLow(), clamped);
        syncGeneratorBandPreset();
    }
    else
    {
        generator.setFrequency (clamped);
    }

    generatorFrequencySlider.setValue ((double) clamped, juce::dontSendNotification);
    updateGeneratorFrequencyEditor();

    if (std::abs (clamped - parsed) > 0.05f)
        statusLabel.setText ("Nilai dibatasi ke " + juce::String ((int) high) + " Hz",
                             juce::dontSendNotification);
    else
        statusLabel.setText (pink ? "Pink noise dibatasi sampai "
                                    + juce::String (parsed, 1) + " Hz (bawah "
                                    + juce::String (generator.getBandLow(), 1) + " Hz)"
                                  : "Frekuensi generator: " + juce::String (parsed, 1) + " Hz",
                             juce::dontSendNotification);
}

void MainComponent::updateGeneratorFrequencyEditor()
{
    if (isEditingGeneratorFrequency())
        return;

    const auto value = (float) generatorFrequencySlider.getValue();

    // Decimals are shown only when they matter, so 1000 stays "1000" but 997.5 keeps
    // its decimal. Measurement frequencies are never rounded away.
    generatorFrequencyEditor.setText (juce::String (value, value < 100.0f || std::fmod (value, 1.0f) > 0.05f ? 1 : 0),
                                      juce::dontSendNotification);
}

void MainComponent::applyGeneratorBandPreset(int index)
{
    const auto presets = SignalGenerator::getBandPresets();

    if (index < 0 || index >= presets.size())
        return;

    const auto& preset = presets[index];
    auto& generator = audioEngine.getGenerator();

    generator.setBandLimits (preset.lowFrequency, preset.highFrequency);

    // Sliders and boxes follow without firing their own callbacks, or each would push
    // its own value back into the generator while the other two are still stale.
    generatorBandLowSlider.setValue ((double) generator.getBandLow(), juce::dontSendNotification);
    generatorFrequencySlider.setValue ((double) generator.getBandHigh(), juce::dontSendNotification);
    updateGeneratorBandLowEditor();
    updateGeneratorFrequencyEditor();

    statusLabel.setText ("Pink noise band " + preset.name, juce::dontSendNotification);
    updateGeneratorInfo();
}

int MainComponent::measurementChannelIndex() const
{
    return juce::jlimit (0, 1, measurementChannelSelector.getSelectedId() - 1);
}

int MainComponent::referenceChannelIndex() const
{
    return juce::jlimit (0, 1, referenceChannelSelector.getSelectedId() - 1);
}

void MainComponent::keepMeasurementAndReferenceDistinct()
{
    const auto measurement = measurementChannelIndex();

    // Comparing a channel with itself would report a flat, meaningless transfer
    // function, so the reference moves to the other input instead.
    if (referenceChannelIndex() != measurement)
        return;

    referenceChannelSelector.setSelectedId (measurement == 0 ? 2 : 1, juce::dontSendNotification);
    statusLabel.setText ("Pengukuran dan referensi harus memakai kanal berbeda, referensi dipindah ke "
                         + AudioEngine::channelName (measurement == 0 ? 1 : 0),
                         juce::dontSendNotification);
}

void MainComponent::resetReferenceTracking()
{
    // A different reference channel means the old averages describe the old wiring.
    referenceFrames = 0;
    transferFunction.reset();
    fftDisplay.clearPeakHold();
    fftDisplay.setDelayAvailable(false);
    fftDisplay.setChannelLabels (AudioEngine::channelName (measurementChannelIndex()),
                                 AudioEngine::channelName (referenceChannelIndex()));
    delayLabel.setText ("Delay --", juce::dontSendNotification);
}

juce::String MainComponent::generatorOutputLabel() const
{
    if (outputSelector.getSelectedId() <= 1)
        return juce::String("-");

    const auto device = outputSelector.getText();
    const auto side = currentGeneratorRouting() == AudioEngine::OutputRouting::Left ? " (kiri)"
                    : currentGeneratorRouting() == AudioEngine::OutputRouting::Right ? " (kanan)"
                                                                                    : juce::String();

    return device + side;
}

AudioEngine::OutputRouting MainComponent::currentGeneratorRouting() const
{
    switch (generatorOutputRoutingSelector.getSelectedId())
    {
        case 2:  return AudioEngine::OutputRouting::Left;
        case 3:  return AudioEngine::OutputRouting::Right;
        default: return AudioEngine::OutputRouting::Both;
    }
}

void MainComponent::syncGeneratorBandPreset()
{
    const auto& generator = audioEngine.getGenerator();
    const auto presets = SignalGenerator::getBandPresets();
    const auto index = SignalGenerator::findBandPreset (generator.getBandLow(),
                                                        generator.getBandHigh());

    // A hand set band reports itself as such, so the list never claims a driver band
    // the signal does not actually match.
    generatorBandPresetSelector.setSelectedId (index >= 0 ? index + 1 : 99,
                                               juce::dontSendNotification);
}

void MainComponent::updateGeneratorGeneratorControls()
{
    const auto type = audioEngine.getGenerator().getType();
    const auto& generator = audioEngine.getGenerator();
    const auto sine = type == SignalGenerator::Type::Sine;
    const auto pink = type == SignalGenerator::Type::Pink;
    const auto sweep = type == SignalGenerator::Type::LogSweep;

    // The same frequency control means different things per signal type: a single
    // tone for sine, the upper band limit for pink noise. The label follows so the
    // meaning is never ambiguous, and only the truly irrelevant controls are dimmed.
    if (sine)
        generatorFrequencyLabel.setText ("Frekuensi", juce::dontSendNotification);
    else if (pink)
        generatorFrequencyLabel.setText ("Batas atas pink noise",
                                         juce::dontSendNotification);
    else
        generatorFrequencyLabel.setText ("Frekuensi",
                                         juce::dontSendNotification);

    const auto live = sine || pink;
    generatorFrequencyLabel.setEnabled (live);
    generatorBandLowLabel.setEnabled (pink);
    generatorBandLowSlider.setEnabled (pink);
    generatorBandLowEditor.setEnabled (pink);
    generatorBandLowEditor.setAlpha (pink ? 1.0f : 0.4f);
    generatorBandPresetLabel.setEnabled (pink);
    generatorBandPresetSelector.setEnabled (pink);
    generatorBandPresetSelector.setAlpha (pink ? 1.0f : 0.4f);
    generatorFrequencySlider.setEnabled (live);
    generatorFrequencyEditor.setEnabled (live);
    generatorFrequencyEditor.setAlpha (live ? 1.0f : 0.4f);

    for (auto* label : { &generatorSweepStartLabel, &generatorSweepEndLabel,
                         &generatorSweepDurationLabel })
        label->setEnabled (sweep);

    generatorSweepStartSlider.setEnabled (sweep);
    generatorSweepEndSlider.setEnabled (sweep);
    generatorSweepDurationSlider.setEnabled (sweep);

    // Point the control at whatever it currently means for this type.
    if (pink)
    {
        generatorFrequencySlider.setValue ((double) generator.getBandHigh(),
                                          juce::dontSendNotification);
        generatorBandLowSlider.setValue ((double) generator.getBandLow(),
                                         juce::dontSendNotification);
        updateGeneratorBandLowEditor();
        syncGeneratorBandPreset();
    }

    updateGeneratorFrequencyEditor();
}

void MainComponent::layoutGenerator()
{
    auto area = generatorPanel.getLocalBounds().reduced(20);
    const auto controlWidth = juce::jlimit(360, 580, area.getWidth() / 2);
    auto panel = area.removeFromLeft(controlWidth);
    area.removeFromLeft(20);
    generatorDisplay.setBounds(area);

    {
        auto generatorRow = panel.removeFromTop (44);
        generatorButton.setBounds (generatorRow.removeFromLeft (160));
        generatorMuteButton.setBounds (generatorRow.removeFromLeft (110).reduced (0, 6));
    }

    generatorInfoLabel.setBounds(panel.removeFromTop(40).reduced(0, 8));
    panel.removeFromTop(10);

    auto column = panel.removeFromLeft(panel.getWidth() / 2).withTrimmedRight(12);

    generatorTypeLabel.setBounds(column.removeFromTop(24));
    generatorTypeSelector.setBounds(column.removeFromTop(36));
    column.removeFromTop(12);

    generatorLevelLabel.setBounds(column.removeFromTop(24));
    generatorLevelSlider.setBounds(column.removeFromTop(36).reduced(0, 4));
    column.removeFromTop(12);

    generatorBandPresetLabel.setBounds(column.removeFromTop(24));
    generatorBandPresetSelector.setBounds(column.removeFromTop(36));
    column.removeFromTop(12);

    generatorOutputRoutingLabel.setBounds(column.removeFromTop(24));
    generatorOutputRoutingSelector.setBounds(column.removeFromTop(36));
    column.removeFromTop(12);

    generatorFrequencyLabel.setBounds(column.removeFromTop(24));
    auto frequencyRow = column.removeFromTop(36).reduced(0, 4);
    generatorFrequencyEditor.setBounds(frequencyRow.removeFromLeft(70).reduced(0, 2));
    frequencyRow.removeFromLeft(6);
    generatorFrequencySlider.setBounds(frequencyRow);
    column.removeFromTop(12);

    generatorBandLowLabel.setBounds(column.removeFromTop(24));
    auto bandLowRow = column.removeFromTop(36).reduced(0, 4);
    generatorBandLowEditor.setBounds(bandLowRow.removeFromLeft(70).reduced(0, 2));
    bandLowRow.removeFromLeft(6);
    generatorBandLowSlider.setBounds(bandLowRow);

    auto sweepColumn = panel;

    generatorSweepStartLabel.setBounds(sweepColumn.removeFromTop(24));
    generatorSweepStartSlider.setBounds(sweepColumn.removeFromTop(36).reduced(0, 4));
    sweepColumn.removeFromTop(12);

    generatorSweepEndLabel.setBounds(sweepColumn.removeFromTop(24));
    generatorSweepEndSlider.setBounds(sweepColumn.removeFromTop(36).reduced(0, 4));
    sweepColumn.removeFromTop(12);

    generatorSweepDurationLabel.setBounds(sweepColumn.removeFromTop(24));
    generatorSweepDurationSlider.setBounds(sweepColumn.removeFromTop(36).reduced(0, 4));
}

float MainComponent::measuredMicLevelDbfs() const
{
    return splMeter.getMeasuredMicLevelDbfs();
}

void MainComponent::runCalibratorWorkflow()
{
    if (! isRunning)
    {
        statusLabel.setText ("Kalibrasi: mulai capture, pasang acoustic calibrator pada mikrofon, "
                             "lalu tekan Kalibrasi Mic di tab SPL Meter",
                             juce::dontSendNotification);
        return;
    }

    const auto reading = measuredMicLevelDbfs();

    if (! std::isfinite (reading) || reading < -80.0f)
    {
        // Better to say nothing was heard than to calibrate against a floor. A calibrator that
        // is off, muted, or on the wrong input would produce a profile that is confidently
        // wrong, and nothing on screen would look wrong.
        statusLabel.setText ("Tidak ada sinyal mic: nyalakan acoustic calibrator dan pastikan "
                             "level terbaca sebelum kalibrasi",
                             juce::dontSendNotification);
        return;
    }

    // Ask for the level printed on the acoustic calibrator; 94 and 114 dB are both common,
    // and assuming one would scale every subsequent SPL reading incorrectly.
    auto* prompt = new juce::AlertWindow (
        "Kalibrasi mikrofon",
        "Cara kalibrasi:\n"
          + juce::String ("1. Pasang acoustic calibrator terkalibrasi rapat pada kapsul mikrofon.\n")
          + "2. Nyalakan calibrator dan tunggu level stabil.\n"
          + "3. Masukkan level dB SPL yang tercetak pada calibrator (mis. 94 atau 114).\n\n"
          + "Level mic yang terbaca sekarang: " + juce::String (reading, 1) + " dBFS.",
        juce::MessageBoxIconType::InfoIcon);

    prompt->addTextEditor ("level", "94", "dB SPL");
    prompt->addButton ("Simpan", 1);
    prompt->addButton ("Batal", 0);

    // Captured by value deliberately: the lambda outlives this scope, and the window has to
    // survive until the user answers it. It deletes itself once answered.
    auto* window = prompt;

    prompt->enterModalState (true,
                             juce::ModalCallbackFunction::create (
                                 [this, window, reading] (int result)
                                 {
                                     if (result == 1)
                                     {
                                         const auto reference =
                                             window->getTextEditorContents ("level").getFloatValue();

                                         if (! (reference >= 40.0f && reference <= 160.0f))
                                         {
                                             statusLabel.setText ("Level calibrator di luar "
                                                                  "rentang yang masuk akal "
                                                                  "(40-160 dB SPL)",
                                                                  juce::dontSendNotification);
                                         }
                                         else
                                         {
                                             microphoneCalibration.calibrateAgainst (
                                                 reference, reading,
                                                 microphoneCalibration.getModelName());
                                             applyCalibrationToSplMeter();

                                             statusLabel.setText (
                                                 "Kalibrasi selesai: " + juce::String (reference, 0)
                                                   + " dB SPL dibaca sebagai "
                                                   + juce::String (reading, 1) + " dBFS",
                                                 juce::dontSendNotification);
                                         }
                                     }

                                     delete window;
                                 }));
}

void MainComponent::saveCalibrationProfile()
{
    if (! microphoneCalibration.hasCalibration())
    {
        statusLabel.setText ("Belum ada kalibrasi untuk disimpan", juce::dontSendNotification);
        return;
    }

    auto* chooser = new juce::FileChooser ("Simpan profil kalibrasi mikrofon",
                                           juce::File::getSpecialLocation (
                                               juce::File::userDocumentsDirectory),
                                           "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              delete chooser;

                              if (file == juce::File())
                                  return;

                              statusLabel.setText (
                                  microphoneCalibration.saveToFile (file)
                                      ? "Profil kalibrasi disimpan: " + file.getFileName()
                                      : "Gagal menyimpan profil kalibrasi",
                                  juce::dontSendNotification);
                          });
}
