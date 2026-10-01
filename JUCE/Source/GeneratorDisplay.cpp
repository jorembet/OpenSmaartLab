#include "GeneratorDisplay.h"
#include "DSP.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff111318);
    const juce::Colour panelColour = juce::Colour(0xff1b1f27);
    const juce::Colour gridColour = juce::Colour(0xff232833);
    const juce::Colour gridBoldColour = juce::Colour(0xff39404f);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff8892a0);
    const juce::Colour waveColour = juce::Colour(0xff4fc3f7);
    const juce::Colour spectrumColour = juce::Colour(0xffffb74d);
    const juce::Colour runningColour = juce::Colour(0xff43a047);
    const juce::Colour stoppedColour = juce::Colour(0xffe53935);

    constexpr int analysisSize = 4096;
    constexpr float minFrequency = 20.0f;
    constexpr float maxFrequency = 20000.0f;
}

GeneratorDisplay::GeneratorDisplay()
{
    scratch.assign((size_t) analysisSize * 2, 0.0f);
    fft = juce::dsp::FFT(juce::roundToInt(std::log2((double) analysisSize)));
    window.fillWindowingTables(analysisSize, juce::dsp::WindowingFunction<float>::hann, false);

    std::vector<float> probe((size_t) analysisSize, 1.0f);
    window.multiplyWithWindowingTable(probe.data(), analysisSize);

    double total = 0.0;

    for (auto value : probe)
        total += value;

    coherentGain = (float) (total / analysisSize);

    setInterceptsMouseClicks(false, false);
}

GeneratorDisplay::~GeneratorDisplay() = default;

void GeneratorDisplay::setRunning(bool shouldRun)
{
    if (running != shouldRun)
    {
        running = shouldRun;
        repaint();
    }
}

void GeneratorDisplay::setSignal(const juce::String& newType, float newLevelDb,
                                 float newSweepProgress, const juce::String& device)
{
    type = newType;
    levelDb = newLevelDb;
    sweepProgress = newSweepProgress;
    outputDevice = device;
}

void GeneratorDisplay::setSamples(const std::vector<float>& newSamples, float newSampleRate)
{
    samples = newSamples;
    sampleRate = std::max(1000.0f, newSampleRate);

    peak = 0.0f;
    double sum = 0.0;

    for (auto value : samples)
    {
        peak = std::max(peak, std::abs(value));
        sum += (double) value * value;
    }

    rms = samples.empty() ? 0.0f : (float) std::sqrt(sum / samples.size());

    analyse();
    repaint();
}

void GeneratorDisplay::analyse()
{
    spectrum.valid = false;

    if ((int) samples.size() < 64)
        return;

    const auto count = std::min(analysisSize, (int) samples.size());
    const auto offset = std::max(0, (int) samples.size() - count);

    juce::FloatVectorOperations::copy(scratch.data(), samples.data() + offset, count);
    juce::FloatVectorOperations::clear(scratch.data() + count, analysisSize - count);
    window.multiplyWithWindowingTable(scratch.data(), analysisSize);

    fft.performRealOnlyForwardTransform(scratch.data(), true);

    const int bins = analysisSize / 2 + 1;
    const auto normalise = 2.0f / ((float) analysisSize * std::max(coherentGain, 1.0e-6f));

    spectrum.freq.resize((size_t) bins);
    spectrum.magnitudeDb.resize((size_t) bins);
    spectrum.sampleRate = sampleRate;

    for (int i = 0; i < bins; ++i)
    {
        const auto re = scratch[(size_t) i * 2];
        const auto im = scratch[(size_t) i * 2 + 1];

        auto amplitude = std::sqrt(re * re + im * im) * normalise;

        if (i == 0 || i == bins - 1)
            amplitude *= 0.5f;

        spectrum.freq[(size_t) i] = (float) i * sampleRate / analysisSize;
        spectrum.magnitudeDb[(size_t) i] = dsp::db20(amplitude);
    }

    spectrum.valid = true;
}

float GeneratorDisplay::octaveSlope() const
{
    if (!spectrum.valid)
        return 0.0f;

    auto levelAt = [&] (float freq)
    {
        const auto index = juce::jlimit(1, (int) spectrum.freq.size() - 1,
                                        juce::roundToInt(freq * analysisSize / sampleRate));
        return spectrum.magnitudeDb[(size_t) index];
    };

    const auto low = levelAt(250.0f);
    const auto high = levelAt(4000.0f);

    return (high - low) / 4.0f;
}

juce::Rectangle<float> GeneratorDisplay::waveformArea() const
{
    auto area = getLocalBounds().toFloat().reduced(14.0f);
    area.removeFromTop(64.0f);

    const auto top = area.removeFromTop(area.getHeight() * 0.45f);
    area.removeFromTop(12.0f);

    return top.reduced(12.0f).withTrimmedTop(24.0f);
}

juce::Rectangle<float> GeneratorDisplay::spectrumArea() const
{
    auto area = getLocalBounds().toFloat().reduced(14.0f);
    area.removeFromTop(64.0f);
    area.removeFromTop(area.getHeight() * 0.45f + 12.0f);

    return area.reduced(12.0f).withTrimmedTop(24.0f);
}

void GeneratorDisplay::drawHeader(juce::Graphics& g, const juce::Rectangle<float>& area) const
{
    g.setColour(panelColour);
    g.fillRoundedRectangle(area, 6.0f);

    g.setFont(juce::Font(15.0f, juce::Font::bold));
    g.setColour(running ? runningColour : stoppedColour);
    g.drawText(running ? "GENERATOR AKTIF" : "GENERATOR MATI",
               area.reduced(14.0f).removeFromTop(24), juce::Justification::left);

    g.setFont(juce::Font(14.0f));
    g.setColour(mutedColour);
    g.drawText(type + "   " + juce::String(levelDb, 1) + " dBFS   output: "
                   + (outputDevice.isEmpty() ? juce::String("-") : outputDevice),
               area.reduced(14.0f).withTrimmedTop(28.0f).withHeight(24.0f),
               juce::Justification::left);

    const auto peakText = "peak " + juce::String(dsp::db20(peak), 1) + " dBFS";
    const auto rmsText = "rms " + juce::String(dsp::db20(rms), 1) + " dBFS";
    const auto slopeText = spectrum.valid && type.contains("Pink")
                         ? juce::String(octaveSlope(), 1) + " dB/oct"
                         : (type.contains("Sweep") ? "sweep " + juce::String((int) (sweepProgress * 100.0f)) + " %"
                                                   : juce::String(spectrum.valid ? octaveSlope() : 0.0f, 1) + " dB/oct");

    g.drawText(peakText + "   " + rmsText + "   " + slopeText,
               area.reduced(14.0f).withTrimmedTop(28.0f).withHeight(24.0f),
               juce::Justification::right);

    if (!samples.empty())
        return;

    g.setColour(juce::Colours::white.withAlpha(0.45f));
    g.setFont(juce::Font(16.0f));
    g.drawText("Belum ada sinyal - nyalakan generator setelah audio berjalan",
               area.reduced(14.0f).withTrimmedTop(24.0f), juce::Justification::centred);
}

void GeneratorDisplay::drawWaveform(juce::Graphics& g, const juce::Rectangle<float>& plot) const
{
    g.setColour(gridColour);

    for (int division = 0; division <= 4; ++division)
    {
        const auto y = plot.getY() + plot.getHeight() * (float) division / 4.0f;
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.0f);
    }

    const auto centre = plot.getCentreY();

    g.setColour(gridBoldColour);
    g.fillRect(plot.getX(), centre, plot.getWidth(), 1.0f);

    if (samples.size() < 4)
        return;

    const auto scale = peak > 1.0e-9f ? plot.getHeight() * 0.45f / peak : 0.0f;

    juce::Path path;
    const auto count = (int) samples.size();

    for (int i = 0; i < count; ++i)
    {
        const auto x = plot.getX() + plot.getWidth() * (float) i / (float) count;
        const auto y = centre - samples[(size_t) i] * scale;

        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(waveColour);
    g.strokePath(path, juce::PathStrokeType(1.0f));

    g.setColour(mutedColour);
    g.setFont(juce::Font(12.0f));
    g.drawText("Waveform " + juce::String((int) (samples.size() / std::max(1.0f, sampleRate) * 1000.0f))
                   + " ms terakhir",
               plot.withY(plot.getY() - 22.0f).withHeight(20.0f),
               juce::Justification::left);
}

void GeneratorDisplay::drawSpectrum(juce::Graphics& g, const juce::Rectangle<float>& plot) const
{
    g.setColour(gridColour);

    for (int division = 0; division <= 4; ++division)
    {
        const auto y = plot.getY() + plot.getHeight() * (float) division / 4.0f;
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.0f);
    }

    for (int decade = 1; decade <= 4; ++decade)
    {
        const auto freq = std::pow(10.0f, (float) decade);
        const auto x = plot.getX() + (std::log10(freq) - std::log10(minFrequency))
                                        / (std::log10(maxFrequency) - std::log10(minFrequency))
                                        * plot.getWidth();

        g.setColour(gridBoldColour);
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(12.0f));
    g.drawText("Spektrum generator", plot.withY(plot.getY() - 22.0f).withHeight(20.0f),
               juce::Justification::left);

    if (!spectrum.valid)
        return;

    auto topDb = -20.0f;
    auto bottomDb = -110.0f;

    for (auto value : spectrum.magnitudeDb)
    {
        if (value > topDb)
            topDb = value;

        if (value < bottomDb && value > -160.0f)
            bottomDb = value;
    }

    bottomDb = std::max(bottomDb, topDb - 90.0f);

    juce::Path path;
    bool started = false;

    for (size_t i = 0; i < spectrum.freq.size(); ++i)
    {
        const auto freq = spectrum.freq[i];

        if (freq < minFrequency)
            continue;

        if (freq > maxFrequency)
            break;

        const auto x = plot.getX() + (std::log10(freq) - std::log10(minFrequency))
                                        / (std::log10(maxFrequency) - std::log10(minFrequency))
                                        * plot.getWidth();
        const auto fraction = juce::jlimit(0.0f, 1.0f, (spectrum.magnitudeDb[i] - bottomDb) / (topDb - bottomDb));
        const auto y = plot.getBottom() - fraction * plot.getHeight();

        if (!started)
        {
            path.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            path.lineTo(x, y);
        }
    }

    g.setColour(spectrumColour);
    g.strokePath(path, juce::PathStrokeType(1.5f));

    for (int decade = 1; decade <= 4; ++decade)
    {
        const auto freq = std::pow(10.0f, (float) decade);
        const auto x = plot.getX() + (std::log10(freq) - std::log10(minFrequency))
                                        / (std::log10(maxFrequency) - std::log10(minFrequency))
                                        * plot.getWidth();

        g.setColour(mutedColour);
        g.setFont(juce::Font(11.0f));
        g.drawText(decade == 3 ? "1k" : (juce::String(freq >= 1000.0f ? (int) freq / 1000 : (int) freq)
                                         + (decade == 4 ? "k" : "")),
                   x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f,
                   juce::Justification::centred);
    }

    g.drawText(juce::String((int) topDb) + " dB", plot.getX() - 2.0f, plot.getY() - 6.0f,
               60.0f, 14.0f, juce::Justification::centredLeft);
    g.drawText(juce::String((int) bottomDb) + " dB", plot.getX() - 2.0f, plot.getBottom() - 8.0f,
               60.0f, 14.0f, juce::Justification::centredLeft);
}

void GeneratorDisplay::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto area = getLocalBounds().toFloat().reduced(14.0f);

    drawHeader(g, area.removeFromTop(64.0f));
    area.removeFromTop(12.0f);

    g.setColour(panelColour);

    const auto waveBounds = waveformArea();
    const auto spectrumBounds = spectrumArea();

    g.fillRoundedRectangle(waveBounds.withTrimmedTop(-24.0f).withTrimmedBottom(-8.0f), 6.0f);
    g.fillRoundedRectangle(spectrumBounds.withTrimmedTop(-24.0f).withTrimmedBottom(-22.0f), 6.0f);

    drawWaveform(g, waveBounds);
    drawSpectrum(g, spectrumBounds);
}

void GeneratorDisplay::resized()
{
    repaint();
}
