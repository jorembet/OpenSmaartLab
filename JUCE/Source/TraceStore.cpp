#include "TraceStore.h"

namespace
{
    struct KindName { TraceStore::Kind kind; const char* name; };

    const KindName kindNames[] =
    {
        { TraceStore::Kind::Live,        "Live" },
        { TraceStore::Kind::Reference,   "Reference" },
        { TraceStore::Kind::Measurement, "Measurement" },
        { TraceStore::Kind::Average,     "Average" },
        { TraceStore::Kind::Memory,      "Memory" },
        { TraceStore::Kind::Difference,  "Difference" }
    };

    struct SourceName { TraceStore::Source source; const char* name; };

    const SourceName sourceNames[] =
    {
        { TraceStore::Source::Spectrum,          "Spectrum" },
        { TraceStore::Source::Rta,               "RTA" },
        { TraceStore::Source::TransferMagnitude, "Magnitude" },
        { TraceStore::Source::TransferPhase,     "Phase" },
        { TraceStore::Source::Coherence,         "Coherence" },
        { TraceStore::Source::Impulse,           "Impulse" }
    };
}

bool TraceStore::Trace::isFinite() const noexcept
{
    for (const auto value : x)
        if (! std::isfinite (value))
            return false;

    for (const auto value : y)
        if (! std::isfinite (value))
            return false;

    return true;
}

TraceStore::TraceStore() = default;

juce::StringArray TraceStore::getKindNames()
{
    juce::StringArray names;

    for (const auto& entry : kindNames)
        names.add (entry.name);

    return names;
}

juce::StringArray TraceStore::getSourceNames()
{
    juce::StringArray names;

    for (const auto& entry : sourceNames)
        names.add (entry.name);

    return names;
}

juce::String TraceStore::kindName (Kind kind)
{
    for (const auto& entry : kindNames)
        if (entry.kind == kind)
            return entry.name;

    return juce::String();
}

juce::String TraceStore::sourceName (Source source)
{
    for (const auto& entry : sourceNames)
        if (entry.source == source)
            return entry.name;

    return juce::String();
}

void TraceStore::setLive (Source source, const std::vector<float>& x,
                          const std::vector<float>& y)
{
    Trace& trace = get (Kind::Live, source);

    // A curve of a different length than the one already live is refused rather than accepted.
    // The traces are compared bin by bin, and comparing bin n of one curve with bin n of a
    // curve of a different length compares two different frequencies and calls it a difference.
    if (trace.valid && trace.size() != x.size())
        return;

    trace.x = x;
    trace.y = y;
    trace.valid = x.size() == y.size() && ! x.empty();
    trace.sourceName = sourceName (source);
}

void TraceStore::setLive (Source source, const Trace& newTrace)
{
    Trace& trace = get (Kind::Live, source);

    if (trace.valid && trace.size() != newTrace.size())
        return;

    trace = newTrace;
    trace.valid = ! trace.x.empty() && trace.x.size() == trace.y.size();
}

bool TraceStore::freeze (Kind kind)
{
    if (kind == Kind::Live || kind == Kind::Difference)
        return false;

    bool any = false;

    for (int source = 0; source < numSources; ++source)
    {
        const auto sourceValue = (Source) source;
        const auto& live = get (Kind::Live, sourceValue);

        if (! live.valid)
            continue;

        auto& stored = get (kind, sourceValue);

        if (stored.valid && stored.size() != live.size())
            continue;

        stored = live;
        any = true;
    }

    return any;
}

int TraceStore::averageInto (Kind kind)
{
    if (kind != Kind::Average)
        return 0;

    auto& averages = traces[std::size_t (Kind::Average) * (size_t) numSources];
    (void) averages;

    int folded = 0;

    for (int source = 0; source < numSources; ++source)
    {
        const auto sourceValue = (Source) source;
        const auto& live = get (Kind::Live, sourceValue);

        if (! live.valid)
            continue;

        auto& running = get (Kind::Average, sourceValue);

        // A running mean over the curves folded in so far, so a long average costs nothing to
        // keep and one late curve does not have to be read with all the others again.
        const auto previous = averagedIn[(size_t) kind];

        if (! running.valid || running.size() != live.size())
        {
            running = live;
            folded = previous + 1;
            continue;
        }

        if (previous <= 0)
        {
            running = live;
            folded = 1;
            continue;
        }

        const auto weight = 1.0f / (float) (previous + 1);

        for (std::size_t i = 0; i < running.y.size(); ++i)
            running.y[i] += (live.y[i] - running.y[i]) * weight;

        folded = previous + 1;
    }

    averagedIn[(size_t) kind] = folded;
    return folded;
}

bool TraceStore::computeDifference (Kind referenceKind, Kind measurementKind)
{
    bool any = false;

    for (int source = 0; source < numSources; ++source)
    {
        const auto sourceValue = (Source) source;

        const auto& reference = get (referenceKind, sourceValue);
        const auto& measured = get (measurementKind, sourceValue);

        if (! reference.valid || ! measured.valid)
            continue;

        if (reference.size() != measured.size())
            continue;

        Trace difference;
        difference.sourceName = sourceName (sourceValue);
        difference.x = reference.x;

        for (std::size_t i = 0; i < reference.y.size(); ++i)
            difference.y.push_back (reference.y[i] - measured.y[i]);

        difference.valid = true;
        get (Kind::Difference, sourceValue) = difference;
        any = true;
    }

    return any;
}

TraceStore::Trace& TraceStore::get (Kind kind, Source source) noexcept
{
    return traces[index (kind, source)];
}

const TraceStore::Trace& TraceStore::get (Kind kind, Source source) const noexcept
{
    return traces[index (kind, source)];
}

bool TraceStore::has (Kind kind, Source source) const
{
    return get (kind, source).valid;
}

std::vector<TraceStore::Source> TraceStore::sourcesFor (Kind kind) const
{
    std::vector<Source> found;

    for (int source = 0; source < numSources; ++source)
        if (get (kind, (Source) source).valid)
            found.push_back ((Source) source);

    return found;
}

int TraceStore::getAveragesFolded (Kind kind) const noexcept
{
    const auto index = (std::size_t) kind;
    return index < averagedIn.size() ? averagedIn[index] : 0;
}

void TraceStore::clear (Kind kind)
{
    for (int source = 0; source < numSources; ++source)
        get (kind, (Source) source).clear();

    averagedIn[(size_t) kind] = 0;
}

void TraceStore::clearAll()
{
    for (auto& trace : traces)
        trace.clear();

    averagedIn.fill (0);
}

void TraceStore::resetAverage()
{
    clear (Kind::Average);
}

void TraceStore::setStored (Kind kind, Source source, const Trace& trace)
{
    traces[index (kind, source)] = trace;
}

bool TraceStore::rmsDifference (const Trace& a, const Trace& b, float& rmsDbOut)
{
    if (! a.valid || ! b.valid || a.y.size() != b.y.size() || a.y.empty())
        return false;

    double sum = 0.0;

    for (std::size_t i = 0; i < a.y.size(); ++i)
    {
        const auto difference = (double) a.y[i] - (double) b.y[i];
        sum += difference * difference;
    }

    rmsDbOut = (float) std::sqrt (sum / (double) a.y.size());
    return true;
}

bool TraceStore::peakDifference (const Trace& a, const Trace& b, float& peakDbOut,
                                 float& frequencyOut)
{
    if (! a.valid || ! b.valid || a.y.size() != b.y.size() || a.y.empty())
        return false;

    float worst = 0.0f;
    std::size_t worstIndex = 0;

    for (std::size_t i = 0; i < a.y.size(); ++i)
    {
        const auto difference = std::abs (a.y[i] - b.y[i]);

        if (difference > worst)
        {
            worst = difference;
            worstIndex = i;
        }
    }

    peakDbOut = worst;
    frequencyOut = worstIndex < a.x.size() ? a.x[worstIndex] : 0.0f;
    return true;
}