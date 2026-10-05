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
        /** The correction to ADD at this frequency, not the capsule's response.

            A microphone's own calibration file records how far off it is, which is negative
            where it reads low. What flattens it is the opposite number, and that is what is
            stored here. Loading a manufacturer's file therefore has to negate it, and getting
            that backwards doubles the error instead of removing it. */
        float db = 0.0f;
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

    /** When this calibration was carried out, as text.

        Kept as text rather than a date type because it is only ever shown and stored, and a
        microphone's own paperwork is written however its manufacturer wrote it. A calibration
        with no date is still usable, but a reader is entitled to know whether it is older than
        the equipment it is correcting. */
    juce::String getCalibrationDate() const { return calibrationDate; }
    void setCalibrationDate (const juce::String& date) { calibrationDate = date; }

    /** Level the calibrator was set to, in dB SPL. 94 dB is an acoustic calibrator's nominal
        level, but they are available at 114 and it is a setting on the box, not a constant. */
    float getCalibratorLevelDb() const { return calibratorLevelDb; }

    /** The level the calibrator was read at, in dB re full scale, before any offset.

        Kept so the figure the calibration was derived from is still visible afterwards, rather
        than leaving only the number it produced. */
    float getCalibratorReadingDb() const { return calibratorReadingDb; }
    bool wasCalibratedAgainstReference() const { return calibratedAgainstReference; }

    /** Runs the calibration against a known reference level.

        A calibrator puts out a known sound pressure, so the difference between the level it
        was set to and the level the chain read is the whole correction. Doing it this way
        rather than by asking for a sensitivity means the interface gain, the preamp and the
        capsule are all included in the number that gets stored, which is what makes the
        reading afterwards an SPL and not just a shape.

        Only the reference level and the measured reading are needed; no assumption is made
        about the microphone's rated sensitivity, because the measurement is the authority. */
    void calibrateAgainst (float referenceSplDb, float measuredDbfs,
                           const juce::String& microphoneName = {});

    /** True when a correction can be trusted to produce SPL.

        Deliberately stricter than isEnabled: a calibration that has been selected but never
        checked against a reference is a number with no measurement behind it, and reporting SPL
        from it would be inventing the authority a calibrator exists to provide. */
    bool hasCalibration() const { return enabled && calibratedAgainstReference; }

    /** Serialises the profile, so a calibration survives closing the application. */
    juce::String toJson() const;

    /** Restores a profile written by toJson. Returns false when the text is not one. */
    bool fromJson (const juce::String& text);

    /** Writes the profile to a file. */
    bool saveToFile (const juce::File& file) const;

    /** A one line summary for the status line. */
    juce::String describe() const;

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
    float calibratorLevelDb = 94.0f;
    float calibratorReadingDb = 0.0f;
    bool calibratedAgainstReference = false;
    juce::String calibrationDate;
    std::vector<CurvePoint> curve;
    std::vector<float> responseDb;
};