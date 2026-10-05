#include "TransferFunctionDisplay.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff383838);
    const juce::Colour gridBoldColour = juce::Colour(0xff555555);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour magnitudeColour = juce::Colour(0xffffb74d);
    const juce::Colour phaseColour = juce::Colour(0xff4fc3f7);
    const juce::Colour coherenceColour = juce::Colour(0xff81c784);
    const juce::Colour warningColour = juce::Colour(0xffe53935);
    const juce::Colour validColour = juce::Colour(0xff43a047);
    const juce::Colour blankedShade = juce::Colour(0xff1f1f1f);

    constexpr float minFrequency = 20.0f;
    constexpr float maxFrequency = 20000.0f;
    constexpr float topDb = 12.0f;
    constexpr float bottomDb = -36.0f;
}

TransferFunctionDisplay::TransferFunctionDisplay()
{
    findDelayButton.setTooltip ("Cari puncak impuls pertama, lalu mulai ulang rata-rata coherence");
    findDelayButton.onClick = [this] { if (onFindDelay != nullptr) onFindDelay(); };
    freezeButton.setToggleState(false, juce::dontSendNotification);
    freezeButton.onClick = [this]
    {
        freezeButton.setToggleState(!freezeButton.getToggleState(), juce::dontSendNotification);
        frozen = freezeButton.getToggleState();
        repaint();
    };
    freezeButton.setTooltip ("Tahan tampilan untuk dibaca");
    validityLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    validityLabel.setJustificationType(juce::Justification::centredLeft);

    readoutLabel.setFont(juce::Font(13.0f));
    readoutLabel.setColour(juce::Label::textColourId, mutedColour);
    readoutLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(findDelayButton);
    addAndMakeVisible(freezeButton);
    eqOverlayButton.setToggleState(false, juce::dontSendNotification);
    eqOverlayButton.onClick = [this]
    {
        showEqOverlay = eqOverlayButton.getToggleState();

        // The target is derived from the curve, so it has to exist before the first
        // repaint of a frozen pane, where no further frame is coming to compute it.
        if (showEqOverlay)
        {
            const juce::ScopedLock lock(dataLock);
            computeEqTarget();
        }

        repaint();
    };
    eqOverlayButton.setTooltip ("Tampilkan kurva koreksi EQ yang meratakan respons");
    addAndMakeVisible(eqOverlayButton);
    addAndMakeVisible(validityLabel);
    addAndMakeVisible(readoutLabel);
}

TransferFunctionDisplay::~TransferFunctionDisplay() = default;

void TransferFunctionDisplay::pushData(const TransferFunction::Result& result)
{
    if (frozen || !result.valid)
        return;

    const juce::ScopedLock lock(dataLock);

    frequency = result.freq;

    // The same third octave smoothing the plot uses, so the reading matches the curve
    // instead of jumping bin to bin.
    dsp::smoothMagnitudeDb(frequency, result.magnitudeDb, 3, magnitudeCurve);
    dsp::smoothMagnitudeDb(frequency, result.coherence, 3, coherenceCurve);
    dsp::smoothPhaseDeg(frequency, result.phaseDeg, 3, phaseCurve);

    // A smoothed point survives only when the bins underneath it were measurable, so the
    // curve breaks where the reference was silent instead of drawing through it.
    const auto bins = (int) frequency.size();
    magnitudeDb.resize(bins);
    coherence.resize(bins);
    phaseDeg.resize(bins);
    binValid.resize(bins);

    for (int i = 0; i < bins; ++i)
    {
        const auto low = juce::jmax(0, i - (int) (frequency.size() / 24));
        const auto high = juce::jmin(bins - 1, i + (int) (frequency.size() / 24));
        auto measurable = 0;

        for (int j = low; j <= high; ++j)
            if (!result.binValid.empty() && result.binValid[(size_t) j] != 0)
                ++measurable;

        binValid[(size_t) i] = measurable > (high - low) / 2 ? 1 : 0;
        magnitudeDb[(size_t) i] = magnitudeCurve[(size_t) i];
        coherence[(size_t) i] = coherenceCurve[(size_t) i];
        phaseDeg[(size_t) i] = phaseCurve[(size_t) i];
    }

    delayMs = result.delayMs;
    averageCoherence = result.averageCoherence;
    peakReferenceDb = result.peakReferenceDb;
    measRmsDb = result.measRmsDb;
    measPeakDb = result.measPeakDb;
    refRmsDb = result.refRmsDb;
    refPeakDb = result.refPeakDb;
    validBins = result.validBins;
    blankedBins = result.blankedBins;
    averages = std::max (1, result.averages);
    delayAvailable = !std::isnan(result.delayMs);

    computeEqTarget();

    // The validity line is the whole point of the pane: coherence under the threshold means
    // the measurement is not usable yet, no matter how good the curve looks. It has three
    // answers rather than two, because a coherence read from too few averages is not a low
    // score, it is no score at all, and calling it low would blame the room for a reading
    // the estimator was never able to make.
    const auto trustworthy = isCoherenceTrustworthy (averages);
    const auto coherent = averageCoherence >= validityThreshold;
    const auto usable = validBins > 0 && trustworthy && coherent;

    validityLabel.setColour (juce::Label::textColourId,
                             usable ? validColour : warningColour);

    if (validBins == 0)
    {
        validityLabel.setText ("Referensi diam - tidak ada yang diukur", juce::dontSendNotification);
    }
    else if (! trustworthy)
    {
        validityLabel.setText ("Belum bisa dinilai - rata-rata " + juce::String (averages)
                               + " frame, coherence belum bermakna", juce::dontSendNotification);
    }
    else if (! coherent)
    {
        validityLabel.setText ("Belum valid - coherence di bawah 80 %", juce::dontSendNotification);
    }
    else
    {
        validityLabel.setText ("Valid - coherence di atas 80 %", juce::dontSendNotification);
    }

    repaint();
}

TransferFunctionDisplay::Panes TransferFunctionDisplay::panes() const
{
    // A column is kept on the left for the value labels, which is wider than the panel
    // border: without it the decibel and degree labels get cut off at the window edge.

    auto area = getLocalBounds().toFloat().reduced(14.0f, 18.0f).withTrimmedLeft(54.0f);
    area.removeFromTop(34.0f);

    // The level sidebar is dropped from the right, so the three panes share what is left.
    area.removeFromRight(sidebarArea().getWidth() + 12.0f);

    Panes result;
    const auto paneHeight = area.getHeight() / 3.0f;

    auto first = area.removeFromTop(paneHeight).reduced(6.0f, 0.0f);
    first.removeFromBottom(20.0f);
    result.magnitude = first;

    auto middle = area.removeFromTop(paneHeight).reduced(6.0f, 0.0f);
    middle.removeFromBottom(20.0f);
    result.phase = middle;

    auto last = area.reduced(6.0f, 0.0f);
    last.removeFromBottom(FrequencyLabels::reservedSpace());
    result.coherence = last;

    return result;
}

juce::Rectangle<float> TransferFunctionDisplay::magnitudeArea() const
{
    return panes().magnitude;
}

juce::Rectangle<float> TransferFunctionDisplay::phaseArea() const
{
    return panes().phase;
}

juce::Rectangle<float> TransferFunctionDisplay::coherenceArea() const
{
    return panes().coherence;
}

float TransferFunctionDisplay::frequencyToX(float frequency, const juce::Rectangle<float>& bounds) const
{
    const auto clamped = juce::jlimit(minFrequency, maxFrequency, frequency);
    const auto position = (std::log10(clamped) - std::log10(minFrequency))
                        / (std::log10(maxFrequency) - std::log10(minFrequency));
    return bounds.getX() + position * bounds.getWidth();
}

float TransferFunctionDisplay::xToFrequency(float x, const juce::Rectangle<float>& bounds) const
{
    const auto position = juce::jlimit(0.0f, 1.0f, (x - bounds.getX()) / juce::jmax(1.0f, bounds.getWidth()));
    return std::pow(10.0f, std::log10(minFrequency) + position * (std::log10(maxFrequency) - std::log10(minFrequency)));
}

void TransferFunctionDisplay::drawAxis(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                                       float top, float bottom, const juce::String& unit)
{
    g.setFont(juce::Font(11.0f));

    for (int division = 0; division <= 4; ++division)
    {
        const auto value = juce::jmax(bottom, juce::jmin(top,
            top + (bottom - top) * (float) division / 4.0f));
        const auto y = bounds.getY() + bounds.getHeight() * (float) division / 4.0f;

        g.setColour(division == 0 ? gridBoldColour : gridColour);
        g.drawHorizontalLine(juce::roundToInt(y), bounds.getX(), bounds.getRight());

        g.setColour(mutedColour);
        const auto text = std::abs(value) < 10.0f && value == std::round(value)
                        ? juce::String((int) value) : juce::String(value, 0);
        g.drawText(unit.isEmpty() ? text : text + " " + unit, bounds.getX() - 52.0f,
                   y - 8.0f, 48.0f, 16.0f, juce::Justification::centredRight);
    }
}

void TransferFunctionDisplay::drawPane(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                                       const juce::String& title, const juce::String& unit,
                                       const std::vector<float>& values,
                                       const std::vector<char>& valid, float top, float bottom,
                                       const juce::Colour& colour, float referenceLine,
                                       bool showReference)
{
    g.setColour(plotColour);
    g.fillRoundedRectangle(bounds.withTrimmedTop(-22.0f).withTrimmedLeft(-8.0f)
                               .withTrimmedRight(-4.0f).withTrimmedBottom(-18.0f), 6.0f);

    drawAxis(g, bounds, top, bottom, unit);

    if (showReference)
    {
        // The 80% line is what separates a usable measurement from a noisy one, so it is
        // drawn rather than left to be remembered.
        const auto fraction = (referenceLine - bottom) / (top - bottom);
        const auto y = bounds.getBottom() - fraction * bounds.getHeight();

        g.setColour(validColour.withAlpha(0.55f));

        for (float x = bounds.getX(); x < bounds.getRight(); x += 8.0f)
            g.drawHorizontalLine(juce::roundToInt(y), x, juce::jmin(bounds.getRight(), x + 4.0f));
        g.setColour(mutedColour);
        g.setFont(juce::Font(11.0f));
        g.drawText("80 %", bounds.getRight() - 40.0f, y - 14.0f, 38.0f, 14.0f,
                   juce::Justification::centredRight);
    }

    if (values.size() != frequency.size() || frequency.empty())
        return;

    juce::Path path;
    auto started = false;
    auto blanked = false;

    for (size_t i = 0; i < frequency.size(); ++i)
    {
        const auto measurable = valid.size() == frequency.size() && valid[i] != 0;

        if (! measurable)
        {
            if (started)
                started = false;

            blanked = true;
            continue;
        }

        const auto x = frequencyToX(frequency[i], bounds);
        const auto value = juce::jlimit(bottom - 1.0f, top + 1.0f, values[i]);
        const auto y = bounds.getBottom() - (value - bottom) / (top - bottom) * bounds.getHeight();

        if (started)
            path.lineTo(x, y);
        else
            path.startNewSubPath(x, y);

        started = true;
    }

    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(1.6f));

    // Regions the reference never excited are shaded, so a gap reads as "no reference
    // here" instead of "the system is quiet there".
    if (blanked)
    {
        juce::Path shade;
        auto shading = false;

        for (size_t i = 0; i < frequency.size(); ++i)
        {
            const auto measurable = valid.size() == frequency.size() && valid[i] != 0;
            const auto x = frequencyToX(frequency[i], bounds);

            if (! measurable && !shading)
            {
                shade.startNewSubPath(x, bounds.getY());
                shade.lineTo(x, bounds.getBottom());
                shading = true;
            }
            else if (measurable && shading)
            {
                shade.lineTo(x, bounds.getBottom());
                shade.lineTo(x, bounds.getY());
                shading = false;
            }
        }

        g.setColour(blankedShade.withAlpha(0.75f));
        g.fillPath(shade);
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(12.0f));
    g.drawText(title, bounds.getX() + 6.0f, bounds.getY() - 18.0f, bounds.getWidth(), 16.0f,
               juce::Justification::left);
}

void TransferFunctionDisplay::drawFrequencyLabels(juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    const auto ticks = dsp::logAxisTicks(minFrequency, maxFrequency,
                                         std::max(2, (int) (bounds.getWidth() / 62.0f)));

    g.setColour(gridBoldColour);

    for (const auto frequency : ticks)
    {
        const auto x = frequencyToX(frequency, bounds);
        g.drawVerticalLine(juce::roundToInt(x), bounds.getY(), bounds.getBottom());
    }

    g.setColour(mutedColour);
    g.setFont(FrequencyLabels::font());

    for (const auto frequency : ticks)
    {
        const auto x = juce::jlimit(bounds.getX(), bounds.getRight() - FrequencyLabels::width(),
                                    frequencyToX(frequency, bounds) - FrequencyLabels::width() * 0.5f);
        g.drawText(dsp::frequencyTickLabel(frequency), (int) x,
                   (int) (bounds.getBottom() + FrequencyLabels::topOffset()),
                   (int) FrequencyLabels::width(), (int) FrequencyLabels::height(),
                   juce::Justification::centred);
    }
}

juce::Rectangle<float> TransferFunctionDisplay::sidebarArea() const
{
    // Wide enough for "Peak -120.0 dBFS" at the font size below, and no wider: this is a
    // readout, not a second plot. It sits on the right, which is where the eye looks after
    // reading a curve, and the panes give up the width for it.
    const auto bounds = getLocalBounds().toFloat().reduced(14.0f, 18.0f);
    const auto width = std::min (150.0f, std::max (0.0f, bounds.getWidth() * 0.18f));

    return bounds.withTrimmedLeft(std::max(54.0f, bounds.getWidth() - width))
                 .withTrimmedTop(34.0f)
                 .reduced(0.0f, 4.0f);
}

void TransferFunctionDisplay::drawLevelSidebar(juce::Graphics& g)
{
    const auto area = sidebarArea();

    if (area.getWidth() < 60.0f)
        return;

    g.setColour(panelColour);
    g.fillRoundedRectangle(area, 6.0f);

    // One block per channel, measured first so it matches the magnitude curve it belongs
    // to. Both figures are named: "RMS / Peak" read as a pair, while a bare dBFS could be
    // taken for either of them.
    const auto block = [&g, &area, this] (const juce::String& title, const juce::Colour& colour,
                                         float rmsDb, float peakDb, float y)
    {
        const auto silent = rmsDb <= dsp::dbFloor + 1.0f && peakDb <= dsp::dbFloor + 1.0f;

        g.setColour(colour);
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(title, area.getX() + 10.0f, y, area.getWidth() - 20.0f, 16.0f,
                   juce::Justification::centredLeft);

        g.setFont(juce::Font(13.0f));
        g.setColour(mutedColour);

        if (silent)
        {
            g.drawText("RMS --", area.getX() + 10.0f, y + 19.0f, area.getWidth() - 20.0f, 16.0f,
                       juce::Justification::centredLeft);
            g.drawText("Peak -- dBFS", area.getX() + 10.0f, y + 37.0f, area.getWidth() - 20.0f, 16.0f,
                       juce::Justification::centredLeft);
            return y + 58.0f;
        }

        g.drawText("RMS " + juce::String(rmsDb, 1), area.getX() + 10.0f, y + 19.0f,
                   area.getWidth() - 20.0f, 16.0f, juce::Justification::centredLeft);
        g.drawText("Peak " + juce::String(peakDb, 1) + " dBFS", area.getX() + 10.0f, y + 37.0f,
                   area.getWidth() - 20.0f, 16.0f, juce::Justification::centredLeft);
        return y + 58.0f;
    };

    auto y = area.getY() + 10.0f;

    g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.setColour(mutedColour);
    g.drawText("Level", area.getX() + 10.0f, y, area.getWidth() - 20.0f, 16.0f,
               juce::Justification::centredLeft);
    y += 22.0f;

    y = block("Mic", magnitudeColour, measRmsDb, measPeakDb, y);
    y = block("Output / Ref", phaseColour, refRmsDb, refPeakDb, y);

    // The delay belongs next to the levels: both describe what arrived, one as a level and
    // one as a time.
    g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.setColour(mutedColour);
    g.drawText(delayAvailable ? juce::String("Delay ") + juce::String(delayMs, 3) + " ms"
                              : juce::String("Delay --"),
               area.getX() + 10.0f, y, area.getWidth() - 20.0f, 16.0f,
               juce::Justification::centredLeft);
}

void TransferFunctionDisplay::computeEqTarget()
{
    eqTargetValid = false;

    if (frequency.size() < 8 || magnitudeDb.size() != frequency.size()
        || binValid.size() != frequency.size())
        return;

    // The target is the average of the band levels, not of the FFT bins. On a log axis
    // the low end owns far more bins than the top, so averaging bins pulls the target
    // down by however many bins the bass happens to have, and the suggested correction
    // then tilts the whole curve with it.
    const auto centres = dsp::octaveBandFrequencies(3, minFrequency, maxFrequency);
    double sum = 0.0;
    int bands = 0;

    for (size_t band = 0; band < centres.size(); ++band)
    {
        const auto low = band == 0 ? minFrequency
                                    : std::sqrt(centres[band - 1] * centres[band]);
        const auto high = band + 1 >= centres.size() ? maxFrequency
                                                      : std::sqrt(centres[band] * centres[band + 1]);

        double bandSum = 0.0;
        int count = 0;

        for (size_t i = 0; i < frequency.size(); ++i)
            if (binValid[i] != 0 && frequency[i] >= low && frequency[i] < high)
            {
                bandSum += magnitudeDb[i];
                ++count;
            }

        if (count > 0)
        {
            sum += bandSum / count;
            ++bands;
        }
    }

    if (bands < 6)
        return;

    eqTargetDb = (float) (sum / bands);
    eqTargetValid = true;
}

void TransferFunctionDisplay::drawEqCorrection(juce::Graphics& g,
                                               const juce::Rectangle<float>& bounds) const
{
    if (!eqTargetValid || frequency.size() != magnitudeDb.size()
        || binValid.size() != frequency.size())
        return;

    const auto correctionColour = juce::Colour(0xffff8a80);

    // The flat line the correction aims for, so the curve can be read against something
    // instead of against the eye's guess of the middle of the plot.
    const auto targetY = bounds.getBottom()
                       - (juce::jlimit(bottomDb, topDb, eqTargetDb) - bottomDb)
                         / (topDb - bottomDb) * bounds.getHeight();

    g.setColour(mutedColour.withAlpha(0.5f));

    for (float x = bounds.getX(); x < bounds.getRight(); x += 8.0f)
        g.drawHorizontalLine(juce::roundToInt(targetY), juce::roundToInt(x),
                             juce::roundToInt(juce::jmin(bounds.getRight(), x + 4.0f)));

    std::vector<juce::Point<float>> points;

    for (size_t i = 0; i < magnitudeDb.size(); ++i)
    {
        if (binValid[i] == 0)
        {
            points.clear();
            continue;
        }

        const auto value = juce::jlimit(-maxCorrectionDb, maxCorrectionDb,
                                        eqTargetDb - magnitudeDb[i]);
        const auto y = bounds.getBottom()
                     - (juce::jlimit(bottomDb, topDb, value) - bottomDb)
                       / (topDb - bottomDb) * bounds.getHeight();

        points.push_back({ frequencyToX(frequency[i], bounds), y });
    }

    // Drawn dash by dash because the correction is a reference curve, not a second
    // measurement, and a reader must not confuse the two.
    g.saveState();
    g.reduceClipRegion(bounds.toNearestInt());
    g.setColour(correctionColour);

    const float dashOn = 6.0f, dashOff = 5.0f;

    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto from = points[i - 1], to = points[i];
        const auto length = from.getDistanceFrom(to);

        if (length < 0.5f)
            continue;

        for (float travelled = 0.0f; travelled < length; travelled += dashOn + dashOff)
        {
            const auto end = std::min(length, travelled + dashOn);
            const auto a = from + (to - from) * (travelled / length);
            const auto b = from + (to - from) * (end / length);
            g.drawLine((float) a.x, (float) a.y, (float) b.x, (float) b.y, 1.4f);
        }
    }

    g.restoreState();

    g.setColour(correctionColour);
    g.setFont(juce::Font(11.0f));
    g.drawText("Koreksi EQ, target " + juce::String(eqTargetDb, 1) + " dB (batas "
                   + juce::String((int) maxCorrectionDb) + " dB)",
               bounds.getX() + 6.0f, bounds.getBottom() - 16.0f, bounds.getWidth() - 12.0f, 16.0f,
               juce::Justification::bottomRight);
}

void TransferFunctionDisplay::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    const juce::ScopedLock lock(dataLock);

    const auto areas = panes();

    drawPane(g, areas.magnitude, "Magnitude", "dB", magnitudeDb, binValid, topDb, bottomDb,
             magnitudeColour);

    if (showEqOverlay)
        drawEqCorrection(g, areas.magnitude);

    drawPane(g, areas.phase, "Phase", "deg", phaseDeg, binValid, 180.0f, -180.0f, phaseColour);
    drawPane(g, areas.coherence, "Coherence", "", coherence, binValid, 1.0f, 0.0f,
             coherenceColour, validityThreshold, true);
    drawFrequencyLabels(g, areas.coherence);
    drawLevelSidebar(g);
}

void TransferFunctionDisplay::updateCursorReadout(const juce::Rectangle<float>& bounds)
{
    const auto frequencyAt = xToFrequency(cursorPosition.x, bounds);

    auto nearest = 0;

    for (size_t i = 1; i < frequency.size(); ++i)
        if (std::abs(frequency[i] - frequencyAt) < std::abs(frequency[(size_t) nearest] - frequencyAt))
            nearest = (int) i;

    if (frequency.empty())
    {
        readoutLabel.setText("Belum ada data transfer function", juce::dontSendNotification);
        return;
    }

    const auto index = (size_t) nearest;
    const auto measurable = binValid.size() == frequency.size() && binValid[index] != 0;

    if (! measurable)
    {
        readoutLabel.setText(dsp::frequencyTickLabel(frequency[index])
                           + " Hz   tanpa sinyal referensi", juce::dontSendNotification);
        return;
    }

    readoutLabel.setText(dsp::frequencyTickLabel(frequency[index]) + " Hz   "
                       + juce::String(magnitudeDb[index], 1) + " dB   "
                       + juce::String(phaseDeg[index], 1) + " deg   coherence "
                       + juce::String(coherence[index] * 100.0f, 0) + " %",
                       juce::dontSendNotification);
}

void TransferFunctionDisplay::mouseMove(const juce::MouseEvent& event)
{
    cursorPosition = event.position;
    const juce::ScopedLock lock(dataLock);
    updateCursorReadout(coherenceArea());
}

void TransferFunctionDisplay::mouseExit(const juce::MouseEvent&)
{
    readoutLabel.setText(juce::String(), juce::dontSendNotification);
}

void TransferFunctionDisplay::resized()
{
    auto area = getLocalBounds().reduced(14.0f, 8.0f);
    auto controls = area.removeFromTop(32.0f);

    findDelayButton.setBounds(controls.removeFromRight(140.0f).reduced(0.0f, 2.0f));
    controls.removeFromRight(8.0f);
    freezeButton.setBounds(controls.removeFromRight(110.0f).reduced(0.0f, 2.0f));
    controls.removeFromRight(8.0f);
    eqOverlayButton.setBounds(controls.removeFromRight(110.0f).reduced(0.0f, 2.0f));
    controls.removeFromRight(8.0f);
    const auto validityWidth = juce::jmin(controls.getWidth(), 420);
    validityLabel.setBounds(controls.withSizeKeepingCentre(juce::jmax(0, validityWidth),
                                                          controls.getHeight())
                                      .reduced(0, 2));

    // The cursor readout sits on the magnitude pane's title row, where there is space.
    auto titleRow = magnitudeArea().toNearestInt().withTrimmedTop(-18).withHeight(18);
    readoutLabel.setBounds(titleRow.withTrimmedLeft(220).reduced(0, 1));

    repaint();
}

juce::String TransferFunctionDisplay::getReadout() const
{
    if (!hasData())
        return "Belum ada transfer function";

    juce::String text = "Coherence rata-rata " + juce::String(averageCoherence * 100.0f, 0) + " %";

    if (delayAvailable)
        text += "   delay " + juce::String(delayMs, 2) + " ms";

    text += "   terukur " + juce::String(validBins) + " band";
    return text;
}
