#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "TransferFunction.h"

/** Impedance magnitude and phase of a speaker, estimated with the two-channel
    method: the reference channel sits across a known series resistor and the
    measurement channel sits across the speaker itself. Z = R * V_meas / V_ref. */
class ImpedanceDisplay : public juce::Component
{
public:
    ImpedanceDisplay();
    ~ImpedanceDisplay() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void pushData(const TransferFunction::Result& result);
    void clear();

private:
    void recompute();

    float seriesResistanceOhm = 8.2f;
    TransferFunction::Result lastResult;
    bool hasLastResult = false;
    std::vector<float> freq;
    std::vector<float> impedanceOhm;
    std::vector<float> phaseDeg;
    std::vector<char> binValid;
    bool hasData = false;

    juce::TextEditor resistorEditor;
    juce::Label resistorLabel;
    juce::Label hintLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImpedanceDisplay)
};
