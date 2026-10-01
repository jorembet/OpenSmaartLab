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
#include "TransferFunctionDisplay.h"

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ComponentListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** The signal generator, so the checks can confirm the panel controls reach it. */
    SignalGenerator& getGenerator() { return audioEngine.getGenerator(); }

    /** Which output channel the generator feeds, for the same reason. */
    AudioEngine::OutputRouting getGeneratorRouting() const { return audioEngine.getGeneratorRouting(); }

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
    void applyTypedFrequency();
    void applyTypedBandLow();
    bool isEditingGeneratorBandLow() const;
    void updateGeneratorFrequencyEditor();
    void updateGeneratorBandLowEditor();
    bool isEditingGeneratorFrequency() const;
    void updateGeneratorGeneratorControls();
    void applyGeneratorBandPreset (int index);
    void syncGeneratorBandPreset();
    AudioEngine::OutputRouting currentGeneratorRouting() const;
    juce::String generatorOutputLabel() const;
    int measurementChannelIndex() const;
    int referenceChannelIndex() const;
    void keepMeasurementAndReferenceDistinct();
    void resetReferenceTracking();
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
    // Measurement and reference are assigned to physical channels on their own, the way
    // a two channel measurement is actually wired: the microphone on one input and the
    // loopback tap on the other. Assigning both to the same channel would compare a
    // signal with itself, so the other selector is moved instead.
    juce::ComboBox measurementChannelSelector;
    juce::ComboBox referenceChannelSelector;
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

    // A dedicated tab: magnitude, phase and coherence of one dual channel measurement,
    // with the delay finder that makes coherence usable.
    TransferFunctionDisplay transferFunctionDisplay;
    bool findDelayRequested = false;

    juce::Component generatorPanel;
    GeneratorDisplay generatorDisplay;
    juce::TextButton generatorButton { "Nyalakan" };
    juce::ComboBox generatorTypeSelector;
    juce::Slider generatorLevelSlider;
    juce::Slider generatorFrequencySlider;
    // A plain Label cannot take typed input, so this is a TextEditor styled to match
    // the rest of the panel. Typing an exact frequency matters for measurement:
    // 997 Hz is not "about 1000 Hz" when lining up a sweep point.
    // TextEditor reports focus through a Listener rather than a callback, so this
    // small adapter tracks whether the user is mid-edit. Without it, the sync from
    // slider back into the box would fight the user while they type.
    class FrequencyEditorListener : public juce::TextEditor::Listener
    {
    public:
        explicit FrequencyEditorListener (MainComponent& owner) : component (owner) {}

        void textEditorReturnKeyPressed (juce::TextEditor&) override;
        void textEditorEscapeKeyPressed (juce::TextEditor&) override;
        void textEditorFocusLost (juce::TextEditor&) override;

        MainComponent& component;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FrequencyEditorListener)
    };

    juce::TextEditor generatorFrequencyEditor;
    juce::TextEditor generatorBandLowEditor;
    juce::Slider generatorBandLowSlider;
    juce::Label generatorBandLowLabel;
    // One entry per driver, so a band can be picked by name instead of hunting for two
    // limits. The last entry reports a hand set band rather than pretending to be one.
    juce::ComboBox generatorBandPresetSelector;
    juce::Label generatorBandPresetLabel;
    // Which output channel carries the signal, so the amplifier can be fed from one side
    // while the other stays silent for the reference tap.
    juce::ComboBox generatorOutputRoutingSelector;
    juce::Label generatorOutputRoutingLabel;
    FrequencyEditorListener frequencyEditorListener { *this };
    FrequencyEditorListener bandLowEditorListener { *this };
    bool frequencyEditorActive = false;
    bool bandLowEditorActive = false;
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
