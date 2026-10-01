#include "SignalGenerator.h"

SignalGenerator::SignalGenerator()
{
    reset();
    prepare(48000.0);
}

SignalGenerator::~SignalGenerator() = default;

juce::StringArray SignalGenerator::getTypeNames()
{
    return { "Pink Noise", "White Noise", "Sine", "Log Sweep" };
}

SignalGenerator::Type SignalGenerator::typeFromName(const juce::String& name)
{
    if (name == "White Noise")
        return Type::White;

    if (name == "Sine")
        return Type::Sine;

    if (name == "Log Sweep")
        return Type::LogSweep;

    return Type::Pink;
}

void SignalGenerator::prepare(double newSampleRate)
{
    sampleRate = std::max(1000.0, newSampleRate);

    sweepLength = (int) std::max(1.0, std::round((double) sweepDuration * sampleRate));

    const auto pinkLength = juce::nextPowerOfTwo(std::max(2048, (int) std::round(sampleRate)));
    pinkBuffer.assign((size_t) pinkLength, 0.0f);

    random.setSeed(0x51a1d0u);
    generatePink(pinkBuffer);

    reset();
    prepared = true;
}

void SignalGenerator::reset()
{
    phase = 0.0;
    sweepPhase = 0.0f;
    sweepPosition = 0;
    sweepProgress = 0.0f;
    pinkPosition = 0;
}

void SignalGenerator::setType(Type newType)
{
    if (type != newType)
    {
        type = newType;
        reset();
    }
}

void SignalGenerator::setLevelDb(float newLevelDb)
{
    levelDb = juce::jlimit(-90.0f, 0.0f, newLevelDb);
    amplitude = (float) std::pow(10.0, (double) levelDb / 20.0);
}

void SignalGenerator::setFrequency(float newFrequency)
{
    frequency = juce::jlimit(1.0f, (float) (sampleRate * 0.5 - 1.0), newFrequency);
}

void SignalGenerator::setSweepRange(float startFrequency, float endFrequency, float durationSeconds)
{
    sweepStart = juce::jlimit(1.0f, 20000.0f, startFrequency);
    sweepEnd = juce::jlimit(sweepStart, 20000.0f, endFrequency);
    sweepDuration = juce::jlimit(0.5f, 120.0f, durationSeconds);
    sweepLength = (int) std::max(1.0, std::round((double) sweepDuration * sampleRate));
    reset();
}

void SignalGenerator::setRunning(bool shouldRun)
{
    if (running != shouldRun)
        reset();

    running = shouldRun;
}

void SignalGenerator::generatePink(std::vector<float>& buffer)
{
    const auto size = (int) buffer.size();
    const auto order = juce::roundToInt(std::log2((double) size));

    if ((1 << order) != size)
        return;

    for (auto& sample : buffer)
        sample = random.nextFloat() * 2.0f - 1.0f;

    juce::dsp::FFT fft(order);
    std::vector<float> data((size_t) size * 2, 0.0f);

    for (int i = 0; i < size; ++i)
        data[(size_t) i] = buffer[(size_t) i];

    fft.performRealOnlyForwardTransform(data.data(), true);

    const auto bins = size / 2 + 1;

    for (int i = 1; i < bins; ++i)
    {
        const auto index = (size_t) i * 2;
        const auto scale = 1.0f / std::sqrt((float) i);

        data[index] *= scale;
        data[index + 1] *= scale;
    }

    data[0] = 0.0f;
    data[1] = 0.0f;

    const auto nyquist = (size_t) (bins - 1) * 2;
    data[nyquist] = 0.0f;
    data[nyquist + 1] = 0.0f;

    fft.performRealOnlyInverseTransform(data.data());

    float peak = 0.0f;

    for (int i = 0; i < size; ++i)
        peak = std::max(peak, std::abs(data[(size_t) i]));

    const auto normalise = 1.0f / std::max(peak, 1.0e-6f);

    for (int i = 0; i < size; ++i)
        buffer[(size_t) i] = data[(size_t) i] * normalise;
}

void SignalGenerator::process(float* output, int numSamples, float levelScale)
{
    if (output == nullptr || numSamples <= 0)
        return;

    if (!running)
    {
        juce::FloatVectorOperations::clear(output, numSamples);
        return;
    }

    if (!prepared)
        prepare(sampleRate);

    const auto level = amplitude * juce::jlimit(0.0f, 4.0f, levelScale);
    const auto ratio = sweepEnd / std::max(1.0f, sweepStart);
    const auto total = std::max(1, sweepLength);

    for (int i = 0; i < numSamples; ++i)
    {
        float sample = 0.0f;

        switch (type)
        {
            case Type::Pink:
            {
                if (pinkPosition >= (int) pinkBuffer.size())
                    pinkPosition = 0;

                sample = pinkBuffer[(size_t) pinkPosition++];
                break;
            }

            case Type::White:
                sample = random.nextFloat() * 2.0f - 1.0f;
                break;

            case Type::Sine:
            {
                phase += 2.0 * dsp::pi * (double) frequency / sampleRate;

                if (phase > 2.0 * dsp::pi)
                    phase -= 2.0 * dsp::pi;

                sample = (float) std::sin(phase);
                break;
            }

            case Type::LogSweep:
            {
                const auto fraction = (float) sweepPosition / (float) total;
                const auto instant = sweepStart * std::pow(ratio, fraction);

                sweepProgress = fraction;

                sweepPhase += instant / (float) sampleRate;

                if (sweepPhase >= 1.0f)
                    sweepPhase -= std::floor(sweepPhase);

                phase += 2.0 * dsp::pi * (double) sweepPhase;

                if (phase > 2.0 * dsp::pi)
                    phase -= 2.0 * dsp::pi;

                sample = (float) std::sin(phase);

                if (++sweepPosition >= total)
                {
                    sweepPosition = 0;
                    sweepProgress = 0.0f;
                }

                break;
            }
        }

        output[i] = juce::jlimit(-1.0f, 1.0f, sample * level);
    }
}
