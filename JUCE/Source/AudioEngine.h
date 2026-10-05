#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "RealtimeRingBuffer.h"
#include "SignalGenerator.h"
#include "WavRecorder.h"

struct AudioDeviceChoice
{
    int typeIndex = 0;
    juce::String name;
    bool enabled = false;
    int inputChannels = 0;
    int outputChannels = 0;
    juce::String pulseSink;
    juce::StringArray inputJacks;
};

struct HardwareInputGainInfo
{
    bool available = false;
    float minimumDb = 0.0f;
    float maximumDb = 0.0f;
    float currentDb = 0.0f;
};

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    /** Which output channels carry the generator.

        Sending the signal to one side only is what makes a two channel setup usable as
        a measurement: the amplifier is fed from one output while the other stays silent,
        so the reference tap and the microphone can be kept apart instead of hearing the
        same signal on both.
    */
    enum class OutputRouting { Both, Left, Right };

    AudioEngine();
    ~AudioEngine() override;

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void scanDevices();

    juce::StringArray getInputDeviceNames() const;
    juce::StringArray getOutputDeviceNames() const;
    juce::StringArray getInputDeviceLabels() const;
    juce::StringArray getOutputDeviceLabels() const;

    void start(const juce::String& inputDeviceName,
               const juce::String& outputDeviceName,
               double sampleRate,
               int bufferSize,
               const juce::String& outputPulseSink = juce::String());

    /** Reopens the device with the settings the last successful start() used.

        Called after an unexpected stop, so a device that was unplugged and plugged back
        in does not need the whole window to be restarted. Returns false and reports the
        reason through onStatusMessage when it cannot be reopened. */
    bool reconnect();

    /** Reopens the device when it has been stopped by something other than stop().

        This is called from the message thread on a timer, never from the audio callback:
        a callback that disappeared is discovered by the device, but only the message
        thread may reopen devices or touch the GUI. Returns true when something was
        reported or attempted. */
    bool pollDeviceHealth();

    void setAutoReconnect(bool shouldReconnect) { autoReconnect.store(shouldReconnect); }
    bool isAutoReconnectEnabled() const { return autoReconnect.load(); }

    /** True when the device stopped on its own, e.g. it was unplugged. */
    bool isDeviceLost() const { return deviceLost.load(std::memory_order_acquire); }

    /** Audio callbacks the device has run since the last start, and the number of samples
        a reader missed. Both are read from the message thread for the status line. */
    uint64_t getCallbackCount() const { return callbackCount.load(std::memory_order_relaxed); }
    int getCurrentSampleRateInt() const { return currentSampleRateInt.load(); }
    int getCurrentBufferSizeInt() const { return currentBufferSizeInt.load(); }
    int getCapturedChannelCount() const { return capturedChannels.load(); }

    /** Channel layout the engine asks for. Two in and two out, so the reference and the
        microphone can be kept apart; a mono device is handled downstream. */
    static constexpr int requestedInputChannels = 2;
    static constexpr int requestedOutputChannels = 2;

    juce::String getOutputPulseSink(int index) const;
    void stop();
    bool isRunning() const { return deviceManager.getCurrentAudioDevice() != nullptr; }

    /** True when the capture storage exists and the callback is allowed to write into
        it. False between stopping and starting, which is what keeps a callback that
        arrives late from touching memory that is being reallocated. */
    bool isReadyForCapture() const { return buffersReady.load(std::memory_order_acquire); }

    /** The two captured input channels, left then right. Which of them is the
        measurement and which the reference is the caller's choice, so the wiring can be
        assigned per measurement instead of being fixed by the capture order. */
    void getLatestBlock(int numSamples, std::vector<float>& left, std::vector<float>& right,
                        std::vector<float>* generated = nullptr);

    /** Copies the newest complete window without consuming the stream reader. Intended for
        a live graph that needs a fresh overlapping FFT frame on every UI refresh. */
    bool readLatestFrame(int frameSize, std::vector<float>& left, std::vector<float>& right,
                         std::vector<float>* generated = nullptr);

    /** Only the samples this reader has not seen yet, in arrival order.

        getLatestBlock always answers with the newest window, which is the right answer
        for a display and the wrong one for a spectrum analyser: consecutive windows
        overlap, so an analyser fed from it would average the same audio over and over
        and report more averaging than actually happened. This reader hands out each
        captured sample exactly once, so the analyser sees one continuous stream. */
    void readNewSamples(std::vector<float>& left, std::vector<float>& right,
                        std::vector<float>* generated = nullptr);

    /** Collects whole frames of exactly frameSize samples, or reports that none is ready yet.

        getLatestBlock cannot be used for a transform. It hands over the audio that arrived
        since the previous call and consumes it, so at a display refresh rate it can only ever
        return about one device block, while a 16384 point FFT needs 16384 contiguous samples
        before it can start at all. Asking it for a frame therefore never succeeds and the
        measurement never runs.

        This reader accumulates instead: samples are appended as they arrive, and once a whole
        frame exists the oldest one is handed over and consumed, with the remainder kept for
        the next call. Every sample is therefore transformed exactly once.

        Returns true and fills the outputs only when a whole frame was available. The channels
        always come back the same length, because the caller walks them together. */
    bool readFrame(int frameSize, std::vector<float>& left, std::vector<float>& right,
                   std::vector<float>* generated = nullptr);

    /** Drops anything half collected, so the next frame starts from the audio that exists
        now rather than joining it to audio from before a device change. */
    void resetFrameReader();

    /** Samples the reader above has not consumed yet. */
    int getPendingSampleCount() const;

    /** Captured samples lost because a reader fell more than a full ring behind. */
    int64_t getCaptureOverrunCount() const;

    /** Bumped every time the capture storage is reallocated, so a reader can tell that
        its cursor was reset underneath it. */
    uint64_t getCaptureGeneration() const { return capture.getGeneration(); }

    /** How an input channel is named on screen, so the assignment, the display and the
        messages all describe a channel the same way. */
    static juce::String channelName(int channelIndex);
    int getCapturedChannels() const { return capturedChannels.load(); }
    double getSampleRate() const { return currentSampleRate.load(); }
    int getBufferSize() const { return currentBufferSize.load(); }

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    /** ALSA hardware capture gain when the selected card exposes a capture-volume control. */
    HardwareInputGainInfo getHardwareInputGainInfo() const;
    bool setHardwareInputGainDb (float gainDb);

    /** Hands the callback's captured audio to a recorder.

        Held as a pointer the callback reads without locking, and only ever dereferenced to call
        one method that does nothing but copy into a ring. The recorder is never called back
        into, so arming one cannot make the audio thread wait on anything. */
    void setRecorder (WavRecorder* newRecorder) { recorder.store (newRecorder, std::memory_order_release); }
    WavRecorder* getRecorder() const { return recorder.load (std::memory_order_acquire); }

    SignalGenerator& getGenerator() { return generator; }
    /** Copies the newest output samples for a live generator graph. */
    void getGeneratorOutput(std::vector<float>& target, int numSamples = 0);

    void setGeneratorRouting(OutputRouting routing) { generatorRouting.store(routing); }
    OutputRouting getGeneratorRouting() const { return generatorRouting.load(); }

    std::function<void(const juce::String&)> onStatusMessage;
    /** Reported from pollDeviceHealth(), so always on the message thread. */
    std::function<void()> onDeviceLost;

private:
    class InputGainControl;

    void setStatus(const juce::String& text);
    void initialiseBuffers(double sampleRate, int bufferSize);
    void releaseBuffers();
    /** Resets a reader cursor when the ring has been reallocated underneath it. */
    void syncReader(uint64_t& cursorGeneration, int64_t& cursor) const;

    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<InputGainControl> inputGainControl;

    std::vector<AudioDeviceChoice> inputChoices;
    std::vector<AudioDeviceChoice> outputChoices;
    juce::StringArray inputLabels;
    juce::StringArray outputLabels;

    std::atomic<double> currentSampleRate{48000.0};
    std::atomic<int> currentBufferSize{1024};
    std::atomic<int> currentSampleRateInt{48000};
    std::atomic<int> currentBufferSizeInt{1024};
    std::atomic<int> capturedChannels{0};
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<bool> deviceLost{false};
    std::atomic<bool> autoReconnect{true};
    /** Cleared by the device thread, read by the message thread. Stops the callback from
        writing into storage that is about to be reallocated. */
    std::atomic<bool> buffersReady{false};

    /** Capture transport. The audio callback is the only writer; every reader keeps its
        own cursor, so the ring replaces the mutex that used to be held across both. */
    RealtimeRingBuffer capture;
    mutable int64_t latestCursor = 0;

    /** Reader for whole transform frames. It keeps its own cursor so it cannot take audio
        from either of the readers above, which would leave one of them short. */
    void readFromRing(uint64_t& cursorGeneration, int64_t& cursor,
                      std::vector<float>& left, std::vector<float>& right,
                      std::vector<float>* generated);

    mutable uint64_t frameGeneration = 0;
    mutable int64_t frameCursor = 0;
    uint64_t frameAccumGeneration = 0;
    std::vector<float> frameLeftAccum;
    std::vector<float> frameRightAccum;
    std::vector<float> frameGeneratedAccum;
    mutable int64_t newSampleCursor = 0;
    mutable uint64_t latestGeneration = 0;
    mutable uint64_t newSampleGeneration = 0;
    /** Reused by the readers so the steady state does not allocate. Message thread only. */
    mutable std::vector<float> captureScratch;

    SignalGenerator generator;

    std::atomic<WavRecorder*> recorder { nullptr };

    std::atomic<OutputRouting> generatorRouting { OutputRouting::Both };

    // Settings of the last successful start, so a reconnect can repeat it exactly.
    juce::String lastInputDeviceName;
    juce::String lastOutputDeviceName;
    juce::String lastPulseSink;
    double lastRequestedSampleRate = 48000.0;
    int lastRequestedBufferSize = 1024;
    bool hasLastSetup = false;
    bool startingOrStopping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
