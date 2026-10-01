#include "SPLMeter.h"

namespace
{
    constexpr float meterMinDb = 20.0f;
    constexpr float meterMaxDb = 130.0f;
    constexpr float historyWindowSeconds = 30.0f;

    const juce::Colour backgroundColour = juce::Colour(0xff111318);
    const juce::Colour panelColour = juce::Colour(0xff1b1f27);
    const juce::Colour gridColour = juce::Colour(0xff2a2e38);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff8892a0);
    const juce::Colour channelOneColour = juce::Colour(0xff4fc3f7);
    const juce::Colour channelTwoColour = juce::Colour(0xff9ccc65);

    float meterPosition(float levelDb)
    {
        return juce::jlimit(0.0f, 1.0f, (levelDb - meterMinDb) / (meterMaxDb - meterMinDb));
    }

    juce::Colour levelColour(float levelDb)
    {
        if (levelDb >= 110.0f)
            return juce::Colour(0xffe53935);

        if (levelDb >= 90.0f)
            return juce::Colour(0xffffc107);

        return juce::Colour(0xff43a047);
    }
}

SPLMeter::SPLMeter()
{
    weightingSelector.addItem("A", 1);
    weightingSelector.addItem("C", 2);
    weightingSelector.addItem("Z", 3);
    weightingSelector.setSelectedId(1);
    weightingSelector.onChange = [this]
    {
        const auto id = weightingSelector.getSelectedId();
        weighting = id == 1 ? 'A' : (id == 2 ? 'C' : 'Z');
        reset();
    };

    responseSelector.addItem("Fast (125 ms)", 1);
    responseSelector.addItem("Slow (1 s)", 2);
    responseSelector.setSelectedId(1);
    responseSelector.onChange = [this]
    {
        fastResponse = responseSelector.getSelectedId() == 1;
        reset();
    };

    calibrationSlider.setRange(60.0, 140.0, 0.1);
    calibrationSlider.setValue(calibration);
    calibrationSlider.setTextValueSuffix(" dB");
    calibrationSlider.onValueChange = [this]
    {
        calibration = (float) calibrationSlider.getValue();
        reset();
    };

    calibrationLabel.setText("Kalibrasi", juce::dontSendNotification);
    calibrationLabel.setFont(juce::Font(13.0f));
    calibrationLabel.setColour(juce::Label::textColourId, mutedColour);

    infoLabel.setText("", juce::dontSendNotification);
    infoLabel.setFont(juce::Font(13.0f));
    infoLabel.setColour(juce::Label::textColourId, mutedColour);
    infoLabel.setJustificationType(juce::Justification::right);

    addAndMakeVisible(weightingSelector);
    addAndMakeVisible(responseSelector);
    addAndMakeVisible(calibrationLabel);
    addAndMakeVisible(calibrationSlider);
    addAndMakeVisible(resetButton);
    addAndMakeVisible(infoLabel);

    resetButton.onClick = [this] { reset(); };

    prepare(48000.0f, 4096);
}

SPLMeter::~SPLMeter() = default;

void SPLMeter::prepare(float sampleRate, int blockSize)
{
    currentSampleRate = juce::jmax(1000.0f, sampleRate);
    analyser.prepare(currentSampleRate, std::max(256, blockSize));

    analysisBlock = analyser.getFftSize();
    pendingRef.assign((size_t) analysisBlock, 0.0f);
    pendingMeas.assign((size_t) analysisBlock, 0.0f);
    pendingSamples = 0;

    reset();
}

void SPLMeter::reset()
{
    for (auto& channel : channels)
        channel = ChannelState();

    juce::ScopedLock lock(historyLock);
    historyRef.clear();
    historyMeas.clear();
    historyTime.clear();
}

void SPLMeter::process(const float* ref, const float* meas, int numSamples)
{
    if (ref == nullptr || meas == nullptr || numSamples <= 0 || analysisBlock <= 0)
        return;

    const auto blockDuration = (double) analysisBlock / (double) currentSampleRate;
    const auto fastAlpha = 1.0 - std::exp(-blockDuration / 0.125);
    const auto slowAlpha = 1.0 - std::exp(-blockDuration / 1.0);

    for (int i = 0; i < numSamples; ++i)
    {
        pendingRef[(size_t) pendingSamples] = ref[i];
        pendingMeas[(size_t) pendingSamples] = meas[i];
        ++pendingSamples;

        if (pendingSamples < analysisBlock)
            continue;

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto source = channel == 0 ? pendingRef.data() : pendingMeas.data();
            const auto weighted = analyser.levelDb(source, weighting);
            const auto weightedPeak = analyser.peakDb(source);
            const auto weightedEnergy = std::pow(10.0, (double) weighted / 10.0);
            auto& state = channels[channel];
            const auto firstBlock = state.leqBlocks == 0;

            state.level = weighted + calibration;

            if (!state.started)
            {
                state.fastEnergy = weightedEnergy;
                state.slowEnergy = weightedEnergy;
                state.started = true;
            }
            else
            {
                state.fastEnergy += fastAlpha * (weightedEnergy - state.fastEnergy);
                state.slowEnergy += slowAlpha * (weightedEnergy - state.slowEnergy);
            }

            state.fast = dsp::db10((float) state.fastEnergy) + calibration;
            state.slow = dsp::db10((float) state.slowEnergy) + calibration;
            state.peak = firstBlock ? state.level : std::max(state.peak, weightedPeak + calibration);

            state.leqEnergy += weightedEnergy;
            ++state.leqBlocks;
            state.leq = dsp::db10((float) (state.leqEnergy / std::max(1, state.leqBlocks))) + calibration;

            if (firstBlock)
            {
                state.minimum = state.level;
                state.maximum = state.level;
            }
            else
            {
                state.minimum = std::min(state.minimum, state.level);
                state.maximum = std::max(state.maximum, state.level);
            }
        }

        pendingSamples = 0;
    }

    const auto now = (float) juce::Time::getMillisecondCounterHiRes() / 1000.0f;

    juce::ScopedLock lock(historyLock);
    historyRef.push_back(channels[0].level);
    historyMeas.push_back(channels[1].level);
    historyTime.push_back(now);

    while (historyTime.size() > 2 && (now - historyTime.front()) > historyWindowSeconds)
    {
        historyRef.erase(historyRef.begin());
        historyMeas.erase(historyMeas.begin());
        historyTime.erase(historyTime.begin());
    }
}

float SPLMeter::getLevel(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].level;
}

float SPLMeter::getPeak(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].peak;
}

float SPLMeter::getLeq(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].leq;
}

float SPLMeter::getMinimum(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].minimum;
}

float SPLMeter::getMaximum(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].maximum;
}

void SPLMeter::resized()
{
    auto area = getLocalBounds().reduced(14);

    auto controls = area.removeFromTop(46);

    weightingSelector.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(8);
    responseSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(14);
    calibrationLabel.setBounds(controls.removeFromLeft(80).reduced(0, 12));
    controls.removeFromLeft(4);
    calibrationSlider.setBounds(controls.removeFromLeft(180).reduced(0, 8));
    controls.removeFromLeft(10);
    resetButton.setBounds(controls.removeFromLeft(80).reduced(0, 8));
    infoLabel.setBounds(controls.withSizeKeepingCentre(std::min(controls.getWidth(), 320), 20));

    infoLabel.setText("Block " + juce::String(analysisBlock)
                      + " smp @ " + juce::String((int) currentSampleRate) + " Hz",
                      juce::dontSendNotification);
}

void SPLMeter::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto area = getLocalBounds().toFloat().reduced(14.0f);
    area.removeFromTop(46.0f);

    const auto metersHeight = juce::jmin(300.0f, area.getHeight() * 0.5f);
    auto meters = area.removeFromTop(metersHeight * 2.0f + 12.0f);

    drawChannelMeter(g, meters.removeFromTop(metersHeight), 0, "Input / Channel 1");
    meters.removeFromTop(12.0f);
    drawChannelMeter(g, meters.removeFromTop(metersHeight), 1, "Measure / Channel 2");

    if (area.getHeight() > 60)
        drawHistory(g, area);
}

void SPLMeter::drawChannelMeter(juce::Graphics& g, const juce::Rectangle<float>& area,
                                int channel, const juce::String& title)
{
    const auto& state = channels[juce::jlimit(0, 1, channel)];
    const auto value = fastResponse ? state.fast : state.slow;

    g.setColour(panelColour);
    g.fillRoundedRectangle(area, 6.0f);

    auto inner = area.reduced(14.0f);
    const auto titleHeight = 20.0f;

    g.setFont(juce::Font(14.0f));
    g.setColour(mutedColour);
    g.drawText(title, inner.removeFromTop(titleHeight), juce::Justification::left);

    g.setFont(juce::Font(15.0f, juce::Font::bold));
    g.setColour(textColour);
    g.drawText("SPL " + juce::String(value, 1) + " dB",
               inner.removeFromTop(28), juce::Justification::left);

    g.setFont(juce::Font(12.0f));
    g.setColour(mutedColour);
    g.drawText("Peak " + juce::String(state.peak, 1)
                   + "   Leq " + juce::String(state.leq, 1)
                   + "   Lmin " + juce::String(state.minimum, 1)
                   + "   Lmax " + juce::String(state.maximum, 1),
               inner.removeFromTop(20), juce::Justification::left);

    auto bar = inner.removeFromTop(juce::jmax(18.0f, inner.getHeight() * 0.35f));
    const auto barArea = bar.withTrimmedLeft(60.0f);
    const auto scale = bar.removeFromLeft(60.0f);

    g.setFont(juce::Font(11.0f));
    g.setColour(mutedColour);

    for (int db = 30; db <= meterMaxDb; db += 20)
    {
        const auto x = barArea.getX() + meterPosition((float) db) * barArea.getWidth();

        g.setColour(gridColour);
        g.fillRect((float) x, barArea.getY(), 1.0f, barArea.getHeight());
        g.drawText(juce::String(db), scale.withX(x - 14.0f).withWidth(28.0f),
                   juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff101318));
    g.fillRoundedRectangle(barArea, 3.0f);

    const auto fillWidth = meterPosition(value) * barArea.getWidth();

    if (fillWidth > 1.0f)
    {
        const auto fill = barArea.withWidth(fillWidth);

        g.setColour(levelColour(value).withAlpha(0.85f));
        g.fillRoundedRectangle(fill, 3.0f);
    }

    const auto peakX = barArea.getX() + meterPosition(state.peak) * barArea.getWidth();

    if (state.peak > meterMinDb)
    {
        g.setColour(juce::Colours::white);
        g.fillRect((float) peakX - 1.0f, barArea.getY() - 2.0f, 2.0f, barArea.getHeight() + 4.0f);
    }

    g.setColour(gridColour);
    g.drawRoundedRectangle(barArea, 3.0f, 1.0f);
}

void SPLMeter::drawHistory(juce::Graphics& g, const juce::Rectangle<float>& area)
{
    g.setColour(panelColour);
    g.fillRoundedRectangle(area, 6.0f);

    const auto plot = area.reduced(14.0f).withTrimmedTop(24.0f);
    const auto now = (float) juce::Time::getMillisecondCounterHiRes() / 1000.0f;

    g.setFont(juce::Font(13.0f));
    g.setColour(mutedColour);
    g.drawText("History " + juce::String((int) historyWindowSeconds) + " detik",
               area.reduced(14.0f).removeFromTop(20), juce::Justification::left);

    juce::ScopedLock lock(historyLock);

    if (historyRef.size() < 2)
        return;

    g.setColour(gridColour);

    for (int db = 30; db <= meterMaxDb; db += 10)
    {
        const auto y = plot.getBottom() - meterPosition((float) db) * plot.getHeight();
        g.drawHorizontalLine((int) y, (int) plot.getX(), (int) plot.getRight());
    }

    const auto start = now - historyWindowSeconds;

    auto drawTrace = [&] (const std::vector<float>& values, const juce::Colour& colour, float thickness)
    {
        juce::Path path;

        for (size_t i = 0; i < values.size() && i < historyTime.size(); ++i)
        {
            const auto x = plot.getX() + (historyTime[i] - start) / historyWindowSeconds * plot.getWidth();
            const auto y = plot.getBottom() - meterPosition(values[i]) * plot.getHeight();

            if (i == 0)
                path.startNewSubPath(juce::jlimit(plot.getX(), plot.getRight(), x), y);
            else
                path.lineTo(juce::jlimit(plot.getX(), plot.getRight(), x), y);
        }

        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(thickness));
    };

    drawTrace(historyRef, channelOneColour, 2.0f);
    drawTrace(historyMeas, channelTwoColour.withAlpha(0.6f), 1.0f);
}
