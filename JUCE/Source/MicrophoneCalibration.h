#pragma once

#include <juce_core/juce_core.h>
#include <vector>

/** Turns a raw microphone reading into calibrated SPL.

    A measurement microphone is never flat and never exactly at its rated
    sensitivity, so an uncalibrated RTA trace is only useful for relative
    comparison. This holds two corrections:

    - the sensitivity offset that converts digital dBFS into dB SPL, and
    - the frequency response, applied per FFT bin so a coloured capsule reads flat.

    The Dayton Audio iMM-6c is the built-in model. Its factory calibration file is
    per-serial-number and published by Dayton, so the defaults here are only the
    nominal published figures and the response curve can be replaced by loading
    the unit's own CSV.
*/
class MicrophoneCalibration
{
public:
    struct CurvePoint
    {
        float frequency = 0.0f;
        float db = 0.0f;      // added to the measured level to flatten the mic
    };

    MicrophoneCalibration();

    /** The published iMM-6c nominal figures. */
    static MicrophoneCalibration daytonImm6c();

    /** Parses a Dayton sensitivity header such as "*1000Hz<TAB>-38.5" or
        "Sensitivity: -38.5 dBV". Returns false when the line carries none. */
    static bool parseSensitivityHeader (const juce::String& line, float& sensitivityDbOut);

    /** True when a calibration is active and corrections are applied. */
    bool isEnabled() const { return enabled; }
    void setEnabled (bool shouldBeEnabled) { enabled = shouldBeEnabled; }

    juce::String getModelName() const { return modelName; }
    void setModelName (const juce::String& name) { modelName = name; }

    /** Microphone sensitivity in dB SPL re full scale. */
    float getSensitivityDb() const { return sensitivityDb; }
    void setSensitivityDb (float value) { sensitivityDb = value; }

    /** True when a sensitivity figure was read from the calibration file. */
    bool hasMeasuredSensitivity() const { return measuredSensitivity; }

    /** Offset applied before the SPL conversion, for interface gain trims. */
    float getInputTrimDb() const { return inputTrimDb; }
    void setInputTrimDb (float value) { inputTrimDb = value; }

    bool hasFrequencyResponse() const { return ! responseDb.empty(); }
    const std::vector<float>& getResponseDb() const { return responseDb; }
    const std::vector<CurvePoint>& getCurve() const { return curve; }

    /** Replaces the response curve. Frequencies are sorted and interpolated on use. */
    void setCurve (std::vector<CurvePoint> newCurve);

    /** Loads a Dayton-style calibration file of "frequency,db" rows.
        Dayton's download tool saves the file as .txt, so any extension is accepted;
        the content decides. Rows that are not two plain numbers are skipped, which
        lets header and note lines through untouched. */
    bool loadCsv (const juce::String& text);

    /** Convenience wrapper that reads a file from disk. */
    bool loadFile (const juce::File& file);

    /** Correction in dB for one frequency. Interpolated between curve points. */
    float correctionDb (float frequency) const;

    /** Converts a raw dBFS level into calibrated dB SPL. */
    float toSpl (float dbfs) const;

    /** Applies calibration to a whole dBFS trace, in place. */
    void apply (std::vector<float>& dbfsTrace, const std::vector<float>& frequencies) const;

    juce::String toString() const;

private:
    bool enabled = false;
    juce::String modelName { "Tanpa kalibrasi" };
    float sensitivityDb = 120.0f;
    bool measuredSensitivity = false;
    float inputTrimDb = 0.0f;
    std::vector<CurvePoint> curve;
    std::vector<float> responseDb;
};