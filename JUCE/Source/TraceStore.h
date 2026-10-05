#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>

/** The six traces a measurement session is made of, and the arithmetic between them.

    The traces exist because a measurement is only useful next to another one. Live is what the
    instrument is reading now. Reference is the curve the room is supposed to have. Measurement
    is a reading that has been frozen so it stops moving. Average accumulates. Memory is a curve
    held across sessions so a speaker can be compared with how it behaved last week. Difference
    is reference minus measurement, which is the only one of the six that is a new number rather
    than a stored one.

    Every trace is stored the same way: a frequency axis and a value per bin, in decibels,
    because that is the form every measurement on the panel is displayed in and the form a
    difference has to be taken in. Averaging and subtracting decibels is the right operation
    here for a reason that is easy to get wrong: a difference in decibels is a ratio, which is
    what a comparison between two readings is asking about. */
class TraceStore
{
public:
    enum class Kind
    {
        Live,
        Reference,
        Measurement,
        Average,
        Memory,
        Difference
    };

    static constexpr int numKinds = 6;

    enum class Source
    {
        Spectrum,
        Rta,
        TransferMagnitude,
        TransferPhase,
        Coherence,
        Impulse
    };

    static constexpr int numSources = 6;

    /** One trace: the frequency axis and the decibels on it.

        Impulse responses are the exception and are stored as a waveform against time rather than
        decibels against frequency, because the shape of the impulse is the reading and a curve
        of it in decibels would lose everything below the top. */
    struct Trace
    {
        bool valid = false;
        juce::String sourceName;
        std::vector<float> x;
        std::vector<float> y;

        std::size_t size() const noexcept { return x.size(); }
        bool isFinite() const noexcept;
        void clear() { valid = false; x.clear(); y.clear(); }
    };

    TraceStore();

    static juce::StringArray getKindNames();
    static juce::StringArray getSourceNames();
    static juce::String kindName (Kind kind);
    static juce::String sourceName (Source source);
    static bool isValidKind (int index) noexcept { return index >= 0 && index < numKinds; }
    static bool isValidSource (int index) noexcept { return index >= 0 && index < numSources; }

    /** Copies a curve in as the live trace. Rejected when the lengths do not match the trace
        already there, because a trace of a different length cannot be compared with anything. */
    void setLive (Source source, const std::vector<float>& x, const std::vector<float>& y);
    void setLive (Source source, const Trace& trace);

    /** Copies the live trace into one of the stored slots. */
    bool freeze (Kind kind);

    /** Folds the live trace into the average, which keeps running mean of every trace ever
        frozen into it. Returns the number of traces folded in. */
    int averageInto (Kind kind);

    /** Reference minus measurement, bin by bin, over the frequencies both share.

        Bin by frequency rather than by index, because the two traces may have come from
        different transform sizes and index n in one is not the same frequency as index n in the
        other. The result covers the frequencies both hold. */

    /** Difference of one stored kind against another, whichever is which.

        Both kinds are named because the useful comparison is not always reference against
        measurement. A curve held in memory from last week against today's reading is the
        comparison that answers whether a speaker has changed, and that pair has no reference
        trace in it at all. */
    bool computeDifference (Kind referenceKind, Kind measurementKind);

    /** Reference against measurement, which is the pair the name refers to. */
    bool computeDifference() { return computeDifference (Kind::Reference, Kind::Measurement); }

    Trace& get (Kind kind, Source source) noexcept;
    const Trace& get (Kind kind, Source source) const noexcept;
    bool has (Kind kind, Source source) const;

    /** Every source currently held for a kind. */
    std::vector<Source> sourcesFor (Kind kind) const;

    void clear (Kind kind);
    void clearAll();
    void resetAverage();

    /** Copies a trace in from a saved session. Bypasses the length rule, because a session file
        is trusted to hold a matched set and refusing to load it would be worse than loading it. */
    void setStored (Kind kind, Source source, const Trace& trace);

    int getAveragesFolded (Kind kind) const noexcept;
    void setAveragesFolded (Kind kind, int count) noexcept { averagedIn[(size_t) kind] = count; }

    /** How far apart two traces are, in decibels, over the frequencies both hold.

        The root mean square of the difference, which is one number for a whole curve and is
        what "how different are these two" has to mean when it is asked as a question rather
        than looked at. */
    static bool rmsDifference (const Trace& a, const Trace& b, float& rmsDbOut);

    /** The largest single bin difference. The thing that jumps out on a plot. */
    static bool peakDifference (const Trace& a, const Trace& b, float& peakDbOut,
                                float& frequencyOut);

private:
    static std::size_t index (Kind kind, Source source) noexcept
    {
        return (size_t) kind * (size_t) numSources + (size_t) source;
    }

    std::array<Trace, (size_t) (numKinds * numSources)> traces;
    std::array<int, (size_t) numKinds> averagedIn { { 0, 0, 0, 0, 0, 0 } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TraceStore)
};