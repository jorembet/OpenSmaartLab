#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace dsp
{
    /** Signal level measurement in the time domain.

        The RMS is integrated over a configurable window rather than reported per block,
        because a block sized RMS moves with the buffer size and cannot be compared
        between a 1024 and a 4096 sample setting. Peak, peak to peak and the crest factor
        are read from the same window, so the four figures on screen always describe the
        same stretch of audio.

        Peak and peak-to-peak describe the latest block, while the peak hold remembers the
        highest block peak and then decays it. This keeps the readings responsive while
        making short transients visible instead of gone before they can be read.

        Nothing here allocates after setIntegrationSeconds() has sized the window, so the
        meter can run on the analysis thread without touching the allocator.
    */
    class LevelMeter
    {
    public:
        struct Readings
        {
            /** Root mean square over the integration window, in dBFS. */
            float rmsDbfs = -200.0f;
            /** Highest absolute sample in the window, in dBFS. */
            float peakDbfs = -200.0f;
            /** Highest minus lowest sample in the window, in dBFS. */
            float peakToPeakDbfs = -200.0f;
            /** Peak over RMS in dB: 0 dB for a square, 3.01 dB for a sine, and larger
                the more transient the signal is. */
            float crestFactorDb = 0.0f;
            /** Highest peak since the last reset, held and then decayed. */
            float peakHoldDbfs = -200.0f;
            /** RMS over the samples handed over most recently, for comparison. */
            float blockRmsDbfs = -200.0f;
            float peakLinear = 0.0f;
            float rmsLinear = 0.0f;
            bool valid = false;
        };

        LevelMeter();
        ~LevelMeter();

        /** Allocates the integration window. Call before any audio is measured. */
        void prepare (double newSampleRate);

        /** Length of the RMS window in seconds, and the peak hold time in seconds. */
        void setIntegrationSeconds (float seconds);
        void setPeakHoldSeconds (float seconds);

        /** How fast the held peak falls once the hold time has passed, in dB per second. */
        void setPeakDecayDbPerSecond (float dbPerSecond);

        void reset();

        /** Feeds captured audio. Any number of samples; the window is filled first. */
        void process (const float* samples, int numSamples);

        const Readings& getReadings() const { return readings; }

        float getIntegrationSeconds() const { return integrationSeconds; }
        int getWindowSamples() const { return windowSamples; }

        static float amplitudeToDb (float amplitude);
        static float crestFactor (float peak, float rms);

    private:
        void updateHold();
        void publish();

        double sampleRate = 48000.0;

        float integrationSeconds = 0.35f;
        float peakHoldSeconds = 1.0f;
        float peakDecayDbPerSecond = 20.0f;

        int windowSamples = 16384;
        std::vector<float> window;
        int windowWrite = 0;
        int windowFilled = 0;
        double windowEnergy = 0.0;

        double blockEnergy = 0.0;
        int blockCount = 0;
        double blockPeak = 0.0;
        double blockMinimum = 0.0;
        double blockMaximum = 0.0;
        bool blockHasData = false;

        double holdPeak = 0.0;
        double samplesSinceHold = 0.0;
        double holdSeconds = 1.0;

        Readings readings;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
    };
}
