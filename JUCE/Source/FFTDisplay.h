#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <vector>
#include "DSP.h"
#include "FrequencyLabels.h"
#include "MicrophoneCalibration.h"
#include "RTASnapshot.h"

class FFTDisplay : public juce::Component
{
public:
    enum class Mode { SingleChannel = 1, DualChannel, TransferFunction, Coherence, Phase };
    enum class Style { Bands = 1, BarLinear, Line };

    struct TraceInfo
    {
        const std::vector<float>* values = nullptr;
        juce::Colour colour;
        juce::String label;
    };

    FFTDisplay();
    ~FFTDisplay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;

    void pushData(const std::vector<float>& freq,
                  const std::vector<float>& refMagnitudeDb,
                  const std::vector<float>& measMagnitudeDb,
                  const std::vector<float>& tfMagnitudeDb,
                  const std::vector<float>& tfPhaseDeg,
                  const std::vector<float>& coherence);

    void setMode(Mode newMode);
    Mode getMode() const { return mode; }

    void setOctaveFraction(int fraction);
    int getOctaveFraction() const { return octaveFraction; }

    void setStyle(Style newStyle);
    Style getStyle() const { return style; }

    void setBandOctaveFraction(int fraction);
    int getBandOctaveFraction() const { return bandOctaveFraction; }
    int getBandCount() const { return (int) bandFrequencies.size(); }

    void setMicrophoneCalibration(const MicrophoneCalibration& newCalibration);
    /** Applies a calibration factor to an already-summarised bar level. */
    /** Shifts stored snapshots by a dB offset so they can be compared against live data. */
    void setBarCalibrationFactor(float factor);
    const MicrophoneCalibration& getMicrophoneCalibration() const { return calibration; }

    void setMicBarColour(const juce::Colour& colour);
    void setOutputBarColour(const juce::Colour& colour);
    juce::Colour getMicBarColour() const { return micBarColour; }
    juce::Colour getOutputBarColour() const { return outputBarColour; }

    void setPeakHold(bool shouldHold);
    bool isPeakHoldEnabled() const { return peakHoldEnabled; }
    void clearPeakHold();

    void setFrozen(bool shouldFreeze);
    bool isFrozen() const { return frozen; }

    void showGeneratorReference(bool show) { generatorReference = show; displayDirty = true; repaint(); }
    void setDelayAvailable(bool available) { delayAvailable = available; }
    void setDelayMs(float newDelayMs) { delayMs = newDelayMs; }
    void setAverageCoherence(float value) { averageCoherence = value; }

    /** Overall RMS and peak of the measurement channel, in dBFS. */
    void setMeasuredLevels(float rmsDb, float peakDb);
    float getMeasuredRmsDb() const { return measuredRmsDb; }
    float getMeasuredPeakDb() const { return measuredPeakDb; }
    void setRunning(bool isNowRunning);

    float getTopDb() const { return topDb; }
    float getBottomDb() const { return bottomDb; }

    void writeCsv(juce::OutputStream& stream) const;

    /** Captures the current trace so it can be stored or recalled later. */
    RTASnapshot captureSnapshot (const juce::String& name) const;

    /** Writes the current trace as a named, commented CSV. */
    bool writeSnapshotTo (const juce::File& file) const;

    /** Shows a stored snapshot instead of live input. */
    void showSnapshot (const RTASnapshot& newSnapshot);
    void clearSnapshot();
    bool isShowingSnapshot() const { return snapshotActive; }

private:
    enum class Axis { Decibels, Coherence, Phase };

    struct Frame
    {
        std::vector<float> freq;
        std::vector<float> refDb;
        std::vector<float> measDb;
        std::vector<float> tfDb;
        std::vector<float> tfPhase;
        std::vector<float> coherence;
        bool valid = false;
    };

    struct Display
    {
        std::vector<float> freq;
        std::vector<TraceInfo> traces;
        Axis axis = Axis::Decibels;
        bool valid = false;
    };

    void rebuildDisplay();
    void updatePeakHold();
    juce::Rectangle<float> plotArea() const;
    float minimumLabelSpacing(const juce::Rectangle<float>& area) const;
    float frequencyToX(float freq, const juce::Rectangle<float>& area) const;
    float valueToY(float value, const juce::Rectangle<float>& area) const;
    float frequencyAtX(float x, const juce::Rectangle<float>& area) const;
    void computeAxisDomain();
    int nearestBin(float x, const juce::Rectangle<float>& area) const;
    void drawGrid(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawTraces(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawCursor(juce::Graphics& g, const juce::Rectangle<float>& area);
    juce::String getAxisLabel() const;
    juce::String frequencyLabel(float freq) const;
    juce::Font frequencyLabelFont() const;
    float frequencyLabelHeight() const;
    float frequencyLabelWidth(const juce::Rectangle<float>& area) const;

    void loadBarColours();
    void saveBarColours() const;

    // juce::ColorButton was removed in JUCE 7, so the swatch is drawn by hand and
    // clicking it opens a ColourSelector.
    class ColourSwatchButton : public juce::Component,
                             private juce::ChangeListener,
                             private juce::TooltipClient
    {
    public:
        explicit ColourSwatchButton (const juce::String& label) : text (label) {}

        void setColour (const juce::Colour& newColour);
        juce::Colour getColour() const { return colour; }
        void registerSelf (ColourSwatchButton& target) { self = &target; }
        void paint (juce::Graphics& g) override;

        void setTooltip (const juce::String& newTooltip) { tip = newTooltip; }
        juce::String getTooltip() { return tip; }

        std::function<void()> onColourChange;

    private:
        void mouseDown (const juce::MouseEvent&) override;
        void changeListenerCallback (juce::ChangeBroadcaster* source) override;

        juce::String text;
        juce::String tip;
        juce::Colour colour { juce::Colours::white };
        ColourSwatchButton* self = nullptr;
    };

    ColourSwatchButton micColourButton { "Mic" };
    ColourSwatchButton outputColourButton { "Output" };
    juce::Colour micBarColour;
    juce::Colour outputBarColour;

    juce::ComboBox styleSelector;
    Style style = Style::Bands;
    bool barStyle = true;
    bool linearAxis = false;
    juce::ComboBox bandSelector;
    int bandOctaveFraction = 3;
    std::vector<float> bandFrequencies;
    juce::ComboBox modeSelector;
    juce::ComboBox octaveSelector;
    juce::ComboBox rangeSelector;
    juce::ToggleButton peakHoldButton { "Peak Hold" };
    juce::ToggleButton freezeButton { "Freeze" };
    juce::Label readoutLabel;

    Mode mode = Mode::SingleChannel;
    int octaveFraction = 3;
    float topDb = 0.0f;
    float bottomDb = -120.0f;
    bool peakHoldEnabled = false;
    bool frozen = false;
    bool running = false;
    bool delayAvailable = false;
    bool generatorReference = false;
    bool levelsValid = false;
    float delayMs = 0.0f;
    float averageCoherence = 0.0f;
    float measuredRmsDb = dsp::dbFloor;
    float measuredPeakDb = dsp::dbFloor;

    MicrophoneCalibration calibration;
    float barCalibrationFactor = 0.0f;

    bool snapshotActive = false;
    RTASnapshot snapshot;
    std::vector<std::vector<float>> snapshotTraces;

    float axisMinLog = 1.30103f;   // log10 (20)
    float axisMaxLog = 4.30103f; // log10 (20000)

    float minFrequency = 20.0f;
    float maxFrequency = 20000.0f;

    Frame current;
    Frame frozenFrame;
    bool displayDirty = true;
    Display display;
    std::vector<std::vector<float>> peakHoldValues;

    std::vector<std::vector<float>> smoothed;
    juce::CriticalSection dataLock;

    bool cursorVisible = false;
    float cursorX = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFTDisplay)
};
