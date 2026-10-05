#include "RtaDisplay.h"
#include "DSP.h"
#include <cmath>

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff383838);
    const juce::Colour gridBoldColour = juce::Colour(0xff555555);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour bandColour = juce::Colour(0xff4fc3f7);
    const juce::Colour bandTopColour = juce::Colour(0xff81d4fa);
    const juce::Colour peakHoldColour = juce::Colour(0xffffb74d);
    const juce::Colour minMaxColour = juce::Colour(0xff9ccc65);
    const juce::Colour cursorColour = juce::Colour(0xffee82ee);
    const juce::Colour unresolvedColour = juce::Colour(0xffe53935);

    juce::String formatFrequency (float frequency)
    {
        if (frequency >= 1000.0f)
        {
            const auto value = frequency / 1000.0f;
            return juce::String (value, std::abs (value - std::round (value)) < 0.05f ? 0 : 1) + "k";
        }

        return juce::String (std::max (1, juce::roundToInt (frequency)));
    }
}

RtaDisplay::RtaDisplay()
{
    const auto resolutionNames = dsp::RtaAnalyser::getResolutionNames();

    for (int i = 0; i < resolutionNames.size(); ++i)
        resolutionSelector.addItem (resolutionNames[i], i + 1);

    resolutionSelector.setSelectedId ((int) resolution + 1);
    resolutionSelector.setTooltip ("Lebar band. 1/3 oktaf adalah standar pengukuran; "
                                   "1/6 dan 1/12 lebih halus tetapi butuh FFT lebih besar.");
    resolutionSelector.onChange = [this]
    {
        resolution = (dsp::RtaAnalyser::Resolution) (resolutionSelector.getSelectedId() - 1);

        if (onSettingsChanged != nullptr)
            onSettingsChanged();

        repaint();
    };

    // The offered ranges are the ones a measurement is actually made in: the full audible
    // band, one driver at the bottom, and everything above the bass where a room problem
    // usually is.
    struct Range { float low; float high; juce::String label; };

    const Range ranges[] =
    {
        { 20.0f,  20000.0f, "20 Hz - 20 kHz" },
        { 20.0f,  2000.0f,  "20 Hz - 2 kHz" },
        { 100.0f, 20000.0f, "100 Hz - 20 kHz" },
        { 200.0f, 20000.0f, "200 Hz - 20 kHz" }
    };

    for (const auto& range : ranges)
        rangeSelector.addItem (range.label, rangeSelector.getNumItems() + 1);

    rangeSelector.setSelectedItemIndex (0);
    rangeSelector.setTooltip ("Rentang band yang ditampilkan. Persempit ke satu driver "
                              "supaya band lain tidak menutupi masalahnya.");
    rangeSelector.onChange = [this, ranges]
    {
        const auto index = rangeSelector.getSelectedItemIndex();

        if (index >= 0 && index < 4)
        {
            rangeLow = ranges[index].low;
            rangeHigh = ranges[index].high;

            if (onSettingsChanged != nullptr)
                onSettingsChanged();
        }

        repaint();
    };

    peakHoldButton.setToggleState (true, juce::dontSendNotification);
    peakHoldButton.setTooltip ("Tahan level tertinggi tiap band.");
    peakHoldButton.onClick = [this] { showPeakHold = peakHoldButton.getToggleState(); repaint(); };

    minMaxButton.setToggleState (false, juce::dontSendNotification);
    minMaxButton.setTooltip ("Tandai level terendah dan tertinggi tiap band.");
    minMaxButton.onClick = [this] { showMinMax = minMaxButton.getToggleState(); repaint(); };

    valuesButton.setToggleState (false, juce::dontSendNotification);
    valuesButton.setTooltip ("Tuliskan angka dB di dalam tiap band.");
    valuesButton.onClick = [this] { showBandValues = valuesButton.getToggleState(); repaint(); };

    clearHoldButton.setTooltip ("Hapus tahanan peak.");
    clearHoldButton.onClick = [this]
    {
        if (onClearPeakHold != nullptr)
            onClearPeakHold();
    };

    cursorLabel.setFont (juce::Font (13.0f, juce::Font::bold));
    cursorLabel.setColour (juce::Label::textColourId, cursorColour);
    cursorLabel.setJustificationType (juce::Justification::centredLeft);

    infoLabel.setFont (juce::Font (12.0f));
    infoLabel.setColour (juce::Label::textColourId, mutedColour);
    infoLabel.setJustificationType (juce::Justification::centredRight);

    addAndMakeVisible (resolutionSelector);
    addAndMakeVisible (rangeSelector);
    addAndMakeVisible (peakHoldButton);
    addAndMakeVisible (minMaxButton);
    addAndMakeVisible (valuesButton);
    addAndMakeVisible (clearHoldButton);
    addAndMakeVisible (cursorLabel);
    addAndMakeVisible (infoLabel);
}

void RtaDisplay::setBands (const std::vector<dsp::RtaAnalyser::Band>& newBands,
                            const std::vector<float>& newFrequencies,
                            const juce::String& statusText)
{
    bands = newBands;
    bandFrequencies = newFrequencies;
    statusLine = statusText;

    updateReadout();
    repaint();
}

void RtaDisplay::setResolution (dsp::RtaAnalyser::Resolution newResolution)
{
    resolution = newResolution;
    resolutionSelector.setSelectedId ((int) resolution + 1, juce::dontSendNotification);
}

void RtaDisplay::setRange (float lowFrequency, float highFrequency)
{
    rangeLow = lowFrequency;
    rangeHigh = highFrequency;

    for (int i = 0; i < rangeSelector.getNumItems(); ++i)
        if (lowFrequency == 100.0f && i == 2)
            rangeSelector.setSelectedItemIndex (i, juce::dontSendNotification);
}

juce::Rectangle<float> RtaDisplay::plotArea() const
{
    auto area = getLocalBounds().toFloat().reduced (14.0f);
    area.removeFromTop (30.0f);

    return area.withTrimmedLeft (52.0f).withTrimmedBottom (44.0f);
}

float RtaDisplay::frequencyToX (float frequency, const juce::Rectangle<float>& bounds) const
{
    const auto clamped = juce::jlimit (rangeLow, rangeHigh, frequency);
    const auto position = (std::log10 (clamped) - std::log10 (rangeLow))
                        / (std::log10 (rangeHigh) - std::log10 (rangeLow));
    return bounds.getX() + position * bounds.getWidth();
}

float RtaDisplay::xToFrequency (float x, const juce::Rectangle<float>& bounds) const
{
    const auto position = juce::jlimit (0.0f, 1.0f,
                                        (x - bounds.getX()) / juce::jmax (1.0f, bounds.getWidth()));
    return std::pow (10.0f, std::log10 (rangeLow)
                          + position * (std::log10 (rangeHigh) - std::log10 (rangeLow)));
}

float RtaDisplay::levelToY (float db, const juce::Rectangle<float>& bounds) const
{
    const auto value = juce::jlimit (bottomDb, topDb, db);
    return bounds.getBottom() - (value - bottomDb) / (topDb - bottomDb) * bounds.getHeight();
}

void RtaDisplay::drawGrid (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    g.setFont (juce::Font (11.0f));

    for (int db = (int) bottomDb; db <= (int) topDb; db += 6)
    {
        const auto y = levelToY ((float) db, bounds);

        g.setColour (db == 0 ? gridBoldColour : gridColour);
        g.drawHorizontalLine (juce::roundToInt (y), bounds.getX(), bounds.getRight());

        g.setColour (mutedColour);
        g.drawText (juce::String (db), bounds.getX() - 50.0f, y - 7.0f, 44.0f, 14.0f,
                    juce::Justification::centredRight);
    }

    // The axis title says what the scale is, because a bare number beside a level plot is
    // read as SPL by anyone who has used a meter before.
    g.setColour (mutedColour);
    g.setFont (juce::Font (12.0f));
    g.drawText ("dB", 4.0f, bounds.getY() - 6.0f, 44.0f, 16.0f, juce::Justification::centredRight);
}

void RtaDisplay::drawBands (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    if (bands.empty())
    {
        g.setColour (mutedColour);
        g.setFont (juce::Font (16.0f));
        g.drawText ("Menunggu audio input", bounds, juce::Justification::centred);
        return;
    }

    // Each band is drawn between its own edges rather than between its centre and the
    // next, so the tiles tile the axis with no gap and no overlap.
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];

        auto low = i == 0 ? rangeLow
                          : std::sqrt (bandFrequencies[i - 1] * bandFrequencies[i]);
        auto high = i + 1 >= bandFrequencies.size() ? rangeHigh
                                                    : std::sqrt (bandFrequencies[i] * bandFrequencies[i + 1]);

        const auto x = frequencyToX (low, bounds);
        const auto xHigh = frequencyToX (high, bounds);
        const auto width = std::max (1.0f, xHigh - x - 1.0f);
        const auto top = levelToY (band.levelDb, bounds);
        const auto bottom = bounds.getBottom();

        if (band.levelDb > bottomDb + 0.5f)
        {
            // A band the FFT size cannot resolve is drawn in red. Reporting it in the
            // normal colour would let a number that belongs to a wider range be read as a
            // measurement of this band.
            g.setColour (band.resolved ? bandColour : unresolvedColour.withAlpha (0.5f));
            g.fillRect (x, top, width, std::max (1.0f, bottom - top));

            // A lighter cap on top of the fill reads as the current level without needing
            // a second series for it.
            g.setColour (band.resolved ? bandTopColour : unresolvedColour);
            g.fillRect (x, top, width, 2.0f);
        }

        if (showPeakHold && band.peakHoldDb > bottomDb + 0.5f)
        {
            const auto y = levelToY (band.peakHoldDb, bounds);
            g.setColour (peakHoldColour);
            g.fillRect (x, y - 1.0f, width, 2.0f);
        }

        if (showMinMax)
        {
            if (band.maxDb > bottomDb + 0.5f)
            {
                g.setColour (minMaxColour);
                g.fillRect (x, levelToY (band.maxDb, bounds) - 1.0f, width, 1.5f);
            }

            if (band.minDb > bottomDb + 0.5f)
            {
                g.setColour (minMaxColour.withAlpha (0.6f));
                g.fillRect (x, levelToY (band.minDb, bounds), width, 1.5f);
            }
        }

        if (showBandValues && width > 22.0f)
        {
            g.setFont (juce::Font (10.0f));
            g.setColour (band.levelDb > bottomDb + 12.0f ? juce::Colour(0xff1a1a1a) : mutedColour);
            g.drawText (juce::String (band.levelDb, 0), x, top + 4.0f, width, 12.0f,
                        juce::Justification::centred);
        }
    }
}

void RtaDisplay::drawBandLabels (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    // At 1/24 octave there are hundreds of bands, so every label is thinned until the
    // remaining ones fit instead of being drawn on top of each other.
    const auto perBand = (float) bounds.getWidth() / (float) std::max<size_t> (1, bands.size());
    const auto step = std::max<size_t> (1, (size_t) std::ceil (58.0f / std::max (1.0f, perBand)));

    g.setFont (juce::Font (11.0f));

    for (size_t i = 0; i < bands.size(); i += step)
    {
        const auto x = frequencyToX (bands[i].frequency, bounds);

        g.setColour (gridBoldColour.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());

        g.setColour (mutedColour);
        g.drawText (formatFrequency (bands[i].frequency), (int) x - 24, (int) bounds.getBottom() + 4,
                    48, 14, juce::Justification::centred);
    }

    g.setColour (mutedColour);
    g.setFont (juce::Font (12.0f));
    g.drawText (formatFrequency (rangeLow) + " Hz sampai " + formatFrequency (rangeHigh)
                    + " Hz, sumbu logaritmik | " + dsp::RtaAnalyser::resolutionToString (resolution)
                    + " oktaf",
                (int) bounds.getX(), (int) bounds.getBottom() + 20, (int) bounds.getWidth(), 16,
                juce::Justification::centredLeft);
}

void RtaDisplay::drawCursor (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    if (! cursorVisible)
        return;

    const auto x = juce::jlimit (bounds.getX(), bounds.getRight(), cursorX);

    g.setColour (cursorColour.withAlpha (0.75f));
    g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());
}

int RtaDisplay::nearestBand (float frequency) const
{
    if (bands.empty())
        return -1;

    auto nearest = 0;

    for (size_t i = 1; i < bands.size(); ++i)
        if (std::abs (bands[i].frequency - frequency)
              < std::abs (bands[(size_t) nearest].frequency - frequency))
            nearest = (int) i;

    return nearest;
}

void RtaDisplay::updateReadout()
{
    if (bands.empty())
    {
        cursorLabel.setText ("Kursor: -", juce::dontSendNotification);
        infoLabel.setText (statusLine, juce::dontSendNotification);
        return;
    }

    if (cursorVisible)
    {
        const auto index = nearestBand (xToFrequency (cursorX, plotArea()));

        if (index >= 0)
        {
            const auto& band = bands[(size_t) index];
            juce::String text = formatFrequency (band.frequency) + "   "
                              + juce::String (band.levelDb, 1) + " dB";

            // The bin count is on screen because a band that only saw one bin at 1/24
            // octave is not a measurement, and the reader should be able to see that
            // without counting bins themselves.
            text += "   " + juce::String (band.binCount) + " bin";
            text += band.resolved ? "" : " (tidak terukur)";
            cursorLabel.setText (text, juce::dontSendNotification);
        }
    }
    else
    {
        cursorLabel.setText ("Kursor: arahkan ke band", juce::dontSendNotification);
    }

    infoLabel.setText (statusLine, juce::dontSendNotification);
}

void RtaDisplay::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto bar = getLocalBounds().reduced (14);
    auto controls = bar.removeFromTop (30);

    resolutionSelector.setBounds (controls.removeFromLeft (120).reduced (0, 2));
    controls.removeFromLeft (8);
    rangeSelector.setBounds (controls.removeFromLeft (180).reduced (0, 2));
    controls.removeFromLeft (8);
    peakHoldButton.setBounds (controls.removeFromLeft (100).reduced (0, 4));
    controls.removeFromLeft (4);
    minMaxButton.setBounds (controls.removeFromLeft (80).reduced (0, 4));
    controls.removeFromLeft (4);
    valuesButton.setBounds (controls.removeFromLeft (70).reduced (0, 4));
    controls.removeFromLeft (4);
    clearHoldButton.setBounds (controls.removeFromLeft (100).reduced (0, 4));
    controls.removeFromLeft (12);
    cursorLabel.setBounds (controls.removeFromLeft (320).reduced (0, 4));
    infoLabel.setBounds (controls.reduced (0, 4));

    const auto bounds = plotArea();

    g.setColour (panelColour);
    g.fillRoundedRectangle (bounds.reduced (-6.0f, -6.0f), 6.0f);

    drawGrid (g, bounds);
    drawBands (g, bounds);
    drawCursor (g, bounds);
    drawBandLabels (g, bounds);
}

void RtaDisplay::resized()
{
    repaint();
}

void RtaDisplay::mouseMove (const juce::MouseEvent& event)
{
    cursorX = event.position.x;
    cursorPosition = event.position;
    cursorVisible = plotArea().contains (cursorPosition);
    updateReadout();
    repaint();
}

void RtaDisplay::mouseExit (const juce::MouseEvent&)
{
    cursorVisible = false;
    updateReadout();
    repaint();
}

void RtaDisplay::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Peak Hold", true, showPeakHold);
        menu.addItem (2, "Min/Max", true, showMinMax);
        menu.addItem (3, "Nilai dB di band", true, showBandValues);
        menu.addSeparator();
        menu.addItem (4, "Reset Peak Hold");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this] (int selection)
        {
            switch (selection)
            {
                case 1: peakHoldButton.setToggleState (!showPeakHold, juce::dontSendNotification);
                        showPeakHold = !showPeakHold; repaint(); break;
                case 2: minMaxButton.setToggleState (!showMinMax, juce::dontSendNotification);
                        showMinMax = !showMinMax; repaint(); break;
                case 3: valuesButton.setToggleState (!showBandValues, juce::dontSendNotification);
                        showBandValues = !showBandValues; repaint(); break;
                case 4: if (onClearPeakHold != nullptr) onClearPeakHold(); break;
            }
        });

        return;
    }

    cursorX = event.position.x;
    cursorPosition = event.position;
    cursorVisible = plotArea().contains (cursorPosition);
    updateReadout();
    repaint();
}
