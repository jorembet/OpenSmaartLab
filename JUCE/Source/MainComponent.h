#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "FFTDisplay.h"
#include "GeneratorDisplay.h"
#include "ImpulseResponse.h"
#include "ReverbDisplay.h"
#include "SPLMeter.h"
#include "SignalGenerator.h"
#include "TransferFunction.h"

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ComponentListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void componentMovedOrResized(juce::Component&, bool, bool) override;
    void layoutGenerator();

    void refreshDeviceSelectors();
    void startStopClicked();
    void exportCSVClicked();
    void calibrationMenuClicked();
    void saveSnapshotClicked();
    void loadSnapshotClicked();
    void clearSnapshotClicked();
    void updateCalibrationLabel();
    void populateSnapshotMenu();
    void chooseSnapshotFile();
    void loadSnapshotFile (const juce::File& file);
    juce::Array<juce::File> collectSnapshots() const;
    juce::File snapshotsDirectory() const;
    juce::File lastSnapshotDirectory() const;
    static juce::String sanitiseName (const juce::String& name);
    void applyCalibrationToSplMeter();
    void generatorToggled();
    void playPinkNoise();
    void updateGeneratorInfo();
    void updateGeneratorDisplay();
    void analysisSettingsChanged();
    void updateStatus();

    juce::Rectangle<int> topBarBounds() const;
    juce::Rectangle<int> statusBarBounds() const;

    AudioEngine audioEngine;
    TransferFunction transferFunction;
    dsp::DelayFinder generatorDelayFinder;
    float generatorDelayMs = 0.0f;
    bool generatorDelayValid = false;
    int generatorDelayCounter = 0;
    double generatorStartedAt = 0.0;

    juce::TooltipWindow deviceTooltips { this, 500 };
    juce::Label inputLabel { {}, "Input / Rekam" };
    juce::Label outputLabel { {}, "Output / Putar" };
    juce::ComboBox inputSelector;
    juce::ComboBox outputSelector;
    juce::ComboBox microphoneChannel;
    juce::ComboBox sampleRateSelector;
    juce::ComboBox bufferSizeSelector;
    juce::ComboBox fftSizeSelector;
    juce::ComboBox averagingSelector;
    juce::TextButton refreshButton { "Refresh" };
    juce::TextButton startStopButton { "Mulai" };
    juce::TextButton pinkNoiseButton { "Play Pink Noise" };
    juce::Slider pinkNoiseLevel;
    juce::Label pinkNoiseHint { {}, "Output -> speaker -> microphone | Delay total termasuk latensi audio" };
    juce::TextButton exportButton { "Export CSV" };
    juce::TextButton calibrationButton { "Kalibrasi Mic" };
    juce::TextButton saveSnapshotButton { "Simpan RTA" };
    juce::TextButton loadSnapshotButton { "Load RTA" };
    juce::PopupMenu snapshotMenu;
    juce::Array<juce::File> snapshotFiles;
    juce::File lastSnapshotFolder;
    juce::Label calibrationLabel;
    MicrophoneCalibration microphoneCalibration;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    FFTDisplay fftDisplay;
    ReverbDisplay reverbDisplay;
    SPLMeter splMeter;

    juce::Component generatorPanel;
    GeneratorDisplay generatorDisplay;
    juce::TextButton generatorButton { "Nyalakan" };
    juce::ComboBox generatorTypeSelector;
    juce::Slider generatorLevelSlider;
    juce::Slider generatorFrequencySlider;
    juce::Slider generatorSweepStartSlider;
    juce::Slider generatorSweepEndSlider;
    juce::Slider generatorSweepDurationSlider;
    juce::Label generatorTypeLabel;
    juce::Label generatorLevelLabel;
    juce::Label generatorFrequencyLabel;
    juce::Label generatorSweepStartLabel;
    juce::Label generatorSweepEndLabel;
    juce::Label generatorSweepDurationLabel;
    juce::Label generatorInfoLabel;

    juce::Label statusLabel;
    juce::Label delayLabel;
    juce::Label coherenceLabel;

    TransferFunction::Result lastResult;

    bool isRunning = false;
    bool generatorOn = false;
    int fftSize = 16384;
    int reverbCounter = 0;
    int referenceFrames = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
