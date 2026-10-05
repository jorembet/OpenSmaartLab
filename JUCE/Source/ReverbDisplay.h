#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ImpulseResponse.h"
#include "ReverbAnalyser.h"

class ReverbDisplay : public juce::Component
{
public:
    ReverbDisplay();
    ~ReverbDisplay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setAcoustics(const ImpulseResponse::Acoustics& acoustics, float sampleRate);
    void setWaterfall(const ImpulseResponse::Waterfall& newWaterfall);
    void setBandRt60(const std::vector<ImpulseResponse::BandRt60>& newBandRt60);

    // The per band reverberation table, kept apart from the legacy band figures so the two
    // cannot be confused for one another on screen.
    void setReverbResult(const dsp::ReverbAnalyser::Result& newResult);
    void clear();
    bool hasData() const { return acoustics.valid; }

private:
    struct Regions
    {
        juce::Rectangle<float> energy;
        juce::Rectangle<float> decay;
        juce::Rectangle<float> waterfall;
        juce::Rectangle<float> bandRt60;
        juce::Rectangle<float> reverbTable;
        juce::Rectangle<float> parameters;
    };

    Regions computeRegions() const;

    void drawPanel(juce::Graphics& g, const juce::Rectangle<float>& area,
                   const juce::String& title, const juce::String& subtitle = {}) const;
    void drawEnergy(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawDecay(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawParameters(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawWaterfall(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawBandRt60(juce::Graphics& g, const juce::Rectangle<float>& area) const;
    void drawReverbTable(juce::Graphics& g, const juce::Rectangle<float>& area) const;

    ImpulseResponse::Acoustics acoustics;
    ImpulseResponse::Waterfall waterfallData;
    std::vector<ImpulseResponse::BandRt60> bandRt60Data;
    dsp::ReverbAnalyser::Result reverbResult;
    float sampleRate = 48000.0f;
    juce::CriticalSection dataLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbDisplay)
};
