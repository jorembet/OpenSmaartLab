#include "DspWorker.h"
#include <algorithm>
#include <chrono>
#include <limits>

DspWorker::DspWorker()
{
    queueLimit = 1u << 18;   // about 5.5 s at 48 kHz, per channel
    queuedMic.reserve (queueLimit);
    queuedReference.reserve (queueLimit);
    queuedGenerator.reserve (queueLimit);
    queuedInputLeft.reserve (queueLimit);
    queuedInputRight.reserve (queueLimit);
}

DspWorker::~DspWorker()
{
    stop();
}

void DspWorker::start()
{
    if (running.load())
        return;

    running.store (true);
    worker = std::thread ([this] { run(); });
}

void DspWorker::stop()
{
    if (! running.exchange (false))
        return;

    if (worker.joinable())
        worker.join();

    published.reset();
}

void DspWorker::setSampleRate (double newSampleRate)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingSampleRate = std::max (1000.0, newSampleRate);
    settingsDirty.store (true);
}

void DspWorker::setFftSize (int newFftSize)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingFftSize = std::max (256, newFftSize);
    analyserFftSize.store (pendingFftSize);
    settingsDirty.store (true);
}

void DspWorker::setRmsIntegrationSeconds (float seconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRmsSeconds = juce::jlimit (0.005f, 10.0f, seconds);
    settingsDirty.store (true);
}

void DspWorker::setPeakHoldSeconds (float seconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingHoldSeconds = juce::jlimit (0.05f, 30.0f, seconds);
    settingsDirty.store (true);
}

void DspWorker::setPeakDecayDbPerSecond (float dbPerSecond)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingDecayDbPerSecond = juce::jlimit (1.0f, 200.0f, dbPerSecond);
    settingsDirty.store (true);
}

void DspWorker::setWindow (dsp::SpectrumAnalyser::Window window)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingWindow = window;
    settingsDirty.store (true);
}

void DspWorker::setOverlapPercent (float percent)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingOverlap = percent;
    settingsDirty.store (true);
}

void DspWorker::setAveraging (dsp::SpectrumAnalyser::Averaging mode, float timeConstantSeconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingAveraging = mode;
    pendingAveragingSeconds = juce::jlimit (0.01f, 120.0f, timeConstantSeconds);
    settingsDirty.store (true);
}

void DspWorker::setRtaResolution (dsp::RtaAnalyser::Resolution resolution)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaResolution = resolution;
    settingsDirty.store (true);
}

void DspWorker::setRtaRange (float lowFrequency, float highFrequency)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaRangeLow = lowFrequency;
    pendingRtaRangeHigh = highFrequency;
    settingsDirty.store (true);
}

void DspWorker::setRtaAveraging (dsp::SpectrumAnalyser::Averaging mode, float timeConstantSeconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaAveraging = mode;
    pendingRtaAveragingSeconds = juce::jlimit (0.01f, 120.0f, timeConstantSeconds);
    settingsDirty.store (true);
}

void DspWorker::setRtaSmoothingSeconds (float seconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaSmoothingSeconds = juce::jlimit (0.0f, 10.0f, seconds);
    settingsDirty.store (true);
}

void DspWorker::setRtaPeakHoldSeconds (float seconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaPeakHoldSeconds = juce::jlimit (0.0f, 60.0f, seconds);
    settingsDirty.store (true);
}

void DspWorker::setRtaPeakDecayDbPerSecond (float dbPerSecond)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaDecayDbPerSecond = juce::jlimit (0.0f, 200.0f, dbPerSecond);
    settingsDirty.store (true);
}

void DspWorker::setRtaMinMaxWindowSeconds (float seconds)
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    pendingRtaMinMaxSeconds = juce::jlimit (0.1f, 600.0f, seconds);
    settingsDirty.store (true);
}

void DspWorker::clearRtaPeakHold()
{
    // Requested rather than done here, because the bands belong to the worker thread.
    clearRtaHoldRequested.store (true, std::memory_order_release);
}

void DspWorker::setAveragingAttackRatio (float ratio)
{
    {
        std::lock_guard<std::mutex> lock (settingsMutex);
        pendingAttackRatio = juce::jlimit (1.0f, 32.0f, ratio);
    }

    // Applied to the running objects as well as the pending settings, so taking effect does not
    // wait for the next setAveraging call to rebuild them.
    analyser.setAttackRatio (juce::jlimit (1.0f, 32.0f, ratio));
    rta.setAttackRatio (juce::jlimit (1.0f, 32.0f, ratio));
}

void DspWorker::resetAveraging()
{
    // Requested rather than done here, because the averages belong to the worker thread.
    resetAveragingRequested.store (true, std::memory_order_release);
}

dsp::RtaAnalyser::Resolution DspWorker::getRtaResolution() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingRtaResolution;
}

float DspWorker::getRtaRangeLow() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingRtaRangeLow;
}

float DspWorker::getRtaRangeHigh() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingRtaRangeHigh;
}

dsp::SpectrumAnalyser::Window DspWorker::getWindow() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingWindow;
}

dsp::SpectrumAnalyser::Averaging DspWorker::getAveragingMode() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingAveraging;
}

float DspWorker::getAveragingTimeConstant() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingAveragingSeconds;
}

float DspWorker::getOverlapPercent() const
{
    std::lock_guard<std::mutex> lock (settingsMutex);
    return pendingOverlap;
}

void DspWorker::pushSamples (const float* inputLeft, const float* inputRight,
                             const float* mic, const float* reference,
                             const float* generator, int numSamples)
{
    if (! running.load() || numSamples <= 0 || mic == nullptr)
        return;

    // Copying into the queue under a short lock is fine here: this runs on the GUI
    // thread, not the audio thread, and it is the only place the two meet.
    std::lock_guard<std::mutex> lock (queueMutex);

    const auto count = std::min<size_t> ((size_t) numSamples,
                                         queueLimit - queuedMic.size());

    if (count == 0)
    {
        droppedSamples.fetch_add ((uint64_t) numSamples, std::memory_order_relaxed);
        return;   // the worker is behind; the incoming samples are discarded
    }

    if (count < (size_t) numSamples)
        droppedSamples.fetch_add ((uint64_t) numSamples - count, std::memory_order_relaxed);

    queuedMic.insert (queuedMic.end(), mic, mic + count);

    const auto appendOrSilence = [count] (std::vector<float>& destination,
                                          const float* source)
    {
        if (source != nullptr)
            destination.insert (destination.end(), source, source + count);
        else
            destination.insert (destination.end(), count, 0.0f);
    };

    appendOrSilence (queuedInputLeft, inputLeft);
    appendOrSilence (queuedInputRight, inputRight);
    appendOrSilence (queuedReference, reference);
    appendOrSilence (queuedGenerator, generator);
}

void DspWorker::run()
{
    while (running.load())
    {
        applyPendingSettings();
        if (drain())
            publish();

        // A short sleep instead of a condition variable: the queue is small and the
        // cadence is set by the audio block, so polling keeps the latency predictable and
        // costs nothing measurable at this rate.
        std::this_thread::sleep_for (std::chrono::milliseconds (4));
    }

    // One last pass so a reading published just before stopping is not lost.
    applyPendingSettings();
    if (drain())
        publish();
}

void DspWorker::applyPendingSettings()
{
    if (! settingsDirty.exchange (false))
        return;

    double sampleRate = 48000.0;
    int fftSize = 2048;
    float rmsSeconds = 0.35f, holdSeconds = 1.0f, decay = 20.0f, overlap = 50.0f, averagingSeconds = 0.5f;
    dsp::SpectrumAnalyser::Window window = dsp::SpectrumAnalyser::Window::Hann;
    dsp::SpectrumAnalyser::Averaging averaging = dsp::SpectrumAnalyser::Averaging::Exponential;

    dsp::RtaAnalyser::Resolution rtaResolution = dsp::RtaAnalyser::Resolution::ThirdOctave;
    float rtaRangeLow = 20.0f, rtaRangeHigh = 20000.0f;
    dsp::SpectrumAnalyser::Averaging rtaAveraging = dsp::SpectrumAnalyser::Averaging::Exponential;
    float rtaAveragingSeconds = 0.5f, rtaSmoothing = 0.0f;
    float rtaHoldSeconds = 1.0f, rtaDecay = 20.0f, rtaMinMaxSeconds = 5.0f;

    {
        std::lock_guard<std::mutex> lock (settingsMutex);
        sampleRate = pendingSampleRate;
        fftSize = pendingFftSize;
        rmsSeconds = pendingRmsSeconds;
        holdSeconds = pendingHoldSeconds;
        decay = pendingDecayDbPerSecond;
        window = pendingWindow;
        overlap = pendingOverlap;
        averaging = pendingAveraging;
        averagingSeconds = pendingAveragingSeconds;
        rtaResolution = pendingRtaResolution;
        rtaRangeLow = pendingRtaRangeLow;
        rtaRangeHigh = pendingRtaRangeHigh;
        rtaAveraging = pendingRtaAveraging;
        rtaAveragingSeconds = pendingRtaAveragingSeconds;
        rtaSmoothing = pendingRtaSmoothingSeconds;
        rtaHoldSeconds = pendingRtaPeakHoldSeconds;
        rtaDecay = pendingRtaDecayDbPerSecond;
        rtaMinMaxSeconds = pendingRtaMinMaxSeconds;
    }

    micMeter.prepare (sampleRate);
    referenceMeter.prepare (sampleRate);
    generatorMeter.prepare (sampleRate);
    inputLeftMeter.prepare (sampleRate);
    inputRightMeter.prepare (sampleRate);
    inputLeftMeter.setPeakHoldSeconds (holdSeconds);
    inputRightMeter.setPeakHoldSeconds (holdSeconds);
    inputLeftMeter.setPeakDecayDbPerSecond (decay);
    inputRightMeter.setPeakDecayDbPerSecond (decay);
    micMeter.setPeakHoldSeconds (holdSeconds);
    referenceMeter.setPeakHoldSeconds (holdSeconds);
    generatorMeter.setPeakHoldSeconds (holdSeconds);
    micMeter.setPeakDecayDbPerSecond (decay);
    referenceMeter.setPeakDecayDbPerSecond (decay);
    generatorMeter.setPeakDecayDbPerSecond (decay);

    analyser.prepare (sampleRate, fftSize);
    analyser.setWindow (window);
    analyser.setOverlapPercent (overlap);
    analyser.setAveraging (averaging, averagingSeconds);
    analyser.setAttackRatio (pendingAttackRatio);

    // The band analyser is prepared for the same FFT size as the spectrum, so the bands
    // are integrated from the very frame the spectrum curve is drawn from rather than
    // from a second transform that could disagree with it.
    rta.prepare (sampleRate, fftSize);
    rta.setResolution (rtaResolution);
    rta.setFrequencyRange (rtaRangeLow, rtaRangeHigh);
    rta.setAveraging (rtaAveraging, rtaAveragingSeconds);
    rta.setAttackRatio (pendingAttackRatio);
    rta.setSmoothingSeconds (rtaSmoothing);
    rta.setPeakHoldSeconds (rtaHoldSeconds);
    rta.setPeakDecayDbPerSecond (rtaDecay);
    rta.setMinMaxWindowSeconds (rtaMinMaxSeconds);
}

bool DspWorker::drain()
{
    {
        std::lock_guard<std::mutex> lock (queueMutex);

        if (queuedMic.empty())
            return false;

        drainMic.swap (queuedMic);
        drainReference.swap (queuedReference);
        drainGenerator.swap (queuedGenerator);
        drainInputLeft.swap (queuedInputLeft);
        drainInputRight.swap (queuedInputRight);
        queuedMic.clear();
        queuedReference.clear();
        queuedGenerator.clear();
        queuedInputLeft.clear();
        queuedInputRight.clear();
    }

    const auto count = (int) drainMic.size();
    processedSamples += (uint64_t) count;

    silence.assign ((size_t) count, 0.0f);

    inputLeftMeter.process (drainInputLeft.empty() ? silence.data() : drainInputLeft.data(), count);
    inputRightMeter.process (drainInputRight.empty() ? silence.data() : drainInputRight.data(), count);
    micMeter.process (drainMic.data(), count);
    referenceMeter.process (drainReference.empty() ? silence.data() : drainReference.data(), count);
    generatorMeter.process (drainGenerator.empty() ? silence.data() : drainGenerator.data(), count);

    // Consumed once, here, before any frame is analysed. Clearing it in two places would mean
    // whichever ran first emptied the flag and the other never saw the request, so one of the
    // two averages would keep the old signal and the display would still look slow to start.
    if (resetAveragingRequested.exchange (false, std::memory_order_acquire))
    {
        // Both together, because the bands are integrated from the spectrum's frames and
        // resetting one without the other would leave the two describing different audio.
        analyser.reset();
        rta.reset();
    }

    if (spectrumResetRequested.exchange (false, std::memory_order_acquire))
    {
        analyser.reset();
        rta.reset();
    }

    const auto previousFrameCount = analyser.getFrame().framesProcessed;
    if (spectrumEnabled.load())
    {
        analyser.pushSamples (drainMic.data(), count);
        analyser.processAvailableFrames();
    }

    // The bands come from the frame the spectrum just produced, so a band level and the
    // curve under it always describe the same audio at the same instant. A short input
    // block must not integrate the previous FFT frame a second time.
    const auto& frame = analyser.getFrame();
    const auto hasNewSpectrumFrame = spectrumEnabled.load() && frame.valid
                                  && frame.framesProcessed != previousFrameCount;

    if (rtaEnabled.load() && hasNewSpectrumFrame)
    {
        if (clearRtaHoldRequested.exchange (false, std::memory_order_acquire))
            rta.clearPeakHold();

        rta.process (frame);
    }

    return true;
}

void DspWorker::publish()
{
    Snapshot snapshot;
    snapshot.inputLeft = inputLeftMeter.getReadings();
    snapshot.inputRight = inputRightMeter.getReadings();
    snapshot.mic = micMeter.getReadings();
    snapshot.reference = referenceMeter.getReadings();
    snapshot.generator = generatorMeter.getReadings();
    snapshot.samplesProcessed = processedSamples;
    snapshot.droppedSamples = (int) std::min<uint64_t> (
        droppedSamples.load (std::memory_order_relaxed), (uint64_t) std::numeric_limits<int>::max());

    if (spectrumEnabled.load() && analyser.getFrame().valid)
    {
        const auto& frame = analyser.getFrame();
        snapshot.spectrumValid = true;
        snapshot.spectrumFrequency = frame.frequency;
        snapshot.spectrumDb = frame.averagedDb;
        snapshot.spectrumFftSize = frame.fftSize;
        snapshot.spectrumBins = frame.numBins;
        snapshot.sampleRate = frame.sampleRate;
    }

    if (rtaEnabled.load() && rta.getBandCount() > 0)
    {
        snapshot.rtaValid = true;
        snapshot.rtaBands = rta.getBands();
        snapshot.rtaFrequencies = rta.getBandFrequencies();
        snapshot.rtaResolution = dsp::RtaAnalyser::resolutionToString (rta.getResolution()).toStdString();
        snapshot.rtaRangeLow = rta.getRangeLow();
        snapshot.rtaRangeHigh = rta.getRangeHigh();
        snapshot.rtaUnresolvedBands = rta.getUnresolvedBandCount();
    }

    snapshot.sequence = publishedSequence.fetch_add (1) + 1;

    published.set (snapshot);
}
