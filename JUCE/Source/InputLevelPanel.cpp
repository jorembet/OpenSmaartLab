#include "InputLevelPanel.h"
#include <algorithm>
#include <cmath>

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff404040);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour micColour = juce::Colour(0xff4fc3f7);
    const juce::Colour referenceColour = juce::Colour(0xff9ccc65);
    const juce::Colour holdColour = juce::Colour(0xffffb74d);
    const juce::Colour warningColour = juce::Colour(0xffe53935);

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterCeilingDb = 0.0f;

    float meterPosition (float levelDb)
    {
        return juce::jlimit (0.0f, 1.0f,
                             (levelDb - meterFloorDb) / (meterCeilingDb - meterFloorDb));
    }

    juce::Colour levelColour (float levelDb)
    {
        if (levelDb >= -1.0f)  return warningColour;
        if (levelDb >= -6.0f)  return holdColour;
        if (levelDb >= -18.0f) return juce::Colour(0xff66bb6a);
        return micColour;
    }
}

InputLevelPanel::InputLevelPanel()
{
    left.title = "INPUT LEFT (Ch 1)";
    right.title = "INPUT RIGHT (Ch 2)";

    inputGainLabel.setFont (juce::Font (13.0f));
    inputGainLabel.setColour (juce::Label::textColourId, textColour);
    inputGainLabel.setText ("Volume Trim", juce::dontSendNotification);
    inputGainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    inputGainSlider.setRange (-60.0, 24.0, 0.5);
    inputGainSlider.setValue (0.0, juce::dontSendNotification);
    inputGainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 24);
    inputGainSlider.setTextValueSuffix (" dB");
    inputGainSlider.setDoubleClickReturnValue (true, 0.0);
    inputGainSlider.setTooltip ("Atur gain capture mikrofon atau trim analisis.");
    inputGainSlider.onValueChange = [this]
    {
        if (onInputGainChanged)
            onInputGainChanged ((float) inputGainSlider.getValue());
    };

    addAndMakeVisible (inputGainLabel);
    addAndMakeVisible (inputGainSlider);
}

void InputLevelPanel::setSnapshot (const DspWorker::Snapshot& snapshot)
{
    left.readings = snapshot.inputLeft;
    right.readings = snapshot.inputRight;

    statusLine = juce::String (snapshot.sampleRate, 0) + " Hz"
               + "  |  " + juce::String ((int) snapshot.samplesProcessed) + " sample diukur"
               + "  |  " + juce::String (snapshot.droppedSamples) + " sample lewat";

    repaint();
}

void InputLevelPanel::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (30 + 20 + 8);
    auto gainRow = area.removeFromTop (34);
    auto controls = gainRow.removeFromLeft (juce::jmin (500, gainRow.getWidth()));
    inputGainLabel.setBounds (controls.removeFromLeft (118));
    inputGainSlider.setBounds (controls.reduced (0, 3));
    repaint();
}

void InputLevelPanel::setInputGainControl (double minimumDb, double maximumDb,
                                           double currentDb, bool hardwareGain)
{
    inputGainSlider.setRange (minimumDb, maximumDb, 0.5);
    inputGainSlider.setValue (currentDb, juce::dontSendNotification);

    const auto label = hardwareGain ? "Volume Mic" : "Volume Trim";
    inputGainLabel.setText (label, juce::dontSendNotification);
    inputGainSlider.setTooltip (hardwareGain
        ? "Mengatur gain capture mikrofon di perangkat audio."
        : "Trim digital untuk level analisis mikrofon; tidak mengubah sinyal rekaman mentah.");
}

void InputLevelPanel::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto area = getLocalBounds().toFloat().reduced (16.0f);

    auto title = area.removeFromTop (30.0f);
    g.setFont (juce::Font (18.0f, juce::Font::bold));
    g.setColour (textColour);
    g.drawText ("Level Input Realtime", title, juce::Justification::centredLeft);

    auto status = area.removeFromTop (20.0f);
    g.setFont (juce::Font (12.0f));
    g.setColour (mutedColour);
    g.drawText (statusLine, status, juce::Justification::centredLeft);
    area.removeFromTop (8.0f);
    auto controlsRow = area.removeFromTop (34.0f);
    controlsRow.removeFromLeft (juce::jmin (500.0f, controlsRow.getWidth()));

    // The slider changes the microphone input gain (or analysis trim where the device
    // has no hardware control); this readout shows stereo balance measured from L/R.
    if (controlsRow.getWidth() > 140.0f)
    {
        auto balanceText = controlsRow.removeFromTop (17.0f);
        const auto bothValid = left.readings.valid && right.readings.valid;
        const auto signalPresent = bothValid
            && juce::jmax (left.readings.rmsDbfs, right.readings.rmsDbfs) > -100.0f;
        const auto differenceDb = bothValid
            ? right.readings.rmsDbfs - left.readings.rmsDbfs : 0.0f;
        const auto absoluteDifference = std::abs (differenceDb);
        const auto balanceDescription = ! bothValid ? juce::String ("menunggu sinyal")
            : ! signalPresent ? juce::String ("senyap")
            : absoluteDifference < 0.5f ? juce::String ("seimbang")
            : differenceDb < 0.0f
                ? "Left lebih kuat " + juce::String (absoluteDifference, 1) + " dB"
                : "Right lebih kuat " + juce::String (absoluteDifference, 1) + " dB";

        g.setColour (mutedColour);
        g.setFont (juce::Font (12.0f));
        g.drawText ("Balance: " + balanceDescription, balanceText,
                    juce::Justification::centredLeft);

        auto track = controlsRow.removeFromTop (9.0f).reduced (12.0f, 1.0f);
        if (track.getWidth() > 24.0f)
        {
            const auto centerX = track.getCentreX();
            const auto position = signalPresent
                ? juce::jlimit (-1.0f, 1.0f, differenceDb / 18.0f) : 0.0f;
            const auto markerX = centerX + position * track.getWidth() * 0.5f;

            g.setColour (plotColour);
            g.fillRoundedRectangle (track, 3.0f);
            g.setColour (gridColour);
            g.fillRect (centerX - 0.5f, track.getY() - 2.0f, 1.0f, track.getHeight() + 4.0f);
            g.setColour (micColour);
            g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (centerX, markerX),
                                                           track.getY(),
                                                           std::max (2.0f, std::abs (markerX - centerX)),
                                                           track.getHeight()), 3.0f);
            g.setColour (textColour);
            g.fillRect (markerX - 1.0f, track.getY() - 3.0f, 2.0f, track.getHeight() + 6.0f);
        }
    }

    area.removeFromTop (8.0f);

    const auto half = (area.getHeight() - 12.0f) / 2.0f;
    auto top = area.removeFromTop (half);
    area.removeFromTop (12.0f);

    drawChannel (g, top, left, micColour);
    drawChannel (g, area, right, referenceColour);
}

void InputLevelPanel::drawChannel (juce::Graphics& g, const juce::Rectangle<float>& area,
                                   const Channel& channel, const juce::Colour& colour)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (area, 6.0f);

    auto inner = area.reduced (16.0f);

    g.setFont (juce::Font (14.0f, juce::Font::bold));
    g.setColour (colour);
    g.drawText (channel.title, inner.removeFromTop (20.0f), juce::Justification::centredLeft);

    const auto& r = channel.readings;

    // The four figures the panel exists for, in the order a reader needs them.
    struct Figure { juce::String label; juce::String value; };
    const Figure figures[] =
    {
        { "RMS",           r.valid ? juce::String (r.rmsDbfs, 1) + " dBFS" : juce::String ("--") },
        { "Peak",          r.valid ? juce::String (r.peakDbfs, 1) + " dBFS" : juce::String ("--") },
        { "Peak-to-Peak",  r.valid ? juce::String (r.peakToPeakDbfs, 1) + " dBFS" : juce::String ("--") },
        { "Crest Factor",  r.valid ? juce::String (r.crestFactorDb, 1) + " dB" : juce::String ("--") }
    };

    const auto figureWidth = inner.getWidth() / 4.0f;

    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<float> (inner.getX() + figureWidth * (float) i,
                                            inner.getY(),
                                            figureWidth - 8.0f, 54.0f);

        g.setFont (juce::Font (12.0f));
        g.setColour (mutedColour);
        g.drawText (figures[i].label, cell.removeFromTop (18.0f), juce::Justification::centredLeft);

        g.setFont (juce::Font (19.0f, juce::Font::bold));
        g.setColour (textColour);
        g.drawText (figures[i].value, cell, juce::Justification::centredLeft);
    }

    // The figure cells are drawn from the current inner top; advance past all four before
    // placing the meter so the scale and fill can never cover the RMS/peak text.
    inner.removeFromTop (54.0f);
    inner.removeFromTop (10.0f);

    auto meter = inner.removeFromTop (juce::jmax (26.0f, inner.getHeight() * 0.4f));
    drawMeter (g, meter, r.rmsDbfs, r.peakDbfs, r.peakHoldDbfs);

    inner.removeFromTop (8.0f);

    g.setFont (juce::Font (12.0f));
    g.setColour (mutedColour);

    const auto extra = juce::String ("Hold ") + juce::String (r.peakHoldDbfs, 1) + " dBFS"
                     + "   |   RMS blok " + juce::String (r.blockRmsDbfs, 1) + " dBFS";

    g.drawText (extra, inner.removeFromTop (18.0f), juce::Justification::centredLeft);

    if (! r.valid)
    {
        g.setFont (juce::Font (14.0f));
        g.setColour (mutedColour);
        g.drawText ("Menunggu audio", inner, juce::Justification::centred);
    }
}

void InputLevelPanel::drawMeter (juce::Graphics& g, juce::Rectangle<float> area,
                                 float rmsDb, float peakDb, float holdDb)
{
    auto scale = area.removeFromLeft (46.0f);    const auto bar = area;
    g.setFont (juce::Font (11.0f));

    for (int db = -60; db <= 0; db += 12)
    {
        const auto x = bar.getX() + meterPosition ((float) db) * bar.getWidth();

        g.setColour (gridColour);
        g.fillRect ((float) x, bar.getY(), 1.0f, bar.getHeight());

        g.setColour (mutedColour);
        g.drawText (juce::String (db), scale.withX (x - 14.0f).withWidth (28.0f),
                    juce::Justification::centred);
    }

    g.setColour (juce::Colour (0xff0a0a0a));
    g.fillRoundedRectangle (bar, 3.0f);

    // The main bar follows the RMS number at the top of the card. Peak and held peak
    // are separate markers, so a short transient cannot make a steady input look full.
    const auto fill = bar.withWidth (meterPosition (rmsDb) * bar.getWidth());

    if (fill.getWidth() > 1.0f)
    {
        g.setColour (levelColour (rmsDb).withAlpha (0.85f));
        g.fillRoundedRectangle (fill, 3.0f);
    }

    if (peakDb > meterFloorDb)
    {
        const auto peakX = bar.getX() + meterPosition (peakDb) * bar.getWidth();
        g.setColour (juce::Colours::white);
        g.fillRect (peakX - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f);
    }

    // The held peak is an amber line; the white line is the latest block peak.
    const auto holdX = bar.getX() + meterPosition (holdDb) * bar.getWidth();

    if (holdDb > meterFloorDb)
    {
        g.setColour (holdColour);
        g.fillRect ((float) holdX - 1.5f, bar.getY() - 3.0f, 3.0f, bar.getHeight() + 6.0f);
    }

    g.setColour (gridColour);
    g.drawRoundedRectangle (bar, 3.0f, 1.0f);
}
