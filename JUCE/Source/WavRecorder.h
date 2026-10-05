#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "RealtimeRingBuffer.h"

/** Writes the audio arriving from the callback out to a wav file.

    The rule this class exists to keep is that the audio callback never touches the disk. The
    callback hands samples to a lock free ring and returns; a pump on the message thread moves
    them from the ring into the file. A recorder that wrote from the callback would put a file
    write on the audio thread, and a file write is a wait on a drive, and a wait on the audio
    thread is an overrun.

    Recording is therefore two hops, and the buffer between them is finite. A pump that falls
    behind loses audio and says so through getDroppedSamples(), because a recording that quietly
    skips is worse than one that admits a gap: the gap is visible in the phase and an unrecorded
    gap is not. */
class WavRecorder
{
public:
    enum class State
    {
        Idle,        ///< Nothing open.
        Recording,   ///< Samples are reaching the file.
        Paused,      ///< The file is open and held; arriving audio is discarded, not written.
        Stopped      ///< The pump has finished; the file can still be saved.
    };

    /** The bit depths a measurement file is worth keeping.

        Sixteen bit is what a wav is expected to be and is fine for a loud signal. Twenty four
        bit is the working depth: it holds a quiet measurement with real headroom and no
        dithering artefacts in the noise floor. Float is for files that will be processed
        again, because nothing is clipped and nothing is quantised on the way in. */
    enum class BitDepth
    {
        Int16,
        Int24,
        Float32
    };

    /** Seconds of audio held between the callback and the pump.

        Two seconds is long enough that a busy message thread does not lose audio, and short
        enough that the memory cost stays a few megabytes at the highest sample rate. */
    static constexpr int ringSeconds = 2;

    WavRecorder();
    ~WavRecorder();

    /** Opens a file and starts accepting audio. Returns false and fills errorOut on refusal. */
    bool startRecording (const juce::File& target, double sampleRate, int numChannels,
                        BitDepth bitDepth, juce::String* errorOut = nullptr);

    /** Holds the recording. Audio arriving while paused is discarded rather than written, so
        the file stays a continuous waveform with no silence inserted into it. The discarded
        time is counted and reported by getPausedSamples(). */
    void pause();

    /** Continues a paused recording from the sample after the pause, not from the sample the
        pause began at. */
    void resume();

    /** Stops accepting audio. Any audio still in the ring is written first. */
    void stop();

    /** Called from the audio callback. Lock free: a memcpy into the ring and nothing else. */
    void pushFromAudioThread (const float* const* planes, int numSamples) noexcept;

    /** Moves buffered audio into the file. Message thread only, and safe to call often. */
    void pump();

    /** Closes the file and leaves it where it was asked for. Safe to call more than once. */
    bool save();

    /** Closes and deletes anything not saved, so a discarded recording leaves nothing behind. */
    void discard();

    State getState() const noexcept { return state.load (std::memory_order_acquire); }
    bool isRecording() const { return getState() == State::Recording; }
    bool isPaused() const { return getState() == State::Paused; }
    bool isOpen() const { return writer != nullptr; }
    BitDepth getBitDepth() const noexcept { return bitDepth; }
    double getSampleRate() const noexcept { return sampleRate; }
    int getNumChannels() const noexcept { return numChannels; }

    /** Samples written to the file so far. */
    int64_t getRecordedSamples() const noexcept { return recordedSamples.load (std::memory_order_acquire); }
    int64_t getPausedSamples() const noexcept { return pausedSamples.load (std::memory_order_acquire); }
    int64_t getDroppedSamples() const noexcept { return droppedSamples.load (std::memory_order_acquire); }
    double getRecordedSeconds() const noexcept;

    juce::File getTargetFile() const { return targetFile; }
    juce::String getLastError() const { return lastError; }

    /** The wav subtype a depth is written as, exposed so the tests and the interface agree. */
    static juce::String getFormatName (BitDepth depth);
    static juce::StringArray getBitDepthNames();
    static juce::StringArray getSupportedSampleRates();
    static bool isSampleRateSupported (double sampleRate);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavRecorder)

private:
    void teardown();

    std::unique_ptr<juce::AudioFormatWriter> writer;

    /** The stream being written into, until the writer takes it.

        An audio format writer deletes its stream in its own destructor. This owns the stream
        only for the moment between opening it and the writer being created, and hands ownership
        over at that point, so exactly one thing ever frees it. */
    std::unique_ptr<juce::OutputStream> stream;
    juce::File targetFile;
    juce::String lastError;

    RealtimeRingBuffer ring;
    int64_t readCursor = 0;
    uint64_t readGeneration = 0;
    std::vector<float> pumpScratch;

    /** Staging buffer handed to the writer, one plane per channel. Reusing it keeps a running
        recording from allocating on the message thread every block. */
    juce::AudioBuffer<float> interleaved;

    std::atomic<State> state { State::Idle };
    std::atomic<int64_t> recordedSamples { 0 };
    std::atomic<int64_t> pausedSamples { 0 };
    std::atomic<int64_t> droppedSamples { 0 };
    std::atomic<bool> finishing { false };

    /** Set once save() has run, so the recording survives the recorder being destroyed.

        Without it, discarding a recorder that had been saved deleted the file that had just
        been written: the state was Stopped rather than Idle after a save, so the discard at the
        end of the recorder's life looked exactly like throwing away an unsaved recording. */
    std::atomic<bool> savedToDisk { false };

    double sampleRate = 48000.0;
    int numChannels = 2;
    BitDepth bitDepth = BitDepth::Int24;
};