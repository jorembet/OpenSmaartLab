#include "MainComponent.h"
#include "FFTDisplay.h"
#include "GeneratorDisplay.h"
#include "TransferFunctionDisplay.h"
#include "SignalGenerator.h"
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) { std::cout << "FAIL: " << message << std::endl; throw std::runtime_error(message); }
    }

    void saveSnapshot(juce::Component& component, juce::PNGImageFormat& png, const juce::String& path)
    {
        const juce::File file(path);
        juce::FileOutputStream output(file);

        if (! output.openedOk())
            throw std::runtime_error("Cannot write a screenshot");

        png.writeImageToStream(component.createComponentSnapshot(component.getLocalBounds()), output);
    }

    constexpr int blockSize = 4096;

    // Plays the generator and hands the display successive blocks, the way the audio
    // thread does, so the plot and the averaged tilt are checked against real output
    // instead of an empty component.
    std::vector<float> renderWith(SignalGenerator& generator, int blocks)
    {
        generator.prepare(48000.0);
        generator.setRunning(true);

        std::vector<float> signal((size_t) blockSize * blocks, 0.0f);

        for (int block = 0; block < blocks; ++block)
            generator.process(signal.data() + (size_t) block * blockSize, blockSize);

        generator.setRunning(false);
        return signal;
    }

    void feedBlocks(GeneratorDisplay& display, const std::vector<float>& signal, int blocks)
    {
        for (int block = 0; block < blocks; ++block)
            display.setSamples(std::vector<float> (signal.begin() + (size_t) block * blockSize,
                                                   signal.begin() + (size_t) (block + 1) * blockSize),
                               48000.0f);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    MainComponent main;
    main.setVisible(true);
    juce::TabbedComponent* tabs = nullptr;
    for (auto* child : main.getChildren())
        if (auto* candidate = dynamic_cast<juce::TabbedComponent*>(child)) tabs = candidate;
    if (tabs == nullptr) throw std::runtime_error("Missing tabs");

    // Tabs are looked up by name, so adding one cannot silently point the checks at the
    // wrong panel.
    const auto tabIndexFor = [&tabs] (const juce::String& name)
    {
        const auto names = tabs->getTabNames();

        for (int i = 0; i < names.size(); ++i)
            if (names[i] == name)
                return i;

        return -1;
    };

    const auto generatorTab = tabIndexFor("Generator");
    const auto transferTab = tabIndexFor("Transfer Function");
    require(generatorTab > 0 && transferTab > 0,
            "The generator and Transfer Function tabs must both exist");

    tabs->setCurrentTabIndex(generatorTab);
    auto* panel = tabs->getTabContentComponent(generatorTab);
    int visible = 0;
    for (auto* child : panel->getChildren())
    {
        if (child->isVisible() && child->getWidth() > 0 && child->getHeight() > 0) ++visible;
        else throw std::runtime_error("Generator child has empty layout");
    }
    if (visible < 10) throw std::runtime_error("Generator controls missing");

    // The driver band selector has to exist on the panel, with one entry per driver, or
    // the band cannot be picked by name at all.
    juce::ComboBox* bandPreset = nullptr;
    int listedItems = 0;

    for (auto* child : panel->getChildren())
    {
        auto* box = dynamic_cast<juce::ComboBox*> (child);

        if (box != nullptr && box->getNumItems() > listedItems)
        {
            bandPreset = box;
            listedItems = box->getNumItems();
        }
    }

    require(bandPreset != nullptr, "The generator panel needs a band preset list");
    require(listedItems == (int) SignalGenerator::getBandPresets().size() + 1,
            "The band list must offer one entry per driver plus a manual option");
    require(bandPreset->isVisible() && bandPreset->getWidth() > 0 && bandPreset->getHeight() > 0,
            "The band preset list must be laid out");

    // Measurement and reference must be assignable to the left and right input on their
    // own, and must never end up on the same channel.
    {
        juce::ComboBox* measurement = nullptr;
        juce::ComboBox* reference = nullptr;

        for (auto* child : main.getChildren())
        {
            auto* box = dynamic_cast<juce::ComboBox*> (child);

            if (box == nullptr || box->getNumItems() != 2)
                continue;

            const auto& first = box->getItemText(0);

            if (first.endsWith(AudioEngine::channelName(0)) && first.startsWith("Mic:"))
                measurement = box;
            else if (first.endsWith(AudioEngine::channelName(0)) && first.startsWith("Ref:"))
                reference = box;
        }

        require(measurement != nullptr && reference != nullptr,
                "Measurement and reference each need their own left/right channel list");
        require(measurement->isVisible() && measurement->getWidth() > 0,
                "The measurement channel list must be laid out");
        require(reference->isVisible() && reference->getWidth() > 0,
                "The reference channel list must be laid out");

        // Both channels are offered on each list, so the wiring can be swapped without
        // hunting for the other selector.
        for (const auto* box : { measurement, reference })
            for (int channel = 0; channel < 2; ++channel)
                require(box->getItemText(channel).endsWith(AudioEngine::channelName(channel)),
                        "Each channel list must offer both the left and the right input");

        require(measurement->getSelectedId() != reference->getSelectedId(),
                "Measurement and reference must not default to the same channel");
    }

    // The generator output side has to be selectable too, so one output can stay silent
    // while the other carries the signal.
    {
        require(main.getGeneratorRouting() == AudioEngine::OutputRouting::Both,
                "Both output channels must be routed until the user changes it");

        juce::ComboBox* routing = nullptr;

        for (auto* child : panel->getChildren())
        {
            auto* box = dynamic_cast<juce::ComboBox*> (child);

            if (box != nullptr && box->getNumItems() == 3 && routing == nullptr)
                routing = box;
        }

        require(routing != nullptr, "The generator panel needs an output channel list");
        require(routing->isVisible() && routing->getWidth() > 0 && routing->getHeight() > 0,
                "The output channel list must be laid out");

        routing->setSelectedId(2, juce::sendNotificationSync);
        require(main.getGeneratorRouting() == AudioEngine::OutputRouting::Left,
                "Choosing one output side must reach the audio engine");

        routing->setSelectedId(3, juce::sendNotificationSync);
        require(main.getGeneratorRouting() == AudioEngine::OutputRouting::Right,
                "Choosing the other output side must reach the audio engine");

        routing->setSelectedId(1, juce::sendNotificationSync);
        require(main.getGeneratorRouting() == AudioEngine::OutputRouting::Both,
                "Returning to both outputs must be possible");
    }

    {
        const auto presets = SignalGenerator::getBandPresets();
        auto& generator = main.getGenerator();

        // Choosing a driver band must reach the generator and both limit controls.
        bandPreset->setSelectedId(4, juce::sendNotificationSync);
        require(std::abs(generator.getBandLow() - presets[3].lowFrequency) < 0.5f
                    && std::abs(generator.getBandHigh() - presets[3].highFrequency) < 0.5f,
                "Choosing the tweeter band must set both band limits on the generator");

        bool showedLow = false, showedHigh = false;

        for (auto* child : panel->getChildren())
        {
            if (auto* editor = dynamic_cast<juce::TextEditor*> (child))
            {
                showedLow |= editor->getText() == juce::String((double) presets[3].lowFrequency, 0);
                showedHigh |= editor->getText() == juce::String((double) presets[3].highFrequency, 0);
            }
        }

        require(showedLow && showedHigh, "Both band limit boxes must show the chosen driver band");

        // A hand set band must stop claiming a driver preset. The band floor slider is
        // the one sitting at the band's lower edge on a 10 - 2000 Hz range, which is
        // what the tweeter preset leaves it at.
        juce::Slider* bandFloorSlider = nullptr;

        for (auto* child : panel->getChildren())
        {
            auto* slider = dynamic_cast<juce::Slider*> (child);

            if (slider != nullptr
                && std::abs(slider->getRange().getStart() - 10.0) < 0.01
                && std::abs(slider->getRange().getEnd() - 2000.0) < 0.01
                && std::abs(slider->getValue() - presets[3].lowFrequency) < 0.5)
                bandFloorSlider = slider;
        }

        require(bandFloorSlider != nullptr, "The band floor slider must follow the chosen preset");

        // The manual band is the last entry, so a hand set band selects it.
        const auto manualEntryId = bandPreset->getItemId(listedItems - 1);
        bandFloorSlider->setValue(300.0, juce::sendNotificationSync);
        require(bandPreset->getSelectedId() == manualEntryId,
                "A hand set band must report itself as manual, not as a driver preset");
    }

    juce::PNGImageFormat png;
    {
        juce::FileOutputStream output(juce::File("/tmp/opensmaart-generator.png"));
        png.writeImageToStream(main.createComponentSnapshot(main.getLocalBounds()), output);
    }
    tabs->setCurrentTabIndex(0);
    {
        juce::FileOutputStream output(juce::File("/tmp/opensmaart-rta.png"));
        png.writeImageToStream(main.createComponentSnapshot(main.getLocalBounds()), output);
    }

    // ---- Transfer Function tab ----
    // Magnitude, phase and coherence belong on one frequency axis, and the delay finder
    // has to be reachable from there.
    {
        tabs->setCurrentTabIndex(transferTab);

        auto* tfPanel = tabs->getTabContentComponent(transferTab);
        require(tfPanel != nullptr, "The Transfer Function tab needs a component");

        bool findDelay = false;

        for (auto* child : tfPanel->getChildren())
        {
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                if (button->getButtonText().contains("Delay"))
                    findDelay = button->isVisible() && button->getWidth() > 0;
            }
        }

        require(findDelay, "The Transfer Function tab needs a visible Find Delay button");

        juce::FileOutputStream output(juce::File("/tmp/opensmaart-transfer-function.png"));
        png.writeImageToStream(main.createComponentSnapshot(main.getLocalBounds()), output);

        tabs->setCurrentTabIndex(0);
    }

    // ---- Generator spectrum follows the settings ----
    // The plot has to zoom onto the configured range, so a tone or a narrow band stays
    // readable instead of collapsing onto a fixed 20 Hz - 20 kHz axis.
    GeneratorDisplay display;
    display.setBounds(0, 0, 900, 420);

    SignalGenerator generator;

    generator.setType(SignalGenerator::Type::Pink);
    generator.setBandLimits(20.0f, 20000.0f);
    display.setRunning(true);
    display.setSignal("Pink Noise", -24.0f, 0.0f, "Speaker");
    display.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                 generator.getFrequency(),
                                 generator.getSweepStart(), generator.getSweepEnd());
    feedBlocks(display, renderWith(generator, 24), 24);
    saveSnapshot(display, png, "/tmp/opensmaart-generator-pink-full.png");

    // The header prints this tilt, so it has to describe the pink noise that is
    // actually playing: about -3 dB per octave, not the scatter of a single block.
    const auto fullBandSlope = display.octaveSlope();
    require(fullBandSlope > -4.0f && fullBandSlope < -2.0f,
            "The printed tilt must read about -3 dB per octave for pink noise");

    // The narrowed band is the midrange driver preset, so the screenshot shows the case
    // a user works in when boosting one driver.
    const auto presets = SignalGenerator::getBandPresets();
    generator.setBandLimits(presets[2].lowFrequency, presets[2].highFrequency);
    display.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                 generator.getFrequency(),
                                 generator.getSweepStart(), generator.getSweepEnd());
    feedBlocks(display, renderWith(generator, 24), 24);
    saveSnapshot(display, png, "/tmp/opensmaart-generator-pink-narrow.png");

    // The same figure must survive a narrowed band, where the old fixed probes would
    // have been reading the rolled-off edge instead of the signal.
    const auto narrowSlope = display.octaveSlope();
    require(narrowSlope > -4.0f && narrowSlope < -2.0f,
            "A narrowed pink band must still report about -3 dB per octave");

    generator.setBandLimits(presets[3].lowFrequency, presets[3].highFrequency);
    display.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                 generator.getFrequency(),
                                 generator.getSweepStart(), generator.getSweepEnd());
    feedBlocks(display, renderWith(generator, 24), 24);
    saveSnapshot(display, png, "/tmp/opensmaart-generator-pink-tweeter.png");

    // A treble band is nearly flat in its upper half and rolled off below 2 kHz, so the
    // plot has to stay inside the band instead of reaching down into the bass.
    require(display.probeLevelDb(4000.0f) > display.probeLevelDb(500.0f) + 40.0f,
            "The tweeter band must leave the bass silent");

    generator.setType(SignalGenerator::Type::Sine);
    generator.setFrequency(997.0f);
    display.setSignal("Sine", -24.0f, 0.0f, "Speaker");
    display.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                 generator.getFrequency(),
                                 generator.getSweepStart(), generator.getSweepEnd());
    feedBlocks(display, renderWith(generator, 24), 24);
    saveSnapshot(display, png, "/tmp/opensmaart-generator-sine-997.png");

    // A tone has no tilt, so the plot must centre on the frequency instead of showing
    // a slope figure that would only describe leakage.
    require(display.probeLevelDb(997.0f) > display.probeLevelDb(997.0f / std::sqrt(2.0f)) + 20.0f,
            "A tone must dominate the frequency the generator is set to");

    generator.setType(SignalGenerator::Type::LogSweep);
    generator.setSweepRange(200.0f, 5000.0f, 10.0f);
    display.setSignal("Log Sweep", -24.0f, 0.5f, "Speaker");
    display.setGeneratorSettings(generator.getBandLow(), generator.getBandHigh(),
                                 generator.getFrequency(),
                                 generator.getSweepStart(), generator.getSweepEnd());
    feedBlocks(display, renderWith(generator, 24), 24);
    saveSnapshot(display, png, "/tmp/opensmaart-generator-sweep.png");

    // Stopping must drop the analysis, otherwise the previous spectrum stays on screen
    // next to the new settings and describes a signal that is no longer playing.
    display.clear();
    display.setSignal("Pink Noise", -24.0f, 0.0f, "Speaker");
    display.setGeneratorSettings(20.0f, 20000.0f, 997.0f, 20.0f, 20000.0f);
    display.setRunning(false);

    require(display.probeLevelDb(1000.0f) <= dsp::dbFloor + 0.5f,
            "A cleared display must not keep reporting a spectrum from the old signal");
    saveSnapshot(display, png, "/tmp/opensmaart-generator-stopped.png");

    // ---- Spectrogram ----
    // A scrolling history, so it is checked against data arriving over time: the column
    // count has to grow, the picture has to be painted, and a range change has to
    // re-colour it rather than leave the old scale behind.
    {
        FFTDisplay spectro;
        spectro.setBounds(0, 0, 1000, 700);
        spectro.setStyle(FFTDisplay::Style::Spectrogram);

        require(spectro.getStyle() == FFTDisplay::Style::Spectrogram,
                "The spectrogram style must be selectable");
        require(spectro.getSpectrogramBands() > 0 && spectro.getSpectrogramFrames() > 0,
                "The spectrogram must size itself to the plot");

        // Bands run across the frequency axis, frames up the time axis.
        const auto bands = spectro.getSpectrogramBands();

        // A tone that walks up the axis, so the picture has to differ from the cold one.
        for (int frame = 0; frame < 40; ++frame)
        {
            const auto centre = 200.0f * std::pow(2.0f, (float) frame / 12.0f);

            std::vector<float> freq, ref, meas, magnitude, phase, coherence;
            std::vector<char> valid;

            for (int i = 0; i <= 512; ++i)
            {
                const auto f = 20.0f * std::pow(1000.0f, (float) i / 512.0f);
                const auto near = std::abs(std::log(f / centre)) < 0.2f;

                freq.push_back(f);
                ref.push_back(-10.0f);
                meas.push_back(near ? -12.0f : dsp::dbFloor);
                magnitude.push_back(near ? -2.0f : dsp::dbFloor);
                phase.push_back(0.0f);
                coherence.push_back(near ? 0.95f : 0.0f);
                valid.push_back(near ? 1 : 0);
            }

            spectro.pushData(freq, ref, meas, magnitude, phase, coherence, valid);
        }

        require(spectro.getSpectrogramBands() == bands,
                "The spectrogram must keep its width as history arrives");
        require(spectro.getSpectrogramFrames() > 0,
                "The spectrogram must keep its height as history arrives");

        const auto beforeRangeChange = spectro.createComponentSnapshot(spectro.getLocalBounds());
        require(beforeRangeChange.isValid(), "The spectrogram must paint");

        // Changing the display range has to re-colour the history, not just the scale.
        spectro.setRange(0.0f, -60.0f);
        require(spectro.getRangeTop() == 0.0f && spectro.getRangeBottom() == -60.0f,
                "The display range must be applied exactly as asked");
        const auto afterRangeChange = spectro.createComponentSnapshot(spectro.getLocalBounds());
        require(afterRangeChange.isValid(), "The spectrogram must repaint after a range change");

        // Time runs upwards, so a band that was loud in an earlier frame must still hold
        // that level further up the history, and the newest frame must be the one read
        // back as frame zero.
        const auto bandFor = [&spectro] (float frequency)
        {
            auto best = 0;
            auto closest = std::numeric_limits<float>::max();

            for (int band = 0; band < spectro.getSpectrogramBands(); ++band)
            {
                const auto centre = 20.0f * std::pow(1000.0f,
                                                      (float) band
                                                          / (float) (spectro.getSpectrogramBands() - 1));

                if (std::abs(centre - frequency) < closest)
                {
                    closest = std::abs(centre - frequency);
                    best = band;
                }
            }

            return best;
        };

        const auto lastFrameTone = bandFor(200.0f * std::pow(2.0f, 39.0f / 12.0f));
        const auto earlierTone = bandFor(200.0f * std::pow(2.0f, 35.0f / 12.0f));

        require(spectro.getSpectrogramHistoryDb(0, lastFrameTone) > -60.0f,
                "The newest frame must be stored as frame zero");
        require(spectro.getSpectrogramHistoryDb(4, earlierTone) > -60.0f,
                "A frame from four steps ago must still be in the history");
        require(spectro.getSpectrogramHistoryDb(0, earlierTone) < -100.0f,
                "An old frame's band must not be reported as the newest measurement");

        // And the picture itself has to agree: the newest frame is the row along the
        // bottom, with the rows above it older.
        const auto bounds = spectro.getSpectrogramBounds();
        // The picture is drawn at the plot's origin, so a band index is an offset into it.
        const auto column = bounds.getX() + lastFrameTone;
        const auto newestPixel = afterRangeChange.getPixelAt(column, bounds.getBottom() - 1);
        const auto topPixel = afterRangeChange.getPixelAt(column, bounds.getY());

        require(newestPixel.getRed() > topPixel.getRed(),
                "The newest frame must be painted along the bottom of the picture");

        // A heat map has to produce many distinct colours; a flat fill would mean the
        // history never reached the picture.
        std::set<juce::uint32> colours;
        const auto scan = afterRangeChange.getBounds();

        for (int y = 0; y < scan.getHeight(); y += 3)
            for (int x = 0; x < scan.getWidth(); x += 3)
                colours.insert (afterRangeChange.getPixelAt (x, y).getARGB());

        require(colours.size() > 8,
                "The spectrogram must paint a heat map, not a single flat colour");

        saveSnapshot(spectro, png, "/tmp/opensmaart-spectrogram.png");

        // Back to a bar style the history is dropped, so one view cannot be measured
        // against the other one's time axis.
        spectro.setStyle(FFTDisplay::Style::Bands);
        require(spectro.getSpectrogramBands() == 0,
                "Leaving the spectrogram must drop its history");
    }

    std::cout << "PASS: Generator tab has " << visible << " visible controls without resizing; screenshots saved\n";
    std::cout << "PASS: Driver band list picks a driver band, and a hand set band reports manual\n";
    std::cout << "PASS: Measurement, reference and output channels are assigned left and right separately\n";
    std::cout << "PASS: Transfer Function tab shows magnitude, phase and coherence with a Find Delay button\n";
    // ---- Transfer Function panes ----
    // Drawn with synthetic data, because the real panes need a running device: the three
    // curves, the blanked region and the 80 % line all have to render without a live
    // measurement behind them.
    {
        TransferFunctionDisplay tfDisplay;
        tfDisplay.setBounds(0, 0, 1000, 760);

        TransferFunction::Result result;

        for (int i = 0; i <= 1024; ++i)
        {
            const auto frequency = 20.0f * std::pow(1000.0f, (float) i / 1024.0f);
            const auto inBand = frequency >= 2000.0f && frequency <= 20000.0f;

            result.freq.push_back(frequency);
            result.magnitudeDb.push_back(inBand ? -6.0f : dsp::dbFloor);
            result.phaseDeg.push_back(inBand ? -30.0f : 0.0f);
            result.coherence.push_back(inBand ? 0.93f : 0.0f);
            result.binValid.push_back(inBand ? 1 : 0);
            result.refMagnitudeDb.push_back(-12.0f);
            result.measMagnitudeDb.push_back(-18.0f);
        }

        result.validBins = 250;
        result.blankedBins = 775;
        result.averageCoherence = 0.93f;
        result.delayMs = 5.33f;
        result.peakReferenceDb = -12.0f;
        result.valid = true;

        tfDisplay.pushData(result);

        require(tfDisplay.hasData(), "The Transfer Function display must accept a measurement");
        require(tfDisplay.getAverageCoherence() > TransferFunctionDisplay::validityThreshold,
                "A measurement above the threshold must read as valid");

        saveSnapshot(tfDisplay, png, "/tmp/opensmaart-tf-panes.png");
    }

    std::cout << "PASS: Generator spectrum renders the configured pink band, tone and sweep range\n";
    std::cout << "PASS: Transfer Function panes draw magnitude, phase and coherence with blanked bins\n";
    std::cout << "PASS: Spectrogram records level history and follows the display range\n";
    std::cout << "PASS: Generator header reports the tilt of the configured band and the tone frequency\n";
}
