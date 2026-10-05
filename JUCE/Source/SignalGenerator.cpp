#include "SignalGenerator.h"

namespace
{
    constexpr int numPinkBuffers = 2;
}

SignalGenerator::SignalGenerator()
{
    prepare (48000.0);
}

SignalGenerator::~SignalGenerator() = default;

namespace
{
    /** Polynomial band-limited step, correcting one discontinuity of a waveform.

        A square or a saw is built from jumps, and a jump is infinitely wide in frequency, so
        sampling it folds energy back down the spectrum from above Nyquist. On a measurement
        generator that is not cosmetic: a 1 kHz square sampled at 48 kHz would put spurious
        energy at 47 kHz, which folds to 1 kHz and very nearly doubles the reading of the
        fundamental. Correcting each edge with this two-piece polynomial removes almost all of
        that, at the cost of a few arithmetic operations per edge and nothing per sample that
        the loop cannot already do.

        t is the position within the cycle and dt the fraction of a cycle one sample covers. */
    inline float polyBlep (float t, float dt)
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0f;
        }

        if (t > 1.0f - dt)
        {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }

        return 0.0f;
    }
}

juce::StringArray SignalGenerator::getTypeNames()
{
    return { "Pink Noise", "White Noise", "Sine", "Square", "Triangle", "Saw",
             "Impulse", "Log Sweep" };
}

SignalGenerator::Type SignalGenerator::typeFromName(const juce::String& name)
{
    if (name == "White Noise")
        return Type::White;

    if (name == "Sine")
        return Type::Sine;

    if (name == "Square")
        return Type::Square;

    if (name == "Triangle")
        return Type::Triangle;

    if (name == "Saw")
        return Type::Saw;

    if (name == "Impulse")
        return Type::Impulse;

    if (name == "Log Sweep")
        return Type::LogSweep;

    return Type::Pink;
}

void SignalGenerator::prepare(double newSampleRate)
{
    const auto rate = std::max (1000.0, newSampleRate);

    sampleRate.store (rate);
    sweepLength.store ((int) std::max (1.0, std::round ((double) sweepDuration.load() * rate)));

    // Both buffers are sized here, once, so publishing a regenerated one later is a
    // pointer swap rather than an allocation on the audio thread.
    const auto pinkLength = juce::nextPowerOfTwo (std::max (2048, (int) std::round (rate)));

    for (auto& buffer : pinkBuffers)
        buffer.assign ((size_t) pinkLength, 0.0f);

    random.setSeed (0x51a1d0u);

    // Buffer zero is built first and is the one published here; every later change is
    // built in buffer one and swapped in, so the audio thread only ever reads a buffer
    // that is already complete.
    activePink.store (1);
    generatePink();
    activePink.store (0);

    prepared.store (true);
    lastPinkBuffer = -1;
    reset();
}

void SignalGenerator::reset()
{
    // The phase belongs to the audio thread, so this only asks for it to be reset. The
    // flag is cleared by the thread that owns the phase.
    resetRequested.store (true, std::memory_order_release);
}

void SignalGenerator::setType(Type newType)
{
    if (type.load (std::memory_order_relaxed) == newType)
        return;

    type.store (newType, std::memory_order_release);
    reset();
}

void SignalGenerator::setLevelDb(float newLevelDb)
{
    const auto clamped = juce::jlimit (-90.0f, 0.0f, newLevelDb);
    levelDb.store (clamped, std::memory_order_relaxed);
    amplitude.store ((float) std::pow (10.0, (double) clamped / 20.0), std::memory_order_relaxed);
}

void SignalGenerator::setFrequency(float newFrequency)
{
    // 20 Hz to 20 kHz, and never past Nyquist. The lower bound is where a measurement starts
    // being about the room rather than the speaker, and the upper one is the top of the band
    // the pink noise and the sweep are generated for, so letting the tone go above it would
    // put energy in where nothing else in the application reaches.
    const auto ceiling = std::min (20000.0f, (float) (sampleRate.load() * 0.5 - 1.0));
    frequency.store (juce::jlimit (20.0f, std::max (20.0f, ceiling), newFrequency),
                     std::memory_order_relaxed);
}

juce::Array<SignalGenerator::BandPreset> SignalGenerator::getBandPresets()
{
    // Edges follow common driver divisions: subwoofer below the first woofer octave,
    // woofer to the lower midrange, midrange up to the presence region, and everything
    // above for the tweeter. The boundaries are also where the slope reading stays
    // meaningful, because each band spans at least two octaves.
    juce::Array<BandPreset> presets;
    presets.add ({ "Subwoofer  20 - 100 Hz", 20.0f, 100.0f });
    presets.add ({ "Woofer  100 - 500 Hz", 100.0f, 500.0f });
    presets.add ({ "Midrange  500 - 2000 Hz", 500.0f, 2000.0f });
    presets.add ({ "Tweeter  2000 - 20000 Hz", 2000.0f, 20000.0f });
    presets.add ({ "Full  20 - 20000 Hz", 20.0f, 20000.0f });
    return presets;
}

int SignalGenerator::findBandPreset(float lowFrequency, float highFrequency)
{
    const auto presets = getBandPresets();

    for (int i = 0; i < presets.size(); ++i)
        if (std::abs (presets[i].lowFrequency - lowFrequency) < 0.5f
            && std::abs (presets[i].highFrequency - highFrequency) < 0.5f)
            return i;

    return -1;
}

void SignalGenerator::setBandLimits(float lowFrequency, float highFrequency)
{
    const auto low = juce::jlimit (10.0f, 2000.0f, lowFrequency);
    const auto high = juce::jlimit (low * 2.0f, 20000.0f, highFrequency);

    if (std::abs (bandLow.load() - low) < 0.01f && std::abs (bandHigh.load() - high) < 0.01f)
        return;

    // The taper edges are published before the buffer is rebuilt, so the buffer is built
    // from the band that was asked for rather than the one before it.
    bandLow.store (low, std::memory_order_release);
    bandHigh.store (high, std::memory_order_release);
    pinkLowFrequency.store (low, std::memory_order_release);
    pinkHighFrequency.store (high, std::memory_order_release);

    // Keep the default sweep inside the band until the user moves it.
    sweepStart.store (juce::jlimit (low, high, sweepStart.load()), std::memory_order_release);
    sweepEnd.store (juce::jlimit (low, high, sweepEnd.load()), std::memory_order_release);
    sweepLength.store ((int) std::max (1.0, std::round ((double) sweepDuration.load()
                                                        * sampleRate.load())),
                       std::memory_order_release);

    generatePink();
}

void SignalGenerator::setSweepRange(float startFrequency, float endFrequency,
                                    float durationSeconds)
{
    sweepStart.store (startFrequency, std::memory_order_release);
    sweepEnd.store (endFrequency, std::memory_order_release);
    sweepDuration.store (durationSeconds, std::memory_order_release);
    sweepLength.store ((int) std::max (1.0, std::round (durationSeconds * sampleRate.load())),
                       std::memory_order_release);
}

void SignalGenerator::setRunning(bool shouldRun)
{
    running.store (shouldRun, std::memory_order_release);
}

void SignalGenerator::generatePink()
{
    if (pinkBuffers[0].empty())
        return;

    // The buffer the audio thread is not reading is rebuilt, then published by swapping
    // one index. Nothing is ever written into the buffer in use, and the swap is the only
    // thing the audio thread ever observes.
    const auto target = 1 - activePink.load (std::memory_order_relaxed);
    auto& buffer = pinkBuffers[(size_t) target];

    if (target < 0 || target >= numPinkBuffers || buffer.empty())
        return;

    // The FFT has to be exactly the length of the buffer, otherwise the mask below is
    // applied to bins that do not exist and the result is silence.
    const auto size = (int) buffer.size();
    const auto order = (int) std::lround (std::log2 ((double) size));

    if ((1 << order) != size)
        return;

    juce::dsp::FFT fft (order);

    // Pink noise is white noise shaped in the frequency domain, so the buffer starts as
    // white noise from the seeded generator. That makes the result reproducible: the
    // same seed and the same band give the same noise every run.
    for (auto& sample : buffer)
        sample = random.nextFloat() * 2.0f - 1.0f;

    // Allocated here, on the thread that asked for the change, never on the audio thread.
    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::memcpy (data.data(), buffer.data(), (size_t) size * sizeof (float));

    fft.performRealOnlyForwardTransform (data.data(), true);

    const auto bins = size / 2 + 1;
    const auto rate = sampleRate.load ();

    // Pink noise is generated over 20 Hz to 20 kHz and tapered at both ends. Without
    // the low-end taper about a third of the power sits below 20 Hz, where it is
    // wasted: most loudspeakers barely reproduce it, and it can cause excursion
    // problems without adding anything audible to the measurement. The high-end
    // taper keeps the signal inside the audible band instead of up against Nyquist.
    const auto binWidth = (float) rate / (float) size;

    // The fade spans a fixed fraction of a decade on each side, independent of the
    // FFT size, so the corner shape does not change with the buffer length.
    constexpr float fadeDecades = 0.08f;
    const auto fadeRatio = (float) std::pow (10.0, (double) fadeDecades);

    const auto lowEdge = pinkLowFrequency.load();
    const auto highEdge = pinkHighFrequency.load();
    const auto fadeStart = lowEdge / fadeRatio;
    const auto fadeEnd = highEdge * fadeRatio;

    const auto taper = [fadeStart, fadeEnd, lowEdge, highEdge] (float frequency)
    {
        if (frequency <= fadeStart || frequency >= fadeEnd)
            return 0.0f;

        if (frequency < lowEdge)
        {
            // Raised cosine from silence to full over the fade, so there is no step at
            // the corner that would show up as a spectral discontinuity.
            const auto fraction = std::log10 (frequency / fadeStart) / fadeDecades;
            return 0.5f * (1.0f - std::cos (juce::jlimit (0.0f, 1.0f, fraction)
                                             * juce::MathConstants<float>::pi));
        }

        if (frequency > highEdge)
        {
            const auto fraction = std::log10 (fadeEnd / frequency) / fadeDecades;
            return 0.5f * (1.0f - std::cos (juce::jlimit (0.0f, 1.0f, fraction)
                                             * juce::MathConstants<float>::pi));
        }

        return 1.0f;
    };

    for (int i = 1; i < bins; ++i)
    {
        const auto index = (size_t) i * 2;
        const auto frequency = (float) i * binWidth;
        const auto scale = taper (frequency) / std::sqrt ((float) i);

        data[index] *= scale;
        data[index + 1] *= scale;
    }

    data[0] = 0.0f;
    data[1] = 0.0f;

    const auto nyquist = (size_t) (bins - 1) * 2;
    data[nyquist] = 0.0f;
    data[nyquist + 1] = 0.0f;

    fft.performRealOnlyInverseTransform (data.data());

    float peak = 0.0f;

    for (int i = 0; i < size; ++i)
        peak = std::max (peak, std::abs (data[(size_t) i]));

    const auto normalise = 1.0f / std::max (peak, 1.0e-6f);

    for (int i = 0; i < size; ++i)
        buffer[(size_t) i] = data[(size_t) i] * normalise;

    // One release store publishes the finished buffer.
    activePink.store (target, std::memory_order_release);
}

void SignalGenerator::process(float* output, int numSamples, float levelScale)
{
    if (output == nullptr || numSamples <= 0)
        return;

    if (! prepared.load (std::memory_order_relaxed))
    {
        // Not prepared yet means no pink buffer exists to read, so the output is silence
        // rather than whatever was in the memory before.
        juce::FloatVectorOperations::clear (output, numSamples);
        return;
    }

    if (! running.load (std::memory_order_relaxed) || muted.load (std::memory_order_relaxed))
    {
        juce::FloatVectorOperations::clear (output, numSamples);
        return;
    }

    if (resetRequested.exchange (false, std::memory_order_acquire))
    {
        phase = 0.0;
        sweepFrequency = sweepStart.load (std::memory_order_relaxed);
        sweepPosition = 0;
        pinkPosition = 0;
        sweepProgress.store (0.0f, std::memory_order_relaxed);
        impulseArmed = true;
    }

    // Settings are read once per block, so the loop below reads nothing shared and the
    // cost per sample is the arithmetic itself.
    const auto level = amplitude.load (std::memory_order_relaxed)
                     * juce::jlimit (0.0f, 4.0f, levelScale);
    const auto rate = sampleRate.load (std::memory_order_relaxed);
    const auto currentType = type.load (std::memory_order_relaxed);
    const auto currentFrequency = frequency.load (std::memory_order_relaxed);
    const auto start = sweepStart.load (std::memory_order_relaxed);
    const auto finish = sweepEnd.load (std::memory_order_relaxed);
    const auto total = std::max (1, sweepLength.load (std::memory_order_relaxed));

    const auto pinkIndex = activePink.load (std::memory_order_acquire);
    const auto* pink = pinkBuffers[(size_t) pinkIndex].data();
    const auto pinkSize = (int) pinkBuffers[(size_t) pinkIndex].size();

    if (pinkIndex != lastPinkBuffer)
    {
        // A regenerated buffer is a new signal, so the read position restarts with it.
        lastPinkBuffer = pinkIndex;
        pinkPosition = 0;
    }

    const auto ratio = finish / std::max (1.0f, start);
    const auto sweepStep = (float) std::pow (ratio, 1.0 / (double) total);

    switch (currentType)
    {
        case Type::Pink:
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (pinkPosition >= pinkSize)
                    pinkPosition = 0;

                output[i] = juce::jlimit (-1.0f, 1.0f, pink[pinkPosition++] * level);
            }

            break;
        }

        case Type::White:
        {
            for (int i = 0; i < numSamples; ++i)
                output[i] = juce::jlimit (-1.0f, 1.0f,
                                          (random.nextFloat() * 2.0f - 1.0f) * level);

            break;
        }

        case Type::Sine:
        {
            const auto step = 2.0 * dsp::pi * (double) currentFrequency / rate;

            for (int i = 0; i < numSamples; ++i)
            {
                phase += step;

                if (phase > 2.0 * dsp::pi)
                    phase -= 2.0 * dsp::pi;

                output[i] = juce::jlimit (-1.0f, 1.0f, (float) std::sin (phase) * level);
            }

            break;
        }

        case Type::Square:
        case Type::Triangle:
        case Type::Saw:
        {
            // One phase accumulator drives all three, and the waveform is read from where that
            // phase sits inside the cycle. Nothing here allocates and nothing transcendental
            // is called per sample, which is the same discipline the sine follows.
            const auto step = 2.0 * dsp::pi * (double) currentFrequency / rate;
            const auto dt = (float) (step / (2.0 * dsp::pi));
            const auto inverseTwoPi = (float) (1.0 / (2.0 * dsp::pi));

            for (int i = 0; i < numSamples; ++i)
            {
                phase += step;

                if (phase >= 2.0 * dsp::pi)
                    phase -= 2.0 * dsp::pi;

                const auto t = (float) phase * inverseTwoPi;
                float value = 0.0f;

                if (currentType == Type::Square)
                {
                    // Equal time up and down, so the two edges sit a half cycle apart and can
                    // share the one correction with a shifted position.
                    value = t < 0.5f ? 1.0f : -1.0f;

                    // The edge at t = 0 rises and the one half a cycle later falls, so they
                    // need opposite corrections. Subtracting both pushed the rising edge to
                    // twice full scale, which is the one thing this is here to prevent.
                    value += polyBlep (t, dt);
                    value -= polyBlep (t < 0.5f ? t + 0.5f : t - 0.5f, dt);
                }
                else if (currentType == Type::Saw)
                {
                    value = 2.0f * t - 1.0f;
                    value -= polyBlep (t, dt);
                }
                else
                {
                    // Continuous, so there is no step to correct: the slope changes but the
                    // value does not, and the harmonics already fall away at 1/n squared.
                    value = t < 0.5f ? (-1.0f + 4.0f * t) : (3.0f - 4.0f * t);
                }

                output[i] = juce::jlimit (-1.0f, 1.0f, value * level);
            }

            break;
        }

        case Type::Impulse:
        {
            // One impulse, then silence until the next reset. An impulse response is measured
            // from a single arrival, and a repeating train would fold every repeat back onto
            // the first in the transform, smearing the very thing being measured.
            for (int i = 0; i < numSamples; ++i)
                output[i] = impulseArmed && i == 0 ? level : 0.0f;

            impulseArmed = false;
            break;
        }

        case Type::LogSweep:
        {
            // A logarithmic sweep is exponential, so the frequency is advanced by one multiply
            // per sample rather than a pow call per sample: the same curve, and the audio
            // thread is not asked for a transcendental inside the callback.
            //
            // The phase is advanced directly by the instantaneous increment. There is no
            // separate fractional accumulator here, and that matters more than it looks: an
            // accumulator that sums the increment and wraps at one whole cycle is only correct
            // when the increment is constant. During a sweep the increment changes every
            // sample, so summing it drove the value up to one and back roughly every two
            // thousand samples, and the signal came out as a wild sweep through the whole
            // band over and over rather than one pass from 20 Hz to 20 kHz.
            auto frequencyNow = sweepFrequency;

            for (int i = 0; i < numSamples; ++i)
            {
                // The increment is under half a cycle because the frequency is clamped below
                // Nyquist, so one subtraction is always enough to bring the phase back in
                // range.
                phase += 2.0 * dsp::pi * ((double) frequencyNow / rate);

                if (phase >= 2.0 * dsp::pi)
                    phase -= 2.0 * dsp::pi;

                output[i] = juce::jlimit (-1.0f, 1.0f, (float) std::sin (phase) * level);

                frequencyNow *= sweepStep;

                if (++sweepPosition >= total)
                {
                    sweepPosition = 0;
                    frequencyNow = start;
                }

                sweepFrequency = frequencyNow;
                sweepProgress.store ((float) sweepPosition / (float) total,
                                     std::memory_order_relaxed);
            }

            break;
        }
    }
}
