#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

/** Single producer, multiple consumer lock free sample ring.

    One producer is the audio callback, which must never block, allocate or take a lock.
    Consumers are analysis readers on other threads, each with its own cursor, so the
    newest block is not stolen from the measurement path by whoever asks first.

    Positions are monotonic counters rather than wrapped indices, so a reader can tell
    how far behind it is and whether the producer has lapped it. Storage is plane major
    and allocated once in prepare(), which is only called while the device is stopped.
*/
class RealtimeRingBuffer
{
public:
    RealtimeRingBuffer() = default;
    ~RealtimeRingBuffer() = default;

    RealtimeRingBuffer (const RealtimeRingBuffer&) = delete;
    RealtimeRingBuffer& operator= (const RealtimeRingBuffer&) = delete;

    /** Allocates the storage. Never call this while the producer is running. */
    bool prepare (int numChannels, int capacitySamples)
    {
        const auto channels = std::max (1, numChannels);

        // A power of two keeps the wrap a mask instead of a division, and the extra head
        // room lets a late reader catch up before the producer laps it.
        int capacity = 1;

        while (capacity < std::max (16, capacitySamples))
            capacity <<= 1;

        storage.assign ((size_t) capacity * (size_t) channels, 0.0f);
        planeStride = capacity;
        this->numChannels = channels;
        this->capacitySamples = capacity;
        generation.fetch_add (1, std::memory_order_release);
        writePosition.store (0, std::memory_order_release);
        return true;
    }

    void reset()
    {
        writePosition.store (0, std::memory_order_release);
        generation.fetch_add (1, std::memory_order_release);
    }

    bool isReady() const noexcept
    {
        return planeStride > 0 && !storage.empty();
    }

    int getNumChannels() const noexcept { return numChannels; }
    int getCapacity() const noexcept { return capacitySamples; }

    int64_t getWritePosition() const noexcept
    {
        return writePosition.load (std::memory_order_acquire);
    }

    uint64_t getGeneration() const noexcept
    {
        return generation.load (std::memory_order_acquire);
    }

    int64_t getOverrunCount() const noexcept
    {
        return overruns.load (std::memory_order_relaxed);
    }

    /** Producer side. Copies numSamples frames, wrapping at the end of the ring.

        No allocation, no lock and no system call: this runs inside the audio callback.
        A null plane is written as silence rather than skipped, so a channel that stops
        being captured cannot leave the last samples it ever had sitting in the ring.
        Returns the number of frames stored, which is zero only when the ring has not
        been prepared. */
    int write (const float* const* source, int numSamples)
    {
        if (! isReady() || numSamples <= 0)
            return 0;

        const auto write = writePosition.load (std::memory_order_relaxed);
        auto offset = (int) (write % (int64_t) planeStride);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            if (source == nullptr || source[channel] == nullptr)
                fillWrapping (channel, offset, numSamples);
            else
                copyWrapping (source[channel], numSamples, channel, offset);
        }

        writePosition.store (write + numSamples, std::memory_order_release);
        return numSamples;
    }

    /** Consumer side, one cursor per reader.

        Returns the number of frames copied into destination, which is plane major:
        destination[channel * frames + i]. A reader that falls more than a full ring
        behind is resynchronised to the oldest data still held and the loss is counted. */
    int read (int64_t& cursor, float* destination, int maxChannels, int maxFrames)
    {
        if (! isReady() || destination == nullptr || maxFrames <= 0)
            return 0;

        const auto write = writePosition.load (std::memory_order_acquire);

        if (cursor > write)
            cursor = write;   // the ring was reset under this reader

        if (write - cursor > (int64_t) planeStride)
        {
            overruns.fetch_add (write - cursor - planeStride, std::memory_order_relaxed);
            cursor = write - planeStride;
        }

        const auto available = (int) std::min<int64_t> (write - cursor, maxFrames);
        auto offset = (int) (cursor % (int64_t) planeStride);
        const auto channels = std::min (maxChannels, numChannels);

        for (int channel = 0; channel < channels; ++channel)
            copyOutWrapping (cursor, offset, destination + (size_t) channel * available, channel, available);

        cursor += available;
        return available;
    }

/** Copies one plane only, for a reader that follows a single channel.

        The generator output is such a case: it is captured as its own plane so the
        callback never has to lock a second copy of it. */
    int readPlane (int64_t& cursor, int plane, float* destination, int maxFrames)
    {
        if (! isReady() || destination == nullptr || maxFrames <= 0)
            return 0;

        const auto write = writePosition.load (std::memory_order_acquire);

        if (cursor > write)
            cursor = write;

        if (write - cursor > (int64_t) planeStride)
        {
            overruns.fetch_add (write - cursor - planeStride, std::memory_order_relaxed);
            cursor = write - planeStride;
        }

        const auto frames = (int) std::min<int64_t> (write - cursor, maxFrames);

        if (frames <= 0 || plane < 0 || plane >= numChannels)
            return 0;

        const auto offset = (int) (cursor % (int64_t) planeStride);
        const auto* source = storage.data() + (size_t) plane * (size_t) planeStride;
        const auto first = std::min (frames, planeStride - offset);
        std::memcpy (destination, source + offset, (size_t) first * sizeof (float));

        if (first < frames)
            std::memcpy (destination + first, source, (size_t) (frames - first) * sizeof (float));

        cursor += frames;
        return frames;
    }

private:
    void copyWrapping (const float* source, int numSamples, int channel, int offset)
    {
        auto* plane = storage.data() + (size_t) channel * (size_t) planeStride;
        const auto first = std::min (numSamples, planeStride - offset);
        std::memcpy (plane + offset, source, (size_t) first * sizeof (float));

        if (first < numSamples)
            std::memcpy (plane, source + first,
                         (size_t) (numSamples - first) * sizeof (float));
    }

    void fillWrapping (int channel, int offset, int numSamples)
    {
        auto* plane = storage.data() + (size_t) channel * (size_t) planeStride;
        const auto first = std::min (numSamples, planeStride - offset);
        std::memset (plane + offset, 0, (size_t) first * sizeof (float));

        if (first < numSamples)
            std::memset (plane, 0, (size_t) (numSamples - first) * sizeof (float));
    }

    void copyOutWrapping (int64_t cursor, int offset, float* destination, int channel,
                          int frames)
    {
        const auto* plane = storage.data() + (size_t) channel * (size_t) planeStride;
        const auto first = std::min (frames, planeStride - offset);
        std::memcpy (destination, plane + offset, (size_t) first * sizeof (float));

        if (first < frames)
            std::memcpy (destination + first, plane, (size_t) (frames - first) * sizeof (float));
    }

    std::vector<float> storage;
    int planeStride = 0;
    int numChannels = 0;
    int capacitySamples = 0;
    std::atomic<int64_t> writePosition { 0 };
    std::atomic<int64_t> overruns { 0 };
    std::atomic<uint64_t> generation { 0 };
};
