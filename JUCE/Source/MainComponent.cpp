#include "MainComponent.h"
#include "DSP.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff111318);
    const juce::Colour barColour = juce::Colour(0xff1b1f27);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff8892a0);
    const juce::Colour runningColour = juce::Colour(0xff43a047);
    const juce::Colour stoppedColour = juce::Colour(0xffe53935);
}

MainComponent::MainComponent()
{
    setSize(1480, 920);
    generatorDelayFinder.prepare(65536);
    pinkNoiseButton.onClick = [this] { playPinkNoise(); };
    pinkNoiseLevel.setRange(-60.0, -3.0, 0.5);
    pinkNoiseLevel.setValue(-24.0, juce::dontSendNotification);
    pinkNoiseLevel.setTextValueSuffix(" dBFS");
    pinkNoiseLevel.setSliderStyle(juce::Slider::LinearHorizontal);
    pinkNoiseLevel.setTextBoxStyle(juce::Slider::TextBoxRight, false, 85, 24);
    pinkNoiseLevel.setTooltip("Level pink noise yang dikirim ke perangkat Output / Putar");
    pinkNoiseLevel.onValueChange = [this] { generatorLevelSlider.setValue(pinkNoiseLevel.getValue()); };
    addAndMakeVisible(pinkNoiseButton);
    addAndMakeVisible(pinkNoiseLevel);
    addAndMakeVisible(pinkNoiseHint);

    audioEngine.onStatusMessage = [this] (const juce::String& text)
    {
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

    inputSelector.onChange = [this] { inputSelector.setTooltip(inputSelector.getText()); };
    outputSelector.onChange = [this] { outputSelector.setTooltip(outputSelector.getText()); };
    sampleRateSelector.onChange = [this] { };
    bufferSizeSelector.onChange = [this] { };

    refreshButton.onClick = [this]
    {
        audioEngine.scanDevices();
        refreshDeviceSelectors();
    };

    startStopButton.onClick = [this] { startStopClicked(); };
    exportButton.onClick = [this] { exportCSVClicked(); };
    calibrationButton.onClick = [this] { calibrationMenuClicked(); };
    saveSnapshotButton.onClick = [this] { saveSnapshotClicked(); };
    loadSnapshotButton.onClick = [this] { loadSnapshotClicked(); };
    addAndMakeVisible (calibrationButton);
    addAndMakeVisible (saveSnapshotButton);
    addAndMakeVisible (loadSnapshotButton);
    calibrationLabel.setFont (juce::Font (12.0f));
    calibrationLabel.setColour (juce::Label::textColourId, mutedColour);
    calibrationLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (calibrationLabel);
    calibrationButton.setTooltip ("Kalibrasi mikrofon: pilih model, muat kurva CSV, atau matikan\n"
                                 "Kalibrasi hanya diterapkan pada kanal Mic pada mode RTA Microphone");
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
    addAndMakeVisible(refreshButton);
    addAndMakeVisible(startStopButton);
    addAndMakeVisible(exportButton);
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
        generatorDelayValid = false;
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

    generatorSweepDurationSlider.setRange(1.0, 60.0, 0.5);
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
    tabs.addTab("SPL Meter", backgroundColour, &splMeter, false);
    tabs.addTab("Generator", backgroundColour, &generatorPanel, false);

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
    audioEngine.getGenerator().setRunning(false);
    audioEngine.stop();
}

void MainComponent::refreshDeviceSelectors()
{
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

    inputSelector.setSelectedId(previousInput > 1 && previousInput <= inputs.size() + 1
                                    ? previousInput : juce::jmin(2, inputs.size() + 1),
                                juce::sendNotification);
    outputSelector.setSelectedId(previousOutput > 1 && previousOutput <= outputs.size() + 1
                                    ? previousOutput : juce::jmin(2, outputs.size() + 1),
                                juce::sendNotification);

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
                                    juce::sendNotification);

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
                                    juce::sendNotification);
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
    if (isRunning)
    {
        audioEngine.stop();
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
        startStopButton.setColour(juce::TextButton::buttonColourId, stoppedColour);
        generatorButton.setButtonText("Nyalakan");
        updateGeneratorInfo();
        updateStatus();
        return;
    }

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
    audioEngine.start(inputName, outputName, rate, bufferSize,
                      audioEngine.getOutputPulseSink(outputIndex));

    if (!audioEngine.isRunning())
    {
        startStopButton.setButtonText("Mulai");
        fftDisplay.setDelayAvailable(false);
        delayLabel.setText("Delay --", juce::dontSendNotification);
        fftDisplay.setRunning(false);
        updateStatus();
        return;
    }

    referenceFrames = 0;
    isRunning = true;
    startStopButton.setButtonText("Berhenti");
    startStopButton.setColour(juce::TextButton::buttonColourId, runningColour);

    splMeter.prepare((float) audioEngine.getSampleRate(), 2048);
    transferFunction.prepare((float) audioEngine.getSampleRate(), fftSize);
    transferFunction.setAveraging(averagingSelector.getSelectedId());

    tabs.setCurrentTabIndex(0);
    fftDisplay.setDelayAvailable(false);
    delayLabel.setText("Delay --", juce::dontSendNotification);
    fftDisplay.setRunning(true);
    fftDisplay.clearPeakHold();

    updateStatus();
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
    generatorDelayValid = false;
    generatorDelayCounter = 0;
    generatorStartedAt = juce::Time::getMillisecondCounterHiRes();
    generatorDelayFinder.reset();
    transferFunction.reset();
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

    std::vector<float> output;
    audioEngine.getGeneratorOutput(output);

    generatorDisplay.setSignal(generatorTypeSelector.getText(), (float) generatorLevelSlider.getValue(),
                               generator.getSweepProgress(), generatorOutputLabel());
    generatorDisplay.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                          generator.getFrequency(),
                                          generator.getSweepStart(), generator.getSweepEnd());
    generatorDisplay.setSamples(output, (float) audioEngine.getSampleRate());
}

void MainComponent::updateStatus()
{
    if (isRunning)
    {
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
    if (!isRunning)
        return;

    std::vector<float> ref;
    std::vector<float> meas;

    std::vector<float> left;
    std::vector<float> right;

    std::vector<float> generated;
    audioEngine.getLatestBlock(generatorOn ? 65536 : transferFunction.getFftSize(), left, right, &generated);

    if (left.size() < (size_t) transferFunction.getFftSize()
        || right.size() < (size_t) transferFunction.getFftSize())
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
        const bool ready = broadbandGenerator && ref.size() == 65536
                       && juce::Time::getMillisecondCounterHiRes() - generatorStartedAt
                          > 1000.0 * 65536.0 / audioEngine.getSampleRate();
        if (!ready)
            generatorDelayValid = false;
        else if (generatorDelayCounter++ % 8 == 0)
        {
            const auto lag = generatorDelayFinder.analyse(ref.data(), meas.data(),
                                                          (float) audioEngine.getSampleRate(), 500.0f);
            generatorDelayMs = lag * 1000.0f / (float) audioEngine.getSampleRate();
            generatorDelayValid = generatorDelayMs >= 0.0f && generatorDelayMs < 499.0f
                              && dsp::delayConfidence(ref, meas, juce::roundToInt(lag)) >= 0.2f;
        }
        const auto keep = (size_t) transferFunction.getFftSize();
        if (ref.size() > keep)
        {
            ref.erase(ref.begin(), ref.end() - (ptrdiff_t) keep);
            meas.erase(meas.begin(), meas.end() - (ptrdiff_t) keep);
        }
    }

    double referenceEnergy = 0.0;
    double measuredEnergy = 0.0;

    for (size_t i = 0; i < ref.size(); ++i)
    {
        referenceEnergy += (double) ref[i] * ref[i];
        measuredEnergy += (double) meas[i] * meas[i];
    }

    const auto referenceLevel = dsp::db10((float) (referenceEnergy / std::max<size_t>(1, ref.size())));
    const auto measuredLevel = dsp::db10((float) (measuredEnergy / std::max<size_t>(1, meas.size())));

    // Peak is sampled separately from the running total: RMS squares and averages, so
    // a single loud transient would be invisible in it.
    auto measuredPeak = 0.0f;
    auto referencePeak = 0.0f;

    for (size_t i = 0; i < meas.size(); ++i)
        measuredPeak = std::max (measuredPeak, std::abs (meas[i]));

    for (size_t i = 0; i < ref.size(); ++i)
        referencePeak = std::max (referencePeak, std::abs (ref[i]));

    fftDisplay.setMeasuredLevels (measuredLevel, dsp::db20 (measuredPeak));
    fftDisplay.setReferenceLevels (referenceLevel, dsp::db20 (referencePeak));

    splMeter.process(ref.data(), meas.data(), (int) ref.size());

    // The delay search needs the raw pair, so the button sets a flag and the frame that
    // follows does the work with fresh data.
    if (findDelayRequested)
    {
        findDelayRequested = false;
        transferFunction.findDelay(ref.data(), meas.data());
    }

    auto result = transferFunction.process(ref.data(), meas.data(), (int) ref.size());

    if (!result.valid)
        return;

    const bool hasReference = (generatorOn || channels > 1) && referenceLevel > -80.0f && measuredLevel > -80.0f;
    referenceFrames = hasReference ? referenceFrames + 1 : 0;
    const bool delayAvailable = generatorOn ? hasReference && generatorDelayValid
                             : referenceFrames >= 8 && hasReference && averagingSelector.getSelectedId() > 1
                               && result.averageCoherence >= 0.5f;
    if (generatorOn)
        result.delayMs = generatorDelayMs;
    fftDisplay.setDelayAvailable(delayAvailable);
    if (!hasReference)
    {
        result.magnitudeDb.clear();
        result.phaseDeg.clear();
        result.coherence.clear();
        result.impulseResponse.clear();
        reverbDisplay.clear();
    }
    lastResult = result;
    if (!delayAvailable)
        lastResult.delayMs = std::numeric_limits<float>::quiet_NaN();
    fftDisplay.setDelayMs(result.delayMs);
    fftDisplay.setAverageCoherence(result.averageCoherence);
    fftDisplay.pushData(result.freq, result.refMagnitudeDb, result.measMagnitudeDb,
                        result.magnitudeDb, result.phaseDeg, result.coherence, result.binValid);

    if (transferFunctionDisplay.isVisible())
        transferFunctionDisplay.pushData(result);

    delayLabel.setText(delayAvailable ? (generatorOn ? "Total " : "Delay ") + juce::String(result.delayMs, 3) + " ms"
                                    : (generatorOn ? "Total -- (menunggu mic)" : "Delay -- (perlu referensi)"),
                       juce::dontSendNotification);

    if (generatorOn && (reverbCounter % 2) == 0)
        updateGeneratorDisplay();

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
    // The SPL meter already adds its own calibration offset to a 0 dBFS reference,
    // so only the sensitivity step is handed over here.
    const auto offset = microphoneCalibration.isEnabled() ? microphoneCalibration.getSensitivityDb()
                                                           : 120.0f;
    splMeter.setCalibrationOffset (offset);
}

void MainComponent::calibrationMenuClicked()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Tanpa kalibrasi (dBFS)", microphoneCalibration.getModelName() == "Tanpa kalibrasi");
    menu.addItem (2, "Dayton Audio iMM-6c",
                  microphoneCalibration.getModelName() == "Dayton Audio iMM-6c");
    menu.addSeparator();
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
    calibrationButton.setBounds (controls.removeFromRight (130).reduced (0, 3));
    controls.removeFromLeft (6);
    saveSnapshotButton.setBounds (controls.removeFromRight (110).reduced (0, 3));
    controls.removeFromLeft (6);
    loadSnapshotButton.setBounds (controls.removeFromRight (100).reduced (0, 3));
    controls.removeFromLeft (6);
    auto generatorRow = controls.removeFromBottom(34);
    pinkNoiseButton.setBounds(generatorRow.removeFromLeft(180));
    generatorRow.removeFromLeft(12);
    pinkNoiseLevel.setBounds(generatorRow.removeFromLeft(250));
    generatorRow.removeFromLeft(12);
    pinkNoiseHint.setBounds(generatorRow);
    controls = controls.removeFromTop(36);
    measurementChannelSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    referenceChannelSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    sampleRateSelector.setBounds(controls.removeFromLeft(100));
    controls.removeFromLeft(8);
    bufferSizeSelector.setBounds(controls.removeFromLeft(100));
    controls.removeFromLeft(8);
    refreshButton.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(12);
    fftSizeSelector.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(8);
    averagingSelector.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(12);
    startStopButton.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(8);
    exportButton.setBounds(controls.removeFromLeft(110));

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

    generatorButton.setBounds(panel.removeFromTop(44).removeFromLeft(160));

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
