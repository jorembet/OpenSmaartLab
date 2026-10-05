#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "DSP.h"
#include "LevelMeter.h"
#include "LatestValue.h"
#include "RtaAnalyser.h"
#include "SpectrumAnalyser.h"

/** Runs the time domain level measurement off the GUI thread and off the audio thread.

    The audio callback only copies captured samples into a ring. This worker drains that
    ring, measures levels, and publishes one reading; the GUI reads the reading when it
    paints. That is the whole point of the class: the measurement cost can grow with the
    FFT size and the integration window without ever being able to make the audio drop
    out, and a slow repaint can no longer hold up the analysis.
*/
class DspWorker
{
public:
    /** One published set of measurements, both physical capture channels and analysis views. */
    struct Snapshot
    {
        dsp::LevelMeter::Readings inputLeft;
        dsp::LevelMeter::Readings inputRight;
        dsp::LevelMeter::Readings mic;
        dsp::LevelMeter::Readings reference;
        dsp::LevelMeter::Readings generator;

        bool spectrumValid = false;
        std::vector<float> spectrumFrequency;
        std::vector<float> spectrumDb;
        int spectrumFftSize = 0;
        int spectrumBins = 0;

        /** Octave band readings, integrated from the same frame as the spectrum, so the
            two views can never disagree about what the audio contained. */
        bool rtaValid = false;
        std::vector<dsp::RtaAnalyser::Band> rtaBands;
        std::vector<float> rtaFrequencies;
        std::string rtaResolution;
        float rtaRangeLow = 20.0f;
        float rtaRangeHigh = 20000.0f;
        int rtaUnresolvedBands = 0;
        double sampleRate = 48000.0;
        /** Capture position the reading corresponds to, so a reader can tell that the
            data moved on while it was not looking. */
        uint64_t sequence = 0;
        uint64_t samplesProcessed = 0;
        int droppedSamples = 0;
    };

    DspWorker();
    ~DspWorker();

    /** Starts the worker. Safe to call again to restart it with new settings. */
    void start();

    /** Stops the worker and joins it. Must not be called from the worker itself. */
    void stop();

    bool isRunning() const { return running.load(); }

    void setSampleRate (double newSampleRate);
    void setFftSize (int newFftSize);
    void setRmsIntegrationSeconds (float seconds);
    void setPeakHoldSeconds (float seconds);
    void setPeakDecayDbPerSecond (float dbPerSecond);
    void setWindow (dsp::SpectrumAnalyser::Window window);
    void setOverlapPercent (float percent);
    void setAveraging (dsp::SpectrumAnalyser::Averaging mode, float timeConstantSeconds);
    void setSpectrumEnabled (bool shouldAnalyse)
    {
        if (spectrumEnabled.exchange (shouldAnalyse, std::memory_order_acq_rel) != shouldAnalyse)
            spectrumResetRequested.store (true, std::memory_order_release);
    }

    // Octave band analyser settings. These are the RTA view's own, separate from the
    // spectrum's, because a band readout wants a different averaging time to the curve it
    // is compared against.
    void setRtaResolution (dsp::RtaAnalyser::Resolution resolution);
    void setRtaRange (float lowFrequency, float highFrequency);
    void setRtaAveraging (dsp::SpectrumAnalyser::Averaging mode, float timeConstantSeconds);
    void setRtaSmoothingSeconds (float seconds);
    void setRtaPeakHoldSeconds (float seconds);
    void setRtaPeakDecayDbPerSecond (float dbPerSecond);
    void setRtaMinMaxWindowSeconds (float seconds);
    void setRtaEnabled (bool shouldAnalyse) { rtaEnabled.store (shouldAnalyse); }
    void clearRtaPeakHold();

    /** Empties the spectrum and band averages, so the next reading starts from the audio that
        is arriving now rather than from whatever was there before.

        The averages are exponential, so they never arrive: after the signal changes they
        converge over several time constants, and switching the generator on left the display
        showing the previous signal fading into the new one over about three seconds. That reads
        as the measurement being slow to start when it is only the average remembering. */
    void resetAveraging();

    /** How much faster a rise in level is followed than a fall, for both the spectrum and the
        bands. One is a plain symmetric average. */
    void setAveragingAttackRatio (float ratio);

    dsp::RtaAnalyser::Resolution getRtaResolution() const;
    float getRtaRangeLow() const;
    float getRtaRangeHigh() const;

    /** Hands over both physical input channels plus routed analysis signals. Called from the
        GUI thread; samples are copied into the worker queue and measured off-thread. */
    void pushSamples (const float* inputLeft, const float* inputRight,
                      const float* mic, const float* reference, const float* generator,
                      int numSamples);

    /** Reads the most recent measurement. Safe to call from the GUI thread while the
        worker is running. */
    bool getSnapshot (Snapshot& destination) const { return published.get (destination); }

    dsp::SpectrumAnalyser::Window getWindow() const;
    dsp::SpectrumAnalyser::Averaging getAveragingMode() const;
    float getAveragingTimeConstant() const;
    float getOverlapPercent() const;

    /** Settings the worker currently has, for showing them next to a reading. */
    int getFftSize() const { return analyserFftSize.load(); }

private:
    void run();
    void applyPendingSettings();
    bool drain();
    void publish();

    // Settings cross the thread boundary as plain values guarded by this flag, so
    // changing them from the GUI never waits for the worker and never tears. The mutex is
    // mutable because the getters are const and still have to read them safely.
    mutable std::mutex settingsMutex;
    double pendingSampleRate = 48000.0;
    int pendingFftSize = 2048;
    float pendingRmsSeconds = 0.35f;
    float pendingHoldSeconds = 1.0f;
    float pendingDecayDbPerSecond = 20.0f;
    dsp::SpectrumAnalyser::Window pendingWindow = dsp::SpectrumAnalyser::Window::Hann;
    float pendingOverlap = 50.0f;
    dsp::SpectrumAnalyser::Averaging pendingAveraging = dsp::SpectrumAnalyser::Averaging::Exponential;
    float pendingAveragingSeconds = 0.5f;

    dsp::RtaAnalyser::Resolution pendingRtaResolution = dsp::RtaAnalyser::Resolution::ThirdOctave;
    float pendingRtaRangeLow = 20.0f;
    float pendingRtaRangeHigh = 20000.0f;
    dsp::SpectrumAnalyser::Averaging pendingRtaAveraging = dsp::SpectrumAnalyser::Averaging::Exponential;
    float pendingRtaAveragingSeconds = 0.5f;
    float pendingRtaSmoothingSeconds = 0.0f;
    float pendingRtaPeakHoldSeconds = 1.0f;
    float pendingRtaDecayDbPerSecond = 20.0f;
    float pendingRtaMinMaxSeconds = 5.0f;
    std::atomic<bool> clearRtaHoldRequested { false };
    std::atomic<bool> resetAveragingRequested { false };
    float pendingAttackRatio = 4.0f;

    std::atomic<bool> running { false };
    std::atomic<bool> spectrumEnabled { true };
    std::atomic<bool> spectrumResetRequested { false };
    std::atomic<bool> rtaEnabled { true };
    std::atomic<int> analyserFftSize { 2048 };
    std::atomic<bool> settingsDirty { false };
    std::atomic<uint64_t> publishedSequence { 0 };
    std::atomic<uint64_t> droppedSamples { 0 };
    uint64_t processedSamples = 0;

    // Capture queue between the GUI thread and the worker. Sized once and reused.
    std::mutex queueMutex;
    std::vector<float> queuedMic;
    std::vector<float> queuedReference;
    std::vector<float> queuedGenerator;
    std::vector<float> queuedInputLeft;
    std::vector<float> queuedInputRight;
    size_t queueLimit = 0;

    // Reused by the worker so draining never allocates either.
    std::vector<float> drainMic;
    std::vector<float> drainReference;
    std::vector<float> drainGenerator;
    std::vector<float> drainInputLeft;
    std::vector<float> drainInputRight;
    std::vector<float> silence;

    dsp::LevelMeter inputLeftMeter;
    dsp::LevelMeter inputRightMeter;
    dsp::LevelMeter micMeter;
    dsp::LevelMeter referenceMeter;
    dsp::LevelMeter generatorMeter;
    dsp::SpectrumAnalyser analyser;
    dsp::RtaAnalyser rta;

    LatestValue<Snapshot> published;

    std::thread worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DspWorker)
};
