#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <vector>
#include "DSP.h"

/** Test and measurement signal generator.

    Everything the audio thread reads is atomic, and the pink noise is double buffered:
    a setting changed from the GUI thread is published only once the whole new buffer has
    been written, so the audio callback can never read a half regenerated buffer and can
    never allocate. Resetting the phase is requested with a flag for the same reason: the
    phase belongs to the audio thread and only the audio thread may move it.
*/
class SignalGenerator
{
public:
    /** Every waveform the generator can produce.

        The names are what the selector shows, so a waveform the user cannot name is a
        waveform they cannot go back to. The order here is the order they appear in the list,
        which runs from the broadband signals used for level and response, through the tones
        used to place a problem, to the shaped ones a loudspeaker is actually tested with. */
    enum class Type
    {
        Pink,
        White,
        Sine,
        Square,
        Triangle,
        Saw,
        Impulse,
        LogSweep
    };

    /** True for the types whose spectrum is a set of harmonics of the set frequency, which
        is what the level and the band controls mean something for. Broadband noise is not
        one of them, so it is reported separately rather than being quietly treated as one. */
    static bool isHarmonicType (Type type)
    {
        return type == Type::Sine || type == Type::Square || type == Type::Triangle
            || type == Type::Saw || type == Type::Impulse;
    }

    SignalGenerator();
    ~SignalGenerator();

    static juce::StringArray getTypeNames();
    static Type typeFromName(const juce::String& name);

    void prepare(double sampleRate);

    void setType(Type newType);
    Type getType() const { return type.load(std::memory_order_relaxed); }

    void setLevelDb(float newLevelDb);
    float getLevelDb() const { return levelDb.load(std::memory_order_relaxed); }

    void setFrequency(float newFrequency);
    float getFrequency() const { return frequency.load(std::memory_order_relaxed); }

    void setSweepRange(float startFrequency, float endFrequency, float durationSeconds);
    float getSweepStart() const { return sweepStart.load(std::memory_order_relaxed); }
    float getSweepEnd() const { return sweepEnd.load(std::memory_order_relaxed); }
    float getSweepDuration() const { return sweepDuration.load(std::memory_order_relaxed); }
    float getSweepProgress() const { return sweepProgress.load(std::memory_order_relaxed); }

    /** Band limits used for pink noise and for the default sweep range. */
    void setBandLimits (float lowFrequency, float highFrequency);
    float getBandLow() const { return bandLow.load(std::memory_order_relaxed); }
    float getBandHigh() const { return bandHigh.load(std::memory_order_relaxed); }

    struct BandPreset
    {
        juce::String name;
        float lowFrequency = 20.0f;
        float highFrequency = 20000.0f;
    };

    /** One band per driver, from subwoofer to tweeter.

        Room correction is done one driver at a time: coherence and level are only
        meaningful inside the band that the driver actually reproduces, so a preset per
        driver keeps that measurement honest instead of averaging a tweeter with a
        subwoofer.
    */
    static juce::Array<BandPreset> getBandPresets();

    /** Index of the preset these limits match, or -1 when the band is a custom one. */
    static int findBandPreset (float lowFrequency, float highFrequency);

    /** Silences the generator without tearing it down.

        Separate from the level, because a level of minus infinity is not a thing that can be
        dialled in: the slider stops at zero, so muting has to be its own control or the last
        step down would be unreachable. It also leaves the waveform running underneath, so
        unmuting brings back the signal in phase rather than restarting it. */
    void setMuted(bool shouldMute) { muted.store (shouldMute, std::memory_order_relaxed); }
    bool isMuted() const { return muted.load (std::memory_order_relaxed); }

    void setRunning(bool shouldRun);
    bool isRunning() const { return running.load(std::memory_order_relaxed); }

    void reset();

    void process(float* output, int numSamples, float levelScale = 1.0f);

private:
    void generatePink();
    void publishPendingType();

    std::atomic<Type> type { Type::Pink };
    std::atomic<float> levelDb { -24.0f };
    std::atomic<float> amplitude { 0.063f };
    std::atomic<float> frequency { 1000.0f };
    std::atomic<float> sweepStart { 20.0f };
    std::atomic<float> sweepEnd { 20000.0f };
    std::atomic<float> sweepDuration { 10.0f };
    std::atomic<float> sweepProgress { 0.0f };
    std::atomic<bool> running { false };
    std::atomic<bool> muted { false };
    std::atomic<bool> prepared { false };

    // Pink noise covers the audible band. Below 20 Hz most speakers barely move and
    // the power is wasted; above 20 kHz it is inaudible and only stresses the
    // converter.
    std::atomic<float> pinkLowFrequency { 20.0f };
    std::atomic<float> pinkHighFrequency { 20000.0f };
    std::atomic<float> bandLow { 20.0f };
    std::atomic<float> bandHigh { 20000.0f };

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> sweepLength { 480000 };

    /** Both buffers are the same length, so switching between them cannot change how far
        the audio thread reads before it has to wrap. */
    std::vector<float> pinkBuffers[2];
    std::atomic<int> activePink { 0 };
    std::atomic<bool> resetRequested { false };

    // Everything below belongs to the audio thread alone.
    int pinkPosition = 0;
    int lastPinkBuffer = -1;
    int sweepPosition = 0;
    double phase = 0.0;
    /** Where the sweep has got to, carried between blocks. Belongs to the audio thread with
        the rest of the sweep state, so a sweep advances continuously instead of restarting
        from its first frequency on every callback. */
    float sweepFrequency = 20.0f;
    /** Set for one block after a reset so the impulse fires once and is then silent. A
        single impulse is the point: an impulse response is measured from one arrival, and a
        train of them would fold every repeat on top of the first in the transform. */
    bool impulseArmed = false;
    double sweepRatioPerSample = 1.0;

    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalGenerator)
};
