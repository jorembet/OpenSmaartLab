#pragma once

#include <juce_graphics/juce_graphics.h>

/** Layout metrics for the RTA frequency labels along the horizontal axis.

    This lives apart from FFTDisplay so the sizing can be checked without pulling in
    the GUI modules, and so the label rules stay in one place: FFTDisplay draws with
    whatever these return, and the tests assert on the same numbers.
*/
struct FrequencyLabels
{
    /** 14 pt bold: readable at the default window size, which 9 pt was not. */
    static juce::Font font()
    {
        return juce::Font (14.0f, juce::Font::bold);
    }

    /** Row height, with room for the comma in "31.5" and any descender. */
    static float height()
    {
        return font().getHeight() + 2.0f;
    }

    /** Labels are wider than the text so neighbours do not touch. */
    static float padding()
    {
        return 8.0f;
    }

    /** Width needed by the widest label in use. */
    static float width()
    {
        const auto typeface = font();
        auto widest = 0.0f;

        for (const auto& candidate : { juce::String ("20"), juce::String ("31.5"),
                                       juce::String ("1k"), juce::String ("12.5k"),
                                       juce::String ("20000") })
            widest = std::max (widest, static_cast<float> (typeface.getStringWidth (candidate)));

        return widest + padding();
    }

    /** Vertical offset from the plot bottom to the top of the label row. */
    static float topOffset()
    {
        return 4.0f;
    }

    /** Space to reserve below the plot so the label row is not clipped. */
    static float reservedSpace()
    {
        return topOffset() + height() + 4.0f;
    }
};