#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "SignalGenerator.h"

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

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
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

    juce::String getOutputPulseSink(int index) const;
    void stop();
    bool isRunning() const { return deviceManager.getCurrentAudioDevice() != nullptr; }

    void getLatestBlock(int numSamples, std::vector<float>& ref, std::vector<float>& meas,
                        std::vector<float>* generated = nullptr);
    int getCapturedChannels() const { return capturedChannels.load(); }
    double getSampleRate() const { return currentSampleRate.load(); }
    int getBufferSize() const { return currentBufferSize.load(); }

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    SignalGenerator& getGenerator() { return generator; }
    void getGeneratorOutput(std::vector<float>& target);

    std::function<void(const juce::String&)> onStatusMessage;

private:
    void setStatus(const juce::String& text);
    void initialiseBuffers(double sampleRate, int bufferSize);

    juce::AudioDeviceManager deviceManager;

    std::vector<AudioDeviceChoice> inputChoices;
    std::vector<AudioDeviceChoice> outputChoices;
    juce::StringArray inputLabels;
    juce::StringArray outputLabels;

    std::atomic<double> currentSampleRate{48000.0};
    std::atomic<int> currentBufferSize{1024};
    std::atomic<int> capturedChannels{0};
    std::atomic<int> writePosition{0};
    std::atomic<int> samplesFilled{0};

    std::vector<std::vector<float>> ringBuffer;
    int capacity = 0;
    std::mutex bufferMutex;

    SignalGenerator generator;

    std::vector<float> generatorCapture;
    int capturedGeneratorSamples = 0;
    juce::CriticalSection generatorLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
