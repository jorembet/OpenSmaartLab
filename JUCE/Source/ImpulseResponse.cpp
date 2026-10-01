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

    result.edt = t10.valid ? juce::jlimit(0.0f, 60.0f, -60.0f / t10.slope) : 0.0f;
    result.t20 = t20.valid ? juce::jlimit(0.0f, 60.0f, -20.0f / t20.slope) : 0.0f;
    result.t30 = t30.valid ? juce::jlimit(0.0f, 60.0f, -60.0f / t30.slope) : 0.0f;
    result.rt60 = t30.valid ? juce::jlimit(0.0f, 120.0f, -60.0f / t30.slope * 2.0f) : 0.0f;
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
