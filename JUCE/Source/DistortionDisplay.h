#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "DistortionAnalyser.h"

/** Shows what a signal's spectrum is made of, once it has been separated.

    The three figures at the top answer three different questions and are deliberately not
    collapsed into one number. THD says how much of the signal is harmonics of the fundamental,
    which is what a non-linearity produces. THD+N says how much of it is anything other than the
    fundamental at all, which also counts the noise. SINAD is the same information the other way
    round. A signal can have excellent THD and useless SINAD, and reporting only the first would
    hide exactly that.

    The table underneath is the evidence. A single distortion figure with nothing behind it is
    not something a reader can argue with, and the numbers in it are what tells one distortion
    from another.
*/
class DistortionDisplay : public juce::Component
{
public:
    DistortionDisplay();

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** A new measurement. Empty result clears the display rather than leaving stale figures. */
    void setResult (const dsp::DistortionAnalyser::Result& result);

    /** Where to look for the fundamental, and how far up the harmonic series to report.

        Exposed so the settings can live with the rest of the analysis controls rather than in a
        dialog that has to be reopened to change one number. */
    void setSearchRange (float lowHz, float highHz);
    void setMaxHarmonic (int order);

    /** The reading the status line quotes, so one measurement is described in one place. */
    juce::String getSummary() const;

private:
    void drawFigures (juce::Graphics& g, const juce::Rectangle<int>& area) const;
    void drawTable (juce::Graphics& g, const juce::Rectangle<int>& area) const;
    juce::Colour figureColour (float db) const;

    dsp::DistortionAnalyser::Result result;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DistortionDisplay)
};