#include "SPLMeter.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    void require (bool condition, const char* message)
    {
        if (! condition)
            throw std::runtime_error (message);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    SPLMeter meter;
    constexpr int blockSize = dsp::SplAnalyser::blockSize;
    constexpr float sampleRate = 48000.0f;

    std::vector<float> fullScaleSine ((size_t) blockSize);
    std::vector<float> lowFrequencySine ((size_t) blockSize);
    std::vector<float> moderateSine ((size_t) blockSize);
    std::vector<float> silence ((size_t) blockSize, 0.0f);
    std::vector<float> clipped ((size_t) blockSize, 0.0f);

    require (meter.getLevel (0) <= dsp::dbFloor + 0.01f
                 && meter.getLevel (1) <= dsp::dbFloor + 0.01f,
             "An empty meter must not report 0 dBFS before receiving audio");

    for (int i = 0; i < blockSize; ++i)
    {
        fullScaleSine[(size_t) i] = std::sin (2.0 * 3.14159265358979323846 * 1000.0 * i
                                              / sampleRate);
        lowFrequencySine[(size_t) i] = std::sin (2.0 * 3.14159265358979323846 * 100.0 * i
                                                 / sampleRate);
        moderateSine[(size_t) i] = 0.53f * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * i
                                                     / sampleRate);
    }

    SPLMeter unityGainMeter;
    SPLMeter trimmedMeter;
    unityGainMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize, 0.0f);
    trimmedMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize, -12.0f);
    require (std::abs ((trimmedMeter.getLevel (1) - unityGainMeter.getLevel (1)) + 12.0f) < 0.1f,
             "Input trim must change the microphone level by the selected gain");
    require (std::abs (trimmedMeter.getLevel (0) - unityGainMeter.getLevel (0)) < 0.1f,
             "Input trim must leave the reference channel unchanged");
    require (std::abs (unityGainMeter.getMeasuredMicLevelDbfs()
                       - trimmedMeter.getMeasuredMicLevelDbfs()) < 0.1f,
             "The calibration reading must remove software trim from the measured mic level");

    SPLMeter uncalibratedMeter;
    uncalibratedMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize);
    const auto rawReference = uncalibratedMeter.getLevel (0);
    const auto rawMicrophone = uncalibratedMeter.getLevel (1);
    require (! uncalibratedMeter.isCalibrated(),
             "An uncalibrated microphone must remain explicitly uncalibrated");

    MicrophoneCalibration profile;
    profile.calibrateAgainst (94.0f, -20.0f, "Test mic");
    SPLMeter calibratedMeter;
    calibratedMeter.setCalibration (profile);
    calibratedMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize);
    require (calibratedMeter.isCalibrated(),
             "A measured reference profile must enable calibrated SPL readings");
    require (std::abs (calibratedMeter.getLevel (0) - rawReference) < 0.1f,
             "Microphone calibration must never shift the reference/output channel");
    require (std::abs ((calibratedMeter.getLevel (1) - rawMicrophone) - 114.0f) < 0.2f,
             "A measured calibration offset must apply only to the microphone reading");
    calibratedMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize, -12.0f);
    require (std::abs (calibratedMeter.getLevel (1) - rawMicrophone - 114.0f) < 0.2f,
             "Software input trim must not change calibrated SPL");
    require (std::abs (calibratedMeter.getLevel (0) - rawReference) < 0.1f,
             "Input trim and microphone calibration must leave reference dBFS unchanged");

    auto curvedProfile = profile;
    curvedProfile.setCurve ({ { 20.0f, 6.0f }, { 20000.0f, 6.0f } });
    SPLMeter curveCorrectedMeter;
    curveCorrectedMeter.setCalibration (curvedProfile);
    curveCorrectedMeter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize);
    require (std::abs (curveCorrectedMeter.getLevel (0) - rawReference) < 0.1f,
             "Microphone response correction must not alter the reference/output channel");
    require (std::abs (curveCorrectedMeter.getLevel (1) - calibratedMeter.getLevel (1) - 6.0f)
                 < 0.2f,
             "A microphone response curve must correct the microphone level");

    std::fill (clipped.begin(), clipped.begin() + 8, 1.0f);
    std::fill (clipped.begin() + 8, clipped.begin() + 16, -1.0f);

    meter.process (fullScaleSine.data(), fullScaleSine.data(), blockSize);
    require (meter.getClipCount (0) == 0 && ! meter.isClipActive (0),
             "A full-scale sine peak must not be mistaken for a flat-topped clip");

    meter.process (lowFrequencySine.data(), lowFrequencySine.data(), blockSize);
    require (meter.getClipCount (0) == 0 && meter.getClipCount (1) == 0,
             "A clean low-frequency sine near the sample ceiling must not create clip events");

    meter.process (moderateSine.data(), moderateSine.data(), blockSize);
    require (std::abs (meter.getSamplePeakDbfs (0) + 5.5f) < 0.1f,
             "The raw sample peak must report a real -5.5 dBFS signal");
    require (meter.getClipCount (0) == 0 && meter.getClipCount (1) == 0,
             "A clean -5.5 dBFS input must never be called clipping");

    meter.process (clipped.data(), clipped.data(), blockSize, -12.0f);
    require (meter.getClipCount (0) == 1 && meter.getClipCount (1) == 1,
             "A flat-topped signal must register one clip event per channel");
    require (meter.isClipActive (0) && meter.isClipActive (1),
             "A new clip event must activate the warning");
    require (meter.getClipPeakDbfs (0) >= -0.01f,
             "The displayed clip-window peak must include the block that triggered it");

    for (int block = 0; block < 4; ++block)
        meter.process (silence.data(), silence.data(), blockSize);

    meter.process (clipped.data(), clipped.data(), blockSize);
    require (meter.getClipCount (0) == 1,
             "Nearby clipped blocks must be grouped into one event");

    for (int block = 0; block < 48; ++block)
        meter.process (silence.data(), silence.data(), blockSize);

    require (! meter.isClipActive (0) && ! meter.isClipActive (1),
             "The active clip warning must clear after clean audio resumes");
    require (meter.getClipCount (0) == 1 && meter.getClipCount (1) == 1,
             "The lifetime event count must remain available after the warning clears");
    require (meter.getClipPeakDbfs (0) <= dsp::dbFloor + 0.01f,
             "The held clip peak must clear with its warning window");

    std::cout << "PASS: calibration and input trim affect only the mic; dBFS reference, flat-top clipping, clean sine, clip grouping, peak, and history checks pass\n";
}
