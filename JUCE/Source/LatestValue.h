#pragma once

#include <atomic>
#include <cstdint>

/** Publishes one value from a producer thread and reads the latest from a consumer.

    The measurement worker publishes, the GUI reads, and neither ever waits for the
    other: a repaint that arrives late simply gets the previous reading, which is what a
    meter is supposed to do. Two slots are used so a reader that is copying one is never
    overwritten by the writer.
*/
template <typename Value>
class LatestValue
{
public:
    LatestValue() = default;

    LatestValue (const LatestValue&) = delete;
    LatestValue& operator= (const LatestValue&) = delete;

    void set (const Value& value)
    {
        const auto generation = state.load (std::memory_order_relaxed);
        slots[generation % 2] = value;
        state.store (generation + 1, std::memory_order_release);
    }

    bool get (Value& destination) const
    {
        // A writer can move on between the generation read and the copy, so the copy is
        // retried rather than returned half written.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto generation = state.load (std::memory_order_acquire);

            if (generation == 0)
                return false;

            destination = slots[(generation - 1) % 2];

            if (state.load (std::memory_order_acquire) == generation)
                return true;
        }

        return true;
    }

    /** Number of values published since the last reset. */
    uint64_t getCount() const { return state.load (std::memory_order_acquire); }

    void reset() { state.store (0, std::memory_order_release); }

private:
    Value slots[2] {};
    std::atomic<uint64_t> state { 0 };
};
