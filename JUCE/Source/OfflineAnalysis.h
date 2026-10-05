#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "SpectrumAnalyser.h"
#include "RtaAnalyser.h"
#include "DistortionAnalyser.h"
#include "SplAnalyser.h"
#include "TransferFunction.h"
#include "ImpulseResponse.h"
#include "ReverbAnalyser.h"
#include "MicrophoneCalibration.h"

/** Runs every measurement the application can make, over a file instead of a live input.

    This class holds no DSP of its own. Every number it reports comes out of the same analyser
    objects the live path uses, fed the same way, which is the only way the answer can be
    trusted to agree with what the meter shows while the signal is playing. A second
    implementation written for files would be a second set of window conventions, averaging
    rules and band edges, and it would drift from the first the moment either was corrected.

    The one thing that genuinely differs is where the samples come from. Live, they arrive a
    block at a time from the audio callback and the analysers see them as they land. Here they
    arrive already in a vector, so the loop below walks the file in blocks of the same size and
    pushes them through the same entry points in the same order. */
class OfflineAnalysis
{
public:
    /** Which measurements to run. All of them are safe to ask for on any file; each reports
        its own reason when the file cannot support it, rather than the whole run failing. */
    enum class Measurement
    {
        Spectrum,
        Rta,
        Distortion,
        TransferFunction,
        ImpulseResponse,
        Reverberation,
        Spl
    };

    struct Settings
    {
        int fftSize = 16384;
        dsp::SpectrumAnalyser::Window window = dsp::SpectrumAnalyser::Window::Hann;
        dsp::SpectrumAnalyser::Averaging averaging = dsp::SpectrumAnalyser::Averaging::Exponential;
        float averagingSeconds = 3.0f;
        float overlapPercent = 50.0f;

        dsp::RtaAnalyser::Resolution resolution = dsp::RtaAnalyser::Resolution::OneOctave;
        float rtaLowHz = 20.0f;
        float rtaHighHz = 20000.0f;

        dsp::DistortionAnalyser::Settings distortion;

        /** Which file channel is the measurement and which the reference.

        The order is the caller's to choose, the same way it is on the live panel, because the
        channel a device puts first is not the channel a measurement wants measured. */
        int measurementChannel = 0;
        int referenceChannel = 1;

        /** Frames averaged into the transfer function.

            Held separately from the distortion analyser's averaging because they are different
            measurements: a spectrum wants a long average to settle, and a transfer function
            wants enough frames to resolve a coherence but not so many that a room changes
            underneath it. */
        int transferAverages = 8;

        /** Track the loop delay and compensate the phase for it, which is what the live path
            does. The delay is still measured and reported either way. */
        bool automaticDelay = true;
        bool delayCompensation = true;
        float temperatureC = 20.0f;

        MicrophoneCalibration calibration;
        dsp::SplAnalyser::TimeWeighting splWeighting = dsp::SplAnalyser::TimeWeighting::Fast;

        bool measureSpl = true;
        bool measureDistortion = true;
        bool measureReverberation = true;
    };

    /** One measurement's answer, plus enough context to say why there isn't one. */
    struct Report
    {
        bool valid = false;
        juce::String skippedReason;

        juce::StringArray lines;
        void add (const juce::String& name, const juce::String& value)
        {
            lines.add (name + ": " + value);
        }
    };

    struct Result
    {
        bool valid = false;
        juce::String error;

        int numChannels = 0;
        double sampleRate = 0.0;
        int64_t numFrames = 0;
        double durationSeconds = 0.0;

        // Spectrum and the noise floor that comes with it.
        std::vector<float> frequency;
        std::vector<float> magnitudeDb;
        std::vector<float> powerDb;
        float averageLevelDb = dsp::dbFloor;
        float peakFrequency = 0.0f;
        float peakMagnitudeDb = dsp::dbFloor;
        float noiseFloorDb = dsp::dbFloor;
        int spectrumFrames = 0;
        int spectrumDropped = 0;

        // RTA.
        std::vector<dsp::RtaAnalyser::Band> rtaBands;
        bool rtaFullyResolved = false;

        // Distortion.
        dsp::DistortionAnalyser::Result distortion;

        // Transfer function, coherence, phase and the impulse it carries.
        TransferFunction::Result transfer;

        // Impulse response, energy decay curve and reverberation times.
        ImpulseResponse::Acoustics acoustics;
        dsp::ReverbAnalyser::Result reverb;

        // Sound pressure level.
        float splZ = dsp::dbFloor, splA = dsp::dbFloor, splC = dsp::dbFloor;
        float leqZ = dsp::dbFloor, leqA = dsp::dbFloor, leqC = dsp::dbFloor;
        float peakZ = dsp::dbFloor, peakA = dsp::dbFloor, peakC = dsp::dbFloor;

        Report spectrumReport, rtaReport, distortionReport, transferReport,
               impulseReport, reverbReport, splReport;

        /** Every report, in the order the measurements are listed. */
        std::vector<std::pair<Measurement, Report>> getReports() const;
    };

    OfflineAnalysis();
    ~OfflineAnalysis();

    /** Runs the measurements over the given channels. Runs to completion on the calling thread,
        so a long file takes a moment and the interface should say so.

        The sample rate is passed rather than guessed. Every frequency in the answer is that
        number divided into the sample count, so reading a file at a rate it was not written at
        would not shift the answer slightly, it would put every band and every peak on the
        wrong frequency. */
    Result analyse (const std::vector<float>& channelA,
                    const std::vector<float>& channelB,
                    double sampleRate,
                    const Settings& settings);

    /** Reads a wav file and runs the measurements over it. Refuses a file it cannot decode. */
    Result analyseFile (const juce::File& file, const Settings& settings);

    /** Reads a wav file into channels without analysing it. */
    static bool readFile (const juce::File& file, std::vector<float>& channelA,
                          std::vector<float>& channelB, double& sampleRate, int& numChannels,
                          juce::String* errorOut = nullptr);

    /** True when the file can be decoded into float samples at all. */
    static bool isSupportedFile (const juce::File& file);

    static juce::StringArray getMeasurementNames();
    static juce::String measurementName (Measurement measurement);
    static juce::String measurementUnit (Measurement measurement);

private:
    void runSpectrum (Result& result, const std::vector<float>& samples, const Settings& settings);
    void runDistortion (Result& result, const std::vector<float>& samples, const Settings& settings);
    void runTransfer (Result& result, const std::vector<float>& channelA,
                      const std::vector<float>& channelB, const Settings& settings);
    void runReverberation (Result& result, const std::vector<float>& samples,
                           const Settings& settings);
    void runSpl (Result& result, const std::vector<float>& samples, const Settings& settings);

    dsp::SpectrumAnalyser spectrum;
    dsp::RtaAnalyser rta;
    dsp::DistortionAnalyser distortion;
    TransferFunction transfer;
    dsp::ReverbAnalyser reverb;
    dsp::SplAnalyser spl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OfflineAnalysis)
};