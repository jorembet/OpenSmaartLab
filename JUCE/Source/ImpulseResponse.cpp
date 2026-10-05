#include "ImpulseResponse.h"

ImpulseResponse::DecayFit ImpulseResponse::fitDecay(const std::vector<float>& time,
                                                     const std::vector<float>& decayDb,
                                                     float upperLevelDb,
                                                     float lowerLevelDb)
{
    DecayFit fit;

    if (time.size() != decayDb.size() || time.size() < 16)
        return fit;

    double sumT = 0.0;
    double sumD = 0.0;
    double sumTT = 0.0;
    double sumTD = 0.0;

    int count = 0;

    fit.upperLevelDb = upperLevelDb;
    fit.lowerLevelDb = lowerLevelDb;

    for (size_t i = 0; i < time.size(); ++i)
    {
        const auto level = decayDb[i];

        if (level > upperLevelDb || level < lowerLevelDb)
            continue;

        sumT += time[i];
        sumD += level;
        sumTT += (double) time[i] * time[i];
        sumTD += (double) time[i] * level;
        ++count;

        if (count == 1)
            fit.startTime = time[i];

        fit.endTime = time[i];
    }

    if (count < 8)
        return fit;

    const auto denominator = (double) count * sumTT - sumT * sumT;

    if (std::abs(denominator) < 1.0e-20)
        return fit;

    fit.slope = (float) (((double) count * sumTD - sumT * sumD) / denominator);

    if (fit.slope >= -1.0e-4f)
        return fit;

    const auto meanT = sumT / (double) count;
    const auto meanD = sumD / (double) count;

    double varianceT = 0.0;
    double varianceD = 0.0;

    int seen = 0;

    for (size_t i = 0; i < time.size(); ++i)
    {
        const auto level = decayDb[i];

        if (level > upperLevelDb || level < lowerLevelDb)
            continue;

        const auto dt = time[i] - meanT;
        const auto dd = level - meanD;

        varianceT += (double) dt * dt;
        varianceD += (double) dd * dd;
        ++seen;
    }

    fit.correlation = (varianceT > 0.0 && varianceD > 0.0)
                    ? juce::jlimit(-1.0f, 1.0f, (float) (varianceT / std::sqrt(varianceT * varianceD)))
                    : 0.0f;

    fit.valid = true;
    return fit;
}

int ImpulseResponse::findDirectArrival(const float* ir, int numSamples)
{
    if (ir == nullptr || numSamples <= 0)
        return 0;

    int peakIndex = 0;
    float peakValue = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto value = std::abs(ir[i]);

        if (value > peakValue)
        {
            peakValue = value;
            peakIndex = i;
        }
    }

    if (peakValue <= 0.0f)
        return 0;

    const auto threshold = peakValue * 0.1f;

    for (int i = peakIndex; i >= 0; --i)
        if (std::abs(ir[i]) >= threshold)
            return i;

    return 0;
}

ImpulseResponse::Acoustics ImpulseResponse::analyse(const float* ir,
                                                    int numSamples,
                                                    float sampleRate)
{
    Acoustics result;

    if (ir == nullptr || numSamples < 64 || sampleRate <= 0.0f)
        return result;

    const auto direct = findDirectArrival(ir, numSamples);
    const auto length = numSamples - direct;

    if (length < 64)
        return result;

    result.directArrivalSample = direct;
    result.directArrivalMs = (float) direct / sampleRate * 1000.0f;

    result.time.resize((size_t) length);
    result.decayDb.resize((size_t) length);
    result.energyDb.resize((size_t) length);

    double totalEnergy = 0.0;

    for (int i = 0; i < length; ++i)
        totalEnergy += (double) ir[direct + i] * ir[direct + i];

    if (totalEnergy <= 1.0e-20)
        return result;

    double tail = 0.0;

    for (int i = length - 1; i >= 0; --i)
    {
        const auto value = ir[direct + i];
        tail += (double) value * value;

        result.time[(size_t) i] = (float) i / sampleRate;
        result.decayDb[(size_t) i] = dsp::db10((float) (tail / totalEnergy));
        result.energyDb[(size_t) i] = dsp::db20((float) (std::abs(value) / std::sqrt(totalEnergy)));
    }

    const auto t10 = fitDecay(result.time, result.decayDb, 0.0f, -10.0f);
    const auto t20 = fitDecay(result.time, result.decayDb, -5.0f, -25.0f);
    const auto t30 = fitDecay(result.time, result.decayDb, -5.0f, -35.0f);

    // Every one of these is extrapolated to sixty decibels, because that is what the name means.
// T20 and T30 name the range they were fitted over, not the distance they are quoted over, so
// dividing T20 by twenty reported the twenty decibel span as though it were a reverberation
// time, and doubling T30 reported sixty decibels as though it were thirty. Both made a room
// read twice as long as it is, and both now go through the same rule as EDT.
    result.edt = t10.valid ? juce::jlimit (0.0f, 60.0f, -60.0f / t10.slope) : 0.0f;
    result.t20 = t20.valid ? juce::jlimit (0.0f, 60.0f, -60.0f / t20.slope) : 0.0f;
    result.t30 = t30.valid ? juce::jlimit (0.0f, 60.0f, -60.0f / t30.slope) : 0.0f;
    result.rt60 = t30.valid ? juce::jlimit (0.0f, 120.0f, -60.0f / t30.slope) : 0.0f;
    result.correlation = t30.valid ? t30.correlation : (t20.valid ? t20.correlation : 0.0f);

    const auto earlyMs = 0.050f;
    const auto midMs = 0.080f;

    double early = 0.0;
    double mid = 0.0;
    double late = 0.0;
    double moment = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto value = ir[direct + i];
        const auto power = (double) value * value;
        const auto seconds = (double) i / sampleRate;

        moment += seconds * power;

        if (seconds < earlyMs)
            early += power;
        else if (seconds < midMs)
            mid += power;
        else
            late += power;
    }

    result.c50 = dsp::db10((float) (early / std::max(early + mid + late, 1.0e-20)));
    result.c80 = dsp::db10((float) ((early + mid) / std::max(early + mid + late, 1.0e-20)));
    result.d50 = dsp::db10((float) (early / std::max(late, 1.0e-20)));
    result.centreTime = (float) (moment / totalEnergy);

    result.valid = true;
    return result;
}

namespace
{
    // FFT size that comfortably holds the impulse response without time aliasing.
    int paddedFftOrder(int numSamples)
    {
        int order = 10;

        while ((1 << order) < numSamples * 2)
            ++order;

        return order;
    }
}

std::vector<ImpulseResponse::BandRt60> ImpulseResponse::bandRt60(
    const float* ir, int numSamples, float sampleRate,
    const std::vector<float>& centres)
{
    std::vector<BandRt60> out;

    if (ir == nullptr || numSamples < 256 || sampleRate <= 0.0f || centres.empty())
        return out;

    const auto order = paddedFftOrder(numSamples);
    const auto size = 1 << order;
    juce::dsp::FFT fft(order);

    std::vector<float> buffer((size_t) size * 2, 0.0f);

    for (int i = 0; i < numSamples; ++i)
        buffer[(size_t) i] = ir[i];

    fft.performRealOnlyForwardTransform(buffer.data());

    const auto numBins = size / 2 + 1;

    for (const auto centre : centres)
    {
        BandRt60 entry;
        entry.frequency = centre;

        if (centre <= 20.0f * 0.7f || centre >= sampleRate * 0.5f * 0.7f)
        {
            out.push_back(entry);
            continue;
        }

        std::vector<float> band((size_t) size * 2, 0.0f);
        const auto low = centre / std::sqrt(2.0f);
        const auto high = centre * std::sqrt(2.0f);

        for (int k = 0; k < numBins; ++k)
        {
            const auto f = (float) k * sampleRate / (float) size;

            if (f >= low && f <= high)
            {
                band[(size_t) k * 2] = buffer[(size_t) k * 2];
                band[(size_t) k * 2 + 1] = buffer[(size_t) k * 2 + 1];
            }
        }

        // Rebuild both halves of the spectrum before the inverse transform.
        for (int k = 1; k < numBins - 1; ++k)
        {
            band[(size_t) (size - k) * 2] = band[(size_t) k * 2];
            band[(size_t) (size - k) * 2 + 1] = -band[(size_t) k * 2 + 1];
        }

        fft.performRealOnlyInverseTransform(band.data());

        const auto acoustics = analyse(band.data(), numSamples, sampleRate);
        entry = { centre, acoustics.rt60, acoustics.valid && acoustics.rt60 > 0.0f };
        out.push_back(entry);
    }

    return out;
}

ImpulseResponse::Waterfall ImpulseResponse::waterfall(
    const float* ir, int numSamples, float sampleRate,
    const std::vector<float>& centres, int frameSize, int hopSize)
{
    Waterfall result;
    result.bandCentres = centres;

    if (ir == nullptr || numSamples < frameSize || sampleRate <= 0.0f || centres.empty())
        return result;

    juce::dsp::FFT fft((int) std::log2((double) frameSize));
    juce::dsp::WindowingFunction<float> window((size_t) frameSize,
                                               juce::dsp::WindowingFunction<float>::hann);

    for (int start = 0; start + frameSize <= numSamples; start += hopSize)
    {
        std::vector<float> frame((size_t) frameSize * 2, 0.0f);

        for (int i = 0; i < frameSize; ++i)
            frame[(size_t) i] = ir[start + i];

        window.multiplyWithWindowingTable(frame.data(), (size_t) frameSize);
        fft.performRealOnlyForwardTransform(frame.data());

        std::vector<float> magnitudes((size_t) frameSize / 2 + 1);

        for (size_t k = 0; k < magnitudes.size(); ++k)
            magnitudes[k] = std::sqrt(frame[k * 2] * frame[k * 2] + frame[k * 2 + 1] * frame[k * 2 + 1]);

        std::vector<float> freqs(magnitudes.size());

        for (size_t k = 0; k < freqs.size(); ++k)
            freqs[k] = (float) k * sampleRate / (float) frameSize;

        auto magnitudesDb = std::vector<float>(magnitudes.size());

        for (size_t k = 0; k < magnitudes.size(); ++k)
            magnitudesDb[k] = dsp::db20(magnitudes[k]);

        result.framesDb.push_back(dsp::bandPeakDb(freqs, magnitudesDb, centres, 20.0f));
        result.times.push_back((float) start / sampleRate);
    }

    // Each row is normalised to its own peak: the waterfall shows how the balance
    // between bands decays, not the absolute level, which falls every row anyway.
    for (auto& frame : result.framesDb)
    {
        auto peak = dsp::dbFloor;

        for (const auto v : frame)
            peak = std::max(peak, v);

        for (auto& v : frame)
            v -= peak;
    }

    result.valid = !result.framesDb.empty();
    return result;
}

