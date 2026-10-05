#include "DspWorker.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    void require (bool condition, const char* message)
    {
        if (! condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit (1);
        }
    }
}

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int sampleCount = 48000;
    constexpr double frequency = 997.0;

    std::vector<float> left ((size_t) sampleCount);
    std::vector<float> right ((size_t) sampleCount);

    for (int i = 0; i < sampleCount; ++i)
    {
        const auto phase = 2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate;
        left[(size_t) i] = (float) (0.5 * std::sin (phase));
        right[(size_t) i] = (float) (0.25 * std::sin (phase));
    }

    DspWorker worker;
    worker.setSpectrumEnabled (false);
    worker.setRtaEnabled (false);
    worker.start();
    worker.pushSamples (left.data(), right.data(), left.data(), right.data(), nullptr,
                        sampleCount);

    DspWorker::Snapshot snapshot;
    auto received = false;

    for (int attempt = 0; attempt < 100 && ! received; ++attempt)
    {
        received = worker.getSnapshot (snapshot)
                && snapshot.inputLeft.valid
                && snapshot.inputRight.valid;

        if (! received)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    worker.stop();

    require (received, "The worker must publish independent Left and Right input readings");
    require (std::abs (snapshot.inputLeft.rmsDbfs - (-9.03f)) < 0.15f,
             "Left input must report the level from the physical Left channel");
    require (std::abs (snapshot.inputRight.rmsDbfs - (-15.05f)) < 0.15f,
             "Right input must report the level from the physical Right channel");
    require (snapshot.samplesProcessed == (uint64_t) sampleCount,
             "The Input Level sample counter must advance with captured audio");

    std::cout << "PASS: stereo input worker preserves Left and Right levels independently\n";
}
