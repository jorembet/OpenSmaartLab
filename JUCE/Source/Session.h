#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "TraceStore.h"
#include "MicrophoneCalibration.h"
#include "WavRecorder.h"

/** Everything a measurement session is made of, saved and loaded as one file.

    The file is json with an .osls extension. Json rather than a binary format for three
    reasons that matter more than size: a session has to be readable when something has gone
    wrong with it, it has to survive a version of the application that knows some of the
    fields and not others, and a saved session is the first thing anyone looks at when a
    measurement is disputed. A format that resists all three makes those three things
    impossible.

    Nothing is written that the application cannot read back. A field it does not recognise is
    kept and written out again untouched, so saving a session in a newer build and opening it in
    an older one loses the fields the older one does not understand instead of the whole file. */
struct Session
{
    /** Bumped whenever the shape changes. A file from a newer major version is refused rather
        than half-read, because half-reading one produces a session that looks complete and is
        not. */
    static constexpr int formatVersion = 1;

    // Device and transport.
    juce::String inputDevice;
    juce::String outputDevice;
    juce::String outputPulseSink;
    int inputChannel = 0;
    int outputChannel = 1;
    int measurementChannel = 0;
    int referenceChannel = 1;

    double sampleRate = 48000.0;
    int bufferSize = 1024;

    // Measurement settings.
    int fftSize = 16384;
    int averaging = 1;
    float averagingSeconds = 3.0f;
    int window = 1;
    float overlapPercent = 50.0f;
    bool thirdOctave = false;
    float rtaLowHz = 20.0f;
    float rtaHighHz = 20000.0f;
    bool automaticDelay = true;
    bool delayCompensation = true;
    float temperatureC = 20.0f;

    // Calibration.
    MicrophoneCalibration calibration;

    // Traces.
    TraceStore traces;

    juce::String notes;
    juce::String createdAt;
    juce::String applicationVersion;

    /** Writes the session. Refuses an unwritable path and says why. */
    bool save (const juce::File& file, juce::String* errorOut = nullptr) const;

    /** Reads a session. Refuses a file that is not one, or one from a newer format. */
    bool load (const juce::File& file, juce::String* errorOut = nullptr);

    juce::ValueTree toValueTree() const;
    bool fromValueTree (const juce::ValueTree& tree, juce::String* errorOut);

    static juce::String getFileExtension() { return ".osls"; }
    static juce::String getFileWildcard() { return "*.osls"; }

    /** A one line summary for a file browser or a session list. */
    juce::String describe() const;

    Session();

    /** Empties a session so it can be reused, the way a fresh one would be. */
    void clear();

    // Not copyable, because the trace store inside is not, and a session that could be copied by
    // accident would quietly duplicate six traces. Nothing needs to copy one: it is written to a
    // file or read from one.
    Session (const Session&) = delete;
    Session& operator= (const Session&) = delete;
};

/** Writes measurements out as csv, json, wav, or a picture of a plot.

    Every export names the units, the sample rate and what produced it in the file itself
    rather than leaving that to whoever opens it. A curve of numbers in a file with no header
    is a list, and six months later nobody can say which of the twelve exported curves was the
    one they wanted. */
class MeasurementExporter
{
public:
    /** What is being written out. */
    enum class Kind
    {
        Fft,
        Rta,
        Magnitude,
        Phase,
        Coherence,
        Impulse,
        Etc,
        Rt60,
        Thd,
        ThdPlusNoise,
        Noise,
        Spl
    };

    static constexpr int numKinds = 12;

    struct Curve
    {
        juce::String columnName;
        juce::String unit;
        std::vector<float> x;
        std::vector<float> y;

        /** Labels for x, when they are not a plain number. A reverberation time is one number
            per frequency band, so its x axis is a band and not a frequency, and saying so in
            the header is the difference between a readable file and a puzzling one. */
        std::vector<juce::String> xLabels;
        juce::String xName;
        juce::String xUnit;
    };

    struct Table
    {
        std::vector<juce::String> columns;
        std::vector<juce::String> units;
        std::vector<std::vector<juce::String>> rows;
        juce::String title;
    };

    MeasurementExporter();

    static juce::StringArray getKindNames();
    static juce::String kindName (Kind kind);
    static bool isValidKind (int index) noexcept { return index >= 0 && index < numKinds; }

    /** A curve as csv: two columns, with a header that names the units. */
    bool exportCsv (const juce::File& file, const Curve& curve, juce::String* errorOut = nullptr) const;

    /** A curve as json, with the axis names and units kept. */
    bool exportJson (const juce::File& file, const Curve& curve, juce::String* errorOut = nullptr) const;

    /** A table as csv, one line per row. */
    bool exportCsv (const juce::File& file, const Table& table, juce::String* errorOut = nullptr) const;

    /** A table as json. */
    bool exportJson (const juce::File& file, const Table& table, juce::String* errorOut = nullptr) const;

    /** Samples as a wav file, at a chosen depth. */
    bool exportWav (const juce::File& file, const std::vector<float>& samples, double sampleRate,
                    int numChannels = 1, WavRecorder::BitDepth bitDepth = WavRecorder::BitDepth::Float32,
                    juce::String* errorOut = nullptr) const;

    /** Two channels as a wav file. */
    bool exportWav (const juce::File& file, const std::vector<float>& left,
                    const std::vector<float>& right, double sampleRate,
                    WavRecorder::BitDepth bitDepth = WavRecorder::BitDepth::Float32,
                    juce::String* errorOut = nullptr) const;

    /** A plot as an svg.

        Svg rather than a bitmap, because a measurement that has to go into a report gets scaled
        and a bitmap goes blurry, and because a vector file can be edited. Drawn with the same
        axis convention as the on-screen plots so a printout matches what was measured. */
    bool exportSvg (const juce::File& file, const Curve& curve, const juce::String& title,
                    juce::String* errorOut = nullptr) const;

    /** A table as an svg, one row of text per line. */
    bool exportSvg (const juce::File& file, const Table& table, juce::String* errorOut = nullptr) const;

    /** The extension each kind of export wants. */
    static juce::String extensionFor (Kind kind);

private:
    juce::String makeHeader() const;
};