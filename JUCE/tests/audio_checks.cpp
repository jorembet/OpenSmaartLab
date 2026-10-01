#include "AudioEngine.h"
#include "DSP.h"
#include "MicrophoneCalibration.h"
#include "RTASnapshot.h"
#include "FrequencyLabels.h"
#include "SignalGenerator.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

void require(bool value, const char* message)
{
    if (!value) { std::cout << "FAIL: " << message << std::endl; throw std::runtime_error(message); }
}

int main()
{
    constexpr int size = 16384;
    constexpr float rate = 48000.0f;
    std::vector<float> tone(size), noise(size), delayed(size, 0.0f);
    for (int i = 0; i < size; ++i)
        tone[i] = 0.5f * std::sin(2.0f * dsp::pi * 1500.0f * i / rate);
    dsp::BlockAnalyser analyser;
    analyser.prepare(rate, size);
    dsp::Spectrum spectrum;
    analyser.analyse(tone.data(), spectrum);
    const auto peak = std::max_element(spectrum.magnitudeDb.begin(), spectrum.magnitudeDb.end());
    require(spectrum.valid, "RTA must produce a valid spectrum");
    require(std::abs(spectrum.freq[peak - spectrum.magnitudeDb.begin()] - 1500.0f) < 3.0f,
            "RTA must locate the 1500 Hz test tone");
    require(std::abs(*peak + 6.02f) < 0.2f, "RTA must report half scale near -6 dBFS");

    std::mt19937 random(42);
    std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
    for (auto& sample : noise) sample = distribution(random);
    constexpr int lag = 480;
    std::copy(noise.begin(), noise.end() - lag, delayed.begin() + lag);
    dsp::DelayFinder finder;
    finder.prepare(size);
    require(std::abs(finder.analyse(noise.data(), delayed.data(), rate, 100.0f) - lag) < 2.0f,
            "Delay must detect a microphone lag of 10 ms");

    AudioEngine engine;
    engine.audioDeviceAboutToStart(nullptr);
    std::vector<float> block(1024);
    const float* inputs[] = { block.data() };
    juce::AudioIODeviceCallbackContext context;
    for (int iteration = 0; iteration < 400; ++iteration)
    {
        std::fill(block.begin(), block.end(), iteration / 1000.0f);
        engine.audioDeviceIOCallbackWithContext(inputs, 1, nullptr, 0, 1024, context);
    }
    std::vector<float> first, second;
    engine.getLatestBlock(1024, first, second);
    require(first.size() == 1024 && std::abs(first.back() - 0.399f) < 0.0001f,
            "Microphone buffer must keep updating after 8 seconds");
    require(engine.getCapturedChannels() == 1 && second.back() == 0.0f,
            "Mono capture must not fabricate a reference signal");
    engine.audioDeviceAboutToStart(nullptr);
    engine.getGenerator().setRunning(true);
    std::vector<float> played;
    std::vector<float> speaker(1024), rightSpeaker(1024);
    float* outputs[] = { speaker.data(), rightSpeaker.data() };
    constexpr int acousticLag = 3072;
    for (int iteration = 0; iteration < 110; ++iteration)
    {
        for (int i = 0; i < 1024; ++i)
        {
            const int source = iteration * 1024 + i - acousticLag;
            block[i] = source >= 0 ? played[(size_t) source] * 0.4f : 0.0f;
        }
        engine.audioDeviceIOCallbackWithContext(inputs, 1, outputs, 2, 1024, context);
        require(speaker == rightSpeaker, "Both speakers must play identical generator samples");
        played.insert(played.end(), speaker.begin(), speaker.end());
    }
    std::vector<float> generated;
    engine.getLatestBlock(65536, first, second, &generated);
    finder.prepare(65536);
    const auto measuredLag = finder.analyse(generated.data(), first.data(), rate, 500.0f);
    require(std::abs(measuredLag - acousticLag) < 2.0f, "Pink output to mono mic must detect total delay 64 ms");
    require(dsp::delayConfidence(generated, first, juce::roundToInt(measuredLag)) > 0.99f,
            "Matching pink noise must have high delay confidence");
    std::fill(first.begin(), first.end(), 0.0f);
    require(dsp::delayConfidence(generated, first, 0) == 0.0f, "Silence must not report a valid delay");
    AudioEngine listing;

    const auto labels = listing.getOutputDeviceLabels();
    const auto outputNames = listing.getOutputDeviceNames();
    require(labels.size() == outputNames.size(), "Output labels and names must stay aligned");

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto lower = labels[i].toLowerCase();
        require(! lower.contains("hdmi") && ! lower.contains("displayport"),
                "HDMI and DisplayPort outputs must be hidden");
        require(! labels[i].contains("Rate Converter Plugin")
                && ! labels[i].contains("Direct hardware device")
                && ! labels[i].contains("Direct sample mixing")
                && ! labels[i].contains("Sound Server")
                && ! labels[i].contains("spacialization")
                && ! labels[i].contains("Speex DSP")
                && ! labels[i].contains("Software Volume Control"),
                "Device labels must not expose raw ALSA descriptions");
        require(! labels[i].contains("(4,6,8)") && ! labels[i].contains(", ;"),
                "Device labels must not leak raw ALSA name fragments");
    }

    if (juce::File("/usr/bin/pactl").existsAsFile())
    {
        const auto capture = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("opensmaartlab-audio-checks-sinks.txt");

        if (std::system(("/usr/bin/pactl list sinks > "
                         + capture.getFullPathName().quoted()).toRawUTF8()) == 0)
        {
            auto expectedBluetooth = 0;

            for (const auto& line : juce::StringArray::fromLines(capture.loadFileAsString()))
            {
                const auto lower = line.toLowerCase();
                if (lower.contains("name:") && lower.contains("bluez"))
                    ++expectedBluetooth;
            }

            capture.deleteFile();

            auto bluetoothCount = 0;

            for (const auto& label : labels)
                if (label.toLowerCase().contains("bluetooth"))
                    ++bluetoothCount;

            require(bluetoothCount == expectedBluetooth,
                    "Every PipeWire Bluetooth sink must appear as a Bluetooth output");

            for (int i = 0; i < labels.size(); ++i)
                if (labels[i].toLowerCase().contains("bluetooth"))
                    require(listing.getOutputPulseSink(i).startsWith("bluez"),
                            "Bluetooth outputs must resolve to their PipeWire sink name");

            
        }
    }

    require(listing.getOutputPulseSink(-1).isEmpty() && listing.getOutputPulseSink(9999).isEmpty(),
            "Out-of-range output indexes must not return a PulseAudio sink");

    const auto inputNames = listing.getInputDeviceNames();
    const auto inputLabelsNow = listing.getInputDeviceLabels();
    require(inputNames.size() == inputLabelsNow.size(), "Input labels and names must stay aligned");

    for (const auto& label : inputLabelsNow)
        require(! label.startsWith ("channel"),
                "Input labels must not show JUCE's generic 'channel' name");

    // Hardware devices must carry a real ALSA jack name rather than the raw description.
    if (juce::File ("/usr/bin/arecord").existsAsFile())
    {
        auto hardware = 0;

        for (int i = 0; i < inputNames.size(); ++i)
        {
            if (! inputNames[i].contains ("sof-hda-dsp")
                 && ! inputNames[i].contains ("USB Audio Device"))
                continue;

            ++hardware;
            require (! inputLabelsNow[i].contains ("Direct hardware device")
                     && ! inputLabelsNow[i].contains ("sample snooping"),
                     "Hardware input labels must not expose raw ALSA descriptions");
            require (! inputLabelsNow[i].startsWith ("channel"),
                     "Hardware input labels must name the jack");
        }

        if (hardware > 0)
            std::cout << "INFO: " << hardware << " hardware input devices labelled from arecord\\n";
    }



    require(std::size(dsp::rtaBands) == 31, "RTA must have 31 frequency labels");

    // ---- Microphone calibration ----
    auto imm6c = MicrophoneCalibration::daytonImm6c();
    require (imm6c.isEnabled(), "The iMM-6c calibration must be active");
    require (imm6c.getModelName() == "Dayton Audio iMM-6c", "The iMM-6c model name must be kept");

    // 10 mV/Pa into 1 kOhm is -40 dBV re 1 V/Pa, so 1 V full scale is 134 dB SPL.
    require (std::abs (imm6c.getSensitivityDb() - 134.0f) < 0.05f,
             "iMM-6c sensitivity must convert 10 mV/Pa into 134 dB SPL at full scale");

    // The app must start uncalibrated: enabled calibration shifts the trace far above
    // the display range and pins every bar to the top of the plot.
    auto plain = MicrophoneCalibration();
    require (! plain.isEnabled(), "A default calibration must be inactive");
    require (plain.toSpl (-12.0f) == -12.0f, "An inactive calibration must leave dBFS untouched");
    require (std::abs (imm6c.toSpl (-12.0f) - 125.0f) < 0.02f,
             "Enabled calibration must convert dBFS into dB SPL");

    // Regression guard: the default display window is 0 to -120 dB. A calibrated
    // reading sits far above it, so calibration must never be on by default or
    // every bar would clamp to the top of the plot.
    require (plain.toSpl (-40.0f) >= -120.0f && plain.toSpl (-40.0f) <= 0.0f,
             "An uncalibrated trace must stay inside the default display window");
    require (imm6c.toSpl (-40.0f) > 0.0f,
             "A calibrated reading must sit above the default window, which is why it is opt-in");

    imm6c.setInputTrimDb (-6.0f);
    require (std::abs (imm6c.toSpl (-12.0f) - 119.0f) < 0.02f,
             "The input trim must shift the calibrated level");
    imm6c.setInputTrimDb (0.0f);

    // A rising curve must lift the level it is compensating for.
    auto flat = MicrophoneCalibration::daytonImm6c();
    flat.setCurve ({ { 100.0f, -2.0f }, { 1000.0f, 0.0f }, { 10000.0f, 2.0f } });
    require (flat.hasFrequencyResponse(), "A loaded curve must be marked as present");
    require (std::abs (flat.correctionDb (1000.0f)) < 0.001f, "A centre point must interpolate exactly");
    require (std::abs (flat.correctionDb (100.0f) + 2.0f) < 0.001f, "The first point must be exact");
    require (std::abs (flat.correctionDb (10000.0f) - 2.0f) < 0.001f, "The last point must be exact");
    // Interpolation is linear in frequency, so 550 Hz between 100 Hz (-2) and
    // 1000 Hz (0) sits at 50% of the span, i.e. -1 dB.
    require (std::abs (flat.correctionDb (550.0f) + 1.0f) < 0.001f, "Midpoints must interpolate linearly");
    require (flat.correctionDb (20.0f) == -2.0f && flat.correctionDb (50000.0f) == 2.0f,
             "Out-of-range frequencies must clamp to the end points");

    // Dayton calibration files are two columns, often with a header.
    const auto daytonCsv = juce::String (
        "# Dayton Audio iMM-6c calibration\r\n"
        "Frequency,Response\r\n"
        "20,0.00\r\n"
        "100,-1.20\r\n"
        "1000,0.35\r\n"
        "10000,1.80\r\n"
        "20000,0.90\r\n");
    auto loaded = MicrophoneCalibration::daytonImm6c();
    require (loaded.loadCsv (daytonCsv), "A Dayton calibration CSV must load");
    require (loaded.getCurve().size() == 5, "All five calibration points must be read");
    require (std::abs (loaded.correctionDb (1000.0f) - 0.35f) < 0.001f, "Loaded points must be exact");
    require (std::abs (loaded.correctionDb (100.0f) + 1.20f) < 0.001f,
             "Negative response values must survive loading");
    require (! loaded.loadCsv ("# only a header\nFrequency,Response\n"),
             "A CSV without usable rows must be rejected");
    require (! loaded.loadCsv (""), "An empty CSV must be rejected");

    // Dayton's download tool saves the file as .txt, with quoted values and a
    // footer, so the loader must cope with that shape too.
    const auto daytonTxt = juce::String (
        "Mic: iMM-6\r\n"
        "Serial: CMM10000\r\n"
        "\r\n"
        "\"Frequency\", \"Sensitivity\"\r\n"
        "\"20\", \"0.00\"\r\n"
        "\"50\", \"-0.42\"\r\n"
        "\"100\", \"-1.20\"\r\n"
        "\"1000\", \"0.35\"\r\n"
        "\"10000\", \"1.80\"\r\n"
        "\"20000\", \"0.90\"\r\n"
        "\r\n"
        "End of file\r\n");
    auto fromTxt = MicrophoneCalibration::daytonImm6c();
    require (fromTxt.loadCsv (daytonTxt), "A quoted Dayton .txt file must load");
    require (fromTxt.getCurve().size() == 6, "All six .txt calibration points must be read");
    require (std::abs (fromTxt.correctionDb (50.0f) + 0.42f) < 0.001f,
             "Quoted negative values must be read correctly");
    require (std::abs (fromTxt.correctionDb (100.0f) + 1.20f) < 0.001f,
             "Quoted negative values in the middle of the curve must be read");
    require (std::abs (fromTxt.correctionDb (20000.0f) - 0.90f) < 0.001f,
             "The last quoted point must be read");

    // A .txt file read from disk must behave the same as from memory.
    const auto txtFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("opensmaartlab-cal-test.txt");
    txtFile.replaceWithText (daytonTxt);

    auto fromFile = MicrophoneCalibration::daytonImm6c();
    require (fromFile.loadFile (txtFile), "A .txt file must load from disk");
    require (fromFile.getCurve().size() == fromTxt.getCurve().size(),
             "Disk and in-memory loading must agree");
    require (! fromFile.loadFile (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("tidak-ada-kalibrasi.txt")),
             "A missing calibration file must be rejected");
    txtFile.deleteFile();

    // Space separated and tab separated files must load too.
    auto spaced = MicrophoneCalibration::daytonImm6c();
    require (spaced.loadCsv ("100 -1.5\n1000 0.25\n10000 1.75\n"), "Space separated rows must load");
    require (spaced.getCurve().size() == 3, "Space separated rows must all be read");
    require (std::abs (spaced.correctionDb (1000.0f) - 0.25f) < 0.001f,
             "Space separated values must be exact");

    // Calibration must be applied across a whole trace, one correction per bin.
    auto trace = std::vector<float> (3, -20.0f);
    const auto traceFreq = std::vector<float> { 100.0f, 1000.0f, 10000.0f };
    flat.apply (trace, traceFreq);
    require (std::abs (trace[0] - (flat.toSpl (-20.0f) - 2.0f)) < 0.02f, "Bin 1 must take its own correction");
    require (std::abs (trace[1] - flat.toSpl (-20.0f)) < 0.02f, "Bin 2 must take its own correction");
    require (std::abs (trace[2] - (flat.toSpl (-20.0f) + 2.0f)) < 0.02f, "Bin 3 must take its own correction");

    auto untouched = std::vector<float> (3, -20.0f);
    plain.apply (untouched, traceFreq);
    require (untouched[0] == -20.0f && untouched[1] == -20.0f && untouched[2] == -20.0f,
             "An inactive calibration must not touch a trace");

    // 1/3 octave must be the default 31-band set with every frequency labelled.
    require(dsp::bandCountToCount(3) == 31, "1/3 octave must be 31 bands");
    require(dsp::octaveBandFrequencies(3, 20.0f, 20000.0f).size() == 31,
            "Default RTA band set must contain 31 bands");

    // The 1/3 octave set must match the ISO 266 nominal table exactly.
    const auto thirdOctave = dsp::octaveBandFrequencies(3, 20.0f, 20000.0f);
    const juce::Array<float> expectedThirdOctave = { 20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f,
                                                    80.0f, 100.0f, 125.0f, 160.0f, 200.0f, 250.0f,
                                                    315.0f, 400.0f, 500.0f, 630.0f, 800.0f, 1000.0f,
                                                    1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
                                                    5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f,
                                                    16000.0f, 20000.0f };
    require(thirdOctave.size() == expectedThirdOctave.size(),
            "1/3 octave must produce 31 bands");
    for (int i = 0; i < thirdOctave.size(); ++i)
    {
        if (std::abs(thirdOctave[(size_t) i] - expectedThirdOctave[i]) >= 0.01f)
            std::cout << "MISMATCH idx=" << i << " got=" << thirdOctave[(size_t) i]
                      << " want=" << expectedThirdOctave[i] << "\n";
        require(std::abs(thirdOctave[(size_t) i] - expectedThirdOctave[i]) < 0.01f,
                "1/3 octave centres must be the ISO 266 nominal frequencies");
    }

    for (const auto fraction : { 1, 2, 3, 4, 6, 12 })
    {
        const auto centres = dsp::octaveBandFrequencies(fraction, 20.0f, 20000.0f);
        const auto expectedCount = dsp::bandCountToCount(fraction);

        require(centres.size() == (size_t) expectedCount,
                "Each octave fraction must produce its nominal band count");
        require(dsp::nominalOctaveFraction(expectedCount) == fraction,
                "nominalOctaveFraction must invert bandCountToCount");
        require(std::abs(centres.front() - 20.0f) < 0.01f, "Bands must start at 20 Hz");

        // The top band is the rounded label nearest the 20 kHz anchor.
        auto lastBand = 20000.0f;

        for (size_t i = 1; i < centres.size(); ++i)
            if (std::abs(centres[i] - 20000.0f) < std::abs(lastBand - 20000.0f))
                lastBand = centres[i];

        require(lastBand >= 19000.0f && lastBand <= 21000.0f,
                "The last band must sit at the 20 kHz anchor");
        require(centres.back() >= lastBand && centres.back() <= 21000.0f,
                "The last band must be the topmost centre");

        for (const auto centre : centres)
            require(std::abs(centre - dsp::labelFrequency(centre, fraction)) < 0.01f,
                    "Every band centre must already be a round label value");

        for (size_t i = 1; i < centres.size(); ++i)
            require(centres[i] > centres[i - 1],
                    "Band centres must stay strictly increasing after rounding");

        // A 1500 Hz tone can fall between coarse bands, so probe each band centre
        // in turn and require the peak to land in the band that was excited.
        for (size_t band = 0; band < centres.size(); band += 7)
        {
            const auto tone = centres[band];
            const auto binWidth = rate / (float) size;
            const auto aligned = std::round(tone / binWidth) * binWidth;
            std::vector<float> toneSamples(size);

            // Snapping the tone to an exact FFT bin avoids window scalloping loss.
            for (int i = 0; i < size; ++i)
                toneSamples[(size_t) i] = 0.5f * std::sin(2.0f * dsp::pi * aligned * i / rate);

            dsp::BlockAnalyser toneAnalyser;
            toneAnalyser.prepare(rate, size);
            dsp::Spectrum toneSpectrum;
            toneAnalyser.analyse(toneSamples.data(), toneSpectrum);

            const auto peaks = dsp::bandPeakDb(toneSpectrum.freq, toneSpectrum.magnitudeDb, centres);
            require(peaks.size() == centres.size(), "Band peaks must match band count");

            auto peakBand = std::max_element(peaks.begin(), peaks.end());
            require(centres[(size_t) (peakBand - peaks.begin())] == tone,
                    "Band peaks must land in the band containing the tone");
            require(std::abs(*peakBand + 6.02f) < 0.2f,
                    "Band peaks must report half scale near -6 dBFS");
        }
    }

    require(dsp::nominalFrequency(25.2f) == 25.0f && dsp::nominalFrequency(31.7f) == 31.5f
            && dsp::nominalFrequency(63.5f) == 63.0f && dsp::nominalFrequency(100.8f) == 100.0f,
            "Geometric band centres must snap to the nearest nominal frequency");

    // Bars must tile the plot without gaps or clipping: the first band reaches 20 Hz,
    // the last reaches 20 kHz, and neighbouring bands meet at a shared midpoint.
    for (const auto fraction : { 1, 2, 3, 4, 6, 12 })
    {
        const auto centres = dsp::octaveBandFrequencies (fraction, 20.0f, 20000.0f);
        auto edges = std::vector<float> (centres.size() + 1);

        for (size_t i = 0; i < centres.size(); ++i)
            edges[i + 1] = i + 1 < centres.size() ? std::sqrt (centres[i] * centres[i + 1]) : 20000.0f;

        edges.front() = 20.0f;

        for (size_t i = 1; i < edges.size(); ++i)
            require(edges[i] > edges[i - 1],
                    "Band edges must increase so bars tile the plot without gaps");

        require(std::abs (edges.front() - 20.0f) < 0.01f, "The first bar must start at 20 Hz");
        require(std::abs (edges.back() - 20000.0f) < 0.01f, "The last bar must reach 20 kHz");

        // Interior centres must sit strictly inside their bar. The outermost bars are
        // clamped to the plot edges, so their centre may sit on the edge itself.
        for (size_t i = 1; i + 1 < centres.size(); ++i)
            require(edges[i] < centres[i] && centres[i] < edges[i + 1],
                    "Every interior band centre must sit inside its own bar");

        for (const auto centre : centres)
            require(centre >= 20.0f - 0.01f && centre <= 21000.0f,
                    "Every band centre must stay within the plotted frequency range");
    }

    // ---- A real Dayton .txt file ----
    // Shape taken from an iMM-6c download: a starred sensitivity line, a blank tab
    // line, then frequency/dB pairs separated by tabs, one per 1/24 octave.
    {
        const auto daytonTxt = juce::String (
            "*1000Hz\t-38.5\n"
            "\t\n"
            "20.00\t-1.2\n"
            "22.29\t0.0\n"
            "100.00\t0.6\n"
            "200.00\t0.5\n"
            "1000.00\t0.0\n"
            "2000.00\t-0.5\n"
            "20000.00\t2.3\n");

        const auto txtFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("opensmaartlab-dayton-real.txt");
        txtFile.replaceWithText (daytonTxt);

        auto mic = MicrophoneCalibration::daytonImm6c();
        require (mic.loadFile (txtFile), "A real Dayton .txt file must load");
        require (mic.hasMeasuredSensitivity(), "The starred line must be read as the sensitivity");

        // -38.5 dB re 1 V/Pa means 1 V full scale is 38.5 dB above 1 Pa.
        require (std::abs (mic.getSensitivityDb() - 132.5f) < 0.05f,
                 "The file sensitivity must win over the nominal 134 dB");

        require (mic.getCurve().size() == 7, "All seven curve points must be read");
        require (std::abs (mic.correctionDb (20.0f) + 1.2f) < 0.001f,
                 "The 20 Hz point must be exact");
        require (std::abs (mic.correctionDb (1000.0f)) < 0.001f,
                 "The 1 kHz point is the reference and must be 0 dB");
        require (std::abs (mic.correctionDb (20000.0f) - 2.3f) < 0.001f,
                 "The 20 kHz point must be exact");

        // The sensitivity line must never be mistaken for a curve point.
        for (const auto& point : mic.getCurve())
            require (point.frequency > 15.0f && point.frequency < 25000.0f,
                     "No curve point may come from the sensitivity line");

        // The header parser must ignore plain data rows, or the first row is lost.
        float rejected = 0.0f;
        require (! MicrophoneCalibration::parseSensitivityHeader ("20.00\t-1.2", rejected),
                 "A data row must not be read as a sensitivity header");
        require (! MicrophoneCalibration::parseSensitivityHeader ("1000.00\t0.0", rejected),
                 "A mid-curve row must not be read as a sensitivity header");
        require (MicrophoneCalibration::parseSensitivityHeader ("*1000Hz\t-38.5", rejected),
                 "The starred line must be recognised");
        require (std::abs (rejected - 132.5f) < 0.05f, "The starred line value must convert");
        require (MicrophoneCalibration::parseSensitivityHeader ("Sensitivity: -38.5 dBV", rejected),
                 "A labelled sensitivity line must be recognised");

        txtFile.deleteFile();
    }

    // ---- RTA snapshot save and load ----
    // A snapshot must survive a save/load round trip with its name and all traces.
    {
        RTASnapshot snapshot;
        snapshot.name = "Ruang uji A";
        snapshot.axisLabel = "dB";
        snapshot.calibrationText = "Mic: Dayton Audio iMM-6c";
        snapshot.topDb = 0.0f;
        snapshot.bottomDb = -120.0f;

        for (int i = 0; i < 64; ++i)
        {
            snapshot.frequency.push_back (20.0f * std::pow (2.0f, (float) i / 4.0f));
        }

        snapshot.traces.resize (2);
        snapshot.traces[0].name = "Mic";
        snapshot.traces[1].name = "Output";

        for (size_t i = 0; i < snapshot.frequency.size(); ++i)
        {
            snapshot.traces[0].values.push_back (-40.0f + (float) i);
            snapshot.traces[1].values.push_back (-80.0f + (float) i * 0.5f);
        }

        require (snapshot.isUsable(), "A complete snapshot must be usable");

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("opensmaartlab-snapshot-test.rta.csv");
        file.deleteFile();
        require (snapshot.writeTo (file), "A snapshot must be written to disk");

        RTASnapshot reloaded;
        require (RTASnapshot::readFrom (file, reloaded), "A written snapshot must load back");
        require (reloaded.name == "Ruang uji A", "The name must round trip");
        require (reloaded.axisLabel == "dB", "The axis label must round trip");
        require (reloaded.calibrationText == "Mic: Dayton Audio iMM-6c",
                 "The calibration note must round trip");
        require (reloaded.topDb == snapshot.topDb && reloaded.bottomDb == snapshot.bottomDb,
                 "The dB range must round trip");
        require (reloaded.frequency.size() == snapshot.frequency.size(),
                 "The frequency axis must round trip");
        require (reloaded.traces.size() == snapshot.traces.size(),
                 "The trace count must round trip");

        for (size_t t = 0; t < snapshot.traces.size(); ++t)
        {
            require (reloaded.traces[t].values.size() == snapshot.frequency.size(),
                     "Every trace must have one value per frequency");

            for (size_t i = 0; i < snapshot.frequency.size(); ++i)
            {
                require (std::abs (reloaded.frequency[i] - snapshot.frequency[i]) < 0.01f,
                         "Frequency points must survive the round trip");
                require (std::abs (reloaded.traces[t].values[i] - snapshot.traces[t].values[i]) < 0.002f,
                         "Trace values must survive the round trip");
            }
        }

        // Trace names must survive so the legend stays meaningful.
        require (reloaded.traces[0].name.contains ("mic")
                 && reloaded.traces[1].name.contains ("output"),
                 "Trace names must round trip");

        // A trace with missing points must be refused, not half shown.
        const auto broken = file.getSiblingFile ("opensmaartlab-snapshot-broken.csv");
        // Two traces are declared but the last row only carries one value, so the
        // traces cannot line up with the frequency axis.
        broken.replaceWithText ("name=rusak\naxis=dB\ntop_db=0\nbottom_db=-120\n"
                                "frequency_hz,mic_db,output_db\n100,-20,-30\n200,-25\n");
        RTASnapshot ignored;
        require (! RTASnapshot::readFrom (broken, ignored),
                 "A trace with missing points must be rejected");
        require (! RTASnapshot::readFrom (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                              .getChildFile ("tidak-ada-file.csv"), ignored),
                 "A missing file must be rejected");
        require (! RTASnapshot::fromCsv ("", ignored), "Empty text must be rejected");
        require (! RTASnapshot::fromCsv ("# only comments\n", ignored),
                 "Comment-only text must be rejected");

        // A trace that is out of step with the axis must fail validation.
        RTASnapshot ragged;
        ragged.frequency = { 100.0f, 200.0f, 300.0f };
        ragged.traces.resize (1);
        ragged.traces[0].values = { 1.0f, 2.0f };
        require (! ragged.isUsable(), "A ragged trace must not pass validation");

        file.deleteFile();
        broken.deleteFile();
    }

    // ---- Band RTA must ignore DC as well ----
    // The lowest band covers 20 Hz up to the first midpoint. Starting it at 0 Hz would
    // sweep in bin 0, where an audio interface's DC offset shows up as a large peak.
    {
        const auto centres = dsp::octaveBandFrequencies(3, 20.0f, 20000.0f);
        std::vector<float> dcFreq, dcMags;
        constexpr int numBins = 8193;
        const auto binHz = rate / (float) (2 * (numBins - 1));

        for (int i = 0; i < numBins; ++i)
        {
            dcFreq.push_back (binHz * (float) i);
            dcMags.push_back (-92.0f + 4.0f * std::log10 (std::max (1.0f, dcFreq[(size_t) i] / 100.0f)));
        }

        dcMags[0] = -40.0f; // DC offset, 50 dB above the rest

        const auto bands = dsp::bandPeakDb(dcFreq, dcMags, centres, 20.0f);
        require(bands.size() == centres.size(), "Band peaks must match band count");
        require(bands[0] < -60.0f, "DC offset must not leak into the 20 Hz band");
        require(bands[0] > -110.0f, "The 20 Hz band must still show the floor level");

        for (size_t i = 1; i < bands.size(); ++i)
            require(bands[i] < -60.0f, "No octave band may be lifted by the DC bin");

        // A tone at 1 kHz must still land in the 1 kHz band.
        std::vector<float> toneFreq, toneMags;
        for (int i = 0; i < numBins; ++i)
        {
            toneFreq.push_back (binHz * (float) i);
            toneMags.push_back (-95.0f);
        }

        toneMags[(size_t) std::lround (1000.0f / binHz)] = -20.0f;

        const auto toneBands = dsp::bandPeakDb(toneFreq, toneMags, centres, 20.0f);
        auto peakBand = std::max_element(toneBands.begin(), toneBands.end());
        require(centres[(size_t) (peakBand - toneBands.begin())] == 1000.0f,
                "A 1 kHz tone must still land in the 1 kHz band");
    }

    constexpr int linearBars = 100;
    const auto linearFreq = dsp::linearBarFrequencies(linearBars, 20.0f, 20000.0f);
    const auto linearPeaks = dsp::linearPeakDb(spectrum.freq, spectrum.magnitudeDb, linearBars, 20.0f, 20000.0f);
    require(linearFreq.size() == linearBars && linearPeaks.size() == linearBars,
            "Linear RTA must produce 100 bar centres and 100 peak values");
    require(std::abs(linearFreq.front() - 119.9f) < 0.5f && std::abs(linearFreq.back() - 19900.1f) < 0.5f,
            "Linear RTA bar centres must sit inside 20 Hz to 20 kHz");
    auto loudest = std::max_element(linearPeaks.begin(), linearPeaks.end());
    require(std::abs(spectrum.freq[peak - spectrum.magnitudeDb.begin()] - 1500.0f) < 3.0f
            && std::abs(linearFreq[(size_t) (loudest - linearPeaks.begin())] - 1500.0f) < 100.0f,
            "Linear RTA must place the 1500 Hz tone in the 1500 Hz bar");
    require(std::abs(*loudest + 6.02f) < 0.2f, "Linear RTA must report half scale near -6 dBFS");

    // A USB interface can sit a few mV off zero, and on a linear axis the first bar
    // is wide enough to swallow bin 0. DC must not be read as spectrum level.
    {
        std::vector<float> dcFreq, dcMags;
        constexpr int numBins = 8193;
        const auto binHz = rate / (float) (2 * (numBins - 1));

        for (int i = 0; i < numBins; ++i)
        {
            dcFreq.push_back (binHz * (float) i);
            dcMags.push_back (-92.0f + 4.0f * std::log10 (std::max (1.0f, dcFreq[(size_t) i] / 100.0f)));
        }

        dcMags[0] = -40.0f; // DC offset, 50 dB above the rest

        const auto bars = dsp::linearPeakDb(dcFreq, dcMags, linearBars, 20.0f, 20000.0f);
        require(*std::max_element(bars.begin(), bars.end()) < -60.0f,
                "DC offset must not lift any linear bar");
        require(bars[0] > -110.0f,
                "The first linear bar must still show the floor, not be discarded");
    }

    // ---- RTA RMS and peak readout ----
    {
        // A full-scale sine is -3.01 dBFS RMS, so the two figures must differ by that
        // much; reporting the same number twice would hide clipping.
        std::vector<float> tone(size);
        for (int i = 0; i < size; ++i)
            tone[(size_t) i] = std::sin(2.0f * dsp::pi * 1000.0f * i / rate);

        double energy = 0.0;
        auto peak = 0.0f;

        for (const auto sample : tone)
        {
            energy += (double) sample * (double) sample;
            peak = std::max (peak, std::abs (sample));
        }

        const auto rmsDb = dsp::db10 ((float) (energy / std::max<size_t> (1, tone.size())));
        const auto peakDb = dsp::db20 (peak);

        require (std::abs (rmsDb + 3.0103f) < 0.05f, "Full-scale sine RMS must read -3.01 dBFS");
        require (std::abs (peakDb) < 0.05f, "Full-scale sine peak must read 0 dBFS");
        require (peakDb - rmsDb > 2.9f && peakDb - rmsDb < 3.1f,
                 "Peak must sit 3.01 dB above RMS for a full-scale sine");

        // Digital silence must report the floor, not a usable-looking level.
        const auto silenceRms = dsp::db10 (0.0f);
        const auto silencePeak = dsp::db20 (0.0f);
        require (silenceRms <= dsp::dbFloor + 0.01f && silencePeak <= dsp::dbFloor + 0.01f,
                 "Silence must report the dB floor so the readout stays hidden");

        // A short transient must move the peak much further than the RMS.
        auto burst = std::vector<float> (size, 0.0f);
        burst[0] = 0.5f;

        auto burstEnergy = 0.0;
        burstEnergy += 0.25;

        const auto burstRms = dsp::db10 ((float) (burstEnergy / std::max<size_t> (1, burst.size())));
        const auto burstPeak = dsp::db20 (0.5f);
        require (burstPeak - burstRms > 40.0f,
                 "A single transient must lift the peak far above the RMS");
    }

    // ---- Manual generator frequency ----
    // A typed frequency has to survive unchanged: measurement work needs 997 Hz to
    // stay 997 Hz, not be rounded to the nearest slider step.
    {
        SignalGenerator generator;
        generator.prepare (rate);
        generator.setType (SignalGenerator::Type::Sine);

        const auto clamp = [] (float typed)
        {
            return juce::jlimit (1.0f, 20000.0f, typed);
        };

        const std::array<float, 8> typed = { 997.0f, 1000.5f, 20.0f, 63.0f,
                                             440.0f, 1000.0f, 19999.0f, 20000.0f };

        for (const auto value : typed)
        {
            generator.setFrequency (clamp (value));
            require (std::abs (generator.getFrequency() - clamp (value)) < 0.001f,
                     "A typed frequency must be applied exactly, not rounded to a slider step");

            // And the generator must actually produce it, not merely store it.
            std::vector<float> out (size);
            generator.setRunning (true);
            generator.process (out.data(), size);
            generator.setRunning (false);

            dsp::BlockAnalyser analyser;
            analyser.prepare (rate, size);
            dsp::Spectrum produced;
            analyser.analyse (out.data(), produced);

            auto strongest = std::max_element (produced.magnitudeDb.begin(),
                                               produced.magnitudeDb.end());
            const auto bin = produced.freq[(size_t) (strongest - produced.magnitudeDb.begin())];

            // One FFT bin at this size is a few Hz, so allow that much slack.
            require (std::abs (bin - generator.getFrequency())
                     < std::max (6.0f, generator.getFrequency() * 0.02f),
                     "The generated signal must sit at the requested frequency");
        }

        // Out-of-range input must be clamped rather than accepted or wrapped.
        require (clamp (0.0f) == 1.0f, "A frequency below the floor must clamp");
        require (clamp (-100.0f) == 1.0f, "A negative frequency must clamp, not wrap");
        require (clamp (25000.0f) == 20000.0f, "A frequency above the ceiling must clamp");
        require (clamp (1e9f) == 20000.0f, "An absurd frequency must clamp");
    }

    // ---- Frequency labels must stay legible and not overlap ----
    {
        const auto typeface = FrequencyLabels::font();
        require(typeface.getHeight() >= 12.0f,
                "The frequency label font must be readable at the default window size");
        require(typeface.isBold(), "The frequency label font must be bold to stand out from the grid");

        const auto rowHeight = FrequencyLabels::height();
        require(rowHeight > typeface.getHeight(),
                "The label row must leave room for the comma in 31.5 and any descender");

        const auto labelWidth = FrequencyLabels::width();
        require(labelWidth > typeface.getStringWidth (juce::String ("12.5k")),
                "The label box must fit the widest label in use");
        require(FrequencyLabels::reservedSpace() > rowHeight,
                "The reserved space below the plot must exceed the label row height");
        require(FrequencyLabels::topOffset() > 0.0f,
                "The label row must start below the plot edge");

        // At the default 1920 px window, all 31 band labels must still fit at this
        // font size. If this fails the labels would overlap and have to be thinned.
        const auto centres = dsp::octaveBandFrequencies(3, 20.0f, 20000.0f);
        constexpr float plotWidth = 1813.0f;

        auto narrowest = std::numeric_limits<float>::max();

        for (size_t i = 1; i < centres.size(); ++i)
        {
            const auto previous = std::log10 (centres[i - 1]);
            const auto current = std::log10 (centres[i]);
            const auto step = std::min (std::log10 (centres.back()) - std::log10 (centres.front())
                                            / (float) (centres.size() - 1),
                                        current - previous);

            // Mirrors computeAxisDomain: the domain widens by half the tightest step.
            const auto domain = (std::log10 (centres.back()) - std::log10 (centres.front())) + step;
            narrowest = std::min (narrowest, step / domain * plotWidth);
        }

        require(narrowest >= labelWidth,
                "All 31 band labels must fit at the larger font size on the default width");

        // A narrower window is allowed to thin labels, but never to zero width.
        require(FrequencyLabels::width() > 0.0f, "The label box width must stay positive");
    }

    std::cout << "PASS: paired pink noise, stereo copies, mono microphone delay 64 ms, silence rejection, 31 bands\n";
    std::cout << "PASS: 100-bar linear RTA centres 119.9-19900 Hz, 1500 Hz tone detection, DC rejected\n";
    std::cout << "PASS: frequency labels are bold, sized for the row, and all 31 fit at 1/3 octave\n";
    std::cout << "PASS: RTA RMS/peak figures are 3.01 dB apart on a full-scale sine and hide silence\n";
    std::cout << "PASS: typed generator frequencies are applied exactly and out-of-range values clamp\n";
    std::cout << "PASS: ISO 266 nominal 1/3 octave table exact; 1/1..1/12 octave counts 11/21/31/41/61/121 strictly increasing\n";
    std::cout << "PASS: bars tile 20 Hz to 20 kHz with no clipping and every centre inside its bar\n";
    std::cout << "PASS: octave bands reject DC offset while keeping the floor level\n";
    std::cout << "PASS: HDMI outputs hidden and PipeWire Bluetooth sinks listed as Bluetooth outputs\n";
    std::cout << "PASS: iMM-6c sensitivity 134 dB SPL at full scale, curve interpolation and CSV loading\n";
    std::cout << "PASS: Dayton .txt calibration files load with quoted values, headers and footers\n";
    std::cout << "PASS: Dayton sensitivity line overrides nominal SPL and stays out of the response curve\n";
    std::cout << "PASS: RTA snapshot save/load round trip with name, calibration note and all traces\n";
    std::cout << "PASS: input labels use ALSA jack names, never JUCE's generic 'channel'\n";
    std::cout << "PASS: 1500 Hz RTA, -6 dBFS, 10 ms delay, continuous mono capture beyond 8 seconds\n";
}
