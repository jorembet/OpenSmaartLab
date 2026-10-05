#include "SpectrumAnalyserDisplay.h"
#include "DSP.h"
#include "FrequencyLabels.h"
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
    const juce::Colour traceColour = juce::Colour(0xff4fc3f7);
    const juce::Colour cursorColour = juce::Colour(0xffffb74d);

    constexpr float minFrequency = 20.0f;
    constexpr float maxFrequency = 20000.0f;
}

SpectrumAnalyserDisplay::SpectrumAnalyserDisplay()
{
    for (const auto size : { 1024, 2048, 4096, 8192, 16384, 32768 })
        fftSizeSelector.addItem (juce::String (size), size);

    fftSizeSelector.setSelectedId (2048);
    fftSizeSelector.setTooltip ("Ukuran FFT. Makin besar, resolusi frekuensi makin halus "
                                "dan respon impuls makin panjang.");

    const auto windowNames = dsp::SpectrumAnalyser::getWindowNames();

    for (int i = 0; i < windowNames.size(); ++i)
        windowSelector.addItem (windowNames[i], i + 1);

    windowSelector.setSelectedId ((int) dsp::SpectrumAnalyser::Window::Hann + 1);
    windowSelector.setTooltip ("Jendela. Setiap jendela punya karakter berbeda: yang lebar "
                               "resolusinya bagus tapi levelnya melebar.");

    const auto averagingNames = dsp::SpectrumAnalyser::getAveragingNames();

    for (int i = 0; i < averagingNames.size(); ++i)
        averagingSelector.addItem (averagingNames[i], i + 1);

    averagingSelector.setSelectedId ((int) dsp::SpectrumAnalyser::Averaging::Exponential + 1);
    averagingSelector.setTooltip ("Rata-rata. Exponential diberi bobot lebih besar pada "
                                  "frame terbaru dan membuat kurva jauh lebih stabil.");

    averagingSlider.setRange (0.05f, 10.0f, 0.05f);
    averagingSlider.setValue (0.5f, juce::dontSendNotification);
    averagingSlider.setTextValueSuffix (" s");
    averagingSlider.setTooltip ("Konstanta waktu rata-rata exponential.");

    overlapSlider.setRange (0.0f, 90.0f, 5.0f);
    overlapSlider.setValue (50.0f, juce::dontSendNotification);
    overlapSlider.setTextValueSuffix (" %");
    overlapSlider.setTooltip ("Tumpang tindih antar frame. 50 % memberi frame rate dua kali "
                              "lebih cepat tanpa kehilangan informasi.");

    cursorLabel.setFont (juce::Font (13.0f, juce::Font::bold));
    cursorLabel.setColour (juce::Label::textColourId, cursorColour);
    cursorLabel.setJustificationType (juce::Justification::centredLeft);

    peakLabel.setFont (juce::Font (13.0f));
    peakLabel.setColour (juce::Label::textColourId, mutedColour);
    peakLabel.setJustificationType (juce::Justification::centredRight);

    // The display owns these controls, so it reports a change rather than letting the
    // owner reach in and wire them up. Every setting the worker needs goes through the
    // same path, whichever control moved.
    const auto changed = [this] (bool fftSize, bool windowChanged, bool averaging)
    {
        if (fftSize && onFftSizeChanged != nullptr)         onFftSizeChanged();
        if (windowChanged && onWindowChanged != nullptr)    onWindowChanged();
        if (averaging && onAveragingChanged != nullptr)     onAveragingChanged();

        repaint();
    };

    fftSizeSelector.onChange = [this, changed] { changed (true, false, false); };
    windowSelector.onChange = [this, changed]
    {
        windowType = (dsp::SpectrumAnalyser::Window) (windowSelector.getSelectedId() - 1);
        changed (false, true, false);
    };
    averagingSelector.onChange = [this, changed]
    {
        averagingMode = (dsp::SpectrumAnalyser::Averaging) (averagingSelector.getSelectedId() - 1);
        changed (false, false, true);
    };
    averagingSlider.onValueChange = [this, changed] { changed (false, false, true); };
    overlapSlider.onValueChange = [this, changed] { changed (false, false, true); };

    addAndMakeVisible (fftSizeSelector);
    addAndMakeVisible (windowSelector);
    addAndMakeVisible (averagingSelector);
    addAndMakeVisible (averagingSlider);
    addAndMakeVisible (overlapSlider);
    addAndMakeVisible (cursorLabel);
    addAndMakeVisible (peakLabel);
}

void SpectrumAnalyserDisplay::setFrame (const dsp::SpectrumAnalyser::Frame& frame)
{
    if (! frame.valid)
        return;

    frequency = frame.frequency;
    levelDb = frame.averagedDb;

    statusLine = "FFT " + juce::String (frame.fftSize)
               + " | hop " + juce::String (frame.hopSize)
               + " | frame " + juce::String (frame.framesProcessed);

    if (frame.framesDropped > 0)
        statusLine += " | frame lewat " + juce::String (frame.framesDropped);

    updateReadout();
    repaint();
}

void SpectrumAnalyserDisplay::setSpectrum (const std::vector<float>& frequencies,
                                           const std::vector<float>& levelsDb,
                                           const juce::String& status)
{
    if (frequencies.size() < 2 || frequencies.size() != levelsDb.size())
        return;

    frequency = frequencies;
    levelDb = levelsDb;
    statusLine = status;

    updateReadout();
    repaint();
}

void SpectrumAnalyserDisplay::setStatus (const juce::String& text)
{
    statusLine = text;
    repaint();
}

void SpectrumAnalyserDisplay::setFftSize (int fftSize)
{
    fftSizeSelector.setSelectedId (fftSize, juce::dontSendNotification);
}
void SpectrumAnalyserDisplay::setWindowType (dsp::SpectrumAnalyser::Window window)
{
    windowType = window;
    windowSelector.setSelectedId ((int) window + 1, juce::dontSendNotification);
}

void SpectrumAnalyserDisplay::setAveragingMode (dsp::SpectrumAnalyser::Averaging mode)
{
    averagingMode = mode;
    averagingSelector.setSelectedId ((int) mode + 1, juce::dontSendNotification);
}

void SpectrumAnalyserDisplay::setAveragingSeconds (float seconds)
{
    averagingSlider.setValue (juce::jlimit (0.05f, 10.0f, seconds),
                              juce::dontSendNotification);
}

void SpectrumAnalyserDisplay::setOverlapPercent (float percent)
{
    overlapSlider.setValue (juce::jlimit (0.0f, 90.0f, percent), juce::dontSendNotification);
}

juce::Rectangle<float> SpectrumAnalyserDisplay::plotArea() const
{
    auto area = getLocalBounds().toFloat().reduced (14.0f);
    area.removeFromTop (34.0f);

    // Room for the decibel scale on the left and the frequency labels underneath.
    return area.withTrimmedLeft (52.0f)
                .withTrimmedBottom (FrequencyLabels::reservedSpace() + 26.0f);
}

float SpectrumAnalyserDisplay::frequencyToX (float frequency,
                                              const juce::Rectangle<float>& bounds) const
{
    const auto clamped = juce::jlimit (minFrequency, maxFrequency, frequency);
    const auto position = (std::log10 (clamped) - std::log10 (minFrequency))
                        / (std::log10 (maxFrequency) - std::log10 (minFrequency));
    return bounds.getX() + position * bounds.getWidth();
}

float SpectrumAnalyserDisplay::xToFrequency (float x,
                                              const juce::Rectangle<float>& bounds) const
{
    const auto position = juce::jlimit (0.0f, 1.0f,
                                        (x - bounds.getX()) / juce::jmax (1.0f, bounds.getWidth()));
    return std::pow (10.0f, std::log10 (minFrequency)
                          + position * (std::log10 (maxFrequency) - std::log10 (minFrequency)));
}

float SpectrumAnalyserDisplay::levelToY (float db, const juce::Rectangle<float>& bounds) const
{
    const auto value = juce::jlimit (bottomDb, topDb, db);
    return bounds.getBottom() - (value - bottomDb) / (topDb - bottomDb) * bounds.getHeight();
}

void SpectrumAnalyserDisplay::drawGrid (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    g.setFont (juce::Font (11.0f));

    for (int db = (int) bottomDb; db <= (int) topDb; db += 20)
    {
        const auto y = levelToY ((float) db, bounds);

        g.setColour (db == 0 ? gridBoldColour : gridColour);
        g.drawHorizontalLine (juce::roundToInt (y), bounds.getX(), bounds.getRight());

        g.setColour (mutedColour);
        g.drawText (juce::String (db), bounds.getX() - 50.0f, y - 7.0f, 44.0f, 14.0f,
                    juce::Justification::centredRight);
    }

    g.setColour (mutedColour);
    g.setFont (juce::Font (12.0f));
    g.drawText ("dB", 4.0f, bounds.getY() - 6.0f, 44.0f, 16.0f, juce::Justification::centredRight);
}

void SpectrumAnalyserDisplay::drawFrequencyLabels (juce::Graphics& g,
                                                   const juce::Rectangle<float>& bounds)
{
    const auto ticks = dsp::logAxisTicks (minFrequency, maxFrequency,
                                          std::max (2, (int) (bounds.getWidth() / 62.0f)));

    g.setFont (FrequencyLabels::font());

    for (const auto tick : ticks)
    {
        const auto x = frequencyToX (tick, bounds);

        g.setColour (gridBoldColour);
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());

        const auto labelX = juce::jlimit (bounds.getX(), bounds.getRight() - FrequencyLabels::width(),
                                          x - FrequencyLabels::width() * 0.5f);

        g.setColour (mutedColour);
        g.drawText (dsp::frequencyTickLabel (tick), (int) labelX,
                    (int) (bounds.getBottom() + 4.0f),
                    (int) FrequencyLabels::width(), (int) FrequencyLabels::height(),
                    juce::Justification::centred);
    }

    g.setColour (mutedColour);
    g.setFont (juce::Font (12.0f));
    g.drawText ("20 Hz sampai 20 kHz, sumbu logaritmik",
                bounds.getX(), bounds.getBottom() + 20.0f, bounds.getWidth(), 16.0f,
                juce::Justification::centredRight);
}

void SpectrumAnalyserDisplay::drawCurve (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    if (frequency.size() < 2 || frequency.size() != levelDb.size())
        return;

    juce::Path path;
    auto started = false;
    const auto columns = juce::jmax (1, (int) std::ceil (bounds.getWidth()));

    if (frequency.size() > (size_t) columns * 2)
    {
        // The FFT may contain tens of thousands of bins, while the display has only a few
        // hundred horizontal pixels. Keep each pixel's strongest bin so narrow tones stay
        // visible without building a path point for every hidden sub-pixel bin.
        for (int column = 0; column < columns; ++column)
        {
            const auto leftX = bounds.getX() + bounds.getWidth() * (float) column / (float) columns;
            const auto rightX = bounds.getX() + bounds.getWidth() * (float) (column + 1)
                                                        / (float) columns;
            const auto low = std::max (minFrequency, xToFrequency (leftX, bounds));
            const auto high = std::min (maxFrequency, xToFrequency (rightX, bounds));
            const auto first = std::lower_bound (frequency.begin(), frequency.end(), low);
            const auto last = std::upper_bound (first, frequency.end(), high);

            if (first == last)
                continue;

            auto strongest = first;
            for (auto bin = first + 1; bin != last; ++bin)
                if (levelDb[(size_t) (bin - frequency.begin())]
                    > levelDb[(size_t) (strongest - frequency.begin())])
                    strongest = bin;

            const auto index = (size_t) (strongest - frequency.begin());
            const auto x = frequencyToX (*strongest, bounds);
            const auto y = levelToY (levelDb[index], bounds);

            if (started)
                path.lineTo (x, y);
            else
            {
                path.startNewSubPath (x, y);
                started = true;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < frequency.size(); ++i)
        {
            // Only the audible band is drawn: the bins below 20 Hz carry the interface's own
            // offset and would otherwise fill the left of the plot with a wall of noise.
            if (frequency[i] < minFrequency || frequency[i] > maxFrequency)
                continue;

            const auto x = frequencyToX (frequency[i], bounds);
            const auto y = levelToY (levelDb[i], bounds);

            if (started)
                path.lineTo (x, y);
            else
            {
                path.startNewSubPath (x, y);
                started = true;
            }
        }
    }

    g.setColour (traceColour);
    g.strokePath (path, juce::PathStrokeType (1.6f));
}

void SpectrumAnalyserDisplay::drawCursor (juce::Graphics& g, const juce::Rectangle<float>& bounds)
{
    if (! cursorVisible)
        return;

    const auto x = juce::jlimit (bounds.getX(), bounds.getRight(), cursorX);

    g.setColour (cursorColour.withAlpha (0.8f));
    g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());

    // The marker on the curve itself, so the reading points at the value it reports.
    if (! frequency.empty())
    {
        const auto wanted = xToFrequency (x, bounds);
        const auto upper = std::lower_bound (frequency.begin(), frequency.end(), wanted);
        auto nearest = (int) std::distance (frequency.begin(), upper);

        if (nearest >= (int) frequency.size())
            nearest = (int) frequency.size() - 1;
        else if (nearest > 0
                 && std::abs (frequency[(size_t) nearest - 1] - wanted)
                    <= std::abs (frequency[(size_t) nearest] - wanted))
            --nearest;

        const auto y = levelToY (levelDb[(size_t) nearest], bounds);
        g.setColour (cursorColour);
        g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
    }
}

void SpectrumAnalyserDisplay::updateReadout()
{
    if (frequency.size() < 2 || frequency.size() != levelDb.size())
    {
        cursorLabel.setText ("Kursor: -", juce::dontSendNotification);
        peakLabel.setText ("Puncak: -", juce::dontSendNotification);
        return;
    }

    if (cursorVisible)
    {
        const auto wanted = xToFrequency (cursorX, plotArea());
        const auto upper = std::lower_bound (frequency.begin(), frequency.end(), wanted);
        auto nearest = (int) std::distance (frequency.begin(), upper);

        if (nearest >= (int) frequency.size())
            nearest = (int) frequency.size() - 1;
        else if (nearest > 0
                 && std::abs (frequency[(size_t) nearest - 1] - wanted)
                    <= std::abs (frequency[(size_t) nearest] - wanted))
            --nearest;

        cursorLabel.setText (juce::String (dsp::frequencyTickLabel (frequency[(size_t) nearest]))
                             + "   " + juce::String (levelDb[(size_t) nearest], 1) + " dB",
                             juce::dontSendNotification);
    }
    else
    {
        cursorLabel.setText ("Kursor: arahkan ke kurva", juce::dontSendNotification);
    }

    const auto firstAudible = std::lower_bound (frequency.begin(), frequency.end(), minFrequency);
    if (firstAudible == frequency.end())
    {
        peakLabel.setText ("Puncak: -", juce::dontSendNotification);
        return;
    }

    const auto firstAudibleIndex = (size_t) std::distance (frequency.begin(), firstAudible);
    const auto peakLevel = std::max_element (levelDb.begin() + (ptrdiff_t) firstAudibleIndex,
                                              levelDb.end());
    const auto peakIndex = (int) std::distance (levelDb.begin(), peakLevel);

    peakLabel.setText ("Puncak " + juce::String (dsp::frequencyTickLabel (frequency[(size_t) peakIndex]))
                       + "  " + juce::String (levelDb[(size_t) peakIndex], 1) + " dB",
                       juce::dontSendNotification);
}

void SpectrumAnalyserDisplay::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto bar = getLocalBounds().reduced (14);
    const auto bounds = plotArea();

    g.setColour (panelColour);
    g.fillRoundedRectangle (bounds.reduced (-6.0f, -6.0f).withTrimmedBottom (24.0f), 6.0f);

    drawGrid (g, bounds);

    if (levelDb.size() < 2)
    {
        g.setColour (mutedColour);
        g.setFont (juce::Font (16.0f));
        g.drawText ("Menunggu audio input", bounds, juce::Justification::centred);
    }

    drawCurve (g, bounds);
    drawCursor (g, bounds);
    drawFrequencyLabels (g, bounds);

    g.setColour (mutedColour);
    g.setFont (juce::Font (12.0f));
    g.drawText (statusLine, (int) bounds.getX(), (int) (bar.getBottom() - 18),
                (int) bounds.getWidth(), 16, juce::Justification::centredRight);
}

void SpectrumAnalyserDisplay::resized()
{
    auto controls = getLocalBounds().reduced (14).removeFromTop (30);
    fftSizeSelector.setBounds (controls.removeFromLeft (110).reduced (0, 2));
    controls.removeFromLeft (8);
    windowSelector.setBounds (controls.removeFromLeft (130).reduced (0, 2));
    controls.removeFromLeft (8);
    averagingSelector.setBounds (controls.removeFromLeft (120).reduced (0, 2));
    controls.removeFromLeft (8);
    averagingSlider.setBounds (controls.removeFromLeft (160).reduced (0, 2));
    controls.removeFromLeft (8);
    overlapSlider.setBounds (controls.removeFromLeft (160).reduced (0, 2));
    controls.removeFromLeft (12);
    cursorLabel.setBounds (controls.removeFromLeft (220).reduced (0, 4));
    peakLabel.setBounds (controls.reduced (0, 4));
    repaint();
}

void SpectrumAnalyserDisplay::mouseMove (const juce::MouseEvent& event)
{
    cursorX = event.position.x;
    cursorPosition = event.position;
    cursorVisible = plotArea().contains (cursorPosition);
    updateReadout();
    repaint();
}

void SpectrumAnalyserDisplay::mouseExit (const juce::MouseEvent&)
{
    cursorVisible = false;
    updateReadout();
    repaint();
}
