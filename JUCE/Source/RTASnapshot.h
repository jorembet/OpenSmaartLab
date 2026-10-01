#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <vector>

/** A stored RTA trace that can be written to disk and read back.

    This lives apart from FFTDisplay so the save and load round trip can be
    verified without a GUI. FFTDisplay converts its live display into one of
    these when the user saves, and back again when they load.
*/
struct RTASnapshot
{
    struct Trace
    {
        juce::String name;
        juce::Colour colour { juce::Colours::white };
        std::vector<float> values;
    };

    juce::String name;
    juce::String axisLabel;
    juce::String calibrationText;
    float topDb = 0.0f;
    float bottomDb = -120.0f;
    std::vector<float> frequency;
    std::vector<Trace> traces;

    /** Validates that every trace lines up with the frequency axis. */
    bool isUsable() const;

    /** Serialises to a commented CSV. */
    juce::String toCsv() const;

    /** Parses what toCsv produced. Returns false and leaves result untouched
        on anything malformed, so a partial file is never shown as a real trace.
    */
    static bool fromCsv (const juce::String& text, RTASnapshot& result);

    bool writeTo (const juce::File& file) const;
    static bool readFrom (const juce::File& file, RTASnapshot& result);
};