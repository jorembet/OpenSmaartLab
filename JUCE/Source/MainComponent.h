#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "FFTDisplay.h"
#include "GeneratorDisplay.h"
#include "ImpulseResponse.h"
#include "ReverbDisplay.h"
#include "ReverbAnalyser.h"
#include "RecorderPanel.h"
#include "OfflinePanel.h"
#include "SessionPanel.h"
#include "ImpedanceDisplay.h"
#include "InputLevelPanel.h"
#include "SpectrumAnalyserDisplay.h"
#include "RtaDisplay.h"
#include "DistortionDisplay.h"
#include "AdvancedMeasurements.h"
#include "DistortionAnalyser.h"
#include "DspWorker.h"
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
    /** Opens the device and starts a session. Separate from the button so that reopening the
        device for a changed setting cannot be mistaken for a toggle. */
    void startAudio();
    void stopAudio();
    /** Applies a setting that only takes effect on a reopened device. Explicitly stops then
        starts, so the result does not depend on the state the notification happened to
        arrive in. */
    void restartAudio();
    void exportCSVClicked();
    void exportIrClicked();
    void calibrationMenuClicked();

    /** Asks for the calibrator's level, measures what the chain is reading, and stores the
        difference as the profile's correction.

        A calibrator puts out a known sound pressure, so it is the only thing that can say what
        a microphone chain is doing in absolute terms. Typed sensitivities cannot: they leave out
        the interface gain, the preamp and whatever the capsule is actually delivering. */
    void runCalibratorWorkflow();

    void saveCalibrationProfile();

    /** Current microphone reading in dBFS using the SPL tab's selected weighting, with software
        trim removed so the stored calibration remains valid if that control changes later. */
    float measuredMicLevelDbfs() const;
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
    void updateInputGainControl();
    float analysisInputGainDb() const;
    void updateGeneratorInfo();
    void updateGeneratorDisplay();
    void analysisSettingsChanged();
    void updateStatus();
    /** Pushes the newest captured audio to the measurement worker and copies its reading
        into the two level views. Called on the timer, never from the audio thread. */
    void pumpMeasurementWorker();
    void rtaControlsChanged();
    /** Restarts the audio device with the sample rate and buffer size now selected. */
    void applyAudioSettingsToDevice();
    void spectrumControlsChanged();

    juce::Rectangle<int> topBarBounds() const;
    juce::Rectangle<int> statusBarBounds() const;

    AudioEngine audioEngine;
    TransferFunction transferFunction;

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
    /** Air temperature assumed when a delay is turned into a distance.

        A selector rather than a free text field because the value only ever feeds the speed of
        sound, and a mistyped number there would silently move every distance on screen. The
        common room temperatures are enough for the accuracy the linear approximation has. */
    juce::ComboBox temperatureSelector;
        juce::TextButton startStopButton { "Mulai" };
    juce::TextButton pinkNoiseButton { "Play Pink Noise" };
    juce::Slider pinkNoiseLevel;
    juce::Label inputGainLabel { {}, "Trim Mic" };
    juce::Slider inputGainSlider;
    bool hardwareInputGainActive = false;
    float softwareInputGainDb = 0.0f;
    juce::Label pinkNoiseHint { {}, "Output -> speaker -> microphone | Delay total termasuk latensi audio" };
    juce::Label startHintLabel { {}, "Pilih input & output di atas, lalu tekan Mulai untuk menangkap sinyal" };
    juce::TextButton exportButton { "Export CSV" };
    juce::TextButton exportIrButton { "Export IR" };
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
    RecorderPanel recorderPanel { audioEngine };
    OfflinePanel offlinePanel;
    SessionPanel sessionPanel { audioEngine };
    ImpedanceDisplay impedanceDisplay;
    SPLMeter splMeter;

    // Level measurement and the spectrum analyser run on this worker. The audio callback
    // only hands over captured samples, so neither the measurement cost nor a slow
    // repaint can reach the audio thread.
    DspWorker dspWorker;
    InputLevelPanel inputLevelPanel;
    SpectrumAnalyserDisplay spectrumDisplay;
    RtaDisplay rtaDisplay;
    DistortionDisplay distortionDisplay;

    dsp::ImdAnalyser imdAnalyser;
    dsp::CrosstalkAnalyser crosstalkAnalyser;
    dsp::PolarityDetector polarityDetector;
    dsp::DistortionAnalyser distortionAnalyser;

    // A dedicated tab: magnitude, phase and coherence of one dual channel measurement,
    // with the delay finder that makes coherence usable.
    TransferFunctionDisplay transferFunctionDisplay;
    bool findDelayRequested = false;

    juce::Component generatorPanel;
    GeneratorDisplay generatorDisplay;
    juce::TextButton generatorButton { "Nyalakan" };
    juce::TextButton generatorMuteButton { "Bisukan" };
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
    dsp::ReverbAnalyser reverbAnalyser;

    // Reused between timer ticks so feeding the worker does not allocate on every tick.
    std::vector<float> workerLeft;
    std::vector<float> workerRight;
    std::vector<float> workerGenerated;

    // The transfer view is refreshed at 30 Hz. Retaining these frame buffers keeps that
    // steady-state path from allocating and freeing several FFT-sized vectors per tick.
    std::vector<float> analysisReference;
    std::vector<float> analysisMeasurement;
    std::vector<float> analysisLeft;
    std::vector<float> analysisRight;
    std::vector<float> analysisGenerated;
    std::vector<float> generatorOutputSamples;
    int referenceFrames = 0;

    /** The last thing the audio engine had to say, kept so a failed start can explain itself.

        The engine reports a device that will not open through onStatusMessage, and the status
        line is the only place that message ever appears. Without holding on to it, the start
        path would overwrite the reason with its own generic wording during the same click and
        the failure would be invisible. */
    juce::String engineStatusMessage;

    /** True while refreshDeviceSelectors() is rebuilding the device lists.

        Emptying a combo box and restoring a selection is reported as a change by JUCE even
        when the value ends up exactly as it was, so without this the sample rate callback
        reopens the device, the reopen rebuilds the lists, and a running session restarts
        itself indefinitely. */
    bool rebuildingSelectors = false;

    /** The sample rate and buffer size the open device was actually configured with, so a
        notification that repeats them can be ignored instead of interrupting the stream. */
    int appliedRateId = 0;
    int appliedBufferSizeId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
