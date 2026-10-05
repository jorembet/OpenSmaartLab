#include "SPLMeter.h"

namespace
{
    constexpr float splMeterMinDb = 20.0f;
    constexpr float splMeterMaxDb = 130.0f;
    constexpr float dbfsMeterMinDb = -60.0f;
    constexpr float dbfsMeterMaxDb = 0.0f;
    constexpr float historyWindowSeconds = 30.0f;
    constexpr float clipThreshold = 0.9995f;
    constexpr float clipPlateauTolerance = 1.0e-7f;
    constexpr int clipRunSamples = 3;
    constexpr float clipHoldSeconds = 1.0f;

    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff404040);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour channelOneColour = juce::Colour(0xff4fc3f7);
    const juce::Colour channelTwoColour = juce::Colour(0xff9ccc65);
    const juce::Colour clipColour = juce::Colour(0xffe53935);

    float meterPosition (float levelDb, bool splScale)
    {
        const auto minimum = splScale ? splMeterMinDb : dbfsMeterMinDb;
        const auto maximum = splScale ? splMeterMaxDb : dbfsMeterMaxDb;
        return juce::jlimit (0.0f, 1.0f, (levelDb - minimum) / (maximum - minimum));
    }

    juce::Colour levelColour (float levelDb, bool splScale)
    {
        if (splScale)
        {
            if (levelDb >= 110.0f)
                return juce::Colour (0xffe53935);

            if (levelDb >= 90.0f)
                return juce::Colour (0xffffc107);

            return juce::Colour (0xff43a047);
        }

        if (levelDb >= -3.0f)
            return juce::Colour (0xffe53935);

        if (levelDb >= -12.0f)
            return juce::Colour (0xffffc107);

        return juce::Colour (0xff43a047);
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

    // Impulse is offered alongside the other two because a peak reading wants the fastest
    // response that still catches a transient, and making the reader give up the two general
    // time constants to have it would be the wrong trade.
    responseSelector.addItem("Impulse (35 ms)", 3);
    responseSelector.setSelectedId(1);
    responseSelector.onChange = [this]
    {
        const auto id = responseSelector.getSelectedId();
        fastResponse = id == 1;

        const auto timeWeighting = id == 3 ? dsp::SplAnalyser::TimeWeighting::Impulse
                               : id == 2 ? dsp::SplAnalyser::TimeWeighting::Slow
                                         : dsp::SplAnalyser::TimeWeighting::Fast;

        for (auto& engine : analysers)
            engine.setTimeWeighting (timeWeighting);

        reset();
    };

    calibrationLabel.setText ("Belum dikalibrasi - dBFS", juce::dontSendNotification);
    calibrationLabel.setFont(juce::Font(13.0f));
    calibrationLabel.setColour(juce::Label::textColourId, mutedColour);
    calibrationLabel.setTooltip ("SPL ditampilkan setelah mikrofon dikalibrasi dengan acuan; sebelum itu meter memakai dBFS.");
    calibrationButton.setTooltip (
        "Cara: pasang acoustic calibrator pada mikrofon, nyalakan pada level tertera (mis. 94 dB SPL), "
        "tunggu pembacaan stabil, lalu klik untuk menyimpan kalibrasi.");
    calibrationButton.onClick = [this]
    {
        if (onCalibrationRequested)
            onCalibrationRequested();
    };

    infoLabel.setText("", juce::dontSendNotification);
    infoLabel.setFont(juce::Font(13.0f));
    infoLabel.setColour(juce::Label::textColourId, mutedColour);
    infoLabel.setJustificationType(juce::Justification::right);

    addAndMakeVisible(weightingSelector);
    addAndMakeVisible(responseSelector);
    addAndMakeVisible(calibrationButton);
    addAndMakeVisible(calibrationLabel);
    addAndMakeVisible(resetButton);
    addAndMakeVisible(infoLabel);

    resetButton.onClick = [this] { reset(); };

    prepare(48000.0f, 4096);
}

SPLMeter::~SPLMeter() = default;

void SPLMeter::prepare(float sampleRate, int blockSize)
{
    currentSampleRate = juce::jmax(1000.0f, sampleRate);
    for (auto& engine : analysers)
        engine.prepare (currentSampleRate);

    // The engine blocks the audio itself, so the outer accumulation matches its block rather
    // than whatever the caller asked for. That keeps the response time a property of the
    // weighting constant instead of of the device buffer size, which the user can change while
    // the meter runs.
    analysisBlock = dsp::SplAnalyser::blockSize;
    pendingRef.assign((size_t) analysisBlock, 0.0f);
    pendingMeas.assign((size_t) analysisBlock, 0.0f);
    pendingRawRef.assign((size_t) analysisBlock, 0.0f);
    pendingRawMeas.assign((size_t) analysisBlock, 0.0f);
    pendingSamples = 0;

    reset();
}

void SPLMeter::reset()
{
    pendingSamples = 0;

    for (auto& channel : channels)
        channel = ChannelState();

    for (auto& engine : analysers)
        engine.reset();

    juce::ScopedLock lock(historyLock);
    historyRef.clear();
    historyMeas.clear();
    historyPeakRef.clear();
    historyPeakMeas.clear();
    historyTime.clear();
    clipEvents.clear();
}

void SPLMeter::process (const float* ref, const float* meas, int numSamples,
                        float inputGainDb)
{
    if (ref == nullptr || meas == nullptr || numSamples <= 0 || analysisBlock <= 0)
        return;

    const auto clampedInputGainDb = juce::jlimit (-60.0f, 24.0f, inputGainDb);
    if (clampedInputGainDb != lastInputGainDb)
        analysers[1].reset();

    lastInputGainDb = clampedInputGainDb;
    const auto gain = juce::Decibels::decibelsToGain (lastInputGainDb);
    float lastBlockPeak[2] = { channels[0].level, channels[1].level };
    bool newClipEvent[2] = { false, false };

    for (int i = 0; i < numSamples; ++i)
    {
        pendingRef[(size_t) pendingSamples] = ref[i];
        pendingMeas[(size_t) pendingSamples] = meas[i] * gain;
        pendingRawRef[(size_t) pendingSamples] = ref[i];
        pendingRawMeas[(size_t) pendingSamples] = meas[i];
        ++pendingSamples;

        if (pendingSamples < analysisBlock)
            continue;

        float blockPeak[2] = { -200.0f, -200.0f };

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto source = channel == 0 ? pendingRef.data() : pendingMeas.data();
            const auto rawSource = channel == 0 ? pendingRawRef.data() : pendingRawMeas.data();
            auto& engine = analysers[channel];
            auto& state = channels[channel];
            engine.process (source, analysisBlock);

            const auto rating = ratingOf (weighting);
            const auto weighted = engine.getLevel (rating);
            // Input trim applies to the microphone channel only. The reference is often a
            // generator/loopback signal and must keep its original level.
            // Only the microphone can become dB SPL. The reference/output stays dBFS even
            // when a microphone calibration is active; a calibration must never change it.
            const auto displayOffsetDb = channel == 1 && calibrated
                                       ? calibrationOffsetDb - lastInputGainDb
                                       : 0.0f;
            blockPeak[channel] = engine.getPeak (rating) + displayOffsetDb;

            const auto blockClip = hasHardClip (rawSource, analysisBlock, state);
            const auto firstBlock = state.leqBlocks == 0;

            if (blockClip)
            {
                if (! state.clipLatched)
                {
                    ++state.clipCount;
                    newClipEvent[channel] = true;
                }

                state.clipLatched = true;
                state.clipHoldSamplesRemaining = (int) (currentSampleRate * clipHoldSeconds);
                state.clipReleaseSamplesRemaining = (int) (currentSampleRate * 0.25f);
                state.clipWindowPeakDbfs = std::max (state.clipWindowPeakDbfs,
                                                      state.samplePeakDbfs);
            }
            else
            {
                state.clipReleaseSamplesRemaining = juce::jmax (0,
                    state.clipReleaseSamplesRemaining - analysisBlock);

                if (state.clipReleaseSamplesRemaining == 0 && state.nearFullScaleRun == 0)
                    state.clipLatched = false;

                state.clipHoldSamplesRemaining = juce::jmax (0,
                    state.clipHoldSamplesRemaining - analysisBlock);

                if (state.clipHoldSamplesRemaining == 0)
                    state.clipWindowPeakDbfs = dsp::dbFloor;
                else
                    state.clipWindowPeakDbfs = std::max (state.clipWindowPeakDbfs,
                                                          state.samplePeakDbfs);
            }

            // Both weightings are inside the engine now, and both are applied to energy rather
            // than to decibels: averaging dB values is not the same as averaging sound energy,
            // and the gap widens with the spread of the levels being averaged.
            //
            // The calibration is added afterwards, not inside, because it belongs to the
            // microphone rather than to the sound. Folded in it would be integrated once per
            // block and the equivalent level would carry it cumulatively.
            state.level = weighted + displayOffsetDb;
            state.fast = state.level;
            state.slow = state.level;
            state.peak = firstBlock ? state.level : std::max (state.peak, blockPeak[channel]);
            state.leq = engine.getLeq (rating) + displayOffsetDb;
            ++state.leqBlocks;
            state.started = true;

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

        lastBlockPeak[0] = blockPeak[0];
        lastBlockPeak[1] = blockPeak[1];
        pendingSamples = 0;
    }

    const auto now = (float) juce::Time::getMillisecondCounterHiRes() / 1000.0f;

    juce::ScopedLock lock(historyLock);
    historyRef.push_back(channels[0].level);
    historyMeas.push_back(channels[1].level);
    historyPeakRef.push_back(lastBlockPeak[0]);
    historyPeakMeas.push_back(lastBlockPeak[1]);

    for (int channel = 0; channel < 2; ++channel)
        if (newClipEvent[channel])
            clipEvents.emplace_back(now, channel);

    historyTime.push_back(now);

    while (historyTime.size() > 2 && (now - historyTime.front()) > historyWindowSeconds)
    {
        historyRef.erase(historyRef.begin());
        historyMeas.erase(historyMeas.begin());
        historyPeakRef.erase(historyPeakRef.begin());
        historyPeakMeas.erase(historyPeakMeas.begin());
        historyTime.erase(historyTime.begin());
    }

    while (!clipEvents.empty() && (now - clipEvents.front().first) > historyWindowSeconds)
        clipEvents.erase(clipEvents.begin());

    repaint();
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

int SPLMeter::getClipCount(int channel) const
{
    return channels[juce::jlimit(0, 1, channel)].clipCount;
}

bool SPLMeter::isClipActive (int channel) const
{
    return channels[juce::jlimit (0, 1, channel)].clipHoldSamplesRemaining > 0;
}

float SPLMeter::getSamplePeakDbfs (int channel) const
{
    return channels[juce::jlimit (0, 1, channel)].samplePeakDbfs;
}

float SPLMeter::getClipPeakDbfs (int channel) const
{
    return channels[juce::jlimit (0, 1, channel)].clipWindowPeakDbfs;
}

float SPLMeter::getMeasuredMicLevelDbfs() const
{
    if (! channels[1].started)
        return dsp::dbFloor;

    // The analyser includes the software input trim, but a calibration offset describes the
    // microphone chain before that user control. Remove it here so calibration stays correct
    // if the analysis trim changes later.
    return analysers[1].getLevel (ratingOf (weighting)) - lastInputGainDb;
}

bool SPLMeter::hasHardClip (const float* samples, int count, ChannelState& state)
{
    if (samples == nullptr || count <= 0)
        return false;

    bool clipped = false;
    float samplePeak = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        const auto sample = samples[i];

        if (! std::isfinite (sample))
        {
            state.nearFullScaleRun = 0;
            state.nearFullScalePolarity = 0;
            state.nearFullScaleSample = 0.0f;
            continue;
        }

        samplePeak = std::max (samplePeak, std::abs (sample));

        if (std::abs (sample) > 1.0f)
        {
            clipped = true;
            state.nearFullScaleRun = 0;
            state.nearFullScalePolarity = 0;
            state.nearFullScaleSample = 0.0f;
            continue;
        }

        const auto polarity = sample >= 0.0f ? 1 : -1;

        if (std::abs (sample) >= clipThreshold)
        {
            // A loud, clean sine can briefly sit extremely close to full scale. Count a
            // clip only when the converter has flattened the waveform into repeated equal
            // rail samples, rather than treating any run near the peak as a clip.
            if (polarity == state.nearFullScalePolarity
                && std::abs (sample - state.nearFullScaleSample) <= clipPlateauTolerance)
                ++state.nearFullScaleRun;
            else
            {
                state.nearFullScalePolarity = polarity;
                state.nearFullScaleRun = 1;
            }

            state.nearFullScaleSample = sample;

            if (state.nearFullScaleRun >= clipRunSamples)
                clipped = true;
        }
        else
        {
            state.nearFullScaleRun = 0;
            state.nearFullScalePolarity = 0;
            state.nearFullScaleSample = 0.0f;
        }
    }

    state.samplePeakDbfs = samplePeak > 0.0f
                         ? 20.0f * std::log10 (samplePeak)
                         : dsp::dbFloor;

    return clipped;
}

void SPLMeter::resized()
{
    auto area = getLocalBounds().reduced(14);

    auto controls = area.removeFromTop(46);

    weightingSelector.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(8);
    responseSelector.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(14);
    calibrationButton.setBounds (controls.removeFromLeft (130));
    controls.removeFromLeft (8);
    calibrationLabel.setBounds (controls.removeFromLeft (210).reduced (0, 12));
    controls.removeFromLeft (8);
    resetButton.setBounds (controls.removeFromRight (80).reduced (0, 8));
    controls.removeFromRight (8);
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

    drawChannelMeter(g, meters.removeFromTop(metersHeight), 0, "Reference / Output");
    meters.removeFromTop(12.0f);
    drawChannelMeter(g, meters.removeFromTop(metersHeight), 1, "Measurement / Mic");

    if (area.getHeight() > 60)
        drawHistory(g, area);
}

void SPLMeter::drawChannelMeter(juce::Graphics& g, const juce::Rectangle<float>& area,
                                int channel, const juce::String& title)
{
    const auto& state = channels[juce::jlimit(0, 1, channel)];
    const auto value = fastResponse ? state.fast : state.slow;
    const auto splScale = calibrated && channel == 1;
    const auto unit = splScale ? juce::String ("dB SPL") : juce::String ("dBFS");

    g.setColour(plotColour);
    g.fillRoundedRectangle(area, 6.0f);

    auto inner = area.reduced(14.0f);
    const auto titleHeight = 20.0f;

    g.setFont(juce::Font(14.0f));
    g.setColour(mutedColour);
    g.drawText(title, inner.removeFromTop(titleHeight), juce::Justification::left);

    g.setFont(juce::Font(15.0f, juce::Font::bold));
    g.setColour(textColour);
    const auto weightingName = juce::String::charToString (weighting);
    const auto levelTitle = splScale ? "L" + weightingName + " "
                                     : weightingName + "-weighted ";
    const auto valueText = state.started
                         ? levelTitle + juce::String (value, 1) + " " + unit
                         : juce::String ("Menunggu audio");
    g.drawText (valueText,
               inner.removeFromTop(28), juce::Justification::left);

    g.setFont(juce::Font(12.0f));
    g.setColour(mutedColour);
    const auto statsText = state.started
                         ? "Peak " + juce::String(state.peak, 1)
                               + "   Leq " + juce::String(state.leq, 1)
                               + "   Lmin " + juce::String(state.minimum, 1)
                               + "   Lmax " + juce::String(state.maximum, 1) + " " + unit
                         : juce::String ("Peak --   Leq --   Lmin --   Lmax --");
    g.drawText(statsText,
               inner.removeFromTop(20), juce::Justification::left);

    // Clipping is counted, not just flagged: one blown sample is a different problem
    // from a level that sat on the ceiling for a second.
    const auto clipActive = isClipActive (channel);
    const auto peakToShow = clipActive ? state.clipWindowPeakDbfs : state.samplePeakDbfs;
    const auto samplePeak = state.started
                          ? "Puncak sampel " + juce::String (peakToShow, 1) + " dBFS"
                          : juce::String ("Puncak sampel -- dBFS");
    const auto clipStatus = ! state.started ? juce::String ("Menunggu audio")
                           : clipActive ? "CLIP aktif - turunkan gain input | " + samplePeak
                                                    + " | kejadian "
                                                    + juce::String (state.clipCount)
                         : state.clipCount > 0 ? "Tanpa clip aktif — " + samplePeak
                                                   + " | total " + juce::String (state.clipCount)
                                               : samplePeak + " | tanpa clipping";
    g.setColour(clipActive ? clipColour : mutedColour);
    g.setFont(juce::Font(12.0f, clipActive ? juce::Font::bold : juce::Font::plain));
    g.drawText(clipStatus,
               inner.removeFromTop(18), juce::Justification::left);

    auto bar = inner.removeFromTop(juce::jmax(18.0f, inner.getHeight() * 0.35f));
    const auto barArea = bar.withTrimmedLeft(60.0f);
    const auto scale = bar.removeFromLeft(60.0f);

    g.setFont(juce::Font(11.0f));
    g.setColour(mutedColour);

    const auto tickStart = splScale ? (int) splMeterMinDb : (int) dbfsMeterMinDb;
    const auto tickEnd = splScale ? (int) splMeterMaxDb : (int) dbfsMeterMaxDb;
    const auto tickStep = splScale ? 20 : 10;

    for (int db = tickStart; db <= tickEnd; db += tickStep)
    {
        const auto x = barArea.getX() + meterPosition ((float) db, splScale) * barArea.getWidth();

        g.setColour(gridColour);
        g.fillRect((float) x, barArea.getY(), 1.0f, barArea.getHeight());
        g.drawText(juce::String(db), scale.withX(x - 14.0f).withWidth(28.0f),
                   juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff0a0a0a));
    g.fillRoundedRectangle(barArea, 3.0f);

    const auto fillWidth = state.started ? meterPosition (value, splScale) * barArea.getWidth()
                                         : 0.0f;

    if (fillWidth > 1.0f)
    {
        const auto fill = barArea.withWidth(fillWidth);

        g.setColour (levelColour (value, splScale).withAlpha (0.85f));
        g.fillRoundedRectangle(fill, 3.0f);
    }

    const auto peakX = barArea.getX() + meterPosition (state.peak, splScale) * barArea.getWidth();

    const auto scaleFloorDb = splScale ? splMeterMinDb : dbfsMeterMinDb;
    if (state.started && state.peak > scaleFloorDb)
    {
        g.setColour(juce::Colours::white);
        g.fillRect((float) peakX - 1.0f, barArea.getY() - 2.0f, 2.0f, barArea.getHeight() + 4.0f);
    }

    g.setColour(gridColour);
    g.drawRoundedRectangle(barArea, 3.0f, 1.0f);
}

void SPLMeter::drawHistory(juce::Graphics& g, const juce::Rectangle<float>& area)
{
    g.setColour(plotColour);
    g.fillRoundedRectangle(area, 6.0f);

    auto chart = area.reduced (14.0f);
    const auto now = (float) juce::Time::getMillisecondCounterHiRes() / 1000.0f;

    g.setFont(juce::Font(13.0f));
    g.setColour(mutedColour);
    g.drawText ("History " + juce::String ((int) historyWindowSeconds)
                    + " detik | garis terang = peak, titik merah = clipping",
                chart.removeFromTop (20), juce::Justification::left);

    juce::ScopedLock lock(historyLock);

    if (historyRef.size() < 2 || (! channels[0].started && ! channels[1].started))
        return;

    const auto start = now - historyWindowSeconds;
    chart.removeFromTop (4.0f);
    const auto rowGap = 6.0f;
    const auto rowHeight = juce::jmax (1.0f, (chart.getHeight() - rowGap) * 0.5f);

    for (int channel = 0; channel < 2; ++channel)
    {
        auto row = chart.removeFromTop (rowHeight);
        if (channel == 0)
            chart.removeFromTop (rowGap);

        const auto splScale = calibrated && channel == 1;
        auto title = row.removeFromTop (16.0f);
        g.setFont (juce::Font (11.0f));
        g.setColour (mutedColour);
        g.drawText (channel == 0 ? "Reference / Output - dBFS"
                                 : splScale ? "Measurement / Mic - dB SPL"
                                            : "Measurement / Mic - dBFS",
                    title, juce::Justification::left);

        auto valueArea = row;
        auto plot = valueArea.withTrimmedLeft (38.0f);
        const auto tickStart = splScale ? (int) splMeterMinDb : (int) dbfsMeterMinDb;
        const auto tickEnd = splScale ? (int) splMeterMaxDb : (int) dbfsMeterMaxDb;
        const auto tickStep = splScale ? 20 : 10;

        for (int db = tickStart; db <= tickEnd; db += tickStep)
        {
            const auto y = plot.getBottom() - meterPosition ((float) db, splScale) * plot.getHeight();
            g.setColour (gridColour);
            g.drawHorizontalLine ((int) y, (int) plot.getX(), (int) plot.getRight());
            g.setFont (juce::Font (10.0f));
            g.setColour (mutedColour);
            g.drawText (juce::String (db), valueArea.withY (y - 7.0f).withHeight (14.0f)
                                               .withWidth (34.0f),
                        juce::Justification::centredRight);
        }

        auto drawTrace = [&] (const std::vector<float>& values, const juce::Colour& colour,
                              float thickness)
        {
            juce::Path path;

            for (size_t i = 0; i < values.size() && i < historyTime.size(); ++i)
            {
                const auto x = plot.getX() + (historyTime[i] - start)
                                             / historyWindowSeconds * plot.getWidth();
                const auto y = plot.getBottom() - meterPosition (values[i], splScale)
                                                 * plot.getHeight();
                const auto boundedX = juce::jlimit (plot.getX(), plot.getRight(), x);

                if (i == 0)
                    path.startNewSubPath (boundedX, y);
                else
                    path.lineTo (boundedX, y);
            }

            g.setColour (colour);
            g.strokePath (path, juce::PathStrokeType (thickness));
        };

        if (channel == 0)
        {
            drawTrace (historyRef, channelOneColour, 2.0f);
            drawTrace (historyPeakRef, juce::Colours::white.withAlpha (0.8f), 1.0f);
        }
        else
        {
            drawTrace (historyMeas, channelTwoColour.withAlpha (0.8f), 2.0f);
            drawTrace (historyPeakMeas, channelTwoColour.brighter(), 1.0f);
        }

        // A clip is an instant, not a level: keep its marker in the corresponding
        // channel row so the two different unit scales remain readable.
        g.setColour (clipColour);
        for (const auto& [time, eventChannel] : clipEvents)
        {
            if (eventChannel != channel)
                continue;

            const auto x = plot.getX() + (time - start) / historyWindowSeconds * plot.getWidth();
            g.drawVerticalLine ((int) x, (int) plot.getY(), (int) plot.getBottom());
            g.fillEllipse ((float) x - 3.0f, plot.getY() + 1.0f, 6.0f, 6.0f);
        }
    }
}

dsp::SplAnalyser::Weighting SPLMeter::ratingOf (char weightingChar)
{
    switch (weightingChar)
    {
        case 'C': case 'c': return dsp::SplAnalyser::Weighting::C;
        case 'Z': case 'z': return dsp::SplAnalyser::Weighting::Z;
        default:           return dsp::SplAnalyser::Weighting::A;
    }
}

char SPLMeter::weightingFromName (const juce::String& name)
{
    if (name.startsWith ("C")) return 'C';
    if (name.startsWith ("Z")) return 'Z';
    return 'A';
}

juce::String SPLMeter::weightingName (char weightingChar)
{
    switch (weightingChar)
    {
        case 'C': case 'c': return "C";
        case 'Z': case 'z': return "Z (flat)";
        default:           return "A";
    }
}

void SPLMeter::setCalibration (const MicrophoneCalibration& profile)
{
    microphoneCalibration = profile;

    // The gate that matters. Without a profile that was measured against a reference the
    // readings stay in dBFS and the meter says so, rather than showing a number in decibels
    // that nobody has grounds for.
    calibrated = microphoneCalibration.hasCalibration();

    calibrationOffsetDb = calibrated
                        ? microphoneCalibration.getSensitivityDb() + 3.0103f
                          + microphoneCalibration.getInputTrimDb()
                        : 0.0f;

    // The frequency response travels with the sensitivity. A profile that corrects only the
    // overall level would still report a coloured capsule as though it were flat, and the two
    // halves of the same profile disagreeing is how a reading ends up neither calibrated nor
    // raw.
    // Response correction is a microphone property too. Keep the reference/output path flat
    // so the plotted comparison does not apply the capsule's correction to the loudspeaker.
    analysers[0].setCalibration (MicrophoneCalibration());
    analysers[1].setCalibration (microphoneCalibration);

    calibrationLabel.setText (calibrated ? "Mic dikalibrasi - dB SPL"
                                         : "Belum dikalibrasi - dBFS",
                              juce::dontSendNotification);

    if (calibrated && microphoneCalibration.getCalibrationDate().isNotEmpty())
        infoLabel.setText ("Kalibrasi " + microphoneCalibration.getCalibrationDate()
                           + "  |  acuan " + juce::String ((int) microphoneCalibration.getCalibratorLevelDb())
                           + " dB SPL",
                           juce::dontSendNotification);
    else
        infoLabel.setText ("", juce::dontSendNotification);

    reset();
    repaint();
}

float SPLMeter::getLeq (dsp::SplAnalyser::Weighting rating) const
{
    // Channel one is the microphone. The reference channel has no microphone in front of it, so
    // a level there is a property of the generator and not of the room.
    const auto offset = calibrated ? calibrationOffsetDb - lastInputGainDb
                                   : calibrationOffsetDb;
    return analysers[1].getLeq (rating) + offset;
}

float SPLMeter::getLeq (char weightingChar) const
{
    return getLeq (ratingOf (weightingChar));
}

double SPLMeter::getLeqDuration() const
{
    return analysers[1].getLeqDuration();
}

void SPLMeter::resetLeq()
{
    for (auto& engine : analysers)
        engine.resetLeq();

    for (auto& state : channels)
        state.leqBlocks = 0;
}
