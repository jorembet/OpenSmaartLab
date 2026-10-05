#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "DspWorker.h"

/** Level readout for the two physical input channels.

    It shows what the DSP worker measured rather than measuring anything itself. The
    balance marker compares the two channel RMS readings; the gain control is shared with
    the main toolbar control.
*/
class InputLevelPanel : public juce::Component
{
public:
    InputLevelPanel();
    ~InputLevelPanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setSnapshot (const DspWorker::Snapshot& snapshot);
    void setInputGainControl (double minimumDb, double maximumDb, double currentDb,
                              bool hardwareGain);

    /** Reports a user change so the application can apply the shared mic gain control. */
    std::function<void(float)> onInputGainChanged;

private:
    struct Channel
    {
        juce::String title;
        dsp::LevelMeter::Readings readings;
    };

    void drawChannel (juce::Graphics& g, const juce::Rectangle<float>& area,
                      const Channel& channel, const juce::Colour& colour);
    void drawMeter (juce::Graphics& g, juce::Rectangle<float> area,
                    float rmsDb, float peakDb, float holdDb);

    juce::Label inputGainLabel;
    juce::Slider inputGainSlider;

    Channel left { "INPUT LEFT (Ch 1)", {} };
    Channel right { "INPUT RIGHT (Ch 2)", {} };
    juce::String statusLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputLevelPanel)
};
