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
    const juce::Colour bandShadeColour = juce::Colour(0xff262c3a);

    constexpr int analysisSize = 4096;
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
    repaint();
}

void GeneratorDisplay::setGeneratorSettings(float newBandLow, float newBandHigh,
                                           float newToneFrequency,
                                           float newSweepStart, float newSweepEnd)
{
    bandLow = std::max(1.0f, newBandLow);
    bandHigh = std::max(bandLow, newBandHigh);
    toneFrequency = std::max(1.0f, newToneFrequency);
    sweepStart = std::max(1.0f, newSweepStart);
    sweepEnd = std::max(sweepStart, newSweepEnd);
    repaint();
}

void GeneratorDisplay::clear()
{
    samples.clear();
    spectrum.valid = false;
    peak = 0.0f;
    rms = 0.0f;
    slopeFrames = 0;
    slopeValue = 0.0f;
    repaint();
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
    updateSlope();
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

float GeneratorDisplay::probeLevelDb(float frequency) const
{
    if (!spectrum.valid)
        return dsp::dbFloor;

    dsp::smoothMagnitudeDb(spectrum.freq, spectrum.magnitudeDb, 3, slopeCurve);

    if (slopeCurve.size() != spectrum.magnitudeDb.size())
        return dsp::dbFloor;

    const auto index = juce::jlimit(0, (int) slopeCurve.size() - 1,
                                    juce::roundToInt(frequency * analysisSize / sampleRate));
    return slopeCurve[(size_t) index];
}

void GeneratorDisplay::updateSlope()
{
    const auto measured = measureSlope();

    // Averaged over the captured blocks. One snapshot of a narrow band cannot pin the
    // tilt to better than a decibel or two per octave, because the wiggle of a single
    // noise realisation survives third octave smoothing; a running mean converges as
    // soon as a few blocks have gone by.
    slopeValue = slopeFrames == 0 ? measured : slopeValue + 0.25f * (measured - slopeValue);
    ++slopeFrames;
}

float GeneratorDisplay::measureSlope() const
{
    if (!spectrum.valid)
        return 0.0f;

    dsp::smoothMagnitudeDb(spectrum.freq, spectrum.magnitudeDb, 3, slopeCurve);

    if (slopeCurve.size() != spectrum.magnitudeDb.size())
        return 0.0f;

    auto levelAt = [&] (float frequency)
    {
        const auto index = juce::jlimit(0, (int) slopeCurve.size() - 1,
                                        juce::roundToInt(frequency * analysisSize / sampleRate));
        return slopeCurve[(size_t) index];
    };

    // Two probe frequencies are not enough. One FFT bin of pink noise moves by several
    // dB between neighbours, and at the bottom of the range a third of an octave holds
    // only a handful of bins, so a pair reports the scatter of the estimate instead of
    // the tilt. Fitting every third octave of the configured band averages it out.
    const auto view = viewRange();
    auto from = view.low;
    auto to = view.high;

    if (type.contains("Pink") && bandHigh > bandLow)
    {
        // Inset by a sixth of an octave so the tapered band edges stay out of the fit.
        from = bandLow * std::pow(2.0f, 1.0f / 6.0f);
        to = bandHigh / std::pow(2.0f, 1.0f / 6.0f);
    }

    const auto step = std::pow(2.0f, 1.0f / 3.0f);
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    auto count = 0;

    for (auto frequency = from; frequency <= to * 1.001f; frequency *= step)
    {
        const auto level = levelAt(frequency);

        if (level <= dsp::dbFloor + 0.5f)
            continue;

        const auto x = std::log2(frequency);
        sumX += x;
        sumY += level;
        sumXX += x * x;
        sumXY += x * level;
        ++count;
    }

    const auto denominator = (double) count * sumXX - sumX * sumX;

    if (count < 3 || std::abs(denominator) < 1.0e-9)
        return 0.0f;

    return (float) (((double) count * sumXY - sumX * sumY) / denominator);
}

dsp::FrequencyRange GeneratorDisplay::viewRange() const
{
    return dsp::generatorViewRange(type, bandLow, bandHigh, toneFrequency, sweepStart, sweepEnd);
}

juce::String GeneratorDisplay::settingsLabel() const
{
    auto asFrequency = [] (float freq)
    {
        return freq >= 1000.0f ? dsp::frequencyTickLabel(freq) : juce::String(freq, 0) + " Hz";
    };

    if (type.contains("Sine"))
        return asFrequency(toneFrequency);

    if (type.contains("Sweep"))
        return asFrequency(sweepStart) + " - " + asFrequency(sweepEnd);

    if (type.contains("Pink"))
        return asFrequency(bandLow) + " - " + asFrequency(bandHigh);

    return "20 Hz - 20k";
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

    // Measured figures only mean something once a signal has been captured. Drawing
    // "peak -200 dBFS" on a stopped generator would only be noise, and it collides with
    // the output device name, which is the longest text in this row.
    if (samples.empty())
        return;

    const auto peakText = "peak " + juce::String(dsp::db20(peak), 1) + " dBFS";
    const auto rmsText = "rms " + juce::String(dsp::db20(rms), 1) + " dBFS";

    // Each signal gets the figure that describes it: a tilt for noise, a position for
    // a sweep and the frequency for a tone.
    const auto detail = type.contains("Sweep") ? "sweep " + juce::String((int) (sweepProgress * 100.0f)) + " %"
                         : type.contains("Sine") ? settingsLabel()
                         : juce::String(octaveSlope(), 1) + " dB/oct";

    g.drawText(peakText + "   " + rmsText + "   " + detail,
               area.reduced(14.0f).withTrimmedTop(28.0f).withHeight(24.0f),
               juce::Justification::right);
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
    const auto view = viewRange();
    const auto logLow = std::log10(view.low);
    const auto logSpan = std::log10(view.high) - logLow;

    auto toX = [&] (float freq)
    {
        return plot.getX() + (std::log10(juce::jlimit(view.low, view.high, freq)) - logLow)
                                        / logSpan * plot.getWidth();
    };

    g.setColour(gridColour);

    for (int division = 0; division <= 4; ++division)
    {
        const auto y = plot.getY() + plot.getHeight() * (float) division / 4.0f;
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.0f);
    }

    // Tick count follows the plot width so the labels cannot overlap on a narrow window.
    const auto ticks = dsp::logAxisTicks(view.low, view.high,
                                         std::max(2, (int) (plot.getWidth() / 62.0f)));

    g.setColour(gridBoldColour);

    for (const auto freq : ticks)
        g.fillRect(toX(freq), plot.getY(), 1.0f, plot.getHeight());

    // Shade the configured range, so the trace can be read against the setting the user
    // picked instead of guessing where the band edges are.
    if (type.contains("Pink"))
    {
        const auto left = toX(bandLow);
        const auto right = toX(bandHigh);

        if (right > left)
        {
            g.setColour(bandShadeColour);
            g.fillRect(left, plot.getY(), right - left, plot.getHeight());
        }
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(12.0f));
    g.drawText("Spektrum generator   " + settingsLabel(),
               plot.withY(plot.getY() - 22.0f).withHeight(20.0f),
               juce::Justification::left);

    if (!spectrum.valid)
        return;

    auto topDb = -20.0f;
    auto bottomDb = -110.0f;

    for (size_t i = 0; i < spectrum.freq.size(); ++i)
    {
        const auto freq = spectrum.freq[i];

        if (freq < view.low || freq > view.high)
            continue;

        const auto value = spectrum.magnitudeDb[i];

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

        if (freq < view.low)
            continue;

        if (freq > view.high)
            break;

        const auto x = toX(freq);
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

    // Sweep moves, so the tone that should be sounding right now gets a marker.
    if (type.contains("Sweep"))
    {
        const auto progress = juce::jlimit(0.0f, 1.0f, sweepProgress);
        const auto current = sweepStart * std::pow(sweepEnd / sweepStart, progress);

        if (current >= view.low && current <= view.high)
        {
            const auto x = toX(current);

            g.setColour(gridBoldColour);
            g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());

            g.setColour(textColour);
            g.setFont(juce::Font(11.0f));
            g.drawText(dsp::frequencyTickLabel(current),
                       juce::jlimit(plot.getX(), plot.getRight() - 40.0f, x - 20.0f),
                       plot.getY() + 2.0f, 40.0f, 14.0f, juce::Justification::centred);
        }
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(11.0f));

    for (const auto freq : ticks)
    {
        const auto x = toX(freq);
        g.drawText(dsp::frequencyTickLabel(freq),
                   juce::jlimit(plot.getX(), plot.getRight() - 40.0f, x - 20.0f),
                   plot.getBottom() + 2.0f, 40.0f, 14.0f,
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

    // The prompt belongs in the empty plot, not in the header, where the output device
    // name already runs the full width.
    if (!samples.empty())
        return;

    g.setColour(juce::Colours::white.withAlpha(0.45f));
    g.setFont(juce::Font(16.0f));
    g.drawText("Belum ada sinyal - nyalakan generator setelah audio berjalan",
               waveBounds.reduced(12.0f), juce::Justification::centred);
}

void GeneratorDisplay::resized()
{
    repaint();
}
