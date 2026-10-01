#include "FFTDisplay.h"

namespace
{
    const juce::Colour spectrogramFloorColour = juce::Colour(0xff141a26);
    constexpr int linearBarCount = 100;

    const juce::Colour backgroundColour = juce::Colour(0xff111318);
    const juce::Colour panelColour = juce::Colour(0xff1b1f27);
    const juce::Colour gridColour = juce::Colour(0xff232833);
    const juce::Colour gridBoldColour = juce::Colour(0xff39404f);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff8892a0);
    const juce::Colour channelOneColour = juce::Colour(0xff4fc3f7);
    const juce::Colour channelTwoColour = juce::Colour(0xff9ccc65);
    const juce::Colour transferColour = juce::Colour(0xffffb74d);
    const juce::Colour coherenceColour = juce::Colour(0xffee82ee);
    const juce::Colour phaseColour = juce::Colour(0xfff06292);

    struct RangePreset
    {
        float top;
        float bottom;
        const char* name;
    };

    struct BandPreset
    {
        int octaveFraction;
        const char* label;
    };

    const BandPreset bandPresets[] = {
        { 1, "1/1 Oktaf (10 band)" },
        { 2, "1/2 Oktaf (16 band)" },
        { 3, "1/3 Oktaf (31 band)" },
        { 4, "1/4 Oktaf (41 band)" },
        { 6, "1/6 Oktaf (61 band)" },
        { 12, "1/12 Oktaf (121 band)" }
    };

    const RangePreset rangePresets[] = {
        { 30.0f, -30.0f, "30 / -30" },
        { 20.0f, -40.0f, "20 / -40" },
        { 20.0f, -60.0f, "20 / -60" },
        { 10.0f, -60.0f, "10 / -60" },
        { 10.0f, -90.0f, "10 / -90" },
        { 0.0f, -120.0f, "0 / -120" }
    };
}

FFTDisplay::FFTDisplay()
{
    loadBarColours();

    micColourButton.registerSelf (micColourButton);
    micColourButton.setTooltip ("Warna bar RTA microphone");
    micColourButton.onColourChange = [this] { setMicBarColour (micColourButton.getColour()); };
    outputColourButton.registerSelf (outputColourButton);
    outputColourButton.setTooltip ("Warna bar output generator");
    outputColourButton.onColourChange = [this] { setOutputBarColour (outputColourButton.getColour()); };

    styleSelector.addItem("Band oktaf", (int) Style::Bands);
    styleSelector.addItem("Bar linear 100", (int) Style::BarLinear);
    styleSelector.addItem("Line", (int) Style::Line);
    styleSelector.addItem("Spektogram", (int) Style::Spectrogram);
    styleSelector.setSelectedId((int) style, juce::dontSendNotification);
    styleSelector.setTooltip("Band oktaf: puncak FFT per band 1/1 oktaf sampai 1/12 oktaf pada sumbu log. "
                             "Bar linear: 100 bar evenly spaced pada sumbu frekuensi linear 0-20 kHz. "
                             "Line: spektrum FFT dengan smoothing pilihan. "
                             "Spektogram: riwayat level bergeser, waktu ke kanan.");
    styleSelector.onChange = [this] { setStyle((Style) styleSelector.getSelectedId()); };
    addAndMakeVisible(styleSelector);

    for (const auto& preset : bandPresets)
        bandSelector.addItem(preset.label, preset.octaveFraction);

    bandSelector.setSelectedId(bandOctaveFraction, juce::dontSendNotification);
    bandSelector.setTooltip("Resolusi band untuk tampilan Bar, dari 1/1 oktaf sampai 1/12 oktaf. "
                            "Frekuensi mengikuti nilai nominal ISO 266 (20 Hz - 20 kHz).");
    bandSelector.onChange = [this] { setBandOctaveFraction(bandSelector.getSelectedId()); };
    addAndMakeVisible(bandSelector);
    setBandOctaveFraction(bandOctaveFraction);

    modeSelector.addItem("RTA Microphone", (int) Mode::SingleChannel);
    modeSelector.addItem("Dual Channel", (int) Mode::DualChannel);
    modeSelector.addItem("Transfer Function", (int) Mode::TransferFunction);
    modeSelector.addItem("Coherence", (int) Mode::Coherence);
    modeSelector.addItem("Phase", (int) Mode::Phase);
    modeSelector.setSelectedId((int) mode);
    modeSelector.onChange = [this]
    {
        mode = (Mode) modeSelector.getSelectedId();
        displayDirty = true;
        repaint();
    };

    octaveSelector.addItem("Off", 100);
    octaveSelector.addItem("1/1 Octave", 1);
    octaveSelector.addItem("1/3 Octave", 3);
    octaveSelector.addItem("1/6 Octave", 6);
    octaveSelector.addItem("1/12 Octave", 12);
    octaveSelector.addItem("1/24 Octave", 24);
    octaveSelector.setSelectedId(3);
    octaveSelector.onChange = [this]
    {
        octaveFraction = octaveSelector.getSelectedId() == 100 ? 0 : octaveSelector.getSelectedId();
        displayDirty = true;
        repaint();
    };

    rangeSelector.addItem(rangePresets[0].name, 1);
    rangeSelector.addItem(rangePresets[1].name, 2);
    rangeSelector.addItem(rangePresets[2].name, 3);
    rangeSelector.addItem(rangePresets[3].name, 4);
    rangeSelector.addItem(rangePresets[4].name, 5);
    rangeSelector.addItem(rangePresets[5].name, 6);
    rangeSelector.setSelectedId(6);
    rangeSelector.onChange = [this]
    {
        const auto index = juce::jlimit(0, (int) std::size(rangePresets) - 1,
                                        rangeSelector.getSelectedId() - 1);
        topDb = rangePresets[index].top;
        bottomDb = rangePresets[index].bottom;
        displayDirty = true;
        repaint();
    };
    topDb = rangePresets[5].top;
    bottomDb = rangePresets[5].bottom;

    peakHoldButton.onClick = [this] { setPeakHold(peakHoldButton.getToggleState()); };
    freezeButton.onClick = [this]
    {
        frozen = freezeButton.getToggleState();

        if (frozen)
        {
            juce::ScopedLock lock(dataLock);
            frozenFrame = current;
        }

        displayDirty = true;
        repaint();
    };

    readoutLabel.setFont(juce::Font(13.0f));
    readoutLabel.setColour(juce::Label::textColourId, mutedColour);
    readoutLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(modeSelector);
    addAndMakeVisible(octaveSelector);
    addAndMakeVisible(rangeSelector);
    addAndMakeVisible(peakHoldButton);
    addAndMakeVisible(freezeButton);
    addAndMakeVisible(readoutLabel);
    addAndMakeVisible(micColourButton);
    addAndMakeVisible(outputColourButton);
}

void FFTDisplay::setChannelLabels(const juce::String& measurement, const juce::String& reference)
{
    measurementLabel = measurement.isNotEmpty() ? measurement : juce::String("Mic");
    referenceLabel = reference.isNotEmpty() ? reference : juce::String("Ref");
    displayDirty = true;
    repaint();
}

void FFTDisplay::ColourSwatchButton::setColour(const juce::Colour& newColour)
{
    colour = newColour;
    repaint();
}

void FFTDisplay::ColourSwatchButton::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(1);

    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(bounds.toFloat(), 3.0f);

    const auto swatch = bounds.reduced(3, 3).withTrimmedRight(2).toFloat();
    g.setColour(colour);
    g.fillRoundedRectangle(swatch, 2.0f);

    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.setFont(juce::Font(9.0f));
    g.drawText(text, swatch.getX() - 12.0f, swatch.getCentreY() - 6.0f,
               68.0f, 12.0f, juce::Justification::centredLeft);
}

void FFTDisplay::ColourSwatchButton::mouseDown(const juce::MouseEvent&)
{
    if (self == nullptr)
        return;

    auto selector = std::make_unique<juce::ColourSelector> (juce::ColourSelector::showColourAtTop
                                                              | juce::ColourSelector::showSliders
                                                              | juce::ColourSelector::showColourspace);
    selector->setCurrentColour (self->colour);
    selector->setSize (300, 320);

    // The selector is a ChangeBroadcaster, so forward every edit to the swatch.
    selector->addChangeListener (self);

    juce::CallOutBox::launchAsynchronously (std::move (selector), getScreenBounds(), nullptr);
}

void FFTDisplay::ColourSwatchButton::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (auto* selector = dynamic_cast<juce::ColourSelector*> (source))
    {
        setColour (selector->getCurrentColour());

        if (onColourChange != nullptr)
            onColourChange();
    }
}

namespace
{
    const char* barColourFileName = "rtaColours.txt";

    juce::File barColourFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("OpenSmaartLab")
                    .getChildFile (barColourFileName);
    }
}

void FFTDisplay::loadBarColours()
{
    auto file = barColourFile();

    const auto read = [&file] (const juce::String& key, const juce::Colour& fallback)
    {
        if (! file.existsAsFile())
            return fallback;

        const auto marker = key + "=";

        for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
            if (line.startsWith (marker))
                return juce::Colour::fromString (line.substring (marker.length()).trim());

        return fallback;
    };

    micBarColour = read ("micColour", channelOneColour);
    outputBarColour = read ("outputColour", transferColour);

    micColourButton.setColour (micBarColour);
    outputColourButton.setColour (outputBarColour);
}

void FFTDisplay::saveBarColours() const
{
    auto file = barColourFile();
    file.getParentDirectory().create();
    file.replaceWithText ("micColour=" + micBarColour.toString() + "\n"
                          + "outputColour=" + outputBarColour.toString() + "\n");
}

void FFTDisplay::setMicrophoneCalibration(const MicrophoneCalibration& newCalibration)
{
    calibration = newCalibration;
    displayDirty = true;
    repaint();
}

void FFTDisplay::setBarCalibrationFactor(float factor)
{
    barCalibrationFactor = factor;
}

void FFTDisplay::setMeasuredLevels(float rmsDb, float peakDb)
{
    measLevels.rmsDb = rmsDb;
    measLevels.peakDb = peakDb;
    levelsValid = rmsDb > dsp::dbFloor + 1.0f || peakDb > dsp::dbFloor + 1.0f;
    repaint();
}

void FFTDisplay::setReferenceLevels(float rmsDb, float peakDb)
{
    refLevels.rmsDb = rmsDb;
    refLevels.peakDb = peakDb;
    repaint();
}

void FFTDisplay::setMicBarColour(const juce::Colour& colour)
{
    if (micBarColour == colour)
        return;

    micBarColour = colour;
    micColourButton.setColour (colour);
    saveBarColours();
    displayDirty = true;
    repaint();
}

void FFTDisplay::setOutputBarColour(const juce::Colour& colour)
{
    if (outputBarColour == colour)
        return;

    outputBarColour = colour;
    outputColourButton.setColour (colour);
    saveBarColours();
    displayDirty = true;
    repaint();
}

FFTDisplay::~FFTDisplay() = default;

void FFTDisplay::setMode(Mode newMode)
{
    if (mode != newMode)
    {
        mode = newMode;
        modeSelector.setSelectedId((int) mode, juce::dontSendNotification);
        displayDirty = true;
        repaint();
    }
}

void FFTDisplay::setOctaveFraction(int fraction)
{
    if (octaveFraction != fraction)
    {
        octaveFraction = fraction;
        octaveSelector.setSelectedId(fraction == 0 ? 100 : fraction, juce::dontSendNotification);
        displayDirty = true;
        repaint();
    }
}

void FFTDisplay::setRange(float top, float bottom)
{
    // The window has to stay a usable size and stay inside the level scale: a top above
    // +12 dB would only add headroom, and a range narrower than 20 dB collapses the axis
    // divisions onto one line and inverts the colour scale.
    topDb = juce::jmin (12.0f, top);
    bottomDb = juce::jmax (-160.0f, bottom);

    if (bottomDb > topDb - 20.0f)
        bottomDb = topDb - 20.0f;

    displayDirty = true;
    repaint();
}

void FFTDisplay::setStyle(Style newStyle)
{
    if (style == newStyle)
        return;

    style = newStyle;
    barStyle = newStyle != Style::Line && newStyle != Style::Spectrogram;
    linearAxis = newStyle == Style::BarLinear;
    computeAxisDomain();
    styleSelector.setSelectedId((int) newStyle, juce::dontSendNotification);
    clearPeakHold();

    // The history belongs to the view it was recorded for: a bar graph of another
    // layout would be measured against the wrong time axis.
    if (newStyle == Style::Spectrogram)
    {
        // Ready before the first frame arrives, so the scale is on screen straight away.
        ensureSpectrogramSize();
        bandSelector.setVisible(false);
        octaveSelector.setVisible(false);
    }
    else
    {
        // The history belongs to the spectrogram: keeping it would hold a couple of
        // megabytes for a view that is not on screen, and the time axis would silently
        // continue from a moment the user had already left.
        resetSpectrogram();
        bandSelector.setVisible(true);
        octaveSelector.setVisible(true);
    }

    displayDirty = true;
    repaint();
}

void FFTDisplay::setBandOctaveFraction(int fraction)
{
    const auto clamped = juce::jlimit(1, 12, fraction);

    if (bandOctaveFraction == clamped && !bandFrequencies.empty())
        return;

    bandOctaveFraction = clamped;
    bandFrequencies = dsp::octaveBandFrequencies(bandOctaveFraction, minFrequency, maxFrequency);
    computeAxisDomain();
    bandSelector.setSelectedId(bandOctaveFraction, juce::dontSendNotification);
    clearPeakHold();
    displayDirty = true;
    repaint();
}

void FFTDisplay::setPeakHold(bool shouldHold)
{
    if (peakHoldEnabled != shouldHold)
    {
        peakHoldEnabled = shouldHold;
        peakHoldButton.setToggleState(shouldHold, juce::dontSendNotification);

        if (!shouldHold)
            peakHoldValues.clear();

        repaint();
    }
}

void FFTDisplay::setFrozen(bool shouldFreeze)
{
    if (frozen != shouldFreeze)
    {
        frozen = shouldFreeze;
        freezeButton.setToggleState(shouldFreeze, juce::dontSendNotification);

        if (frozen)
        {
            juce::ScopedLock lock(dataLock);
            frozenFrame = current;
        }

        repaint();
    }
}

void FFTDisplay::setRunning(bool isNowRunning)
{
    if (running != isNowRunning)
    {
        running = isNowRunning;

        if (!running)
        {
            setFrozen(false);
            setPeakHold(false);
        }

        repaint();
    }
}

void FFTDisplay::clearPeakHold()
{
    peakHoldValues.clear();
    repaint();
}

void FFTDisplay::pushData(const std::vector<float>& freq,
                          const std::vector<float>& refMagnitudeDb,
                          const std::vector<float>& measMagnitudeDb,
                          const std::vector<float>& tfMagnitudeDb,
                          const std::vector<float>& tfPhaseDeg,
                          const std::vector<float>& coherence,
                          const std::vector<char>& binValid)
{
    if (frozen)
        return;

    juce::ScopedLock lock(dataLock);

    current.freq = freq;
    current.refDb = refMagnitudeDb;
    current.measDb = measMagnitudeDb;
    current.tfDb = tfMagnitudeDb;
    current.tfPhase = tfPhaseDeg;
    current.coherence = coherence;
    current.binValid = binValid;
    current.valid = !freq.empty();

    // One column per frame of live data, so the time axis is real time and not the
    // refresh rate of the window.
    if (style == Style::Spectrogram)
        pushSpectrogramFrame(current);

    // Only the microphone channel is calibrated. The reference channel is usually a
    // loopback of the generator, so correcting it would break the transfer function.
    if (calibration.isEnabled() && mode == Mode::SingleChannel)
        calibration.apply (current.measDb, current.freq);

    rebuildDisplay();
    updatePeakHold();
    repaint();
}

void FFTDisplay::rebuildDisplay()
{
    const auto& frame = frozen ? frozenFrame : current;

    smoothed.clear();
    smoothed.reserve(2);
    displayDirty = false;

    // A loaded snapshot owns the display until it is dismissed.
    if (snapshotActive)
        return;

    display.freq = frame.freq;
    display.binValid = frame.binValid;
    display.traces.clear();
    display.valid = frame.valid && !frame.freq.empty();

    if (mode == Mode::SingleChannel && linearAxis)
        display.freq = dsp::linearBarFrequencies(linearBarCount, minFrequency, maxFrequency);

    if (!display.valid)
    {
        display.axis = Axis::Decibels;
        return;
    }

    switch (mode)
    {
        case Mode::SingleChannel:
        {
            const auto summarise = [this, &frame] (const std::vector<float>& magnitudes)
            {
                if (style == Style::BarLinear)
                    return dsp::linearPeakDb(frame.freq, magnitudes, linearBarCount, minFrequency, maxFrequency);

                if (style == Style::Bands)
                    return dsp::bandPeakDb(frame.freq, magnitudes, bandFrequencies, minFrequency);

                return std::vector<float>();
            };

            if (barStyle)
            {
                if (style == Style::Bands)
                    display.freq = bandFrequencies;

                smoothed.push_back(summarise(frame.measDb));
            }
            else
                dsp::smoothMagnitudeDb(frame.freq, frame.measDb, octaveFraction, smoothed.emplace_back());
            display.traces.push_back({ &smoothed.back(), barStyle ? micBarColour : channelOneColour,
                                       measurementLabel });
            if (generatorReference)
            {
                if (barStyle)
                    smoothed.push_back(summarise(frame.refDb));
                else
                    dsp::smoothMagnitudeDb(frame.freq, frame.refDb, octaveFraction, smoothed.emplace_back());
                display.traces.push_back({ &smoothed.back(), barStyle ? outputBarColour : transferColour, "Output" });
            }
            display.axis = Axis::Decibels;
            break;
        }

        case Mode::DualChannel:
        {
            dsp::smoothMagnitudeDb(frame.freq, frame.refDb, octaveFraction, smoothed.emplace_back());
            dsp::smoothMagnitudeDb(frame.freq, frame.measDb, octaveFraction, smoothed.emplace_back());
            display.traces.push_back({ &smoothed[0], channelOneColour, referenceLabel });
            display.traces.push_back({ &smoothed[1], channelTwoColour, measurementLabel });
            display.axis = Axis::Decibels;
            break;
        }

        case Mode::TransferFunction:
        {
            dsp::smoothMagnitudeDb(frame.freq, frame.tfDb, octaveFraction, smoothed.emplace_back());
            display.traces.push_back({ &smoothed.back(), transferColour, "TF" });
            display.axis = Axis::Decibels;
            break;
        }

        case Mode::Coherence:
        {
            dsp::smoothCoherence(frame.freq, frame.coherence, octaveFraction, smoothed.emplace_back());
            display.traces.push_back({ &smoothed.back(), coherenceColour, "Coh" });
            display.axis = Axis::Coherence;
            break;
        }

        case Mode::Phase:
        {
            dsp::smoothPhaseDeg(frame.freq, frame.tfPhase, octaveFraction, smoothed.emplace_back());
            display.traces.push_back({ &smoothed.back(), phaseColour, "Phase" });
            display.axis = Axis::Phase;
            break;
        }
    }
}

void FFTDisplay::updatePeakHold()
{
    if (!peakHoldEnabled || display.traces.empty())
        return;

    const auto count = display.traces.size();
    const auto size = display.freq.size();

    if (peakHoldValues.size() != count)
        peakHoldValues.assign(count, std::vector<float>());

    for (size_t t = 0; t < count; ++t)
    {
        const auto& trace = *display.traces[t].values;

        if (peakHoldValues[t].size() != trace.size())
            peakHoldValues[t] = trace;

        for (size_t i = 0; i < trace.size(); ++i)
            peakHoldValues[t][i] = std::max(peakHoldValues[t][i], trace[i]);
    }
}

juce::Rectangle<float> FFTDisplay::plotArea() const
{
    auto area = getLocalBounds().toFloat();
    area.removeFromTop(40.0f);
    // Room is reserved below the plot for the frequency label row, which is taller
    // than the 18 pt this used to allow.
    area = area.reduced (24.0f, 18.0f)
               .withTrimmedLeft (46.0f)
               .withTrimmedBottom (FrequencyLabels::reservedSpace());

    return area;
}

// Smallest on-screen gap between two band centres, used to decide how many
// frequency labels fit without overlapping.
float FFTDisplay::minimumLabelSpacing(const juce::Rectangle<float>& area) const
{
    if (bandFrequencies.size() < 2)
        return area.getWidth();

    auto smallest = std::numeric_limits<float>::max();

    for (size_t band = 1; band < bandFrequencies.size(); ++band)
        smallest = std::min (smallest, std::abs (frequencyToX (bandFrequencies[band], area)
                                                 - frequencyToX (bandFrequencies[band - 1], area)));

    return smallest < 1.0f ? 1.0f : smallest;
}

// Log axis limits for the bar views. The first and last band centres sit exactly on
    // the nominal 20 Hz and 20 kHz axis limits, so a bar centred there would be half
    // outside the plot. Widening the domain by half a band step on each side lets
    // every bar tile the full width while its centre still tracks its own frequency.
void FFTDisplay::computeAxisDomain()
{
if (linearAxis)
    {
        axisMinLog = 0.0f;
        axisMaxLog = std::log10 (maxFrequency);
        return;
    }

    // The linear view maps minFrequency to the left edge as well, so the two axes
    // must agree or the cursor readout would not match the plotted bars.

        axisMinLog = std::log10 (minFrequency);
        axisMaxLog = std::log10 (maxFrequency);

        if (! barStyle || bandFrequencies.size() < 2)
            return;

        const auto& centres = bandFrequencies;
        const auto first = std::log10 (centres.front());
        const auto last = std::log10 (centres.back());
        auto step = (last - first) / (float) (centres.size() - 1);

        // A nominal band grid is not perfectly even, so use the tightest gap as the
        // true step; that keeps the widening as small as possible.
        for (size_t i = 1; i < centres.size(); ++i)
            step = std::min (step, std::log10 (centres[i]) - std::log10 (centres[i - 1]));

        axisMinLog = first - step * 0.5f;
        axisMaxLog = last + step * 0.5f;
    }

float FFTDisplay::frequencyToX(float freq, const juce::Rectangle<float>& area) const
{
    const auto clamped = juce::jlimit (minFrequency, maxFrequency, freq);

    if (linearAxis)
        return area.getX() + (clamped - minFrequency) / (maxFrequency - minFrequency) * area.getWidth();

    const auto logMin = axisMinLog;
    const auto logMax = axisMaxLog;

    return area.getX() + (std::log10(clamped) - logMin) / (logMax - logMin) * area.getWidth();
}

float FFTDisplay::frequencyAtX(float x, const juce::Rectangle<float>& area) const
{
    if (linearAxis)
    {
        const auto fraction = juce::jlimit(0.0f, 1.0f, (x - area.getX()) / std::max(1.0f, area.getWidth()));
        return minFrequency + fraction * (maxFrequency - minFrequency);
    }

    const auto fraction = juce::jlimit(0.0f, 1.0f, (x - area.getX()) / std::max(1.0f, area.getWidth()));

    return std::pow(10.0f, axisMinLog + fraction * (axisMaxLog - axisMinLog));
}

float FFTDisplay::valueToY(float value, const juce::Rectangle<float>& area) const
{
    switch (display.axis)
    {
        case Axis::Coherence:
        {
            const auto fraction = juce::jlimit(0.0f, 1.0f, value);
            return area.getBottom() - fraction * area.getHeight();
        }

        case Axis::Phase:
        {
            const auto fraction = juce::jlimit(0.0f, 1.0f, (value + 180.0f) / 360.0f);
            return area.getBottom() - fraction * area.getHeight();
        }

        case Axis::Decibels:
        default:
        {
            const auto fraction = juce::jlimit(0.0f, 1.0f, (value - bottomDb) / (topDb - bottomDb));
            return area.getBottom() - fraction * area.getHeight();
        }
    }
}

int FFTDisplay::nearestBin(float x, const juce::Rectangle<float>& area) const
{
    if (display.freq.size() < 2)
        return -1;

    const auto target = frequencyAtX(x, area);
    int best = 0;
    float bestDistance = std::abs(display.freq[0] - target);

    for (size_t i = 1; i < display.freq.size(); ++i)
    {
        const auto distance = std::abs(display.freq[i] - target);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = (int) i;
        }
    }

    return best;
}

juce::String FFTDisplay::frequencyLabel(float freq) const
{
    if (freq >= 1000.0f)
    {
        const auto khz = freq / 1000.0f;
        return juce::String(khz, std::fmod(khz, 1.0f) > 0.05f ? 1 : 0) + "k";
    }

    return juce::String(freq, std::fmod(freq, 1.0f) > 0.05f ? 1 : 0);
}

// Horizontal frequency labels. The sizing rules live in FrequencyLabels so the
// drawing code and the tests use the same numbers.
// The readout colours match the trace swatches so the two can be linked at a glance.
juce::Colour FFTDisplay::traceColourFor(size_t index) const
{
    juce::ScopedLock lock(dataLock);

    if (index < display.traces.size())
        return display.traces[index].colour;

    return index == 0 ? channelOneColour : transferColour;
}

juce::Font FFTDisplay::frequencyLabelFont() const
{
    return FrequencyLabels::font();
}

float FFTDisplay::frequencyLabelHeight() const
{
    return FrequencyLabels::height();
}

float FFTDisplay::frequencyLabelWidth(const juce::Rectangle<float>& area) const
{
    juce::ignoreUnused (area);
    return FrequencyLabels::width();
}

juce::String FFTDisplay::getAxisLabel() const
{
    switch (display.axis)
    {
        case Axis::Coherence:  return "Coherence";
        case Axis::Phase:      return "Phase (deg)";
        default:               return "dB";
    }
}

void FFTDisplay::resized()
{
    auto bar = getLocalBounds().reduced(12, 8);
    auto controls = bar.removeFromTop(32);

    modeSelector.setBounds(controls.removeFromLeft(170));
    controls.removeFromLeft(8);
    styleSelector.setBounds(controls.removeFromLeft(130));
    controls.removeFromLeft(8);
    bandSelector.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(8);
    octaveSelector.setBounds(controls.removeFromLeft(130));
    controls.removeFromLeft(8);
    rangeSelector.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(12);
    peakHoldButton.setBounds(controls.removeFromLeft(110).reduced(0, 4));
    controls.removeFromLeft(6);
    freezeButton.setBounds(controls.removeFromLeft(90).reduced(0, 4));
    readoutLabel.setBounds(controls.removeFromRight(300).reduced(0, 4));
    controls.removeFromLeft(12);
    outputColourButton.setBounds(controls.removeFromRight(58).reduced(8, 4));
    controls.removeFromLeft(4);
    micColourButton.setBounds(controls.removeFromRight(58).reduced(8, 4));

    if (style == Style::Spectrogram)
        ensureSpectrogramSize();
}

void FFTDisplay::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    const auto area = plotArea();

    juce::ScopedLock lock(dataLock);

    if (displayDirty)
        rebuildDisplay();

    if (style == Style::Spectrogram)
        drawSpectrogram(g, area);
    else
    {
        drawGrid(g, area);
        drawTraces(g, area);
    }

    // The spectrogram has no vertical value axis: the colour scale carries the dB, so a
    // second label down the side would only say the same thing twice.
    if (style != Style::Spectrogram)
    {
        g.setColour(mutedColour);
        g.setFont(juce::Font(12.0f));
        g.drawText(getAxisLabel(), area.getX() - 44, area.getCentreY() - 10, 40, 20,
                   juce::Justification::centredRight);
    }

    drawCursor(g, area);

    if (!running)
    {
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::Font(20.0f));
        g.drawText("Tekan Mulai untuk menjalankan pengukuran", getLocalBounds().toFloat(),
                   juce::Justification::centred);
    }
}

float FFTDisplay::bandFrequency(int band) const
{
    const auto bands = juce::jmax(1, spectrogramBands);
    const auto fraction = juce::jlimit(0.0f, 1.0f, (float) band / (float) (bands - 1));
    return minFrequency * std::pow(maxFrequency / minFrequency, fraction);
}

juce::Colour FFTDisplay::spectrogramColourFor(float db) const
{
    // A heat ramp from cold to hot, with the floor still distinguishable from the panel:
    // the band that has never been excited must not look like a low band.
    struct Stop { float db; juce::Colour colour; };

    const Stop ramp[] = {
        { dsp::dbFloor, spectrogramFloorColour },
        { -90.0f,     juce::Colour(0xff1b3a5c) },
        { -70.0f,     juce::Colour(0xff1f7a8c) },
        { -50.0f,     juce::Colour(0xff3fa34d) },
        { -30.0f,     juce::Colour(0xffc9c94a) },
        { -10.0f,     juce::Colour(0xffe07b39) },
        { 0.0f,       juce::Colour(0xffe8453c) },
        { 12.0f,      juce::Colour(0xfff5f5f5) }
    };

    const auto level = juce::jlimit(dsp::dbFloor, 12.0f, db);

    for (size_t i = 1; i < std::size(ramp); ++i)
    {
        if (level > ramp[i].db)
            continue;

        const auto span = ramp[i].db - ramp[i - 1].db;

        if (span <= 0.0f)
            continue;

        const auto fraction = (float) juce::jlimit(0.0f, 1.0f, (level - ramp[i - 1].db) / span);
        return ramp[i - 1].colour.interpolatedWith(ramp[i].colour, fraction);
    }

    return ramp[std::size(ramp) - 1].colour;
}

float FFTDisplay::spectrogramBandLevel(const Frame& frame, int band) const
{
    if (frame.measDb.empty() || frame.freq.empty())
        return dsp::dbFloor;

    const auto low = bandFrequency(band);
    const auto high = bandFrequency(band + 1);
    double total = 0.0;
    auto count = 0;

    for (size_t i = 0; i < frame.freq.size(); ++i)
    {
        if (frame.freq[i] < low || frame.freq[i] > high)
            continue;

        total += std::pow(10.0, frame.measDb[i] / 10.0);
        ++count;
    }

    // A band with no FFT bin in it still has a frequency of its own; reporting the floor
    // keeps the history continuous instead of leaving holes that were never measured.
    if (count == 0)
        return dsp::dbFloor;

    return dsp::db10((float) (total / (double) count));
}

void FFTDisplay::resetSpectrogram()
{
    spectrogramFront = {};
    spectrogramBack = {};
    spectrogramBands = 0;
    spectrogramFrames = 0;
    repaint();
}

float FFTDisplay::getSpectrogramHistoryDb(int framesAgo, int band) const
{
    if (historyDb.empty() || band < 0 || band >= spectrogramBands
        || framesAgo < 0 || framesAgo >= spectrogramFrames)
        return dsp::dbFloor;

    const auto frame = ((historyHead - 1 - framesAgo) % spectrogramFrames
                        + spectrogramFrames) % spectrogramFrames;

    return historyDb[(size_t) frame * (size_t) spectrogramBands + (size_t) band];
}

void FFTDisplay::ensureSpectrogramSize()
{
    // Bands run across the frequency axis and frames up the time axis, so both
    // dimensions are the size of the plot and the picture is blitted one to one.
    const auto plot = plotArea().toNearestInt();
    const auto bands = juce::jlimit(120, 1600, plot.getWidth());
    const auto frames = juce::jlimit(64, 700, plot.getHeight());

    if (spectrogramFront.isValid() && spectrogramBands == bands && spectrogramFrames == frames)
        return;

    resetSpectrogram();

    spectrogramBands = bands;
    spectrogramFrames = frames;
    historyDb.assign((size_t) bands * (size_t) frames, dsp::dbFloor);
    spectrogramFront = juce::Image(juce::Image::ARGB, spectrogramBands, spectrogramFrames, true);
    spectrogramBack = juce::Image(juce::Image::ARGB, spectrogramBands, spectrogramFrames, true);

    // Starting from the floor colour rather than transparent black keeps a band that has
    // not been excited visibly cold instead of showing the panel through.
    for (auto* image : { &spectrogramFront, &spectrogramBack })
    {
        juce::Graphics g(*image);
        g.fillAll(spectrogramFloorColour);
    }

    syncSpectrogramRange();
}

void FFTDisplay::syncSpectrogramRange() const
{
    if (! spectrogramFront.isValid())
        return;

    if (std::abs(spectrogramTopDb - topDb) < 0.01f
        && std::abs(spectrogramBottomDb - bottomDb) < 0.01f)
        return;

    spectrogramTopDb = topDb;
    spectrogramBottomDb = bottomDb;
    recolourSpectrogram();
}

void FFTDisplay::recolourSpectrogram() const
{
    if (! spectrogramFront.isValid() || historyDb.empty())
        return;

    // Repainted from the oldest frame at the top to the newest at the bottom, matching
    // the order the incremental writer produces.
    for (int y = 0; y < spectrogramFrames; ++y)
    {
        const auto sourceFrame = ((historyHead - spectrogramFrames + y) % spectrogramFrames
                                  + spectrogramFrames) % spectrogramFrames;

        for (int x = 0; x < spectrogramBands; ++x)
        {
            const auto db = historyDb[(size_t) sourceFrame * (size_t) spectrogramBands + (size_t) x];
            spectrogramFront.setPixelAt(x, y, spectrogramColourFor(db));
        }
    }

    juce::Graphics g(spectrogramBack);
    g.drawImageAt(spectrogramFront, 0, 0);
}

void FFTDisplay::pushSpectrogramFrame(const Frame& frame)
{
    ensureSpectrogramSize();

    if (! spectrogramFront.isValid())
        return;

    // Shift up one row and draw the new frame along the bottom, so the past moves up and
    // the newest measurement stays where the frequency axis is. The blit goes to the
    // second image, because an image cannot be drawn onto itself.
    {
        juce::Graphics g(spectrogramBack);
        g.drawImageAt(spectrogramFront, 0, -1);
    }

    for (int band = 0; band < spectrogramBands; ++band)
    {
        const auto level = spectrogramBandLevel(frame, band);

        historyDb[(size_t) historyHead * (size_t) spectrogramBands + (size_t) band] = level;
        spectrogramBack.setPixelAt(band, spectrogramFrames - 1, spectrogramColourFor(level));
    }

    historyHead = (historyHead + 1) % spectrogramFrames;
    std::swap(spectrogramFront, spectrogramBack);
    repaint();
}

void FFTDisplay::drawSpectrogram(juce::Graphics& g, const juce::Rectangle<float>& area) const
{
    g.setColour(panelColour);
    g.fillRect(area);

    const auto ticks = dsp::logAxisTicks(minFrequency, maxFrequency,
                                         std::max (2, (int) (area.getWidth() / 62.0f)));

    // The frequency grid goes down first: those lines mark the axis, they are not part of
    // the measurement, and a line drawn over the history would read as a signal.
    for (const auto frequency : ticks)
    {
        g.setColour(gridBoldColour.withAlpha(0.35f));
        g.drawVerticalLine(juce::roundToInt(frequencyToX(frequency, area)),
                           area.getY(), area.getBottom());
    }

    syncSpectrogramRange();

    if (spectrogramFront.isValid())
        g.drawImageAt(spectrogramFront, area.getX(), area.getY());

    // The colour scale lives in the label column, because the time axis now owns the
    // vertical space inside the plot and a heat map without a scale is only a picture.
    const auto scaleRect = juce::Rectangle<float>(area.getX() - 42.0f, area.getY(),
                                                  11.0f, area.getHeight());

    for (int y = 0; y < (int) scaleRect.getHeight(); ++y)
    {
        const auto db = topDb - (bottomDb - topDb) * (float) y
                                   / std::max (1.0f, scaleRect.getHeight() - 1.0f);

        g.setColour(spectrogramColourFor(db));
        g.drawHorizontalLine(juce::roundToInt(scaleRect.getY() + y), scaleRect.getX(),
                             scaleRect.getRight());
    }

    g.setColour(mutedColour);
    g.setFont(juce::Font(10.0f));
    g.drawText(juce::String((int) topDb), scaleRect.getX() - 32.0f, scaleRect.getY() - 7.0f,
               30.0f, 14.0f, juce::Justification::centredRight);
    g.drawText(juce::String((int) bottomDb), scaleRect.getX() - 32.0f,
               scaleRect.getBottom() - 7.0f, 30.0f, 14.0f, juce::Justification::centredRight);
    g.drawText("dB", scaleRect.getX() - 32.0f, area.getCentreY() - 8.0f, 30.0f, 16.0f,
               juce::Justification::centredRight);

    // Time runs upwards, so the newest frame is named at the bottom of the plot and the
    // oldest at the top.
    g.drawText("lama", scaleRect.getX() - 32.0f, area.getY() + 16.0f, 30.0f, 14.0f,
               juce::Justification::centredRight);
    g.drawText("baru", scaleRect.getX() - 32.0f, area.getBottom() - 30.0f, 30.0f, 14.0f,
               juce::Justification::centredRight);

    g.setFont(frequencyLabelFont());

    for (const auto frequency : ticks)
    {
        const auto x = frequencyToX(frequency, area);

        g.setColour(mutedColour);
        const auto labelWidth = frequencyLabelWidth(area);
        g.drawText(dsp::frequencyTickLabel(frequency),
                   juce::jlimit(area.getX(), area.getRight() - labelWidth, x - labelWidth * 0.5f),
                   area.getBottom() + FrequencyLabels::topOffset(),
                   labelWidth, frequencyLabelHeight(), juce::Justification::centred);
    }
}

void FFTDisplay::drawGrid(juce::Graphics& g, const juce::Rectangle<float>& area) const
{
    g.setColour(panelColour);
    g.fillRect(area);

    g.setFont(juce::Font(11.0f));

    auto valueStep = 10.0f;

    if (topDb - bottomDb <= 40.0f)
        valueStep = 5.0f;
    else if (topDb - bottomDb > 100.0f)
        valueStep = 20.0f;

    for (float value = bottomDb; value <= topDb + 0.1f; value += valueStep)
    {
        const auto y = valueToY(value, area);

        g.setColour(gridColour);
        g.fillRect(area.getX(), y, area.getWidth(), 1.0f);
        g.setColour(mutedColour);
        g.drawText(juce::String((int) std::round(value)), area.getX() - 42, y - 8, 38, 16,
                   juce::Justification::centredRight);
    }

    if (display.axis == Axis::Coherence)
    {
        for (int step = 1; step <= 4; ++step)
        {
            const auto y = valueToY((float) step * 0.25f, area);

            g.setColour(gridBoldColour);
            g.fillRect(area.getX(), y, area.getWidth(), 1.0f);
        }
    }

    if (display.axis == Axis::Phase)
    {
        for (int degree = -180; degree <= 180; degree += 60)
        {
            const auto y = valueToY((float) degree, area);

            g.setColour(degree == 0 ? gridBoldColour : gridColour);
            g.fillRect(area.getX(), y, area.getWidth(), 1.0f);
            g.setColour(mutedColour);
            g.drawText(juce::String(degree), area.getX() - 42, y - 8, 38, 16,
                       juce::Justification::centredRight);
        }
    }

    const auto labelWidth = frequencyLabelWidth (area);

    if (linearAxis)
    {
        for (int hz = 2000; hz <= (int) maxFrequency; hz += 2000)
        {
            const auto x = frequencyToX((float) hz, area);
            g.setColour(gridColour);
            g.fillRect(x, area.getY(), 1.0f, area.getHeight());
            g.setColour(mutedColour);
            g.setFont (frequencyLabelFont());
            g.drawText(juce::String(hz / 1000) + "k", x - labelWidth * 0.5f,
                       area.getBottom() + FrequencyLabels::topOffset(),
                       labelWidth, frequencyLabelHeight(), juce::Justification::centred);
        }
    }
    else
    {
        // Every band gets a frequency label. Labels are skipped only when the plot is
        // too narrow to fit them all without overlap.
        const auto count = bandFrequencies.size();
        const auto stride = juce::jmax (1, (int) std::ceil (labelWidth / minimumLabelSpacing (area)));

        for (size_t band = 0; band < count; ++band)
        {
            const auto freq = bandFrequencies[band];
            if (freq < minFrequency || freq > maxFrequency) continue;
            const auto x = frequencyToX(freq, area);

            g.setColour(gridColour);
            g.fillRect(x, area.getY(), 1.0f, area.getHeight());

            if (band % (size_t) stride != 0 && band + 1 != count)
                continue;

            g.setColour(mutedColour);
            g.setFont (frequencyLabelFont());
            g.drawText(frequencyLabel(freq), x - labelWidth * 0.5f,
                       area.getBottom() + FrequencyLabels::topOffset(),
                       labelWidth, frequencyLabelHeight(), juce::Justification::centred);
        }
    }

    g.setColour(gridBoldColour);
    g.drawRect(area, 1.0f);
}

void FFTDisplay::drawTraces(juce::Graphics& g, const juce::Rectangle<float>& area) const
{
    if (!display.valid || display.freq.empty())
        return;

    if (peakHoldEnabled && peakHoldValues.size() == display.traces.size())
    {
        for (size_t t = 0; t < display.traces.size(); ++t)
        {
            const auto& values = peakHoldValues[t];

            if (values.size() != display.freq.size())
                continue;

            juce::Path path;

            for (size_t i = 0; i < values.size(); ++i)
            {
                const auto x = frequencyToX(display.freq[i], area);
                const auto y = valueToY(values[i], area);

                if (i == 0)
                    path.startNewSubPath(x, y);
                else
                    path.lineTo(x, y);
            }

            g.setColour(display.traces[t].colour.withAlpha(0.45f));
            g.strokePath(path, juce::PathStrokeType(1.0f));
        }
    }

    size_t traceIndex = 0;
    for (const auto& trace : display.traces)
    {
        if (trace.values == nullptr || trace.values->size() != display.freq.size())
            continue;

        if (barStyle && mode == Mode::SingleChannel)
        {
            const auto count = display.freq.size();
            const auto traces = std::max<size_t> (1, display.traces.size());

            // Each bar spans from the midpoint to its neighbour, so bars tile the plot
            // exactly with no gaps and no overlap. The axis domain is widened by half a
            // band step, which is what lets the 20 Hz and 20 kHz bars sit fully inside
            // the plot instead of being cut in half by the edge.
            auto barLeft = std::vector<float> (count);
            auto barRight = std::vector<float> (count);

            for (size_t i = 0; i < count; ++i)
            {
                const auto centre = display.freq[i];
                const auto previous = i == 0 ? 0.0f : std::sqrt (display.freq[i - 1] * centre);
                const auto next = i + 1 >= count ? 0.0f : std::sqrt (centre * display.freq[i + 1]);

                barLeft[i] = i == 0 ? area.getX() : frequencyToX (previous, area);
                barRight[i] = i + 1 >= count ? area.getRight() : frequencyToX (next, area);
            }

            barLeft.front() = area.getX();
            barRight.back() = area.getRight();

            g.setColour(trace.colour.withAlpha(0.85f));

            for (size_t i = 0; i < count; ++i)
            {
                const auto slot = (barRight[i] - barLeft[i]) / (float) traces;
                const auto left = barLeft[i] + slot * (float) traceIndex;
                const auto y = valueToY((*trace.values)[i], area);

                g.fillRect (left, y, std::max (1.0f, slot - 1.0f), area.getBottom() - y);
            }

            ++traceIndex;
            continue;
        }
        juce::Path path;
        auto started = false;

        for (size_t i = 0; i < display.freq.size(); ++i)
        {
            const auto freq = display.freq[i];

            if (freq < minFrequency || freq > maxFrequency)
                continue;

            // A bin the reference never excited carries no transfer function, so the line
            // breaks there instead of drawing a run of zeros as if it were measured.
            if (!display.binValid.empty() && display.binValid[i] == 0)
            {
                started = false;
                continue;
            }

            const auto x = frequencyToX(freq, area);
            const auto y = valueToY((*trace.values)[i], area);

            if (started)
                path.lineTo(x, y);
            else
                path.startNewSubPath(x, y);

            started = true;
        }

        g.setColour(trace.colour);
        g.strokePath(path, juce::PathStrokeType(2.0f));
    }

    g.setFont(juce::Font(12.0f));

    float legendX = area.getX() + 10.0f;

    for (const auto& trace : display.traces)
    {
        g.setColour(trace.colour);
        g.drawText(trace.label, (int) legendX, (int) area.getY() + 6, 70, 18,
                   juce::Justification::centredLeft);
        legendX += 60.0f;
    }

    drawLevelReadout(g, area);
}

void FFTDisplay::drawLevelReadout(juce::Graphics& g, const juce::Rectangle<float>& area) const
{
    const auto right = area.getRight() - 6.0f;
    auto y = area.getBottom() - 42.0f;

    // One line per channel, Mic first, so the reading matches the trace colours.
    // Both figures are named explicitly: "RMS -37.7 / -2.4 dBFS" reads as a ratio
    // at a glance, and "dBFS" at the end could be mistaken for the Peak alone.
    const auto line = [] (const ChannelLevels& levels)
    {
        return "RMS " + juce::String (levels.rmsDb, 1)
             + "    Peak " + juce::String (levels.peakDb, 1) + " dBFS";
    };

    const auto aboveFloor = [this] (const ChannelLevels& l)
    {
        return l.rmsDb > dsp::dbFloor + 1.0f || l.peakDb > dsp::dbFloor + 1.0f;
    };

    g.setFont (juce::Font (13.0f, juce::Font::bold));

    if (levelsValid && aboveFloor (measLevels))
    {
        g.setColour (traceColourFor (0));
        g.drawText (measurementLabel, area.getX() + 10.0f, y, 110.0f, 18.0f,
                    juce::Justification::centredLeft);
        g.setColour (mutedColour);
        g.drawText ("RMS " + line (measLevels), area.getX() + 124.0f, y,
                    area.getWidth() - 140.0f, 18.0f, juce::Justification::centredRight);
        y += 19.0f;
    }

    if (generatorReference && aboveFloor (refLevels))
    {
        g.setColour (traceColourFor (1));
        g.drawText (referenceLabel, area.getX() + 10.0f, y, 110.0f, 18.0f,
                    juce::Justification::centredLeft);
        g.setColour (mutedColour);
        g.drawText ("RMS " + line (refLevels), area.getX() + 124.0f, y,
                    area.getWidth() - 140.0f, 18.0f, juce::Justification::centredRight);
    }

    // The delay line stays where it was, just above the plot edge.
    g.setFont (juce::Font (12.0f));
    g.setColour (mutedColour);
    g.drawText(delayAvailable ? (generatorReference ? "Delay total " : "Delay ") + juce::String(delayMs, 3) + " ms   Coherence "
                   + juce::String(averageCoherence * 100.0f, 1) + " %"
                   : (generatorReference ? "Delay total -- : menunggu suara generator di microphone"
                                         : "Delay -- : perlu sinyal referensi di kanal lain"),
               area.getX() - 6.0f, area.getBottom() - 22.0f, area.getWidth() - 8.0f, 18.0f,
               juce::Justification::centredRight);
    (void) right;
}

void FFTDisplay::drawCursor(juce::Graphics& g, const juce::Rectangle<float>& area)
{
    if (!cursorVisible || !display.valid)
        return;

    const auto index = nearestBin(cursorX, area);

    if (index < 0 || index >= (int) display.freq.size())
        return;

    const auto x = frequencyToX(display.freq[(size_t) index], area);

    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.fillRect(x, area.getY(), 1.0f, area.getHeight());
    g.fillRect(area.getX(), area.getCentreY(), area.getWidth(), 1.0f);

    const auto freq = display.freq[(size_t) index];
    juce::String text = frequencyLabel(freq) + " Hz";

    for (const auto& trace : display.traces)
    {
        if (trace.values == nullptr || trace.values->size() != display.freq.size())
            continue;

        const auto value = (*trace.values)[(size_t) index];

        if (display.axis == Axis::Coherence)
            text += "   " + trace.label + ": " + juce::String(value, 3);
        else if (display.axis == Axis::Phase)
            text += "   " + trace.label + ": " + juce::String(value, 1) + " deg";
        else
            text += "   " + trace.label + ": " + juce::String(value, 1) + " dB";
    }

    readoutLabel.setText(text, juce::dontSendNotification);

    g.setColour(panelColour.withAlpha(0.9f));
    g.fillRoundedRectangle(juce::Rectangle<float>(x + 10.0f, area.getY() + 30.0f,
                                                  juce::jmin(420.0f, area.getRight() - x - 20.0f),
                                                  26.0f), 4.0f);
    g.setColour(textColour);
    g.setFont(juce::Font(12.0f));
    g.drawText(text, juce::Rectangle<float>(x + 16.0f, area.getY() + 32.0f,
                                            juce::jmin(410.0f, area.getRight() - x - 30.0f), 22.0f),
               juce::Justification::centredLeft);
}

void FFTDisplay::mouseMove(const juce::MouseEvent& event)
{
    cursorX = event.position.x;
    cursorVisible = true;
    repaint();
}

void FFTDisplay::mouseExit(const juce::MouseEvent&)
{
    cursorVisible = false;
    readoutLabel.setText("", juce::dontSendNotification);
    repaint();
}

void FFTDisplay::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Peak Hold", true, peakHoldEnabled);
        menu.addItem(2, "Freeze", true, frozen);
        menu.addSeparator();
        menu.addItem(3, "Bersihkan Peak Hold");
        menu.addSeparator();
        menu.addItem(10, rangePresets[0].name);
        menu.addItem(11, rangePresets[1].name);
        menu.addItem(12, rangePresets[2].name);
        menu.addItem(13, rangePresets[3].name);
        menu.addItem(14, rangePresets[4].name);
        menu.addItem(15, rangePresets[5].name);

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [this] (int selection)
        {
            if (selection == 1)
                setPeakHold(!peakHoldEnabled);
            else if (selection == 2)
                setFrozen(!frozen);
            else if (selection == 3)
                clearPeakHold();
            else if (selection >= 10 && selection <= 15)
            {
                const auto index = selection - 10;
                topDb = rangePresets[index].top;
                bottomDb = rangePresets[index].bottom;
                rangeSelector.setSelectedId(index + 1, juce::dontSendNotification);
                displayDirty = true;
                repaint();
            }
        });

        return;
    }

    cursorX = event.position.x;
    cursorVisible = true;
    repaint();
}

RTASnapshot FFTDisplay::captureSnapshot(const juce::String& name) const
{
    juce::ScopedLock lock(dataLock);

    RTASnapshot result;
    result.name = name;
    result.axisLabel = getAxisLabel();
    result.calibrationText = calibration.toString();
    result.topDb = topDb;
    result.bottomDb = bottomDb;
    result.frequency = display.freq;

    for (const auto& trace : display.traces)
    {
        RTASnapshot::Trace captured;
        captured.name = trace.label;
        captured.colour = trace.colour;
        captured.values = trace.values != nullptr && trace.values->size() == display.freq.size()
                              ? *trace.values
                              : std::vector<float>();

        result.traces.push_back (std::move (captured));
    }

    return result;
}

bool FFTDisplay::writeSnapshotTo(const juce::File& file) const
{
    return captureSnapshot (file.getFileNameWithoutExtension()).writeTo (file);
}

void FFTDisplay::showSnapshot(const RTASnapshot& newSnapshot)
{
    snapshot = newSnapshot;
    snapshotActive = true;

    juce::ScopedLock lock(dataLock);

    display.freq = snapshot.frequency;
    display.traces.clear();
    display.valid = snapshot.isUsable();
    display.axis = snapshot.axisLabel.contains ("Coherence") ? Axis::Coherence
                    : snapshot.axisLabel.contains ("Phase") ? Axis::Phase
                    : Axis::Decibels;

    // The snapshot owns its own vectors, so they are copied out here for the
    // lifetime of the component rather than into the reused smoothing buffers.
    snapshotTraces.clear();
    snapshotTraces.reserve (snapshot.traces.size());

    for (const auto& trace : snapshot.traces)
        snapshotTraces.push_back (trace.values);

    for (size_t t = 0; t < snapshotTraces.size(); ++t)
    {
        const auto colour = t < snapshot.traces.size() ? snapshot.traces[t].colour : juce::Colours::white;
        const auto label = t < snapshot.traces.size() ? snapshot.traces[t].name : "Trace";
        display.traces.push_back ({ &snapshotTraces[t], colour, label });
    }

    peakHoldValues.clear();
    repaint();
}

void FFTDisplay::clearSnapshot()
{
    snapshotActive = false;
    snapshotTraces.clear();
    displayDirty = true;
    repaint();
}

void FFTDisplay::writeCsv(juce::OutputStream& stream) const
{
    juce::ScopedLock lock(dataLock);

    if (!display.valid)
        return;

    stream.writeText("frequency_hz,ch1_db,ch2_db,transfer_db,phase_deg,coherence\n", false, false, nullptr);

    const auto size = current.freq.size();

    for (size_t i = 0; i < size; ++i)
    {
        juce::String line;
        line << juce::String(current.freq[i], 3) << ","
             << (i < current.refDb.size() ? juce::String(current.refDb[i], 3) : juce::String(0.0)) << ","
             << (i < current.measDb.size() ? juce::String(current.measDb[i], 3) : juce::String(0.0)) << ","
             << (i < current.tfDb.size() ? juce::String(current.tfDb[i], 3) : juce::String(0.0)) << ","
             << (i < current.tfPhase.size() ? juce::String(current.tfPhase[i], 3) : juce::String(0.0)) << ","
             << (i < current.coherence.size() ? juce::String(current.coherence[i], 4) : juce::String(0.0));

        stream.writeText(line + "\n", false, false, nullptr);
    }
}
