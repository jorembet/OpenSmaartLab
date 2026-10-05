#include "ImpedanceDisplay.h"
#include "DSP.h"

namespace
{
    const juce::Colour backgroundColour = juce::Colour(0xff1d1d1d);
    const juce::Colour panelColour = juce::Colour(0xff262626);
    const juce::Colour plotColour = juce::Colour(0xff050505);
    const juce::Colour gridColour = juce::Colour(0xff383838);
    const juce::Colour textColour = juce::Colour(0xffe6e9ef);
    const juce::Colour mutedColour = juce::Colour(0xff9a9a9a);
    const juce::Colour impedanceColour = juce::Colour(0xffffb74d);
    const juce::Colour phaseColour = juce::Colour(0xff4fc3f7);
    const juce::Colour warningColour = juce::Colour(0xffe53935);

    constexpr float minFrequency = 20.0f;
    constexpr float maxFrequency = 20000.0f;
    constexpr float minImpedance = 0.1f;
    constexpr float maxImpedance = 10000.0f;

    float impedanceToY(float ohm, const juce::Rectangle<float>& area)
    {
        const auto clamped = juce::jlimit(minImpedance, maxImpedance, ohm);
        return area.getBottom()
               - (std::log10(clamped) - std::log10(minImpedance))
                     / (std::log10(maxImpedance) - std::log10(minImpedance)) * area.getHeight();
    }

    float frequencyToX(float f, const juce::Rectangle<float>& area)
    {
        return area.getX()
               + std::log10(f / minFrequency) / std::log10(maxFrequency / minFrequency) * area.getWidth();
    }
}

ImpedanceDisplay::ImpedanceDisplay()
{
    resistorLabel.setText("Resistor seri (ohm):", juce::dontSendNotification);
    resistorLabel.setColour(juce::Label::textColourId, mutedColour);
    resistorLabel.setJustificationType(juce::Justification::centredRight);

    resistorEditor.setText("8.2");
    resistorEditor.setInputRestrictions(8, "0123456789.");
    resistorEditor.onReturnKey = [this] { recompute(); resistorEditor.giveAwayKeyboardFocus(); };
    resistorEditor.onFocusLost = [this] { recompute(); };

    hintLabel.setText("Sambungkan: Ref melintasi resistor seri, Mic melintasi speaker. "
                      "Z = R x Vmic/Vref",
                      juce::dontSendNotification);
    hintLabel.setColour(juce::Label::textColourId, mutedColour);
    hintLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(resistorLabel);
    addAndMakeVisible(resistorEditor);
    addAndMakeVisible(hintLabel);
}

void ImpedanceDisplay::resized()
{
    auto area = getLocalBounds().reduced(14);
    auto top = area.removeFromTop(32);

    resistorLabel.setBounds(top.removeFromLeft(150));
    resistorEditor.setBounds(top.removeFromLeft(80).reduced(0, 2));
    top.removeFromLeft(12);
    hintLabel.setBounds(top);

    repaint();
}

void ImpedanceDisplay::recompute()
{
    const auto r = resistorEditor.getText().getFloatValue();

    if (r > 0.0f)
        seriesResistanceOhm = r;

    if (hasLastResult)
        pushData(lastResult);
}

void ImpedanceDisplay::pushData(const TransferFunction::Result& result)
{
    if (!result.valid || result.freq.empty()
        || result.measMagnitudeDb.size() != result.freq.size()
        || result.refMagnitudeDb.size() != result.freq.size())
        return;

    lastResult = result;
    hasLastResult = true;
    freq = result.freq;
    binValid = result.binValid;
    phaseDeg = result.phaseDeg;

    impedanceOhm.resize(freq.size());

    for (size_t i = 0; i < freq.size(); ++i)
    {
        const auto ratioDb = result.measMagnitudeDb[i] - result.refMagnitudeDb[i];
        impedanceOhm[i] = seriesResistanceOhm * std::pow(10.0f, ratioDb / 20.0f);
    }

    hasData = true;
    repaint();
}

void ImpedanceDisplay::clear()
{
    hasData = false;
    hasLastResult = false;
    freq.clear();
    impedanceOhm.clear();
    phaseDeg.clear();
    repaint();
}

void ImpedanceDisplay::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto area = getLocalBounds().reduced(14).toFloat();
    area.removeFromTop(32.0f);

    const auto phaseHeight = area.getHeight() * 0.35f;
    const auto magArea = area.withTrimmedBottom(phaseHeight + 8.0f).withTrimmedLeft(44.0f);
    const auto phaseArea = area.removeFromBottom(phaseHeight).withTrimmedLeft(44.0f);

    g.setColour(panelColour);
    g.fillRoundedRectangle(magArea.expanded(0, 0), 6.0f);
    g.fillRoundedRectangle(phaseArea, 6.0f);

    for (const int exp : { -1, 0, 1, 2, 3, 4 })
    {
        const auto ohm = std::pow(10.0f, (float) exp);
        const auto y = impedanceToY(ohm, magArea);
        g.setColour(gridColour);
        g.fillRect(magArea.getX(), y, magArea.getWidth(), 1.0f);
        g.setColour(mutedColour);
        g.setFont(juce::Font(10.0f));
        const auto label = ohm >= 1000.0f ? juce::String(ohm / 1000.0f, 0) + "k" : juce::String(ohm, 1);
        g.drawText(label + " ohm", magArea.getX() - 40, y - 7, 36, 14, juce::Justification::centredRight);
    }

    for (const auto f : { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 })
    {
        const auto x = frequencyToX((float) f, magArea);
        g.setColour(gridColour);
        g.fillRect(x, magArea.getY(), 1.0f, magArea.getHeight());
        g.setColour(mutedColour);
        g.setFont(juce::Font(10.0f));
        g.drawText(f >= 1000 ? juce::String(f / 1000) + "k" : juce::String(f),
                   x - 16, magArea.getBottom() + 2, 32, 12, juce::Justification::centred);
    }

    g.setColour(impedanceColour);
    juce::Path magPath;

    if (hasData)
    {
        bool started = false;

        for (size_t i = 0; i < impedanceOhm.size(); ++i)
        {
            if (!binValid.empty() && binValid[i] == 0)
                continue;

            const auto x = frequencyToX(freq[i], magArea);
            const auto y = impedanceToY(impedanceOhm[i], magArea);

            if (!started)
            {
                magPath.startNewSubPath(x, y);
                started = true;
            }
            else
                magPath.lineTo(x, y);
        }
    }

    g.saveState();
    g.reduceClipRegion(magArea.toNearestInt());
    g.strokePath(magPath, juce::PathStrokeType(1.5f));
    g.restoreState();

    g.setColour(mutedColour);
    g.setFont(juce::Font(12.0f));
    g.drawText("Impedansi |Z| (ohm)", magArea.getX() + 8, magArea.getY() + 6, 200, 16,
               juce::Justification::topLeft);

    g.setColour(phaseColour);
    juce::Path phasePath;

    if (hasData)
    {
        const auto toY = [&] (float deg)
        {
            return phaseArea.getBottom() - (deg + 90.0f) / 180.0f
                   * phaseArea.getHeight();
        };

        bool started = false;

        for (size_t i = 0; i < phaseDeg.size(); ++i)
        {
            if (!binValid.empty() && binValid[i] == 0)
                continue;

            const auto x = frequencyToX(freq[i], phaseArea);
            const auto y = toY(juce::jlimit(-90.0f, 90.0f, phaseDeg[i]));

            if (!started)
            {
                phasePath.startNewSubPath(x, y);
                started = true;
            }
            else
                phasePath.lineTo(x, y);
        }
    }

    g.saveState();
    g.reduceClipRegion(phaseArea.toNearestInt());
    g.strokePath(phasePath, juce::PathStrokeType(1.5f));
    g.restoreState();

    g.setColour(mutedColour);
    g.drawText("Fase Z (deg)", phaseArea.getX() + 8, phaseArea.getY() + 6, 200, 16,
               juce::Justification::topLeft);

    if (!hasData)
    {
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(juce::Font(16.0f));
        g.drawText("Belum ada data impedansi", area, juce::Justification::centred);
    }
}
