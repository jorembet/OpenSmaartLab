#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "DSP.h"
#include "FrequencyLabels.h"
#include "TransferFunction.h"

/** Magnitude, phase and coherence of one dual channel measurement, stacked.

    These three belong together: the magnitude says how much of the signal survives,
    the phase says when it arrives, and coherence says whether the two channels are
    related at all. Read apart, each one is easy to misread, so they share one frequency
    axis here. Bins where the reference carries no signal are left blank rather than
    drawn, because a coherence value there is a ratio of two near-zero powers and says
    nothing about the system.
*/
class TransferFunctionDisplay : public juce::Component
{
public:
    TransferFunctionDisplay();
    ~TransferFunctionDisplay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;

    /** Where the pointer is, so the readout can be updated from a timer as well as from
        a mouse move; JUCE only keeps a position once the pointer is over the component. */
    juce::Point<float> cursorPosition;

    /** One frame of transfer function and coherence data. */
    void pushData(const TransferFunction::Result& result);

    /** Coherence below this makes the measurement unusable rather than merely uneven. */
    static constexpr float validityThreshold = 0.8f;

    /** Fewest averages that can tell a coherent pair from an unrelated one.

        Coherence is a ratio of averaged spectra, and for a single frame it is identically
        one: |conj(X)Y|^2 = |X|^2|Y|^2, so two signals with nothing in common read a perfect
        1.0. Averaging N frames pulls the reading for unrelated signals down towards 1/N, so
        the estimate only carries information once 1/N sits well under the threshold. Below
        this many averages the number is arithmetically correct and completely uninformative,
        and reporting it as confidence would be the worst kind of wrong: it would reassure
        the reader about a measurement that proved nothing. */
    static constexpr int minimumTrustworthyAverages = 4;

    /** True when the average is deep enough for the coherence to mean anything. */
    static bool isCoherenceTrustworthy (int averages)
    {
        return averages >= minimumTrustworthyAverages;
    }

    bool hasData() const { return !frequency.empty(); }
    float getAverageCoherence() const { return averageCoherence; }
    float getMeasuredRmsDb() const { return measRmsDb; }
    float getMeasuredPeakDb() const { return measPeakDb; }
    float getReferenceRmsDb() const { return refRmsDb; }
    float getReferencePeakDb() const { return refPeakDb; }
    juce::String getReadout() const;

    std::function<void()> onFindDelay;

private:
    /** The three pane rects, split down the available height. */
    struct Panes
    {
        juce::Rectangle<float> magnitude;
        juce::Rectangle<float> phase;
        juce::Rectangle<float> coherence;
    };

    void drawPane(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                  const juce::String& title, const juce::String& unit,
                  const std::vector<float>& values, const std::vector<char>& valid,
                  float top, float bottom, const juce::Colour& colour,
                  float referenceLine = 0.0f, bool showReference = false);
    void drawAxis(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                  float top, float bottom, const juce::String& unit);
    void drawFrequencyLabels(juce::Graphics& g, const juce::Rectangle<float>& bounds);
    void drawLevelSidebar(juce::Graphics& g);
    void drawEqCorrection(juce::Graphics& g, const juce::Rectangle<float>& bounds) const;
    void computeEqTarget();
    Panes panes() const;
    juce::Rectangle<float> sidebarArea() const;
    float frequencyToX(float frequency, const juce::Rectangle<float>& bounds) const;
    float xToFrequency(float x, const juce::Rectangle<float>& bounds) const;
    juce::Rectangle<float> magnitudeArea() const;
    juce::Rectangle<float> phaseArea() const;
    juce::Rectangle<float> coherenceArea() const;
    void updateCursorReadout(const juce::Rectangle<float>& bounds);

    std::vector<float> frequency;
    std::vector<float> magnitudeDb;
    std::vector<float> phaseDeg;
    std::vector<float> coherence;
    std::vector<char> binValid;

    float delayMs = 0.0f;
    float averageCoherence = 0.0f;
    float peakReferenceDb = dsp::dbFloor;
    float measRmsDb = dsp::dbFloor;
    float measPeakDb = dsp::dbFloor;
    float refRmsDb = dsp::dbFloor;
    float refPeakDb = dsp::dbFloor;
    int validBins = 0;
    int blankedBins = 0;
    /** Averages behind the current reading, which is what decides whether the coherence can
        be believed. Carried here because the banner has to explain a low coherence, and
        "too few averages to tell" is a different answer from "the two signals disagree". */
    int averages = 1;
    bool delayAvailable = false;
    bool frozen = false;
    bool showEqOverlay = false;
    // The level a flat correction aims at, and how much correction is allowed at most.
    // Anything beyond that is a measurement artefact, not a room.
    float eqTargetDb = 0.0f;
    bool eqTargetValid = false;
    static constexpr float maxCorrectionDb = 12.0f;

    juce::TextButton findDelayButton { "Cari Delay" };
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton eqOverlayButton { "Koreksi EQ" };
    juce::Label validityLabel;
    juce::Label readoutLabel;

    std::vector<float> magnitudeCurve;
    std::vector<float> phaseCurve;
    std::vector<float> coherenceCurve;

    juce::CriticalSection dataLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransferFunctionDisplay)
};
