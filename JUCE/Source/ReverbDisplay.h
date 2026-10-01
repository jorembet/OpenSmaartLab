#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ImpulseResponse.h"

class ReverbDisplay : public juce::Component
{
public:
    ReverbDisplay();
    ~ReverbDisplay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setAcoustics(const ImpulseResponse::Acoustics& acoustics, float sampleRate);
    void clear();
    bool hasData() const { return acoustics.valid; }

private:
    struct Regions
    {
        juce::Rectangle<float> energy;
        juce::Rectangle<float> decay;
        juce::Rectangle<float> parameters;
    };

    Regions computeRegions() const;

    void drawPanel(juce::Graphics& g, const juce::Rectangle<float>& area,
                   const juce::String& title) const;
    void drawEnergy(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawDecay(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawParameters(juce::Graphics& g, const juce::Rectangle<float>& area) const;

    ImpulseResponse::Acoustics acoustics;
    float sampleRate = 48000.0f;
    juce::CriticalSection dataLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbDisplay)
};
