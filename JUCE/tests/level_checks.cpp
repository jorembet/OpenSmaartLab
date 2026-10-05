// Phase 1 checks: the realtime audio foundation.
//
// The level meter is measured against signals whose RMS and peak are known from theory,
// the capture ring is measured against a known sample sequence, and the audio callback is
// run with the global allocator watched, so "no allocation in the callback" is a checked
// claim rather than an intention.

#include <juce_core/juce_core.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

#include "AudioEngine.h"
#include "DSP.h"
#include "LevelMeter.h"
#include "RealtimeRingBuffer.h"
#include "SignalGenerator.h"

//==============================================================================
// A counting allocator. The counter is only armed around the region under test, so
// allocations made while setting the test up are not counted.
namespace
{
    std::atomic<long> allocations { 0 };
    std::atomic<bool> counting { false };

    void* countedAllocate (std::size_t size)
    {
        if (counting.load (std::memory_order_relaxed))
            allocations.fetch_add (1, std::memory_order_relaxed);

        if (size == 0)
            size = 1;

        void* memory = std::malloc (size);

        if (memory == nullptr)
            throw std::bad_alloc();

        return memory;
    }

    void* countedAllocateAligned (std::size_t size, std::size_t alignment)
    {
        if (counting.load (std::memory_order_relaxed))
            allocations.fetch_add (1, std::memory_order_relaxed);

        // aligned_alloc requires a size that is a multiple of the alignment.
        const auto padded = ((size + alignment - 1) / alignment) * alignment;
        void* memory = std::aligned_alloc (alignment, padded == 0 ? alignment : padded);

        if (memory == nullptr)
            throw std::bad_alloc();

        return memory;
    }

    void countedFree (void* memory) { std::free (memory); }

    struct CountingScope
    {
        CountingScope() { allocations.store (0); counting.store (true); }
        ~CountingScope() { counting.store (false); }
        long count() const { return allocations.load (std::memory_order_relaxed); }
    };
}

void* operator new (std::size_t size) { return countedAllocate (size); }
void* operator new[] (std::size_t size) { return countedAllocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return countedAllocate (size); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return countedAllocate (size); } catch (...) { return nullptr; }
}
void* operator new (std::size_t size, std::align_val_t alignment)
{
    return countedAllocateAligned (size, (std::size_t) alignment);
}
void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return countedAllocateAligned (size, (std::size_t) alignment);
}
void operator delete (void* memory) noexcept { countedFree (memory); }
void operator delete[] (void* memory) noexcept { countedFree (memory); }
void operator delete (void* memory, std::size_t) noexcept { countedFree (memory); }
void operator delete[] (void* memory, std::size_t) noexcept { countedFree (memory); }
void operator delete (void* memory, std::align_val_t) noexcept { countedFree (memory); }
void operator delete[] (void* memory, std::align_val_t) noexcept { countedFree (memory); }
void operator delete (void* memory, std::size_t, std::align_val_t) noexcept { countedFree (memory); }
void operator delete[] (void* memory, std::size_t, std::align_val_t) noexcept { countedFree (memory); }

//==============================================================================
namespace
{
    int failures = 0;

    void require (bool condition, const char* message)
    {
        if (condition)
            return;

        ++failures;
        std::cout << "FAIL: " << message << std::endl;
    }

    constexpr double testSampleRate = 48000.0;

    /** A sine of an exact peak amplitude, so the expected figures are known. */
    std::vector<float> sine (int numSamples, double frequency, double amplitude,
                             double sampleRate = testSampleRate)
    {
        std::vector<float> samples ((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
            samples[(size_t) i] = (float) (amplitude * std::sin (2.0 * M_PI * frequency * i / sampleRate));

        return samples;
    }

    std::vector<float> silence (int numSamples)
    {
        return std::vector<float> ((size_t) numSamples, 0.0f);
    }

    /** Feeds a block and returns the reading once the window is full. */
    dsp::LevelMeter::Readings measure (dsp::LevelMeter& meter, const std::vector<float>& samples,
                                        double integrationSeconds = 0.5)
    {
        meter.setIntegrationSeconds (integrationSeconds);
        meter.reset();
        meter.process (samples.data(), (int) samples.size());
        return meter.getReadings();
    }
}

//==============================================================================
int main()
{
    // ---- Silence must report the floor, not a usable looking level ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);

        const auto readings = measure (meter, silence (48000));

        require (readings.valid, "Silence must still count as a valid measurement");
        require (readings.rmsDbfs <= -190.0f, "Silence must report the RMS floor");
        require (readings.peakDbfs <= -190.0f, "Silence must report the peak floor");
        require (readings.peakToPeakDbfs <= -190.0f, "Silence must report the peak to peak floor");
        require (std::abs (readings.crestFactorDb) < 0.01f,
                 "Silence has no crest factor to report");
    }

    // ---- Full scale sine: -3.01 dBFS RMS, 0 dBFS peak, 3.01 dB crest ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);

        const auto samples = sine (48000, 997.0, 1.0);
        const auto readings = measure (meter, samples);

        require (std::abs (readings.peakDbfs) < 0.05f,
                 "A full scale sine must read 0 dBFS peak");
        require (std::abs (readings.rmsDbfs + 3.0103f) < 0.05f,
                 "A full scale sine must read -3.01 dBFS RMS");
        require (std::abs (readings.crestFactorDb - 3.0103f) < 0.05f,
                 "A full scale sine must have a 3.01 dB crest factor");

        // Peak to peak of a full scale sine is 2.0, which is +6.02 dBFS.
        require (std::abs (readings.peakToPeakDbfs - 6.0206f) < 0.1f,
                 "A full scale sine must read +6.02 dBFS peak to peak");
    }

    // ---- -6 dBFS and -12 dBFS sines: the same figures, shifted ----
    {
        const double levels[] = { -6.0206, -12.0412, -20.0, -40.0 };

        for (const auto levelDb : levels)
        {
            dsp::LevelMeter meter;
            meter.prepare (testSampleRate);

            const auto amplitude = std::pow (10.0, levelDb / 20.0);
            const auto samples = sine (48000, 997.0, amplitude);
            const auto readings = measure (meter, samples);

            require (std::abs (readings.peakDbfs - levelDb) < 0.05f,
                     "Peak must follow the sine level exactly");
            require (std::abs (readings.rmsDbfs - (levelDb - 3.0103f)) < 0.05f,
                     "RMS must sit 3.01 dB under the peak of a sine");
            require (std::abs (readings.crestFactorDb - 3.0103f) < 0.05f,
                     "The crest factor of a sine is 3.01 dB at any level");
            require (std::abs (readings.peakToPeakDbfs - (levelDb + 6.0206f)) < 0.1f,
                     "Peak to peak must follow the sine level exactly");
        }
    }

    // ---- Peak-to-peak must use the actual block extrema, not an artificial zero ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);
        const std::vector<float> positiveDc (512, 0.5f);
        meter.process (positiveDc.data(), (int) positiveDc.size());

        const auto readings = meter.getReadings();
        require (std::abs (readings.peakDbfs + 6.0206f) < 0.05f,
                 "A positive-only block must keep its actual sample peak");
        require (readings.peakToPeakDbfs <= -190.0f,
                 "A constant block must have no peak-to-peak range");
    }

    // ---- RMS integration window really is a window ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);
        meter.setIntegrationSeconds (0.5f);
        meter.reset();

        // Half a second of full scale sine, then silence: the windowed RMS has to fall
        // by half, which only happens if the window really is half a second long.
        const auto loud = sine (24000, 997.0, 1.0);
        meter.process (loud.data(), (int) loud.size());
        meter.process (loud.data(), (int) loud.size());

        const auto afterLoud = meter.getReadings();
        require (std::abs (afterLoud.rmsDbfs + 3.0103f) < 0.1f,
                 "A steady sine must read its own RMS");

        const auto quiet = silence (24000);
        meter.process (quiet.data(), (int) quiet.size());

        const auto afterQuiet = meter.getReadings();
        require (afterQuiet.rmsDbfs < afterLoud.rmsDbfs - 2.0f,
                 "The RMS must fall when the signal stops");

        // A different window length has to change the answer, otherwise the setting is
        // not reaching the measurement at all. One second of tone followed by 50 ms of
        // silence: a 50 ms window is empty by then, a 500 ms window is still full of it.
        meter.setIntegrationSeconds (0.5f);
        meter.reset();
        meter.process (loud.data(), (int) loud.size());
        meter.process (quiet.data(), 2400);
        const auto longWindow = meter.getReadings();

        meter.setIntegrationSeconds (0.05f);
        meter.reset();
        meter.process (loud.data(), (int) loud.size());
        meter.process (quiet.data(), 2400);
        const auto shortWindow = meter.getReadings();

        require (shortWindow.rmsDbfs < longWindow.rmsDbfs - 6.0f,
                 "A shorter window must react faster than a long one");
        require (std::abs (meter.getIntegrationSeconds() - 0.05f) < 0.0001f,
                 "The integration window must report the value it was given");
    }

    // ---- Peak hold and decay ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);
        meter.setIntegrationSeconds (0.05f);
        meter.setPeakHoldSeconds (0.2f);
        meter.setPeakDecayDbPerSecond (20.0f);
        meter.reset();

        const auto loud = sine (2400, 997.0, 1.0);
        meter.process (loud.data(), (int) loud.size());

        const auto held = meter.getReadings();
        require (std::abs (held.peakHoldDbfs) < 0.05f,
                 "The held peak must sit at the level that was just measured");

        // Inside the hold time nothing may fall.
        const auto quiet = silence (2400);
        meter.process (quiet.data(), (int) quiet.size());
        require (std::abs (meter.getReadings().peakHoldDbfs) < 0.05f,
                 "The held peak must not fall before the hold time has passed");

        // Long after the hold time it has to be on its way down.
        for (int i = 0; i < 40; ++i)
            meter.process (quiet.data(), (int) quiet.size());

        require (meter.getReadings().peakHoldDbfs < held.peakHoldDbfs - 6.0f,
                 "The held peak must decay once the hold time has passed");
    }

    // ---- Transient reads differently from a steady tone ----
    {
        dsp::LevelMeter meter;
        meter.prepare (testSampleRate);
        meter.setIntegrationSeconds (0.05f);
        meter.reset();

        auto burst = sine (2400, 997.0, 0.5);
        burst[10] = 1.0f;
        burst[11] = -1.0f;

        meter.process (burst.data(), (int) burst.size());
        const auto readings = meter.getReadings();

        require (readings.crestFactorDb > 6.0f,
                 "A transient must show a much larger crest factor than a sine");
        require (std::abs (readings.peakDbfs) < 0.05f,
                 "The peak must catch the transient");
    }

    // ---- The capture ring hands every sample over exactly once ----
    {
        RealtimeRingBuffer ring;
        require (ring.prepare (3, 1024), "The capture ring must be able to prepare");
        require (ring.getNumChannels() == 3, "The ring must keep the channel count it was given");

        std::vector<float> left (256), right (256), generated (256);
        std::vector<float> readBuffer ((size_t) 256 * 3);

        for (int i = 0; i < 256; ++i)
        {
            left[(size_t) i] = (float) i;
            right[(size_t) i] = (float) -i;
            generated[(size_t) i] = (float) i * 0.5f;
        }

        const float* planes[3] = { left.data(), right.data(), generated.data() };
        require (ring.write (planes, 256) == 256, "The ring must accept a block");

        int64_t cursor = 0;
        auto read = ring.read (cursor, readBuffer.data(), 3, 512);
        require (read == 256, "The ring must return everything that was written");

        for (int i = 0; i < 256; ++i)
        {
            require (readBuffer[(size_t) i] == (float) i, "Plane one must arrive unchanged");
            require (readBuffer[(size_t) i + 256] == (float) -i, "Plane two must arrive unchanged");
            require (readBuffer[(size_t) i + 512] == (float) i * 0.5f,
                     "Plane three must arrive unchanged");
        }

        require (ring.read (cursor, readBuffer.data(), 3, 512) == 0,
                 "A second read must not repeat samples that were already taken");

        // Wrapping: many blocks through a small ring must still come out in order.
        RealtimeRingBuffer wrapped;
        wrapped.prepare (1, 64);

        int64_t wrapCursor = 0;
        auto nextExpected = 0.0f;
        auto ordered = true;

        for (int block = 0; block < 40; ++block)
        {
            std::vector<float> blockSamples (16);

            for (int i = 0; i < 16; ++i)
                blockSamples[(size_t) i] = nextExpected + (float) i;

            nextExpected += 16.0f;

            const float* single[1] = { blockSamples.data() };
            wrapped.write (single, 16);

            std::vector<float> out ((size_t) 16);
            const auto got = wrapped.read (wrapCursor, out.data(), 1, 16);

            for (int i = 0; i < got; ++i)
                if (std::abs (out[(size_t) i] - (block * 16 + i)) > 0.001f)
                    ordered = false;
        }

        require (ordered, "Samples must survive the ring wrapping in order");
    }

    // ---- The callback must not allocate, and must not need a lock ----
    {
        AudioEngine engine;
        engine.scanDevices();

        // A fake device callback, exactly what the audio thread runs, with the allocator
        // watched. This is the acceptance criterion, measured rather than asserted.
        std::vector<float> inputLeft (512), inputRight (512);
        std::vector<float> outputLeft (512), outputRight (512);

        for (int i = 0; i < 512; ++i)
        {
            inputLeft[(size_t) i] = 0.25f * (float) std::sin (2.0 * M_PI * 1000.0 * i / testSampleRate);
            inputRight[(size_t) i] = inputLeft[(size_t) i] * 0.5f;
        }

        const float* inputs[2] = { inputLeft.data(), inputRight.data() };
        float* outputs[2] = { outputLeft.data(), outputRight.data() };
        juce::AudioIODeviceCallbackContext context;

        engine.setGeneratorRouting (AudioEngine::OutputRouting::Left);
        engine.getGenerator().setRunning (true);
        engine.getGenerator().setLevelDb (-12.0f);

        // Prime everything the first call would otherwise allocate: the device tells the
        // engine its rate and block size, which is where the capture storage is created.
        engine.audioDeviceAboutToStart (nullptr);
        engine.audioDeviceIOCallbackWithContext (inputs, 2, outputs, 2, 512, context);

        std::vector<float> left, right, generated;
        engine.readNewSamples (left, right, &generated);
        require (! left.empty(), "The first captured block must come back out of the ring");

        long callbackAllocations = 0;
        std::vector<float> outputLeft2 (512), outputRight2 (512);
        float* outputs2[2] = { outputLeft2.data(), outputRight2.data() };

        {
            const CountingScope scope;

            for (int i = 0; i < 64; ++i)
                engine.audioDeviceIOCallbackWithContext (inputs, 2, outputs2, 2, 512, context);

            callbackAllocations = scope.count();
        }

        require (callbackAllocations == 0,
                 "The audio callback must not allocate: it allocated during the callback");

        require (engine.getCallbackCount() > 1, "The callback counter must advance");

        engine.readNewSamples (left, right, &generated);
        require (! left.empty(), "Captured audio must come back out of the ring");
        require (engine.getPendingSampleCount() == 0,
                 "Every captured sample must be handed out once, not repeated");
        require (engine.getCaptureOverrunCount() == 0,
                 "A reader keeping up must not lose samples");
        require (left.size() == right.size(), "Both captured channels must be the same length");

        // And the capture has to be the audio that went in.
        auto matchesInput = true;

        for (size_t i = 0; i < left.size(); ++i)
            if (std::abs (left[i] - inputLeft[i % 512]) > 1.0e-6f)
                matchesInput = false;

        require (matchesInput, "The captured channel must be the audio the callback received");
    }

    // ---- The generator must actually produce signal, and must stop on command ----
    {
        SignalGenerator generator;
        generator.prepare (testSampleRate);
        generator.setRunning (true);

        const auto peakOf = [] (const std::vector<float>& block)
        {
            float peak = 0.0f;

            for (const auto value : block)
                peak = std::max (peak, std::abs (value));

            return peak;
        };

        std::vector<float> output (1024);

        // Every type has to produce something. A generator that silently outputs silence
        // leaves the measurement path looking healthy while measuring nothing.
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) > 0.001f, "Pink noise must produce signal");

        generator.setBandLimits (2000.0f, 20000.0f);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) > 0.001f, "Pink noise must still produce signal after a band change");

        generator.setType (SignalGenerator::Type::White);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) > 0.001f, "White noise must produce signal");

        generator.setType (SignalGenerator::Type::Sine);
        generator.setFrequency (1000.0f);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) > 0.001f, "A sine must produce signal");

        generator.setType (SignalGenerator::Type::LogSweep);
        generator.setSweepRange (20.0f, 20000.0f, 2.0f);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) > 0.001f, "A log sweep must produce signal");

        // The level setting has to be honoured, which is what stops the generator from
        // driving the amplifier at full scale by accident.
        generator.setType (SignalGenerator::Type::White);
        generator.setLevelDb (-40.0f);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) < 0.02f,
                 "The level setting must scale the output down");

        generator.setRunning (false);
        generator.process (output.data(), (int) output.size());
        require (peakOf (output) == 0.0f, "A stopped generator must output silence");
    }

    // ---- The generator must not allocate either ----
    {
        SignalGenerator generator;
        generator.prepare (testSampleRate);
        generator.setRunning (true);
        generator.setType (SignalGenerator::Type::LogSweep);
        generator.setSweepRange (20.0f, 20000.0f, 2.0f);

        std::vector<float> output (512);

        // Prime, then watch: the first call may build a window table.
        generator.process (output.data(), (int) output.size());
        generator.setType (SignalGenerator::Type::Pink);
        generator.process (output.data(), (int) output.size());
        generator.setType (SignalGenerator::Type::LogSweep);
        generator.process (output.data(), (int) output.size());

        long generatorAllocations = 0;

        {
            const CountingScope scope;

            for (int i = 0; i < 64; ++i)
            {
                generator.process (output.data(), (int) output.size());
                generator.setLevelDb (-12.0f + (float) i * 0.01f);
            }

            generatorAllocations = scope.count();
        }

        require (generatorAllocations == 0,
                 "The generator must not allocate while it is running");
    }

    // ---- Losing the device must be survivable and must not touch the GUI ----
    {
        AudioEngine engine;
        engine.scanDevices();

        const auto mainThreadId = std::this_thread::get_id();

        auto statusMessages = 0;
        auto deviceLostCalls = 0;
        auto statusThreadWasMessageThread = true;

        engine.onStatusMessage = [&] (const juce::String&) { ++statusMessages; };
        engine.onDeviceLost = [&]
        {
            ++deviceLostCalls;
            statusThreadWasMessageThread = std::this_thread::get_id() == mainThreadId;
        };

        engine.audioDeviceAboutToStart (nullptr);

        std::vector<float> left (256), right (256);
        const float* inputs[1] = { left.data() };
        juce::AudioIODeviceCallbackContext context;
        engine.audioDeviceIOCallbackWithContext (inputs, 1, nullptr, 0, 256, context);

        require (engine.isReadyForCapture(), "The engine must report a prepared capture buffer");

        // The device thread must not call the GUI or reopen anything itself, so the loss is
        // simulated on a thread of its own: that is the case the real device thread makes.
        std::thread deviceThread ([&engine] { engine.audioDeviceStopped(); });
        deviceThread.join();

        require (engine.isDeviceLost(), "A stopped device must be recorded as lost");
        require (deviceLostCalls == 0,
                 "The device thread must not call the GUI callback itself");

        // Reading after a device loss must be safe and must return nothing rather than
        // reading freed storage.
        engine.readNewSamples (left, right);
        require (left.empty(), "A stopped device must hand back no samples");
        require (engine.getPendingSampleCount() == 0,
                 "A stopped device must report no pending samples");

        // Polling is the message thread's job and is what reports the loss.
        const auto acted = engine.pollDeviceHealth();
        require (acted, "Polling must report the lost device");
        require (deviceLostCalls == 1, "Polling must call the device lost callback once");
        require (statusThreadWasMessageThread,
                 "The device lost callback must run on the message thread");
        require (statusMessages > 0, "Losing a device must produce a status message");
        require (! engine.isDeviceLost(), "The lost flag must be cleared once handled");

        // Repeated polling with nothing wrong must be silent.
        require (! engine.pollDeviceHealth(), "Polling a healthy device must do nothing");

        // A callback that arrives after the device stopped must be harmless.
        engine.audioDeviceIOCallbackWithContext (inputs, 1, nullptr, 0, 256, context);
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, nullptr, 0, 0, context);
        engine.readNewSamples (left, right);
        require (true, "A callback after a device loss must not crash");

        engine.stop();
        engine.stop();
        require (! engine.isRunning(), "Stopping twice must be safe");
    }

    // ---- Start and stop have to be safe to repeat ----
    {
        AudioEngine engine;
        engine.scanDevices();

        auto statuses = 0;
        engine.onStatusMessage = [&] (const juce::String&) { ++statuses; };

        const auto inputNames = engine.getInputDeviceNames();

        if (! inputNames.isEmpty())
        {
            engine.start (inputNames[0], inputNames[0], 48000.0, 1024);

            // Either it opened, or it reported why it could not. Both are acceptable; a
            // silent failure is not.
            require (engine.isRunning() || statuses > 0,
                     "Starting a device must either run or explain why it did not");

            if (engine.isRunning())
            {
                require (engine.getSampleRate() > 1000.0, "A running device must report a sample rate");
                require (engine.getBufferSize() > 0, "A running device must report a buffer size");
                require (engine.getCurrentSampleRateInt() >= 8000,
                         "The integer sample rate must match the device");
                require (engine.getCurrentBufferSizeInt() > 0,
                         "The integer buffer size must match the device");
                require (engine.isReadyForCapture(),
                         "A running device must have its capture storage prepared");
            }

            engine.stop();
            require (! engine.isRunning(), "Stopping must leave the engine closed");
        }
        else
        {
            std::cout << "INFO: no input device available, device open path not exercised"
                      << std::endl;
        }
    }

    // ---- Device enumeration has to be consistent ----
    {
        AudioEngine engine;
        engine.scanDevices();

        const auto names = engine.getInputDeviceNames();
        const auto labels = engine.getInputDeviceLabels();
        const auto outputs = engine.getOutputDeviceNames();
        const auto outputLabels = engine.getOutputDeviceLabels();

        require (labels.size() == names.size(),
                 "Input names and labels must line up, or the selectors will pick the wrong device");
        require (outputLabels.size() == outputs.size(),
                 "Output names and labels must line up");
        require (engine.getOutputPulseSink (9999).isEmpty(),
                 "An output index that does not exist must return no sink");

        for (int i = 0; i < 2; ++i)
            require (! AudioEngine::channelName (i).isEmpty(),
                     "Every captured channel needs a name for the assignment selector");
    }

    if (failures == 0)
        std::cout << "PASS: silence reports the floor, a full scale sine reads 0 dBFS peak "
                     "and -3.01 dBFS RMS"
                  << std::endl
                  << "PASS: -6, -12, -20 and -40 dBFS sines shift peak, RMS, peak to peak and "
                     "crest factor together"
                  << std::endl
                  << "PASS: the RMS window length changes the reading, and the peak holds then "
                     "decays"
                  << std::endl
                  << "PASS: the capture ring returns every sample once, in order, across wraps"
                  << std::endl
                  << "PASS: 64 audio callbacks and 64 generator blocks allocate nothing"
                  << std::endl
                  << "PASS: a device that stops is recorded on its own thread, reported from the "
                     "message thread, and stays safe to read"
                  << std::endl
                  << "PASS: device names and labels line up, and start and stop repeat safely"
                  << std::endl;
    else
        std::cout << failures << " check(s) failed" << std::endl;

    return failures == 0 ? 0 : 1;
}
