#include "AudioEngine.h"

#include <cstdlib>

#if JUCE_LINUX
namespace
{
    struct PulseSink
    {
        juce::String name;
        juce::String description;
    };

    juce::String findPactl()
    {
        for (const auto& dir : { "/usr/bin", "/bin", "/usr/local/bin" })
            if (juce::File(juce::String(dir) + "/pactl").existsAsFile())
                return juce::String(dir) + "/pactl";

        return {};
    }

    juce::Array<PulseSink> queryPulseSinks()
    {
        juce::Array<PulseSink> sinks;
        const auto pactl = findPactl();

        if (pactl.isEmpty())
            return sinks;

        const auto captureFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("opensmaartlab-sinks.txt");

        if (std::system((pactl + " list sinks > " + captureFile.getFullPathName().quoted()).toRawUTF8()) != 0)
        {
            captureFile.deleteFile();
            return sinks;
        }

        const auto text = captureFile.loadFileAsString();
        captureFile.deleteFile();

        const auto allLines = juce::StringArray::fromLines(text);

        for (const auto& rawLine : allLines)
        {
            const auto line = rawLine.trim();
            const auto colon = line.indexOfChar(':');

            if (line.containsIgnoreCase("Sink #"))
            {
                sinks.add({});
                continue;
            }

            if (colon <= 0 || sinks.isEmpty())
                continue;

            const auto key = line.substring(0, colon).trim();
            const auto value = line.substring(colon + 1).trim();

            auto& sink = sinks.getReference(sinks.size() - 1);

            if (key.equalsIgnoreCase("Name"))
                sink.name = value;
            else if (key.equalsIgnoreCase("Description"))
                sink.description = value;
        }

        sinks.removeIf([] (const PulseSink& sink) { return sink.name.isEmpty(); });
        return sinks;
    }

    void setPulseDefaultSink(const juce::String& sink)
    {
        const auto pactl = findPactl();

        if (pactl.isEmpty() || sink.isEmpty())
            return;

        if (std::system((pactl + " set-default-sink " + sink.quoted()).toRawUTF8()) != 0)
            jassertfalse;
    }
#endif

    // ALSA exposes long, technical device descriptions. This turns them into short,
    // recognisable names while keeping the raw name available for opening the device.
    // ALSA names built-in devices "<card>, ; <description>" and "<card>, USB Audio, ...".
    // The card part is the useful part; the description after it is technical noise.
    juce::String alsaCardName(const juce::String& name)
    {
        if (const auto separator = name.indexOf (", ;"))
            return name.substring (0, separator).trim();

        const auto comma = name.indexOfChar (',');
        const auto semicolon = name.indexOfChar (';');

        if (comma > 0 && semicolon > comma)
            return name.substring (0, comma).trim();

        return {};
    }

    juce::String friendlyDeviceName(const juce::String& name)
    {
        const auto lower = name.toLowerCase().trim();

        if (lower.isEmpty())
            return name;

        if (lower == "default" || lower == "sysdefault" || lower.startsWith("default alsa"))
            return "Sistem (audio bawaan OS)";

        if (lower.contains("pipewire sound server") || lower.contains("pulseaudio sound server"))
            return "Sistem (PipeWire)";

        if (lower == "pulse" || lower == "pipewire")
            return "Sistem (PipeWire)";

        // ALSA names built-in devices "<card>, ; <description> (<index>)". Only that exact
        // separator marks a card prefix, so commas inside names like "(4,6,8)" are kept.
        const auto card = alsaCardName (name);
        const auto cardLabel = card.isNotEmpty() ? card : juce::String("Kartu suara");

        if (lower.contains("direct hardware device"))
            return cardLabel + " (langsung)";

        if (lower.contains("direct sample mixing device"))
            return cardLabel + " (mix stereo)";

        if (lower.contains("direct sample snooping device"))
            return cardLabel + " (loopback stereo)";

        if (lower.contains("plugin for channel downmix") || lower.contains("plugin for channel upmix"))
            return cardLabel + " (campuran kanal)";

        if (lower.contains("rate converter plugin"))
            return cardLabel + " (resampler)";

        if (lower.contains("speex dsp"))
            return cardLabel + " (Mic: resample + pengurangan noise)";

        if (lower.contains("spacialization"))
            return "Kartu suara (campuran kanal)";

        return name.trim();
    }

    #if JUCE_LINUX
    // JUCE labels every ALSA input channel just "channel", so the real jack name has to
    // come from ALSA itself. "arecord -l" prints one line per capture device with the
    // card id, device number and a descriptive name, e.g. "sof-hda-dsp ... DMIC Raw".
    struct AlsaInput
    {
        juce::String card;
        int device = 0;
        juce::String name;
    };

    juce::Array<AlsaInput> queryAlsaInputs()
    {
        juce::Array<AlsaInput> inputs;

        for (const auto& dir : { "/usr/bin", "/bin" })
        {
            if (! juce::File (juce::String (dir) + "/arecord").existsAsFile())
                continue;

            auto text = juce::String();
            auto ok = false;
            const auto command = juce::String (dir) + "/arecord -l";

            const auto target = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("opensmaartlab-arecord.txt");

            // arecord prints the device list on stdout, so plain redirection is enough.
            if (std::system ((command + " > " + target.getFullPathName().quoted()).toRawUTF8()) != 0)
                continue;

            text = target.loadFileAsString();
            target.deleteFile();
            ok = true;

            if (! ok)
                continue;

            for (const auto& rawLine : juce::StringArray::fromLines (text))
            {
                const auto line = rawLine.trim();

                if (! line.startsWith ("card "))
                    continue;

                const auto firstOpen = line.indexOfChar ('[');
                const auto firstClose = line.indexOfChar (']');
                const auto comma = line.indexOfChar (',');

                if (firstOpen < 0 || firstClose < firstOpen || comma < firstClose)
                    continue;

                AlsaInput input;
                input.card = line.substring (firstOpen + 1, firstClose).trim();

                // After the card comes ", device N: <name>".
                const auto deviceMarker = line.substring (comma + 1).trim();
                const auto deviceStart = deviceMarker.toLowerCase().startsWith ("device ")
                                          ? deviceMarker.substring (7).trim()
                                          : deviceMarker;
                const auto deviceColon = deviceStart.indexOfChar (':');

                if (deviceColon < 0)
                    continue;

                input.device = deviceStart.substring (0, deviceColon).trim().getIntValue();
                input.name = deviceStart.substring (deviceColon + 1).trim();

                // Trim arecord flags: "HDA Analog (*) []" -> "HDA Analog".
                const auto nameEnd = input.name.indexOfChar ('[');
                if (nameEnd >= 0)
                    input.name = input.name.substring (0, nameEnd).trim();

                if (input.name.endsWith ("(*)"))
                    input.name = input.name.substring (0, input.name.length() - 3).trim();

                if (input.name.isNotEmpty())
                    inputs.add (input);
            }

            break;
        }

        return inputs;
    }

    juce::String jackNameForAlsaDevice(const juce::String& card, int deviceNumber,
                                       const juce::Array<AlsaInput>& inputs)
    {
        for (const auto& input : inputs)
        {
            if (input.card.equalsIgnoreCase (card) && input.device == deviceNumber)
                return input.name;
        }

        return {};
    }
#endif

    // A device reports one name per channel ("Mic1", "Mic2", ...). Collapse those into
    // distinct jack names so the label can show "Mic" instead of 32 channels.
    juce::StringArray distinctJacks(const juce::StringArray& channelNames)
    {
        juce::StringArray jacks;

        for (const auto& channelName : channelNames)
        {
            auto trimmed = channelName.trim();

            if (trimmed.isEmpty())
                continue;

            // Strip a trailing channel index so "Mic 1" and "Mic 2" collapse to "Mic".
            const auto trailing = trimmed.getTrailingIntValue();
            const auto suffix = juce::String (trailing);

            if (trailing > 0 && trimmed.length() > suffix.length())
            {
                auto stem = trimmed.substring (0, trimmed.length() - suffix.length()).trim();

                if (stem.isNotEmpty() && ! std::isdigit (static_cast<unsigned char> (stem[stem.length() - 1])))
                    trimmed = stem;
            }

            if (! jacks.contains (trimmed))
                jacks.add (trimmed);
        }

        return jacks;
    }

    juce::String deviceLabel(const juce::String& name, bool input, int channels,
                             const juce::String& backend, const juce::StringArray& inputJacks = {})
    {
        const auto lower = name.toLowerCase();
        juce::String kind = input ? "Input audio" : "Output audio";

        if (lower.contains("monitor") || lower.contains("loopback"))
            kind = "Loopback / monitor";
        else if (input && (lower.contains("microphone") || lower.containsWholeWord("mic") || lower.contains("headset")))
            kind = "Mic";
        else if (!input && (lower.contains("headphone") || lower.contains("headset")
                            || lower.contains("earphone") || lower.contains("earbud")))
            kind = "Headphone";
        else if (!input && lower.contains("speaker"))
            kind = "Speaker";
        else if (input)
            kind = "Line in";

        if (lower.contains("bluetooth") || lower.contains("bluez") || lower.contains("a2dp"))
            kind += " Bluetooth";
        else if (lower.contains("usb"))
            kind += " USB";

        if (input && ! inputJacks.isEmpty())
            kind = inputJacks.joinIntoString (" / ");

        return kind + " - " + friendlyDeviceName(name)
             + "  ·  " + juce::String(channels) + " ch  ·  " + backend;
    }
}


AudioEngine::AudioEngine()
{
    generator.prepare(48000.0);
    scanDevices();
}

AudioEngine::~AudioEngine()
{
    stop();
    generator.setRunning(false);
}

void AudioEngine::setStatus(const juce::String& text)
{
    if (onStatusMessage != nullptr)
        onStatusMessage(text);
}

void AudioEngine::scanDevices()
{
    inputChoices.clear();
    outputChoices.clear();
    inputLabels.clear();
    outputLabels.clear();

    deviceManager.initialise(0, 0, nullptr, false);

    auto& types = deviceManager.getAvailableDeviceTypes();

#if JUCE_LINUX
    int pulseDeviceIndex = -1;
    juce::String pulseDeviceName;
#endif

    struct Candidate
    {
        AudioDeviceChoice choice;
        juce::String backend;
    };

    #if JUCE_LINUX
    const auto alsaInputs = queryAlsaInputs();
#endif

    std::vector<Candidate> candidates;

    for (int i = 0; i < types.size(); ++i)
    {
        auto* type = types[i];

        if (type == nullptr)
            continue;

        type->scanForDevices();


        // ALSA lists input and output devices separately. Both directions are visited so
        // capture-only devices appear, then duplicates are merged afterwards.
        juce::StringArray names;

        for (const auto isInput : { true, false })
        {
            for (const auto& name : type->getDeviceNames (isInput))
                if (name.isNotEmpty() && ! names.contains (name))
                    names.add (name);
        }

        for (const auto& name : names)
        {
            if (name.isEmpty())
                continue;

            auto inputs = 0;
            auto outputs = 0;
            juce::StringArray jacks;

            if (auto* device = type->createDevice (name, name))
            {
                const auto channelNames = device->getInputChannelNames();
                inputs = channelNames.size();
                outputs = device->getOutputChannelNames().size();

                for (const auto& channelName : channelNames)
                    jacks.add (channelName);

#if JUCE_LINUX
                // JUCE names every ALSA input channel just "channel", so the real
                // jack name is taken from "arecord -l", which reports it per device.
                if (type->getTypeName().containsIgnoreCase ("ALSA") && inputs > 0)
                {
                    // JUCE prefixes built-in devices with the card id, then ", ; <description>".
                    const auto separator = name.indexOf (", ;");
                    const auto cardName = (separator > 0 ? name.substring (0, separator)
                                                         : name.substring (0, name.indexOfChar (':'))).trim();

                    auto matched = false;

                    for (const auto& alsaInput : alsaInputs)
                    {
                        if (! alsaInput.card.equalsIgnoreCase (cardName))
                            continue;

                        jacks = { alsaInput.name };
                        matched = true;
                        break;
                    }

                    // JUCE reports the same "channel" name for every input, which is
                    // meaningless. Fall back to a generic jack name rather than
                    // showing "channel" to the user.
                    if (! matched && jacks.size() > 1)
                        jacks = { "Mic" };
                    else if (! matched)
                        jacks.clear();
                }
#endif

                delete device;
            }

            if (inputs == 0 && outputs == 0)
                continue;

            Candidate candidate;
            candidate.choice = { i, name, true, inputs, outputs };
            candidate.choice.inputJacks = distinctJacks (jacks);
            candidate.backend = type->getTypeName();

#if JUCE_LINUX
                if (type->getTypeName().containsIgnoreCase ("ALSA") && outputs > 0
                 && (name.equalsIgnoreCase ("pulse") || name.equalsIgnoreCase ("pipewire")
                     || name.containsIgnoreCase ("PulseAudio Sound Server")
                     || name.containsIgnoreCase ("PipeWire Sound Server")))
            {
                pulseDeviceName = name;

                if (pulseDeviceIndex < 0)
                    pulseDeviceIndex = i;
            }
#endif

            candidates.push_back (candidate);
        }
    }

    auto looksAuxiliary = [] (const juce::String& name)
    {
        const auto lower = name.toLowerCase();

        return lower.contains("plugin") || lower.contains("rate converter")
            || lower.contains("gadget") || lower.contains("dmix")
            || lower.contains("upmix") || lower.contains("downmix");
    };

    auto looksHdmi = [] (const juce::String& name)
    {
        const auto lower = name.toLowerCase();

        return lower.contains("hdmi") || lower.contains("displayport");
    };

    std::sort(candidates.begin(), candidates.end(), [&] (const Candidate& a, const Candidate& b)
    {
        const auto auxiliaryA = looksAuxiliary(a.choice.name);
        const auto auxiliaryB = looksAuxiliary(b.choice.name);

        if (auxiliaryA != auxiliaryB)
            return auxiliaryB;

        return a.choice.name < b.choice.name;
    });

    for (const auto& candidate : candidates)
    {
        if (candidate.choice.inputChannels > 0)
        {
            inputChoices.push_back(candidate.choice);
            inputLabels.add(deviceLabel(candidate.choice.name, true, candidate.choice.inputChannels,
                                      candidate.backend, candidate.choice.inputJacks));
        }

        if (candidate.choice.outputChannels > 0 && !looksHdmi(candidate.choice.name))
        {
            outputChoices.push_back(candidate.choice);
            outputLabels.add(deviceLabel(candidate.choice.name, false, candidate.choice.outputChannels, candidate.backend));
        }
    }

#if JUCE_LINUX
    // ALSA cannot enumerate PipeWire/PulseAudio sinks, so Bluetooth and other
    // server-managed outputs are added here and reached through the ALSA "pulse" PCM.
    if (pulseDeviceIndex >= 0)
    {
        for (const auto& sink : queryPulseSinks())
        {
            if (looksHdmi(sink.name) || looksHdmi(sink.description))
                continue;

            const auto lower = sink.name.toLowerCase();
            const auto bluetooth = lower.contains("bluez") || lower.contains("bluetooth")
                                || lower.contains("a2dp");

            AudioDeviceChoice choice;
            choice.typeIndex = pulseDeviceIndex;
            choice.name = pulseDeviceName;
            choice.enabled = true;
            choice.outputChannels = 2;
            choice.pulseSink = sink.name;

            outputChoices.push_back(choice);

            const auto display = friendlyDeviceName(sink.description.isNotEmpty() ? sink.description : sink.name);

            outputLabels.add(juce::String(bluetooth ? "Speaker Bluetooth" : "Speaker sistem (PipeWire)")
                             + " - " + display + "  ·  2 ch  ·  PipeWire");
        }
    }
#endif

    if (inputChoices.empty())
        inputChoices.push_back({ -1, "Tidak ada perangkat input", false, 0, 0 });

    if (inputLabels.isEmpty())
        inputLabels.add("Tidak ada perangkat input");

    if (outputChoices.empty())
        outputChoices.push_back({ -1, "Tidak ada perangkat output", false, 0, 0 });

    if (outputLabels.isEmpty())
        outputLabels.add("Tidak ada perangkat output");
}

juce::StringArray AudioEngine::getInputDeviceNames() const
{
    juce::StringArray names;

    for (const auto& choice : inputChoices)
        if (choice.enabled)
            names.add(choice.name);

    return names;
}

juce::StringArray AudioEngine::getOutputDeviceNames() const
{
    juce::StringArray names;

    for (const auto& choice : outputChoices)
        if (choice.enabled)
            names.add(choice.name);

    return names;
}

juce::StringArray AudioEngine::getInputDeviceLabels() const
{
    return inputLabels;
}

juce::StringArray AudioEngine::getOutputDeviceLabels() const
{
    return outputLabels;
}

juce::String AudioEngine::getOutputPulseSink(int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) outputChoices.size()))
        return {};

    return outputChoices[(size_t) index].pulseSink;
}

void AudioEngine::start(const juce::String& inputDeviceName,
                        const juce::String& outputDeviceName,
                        double sampleRate,
                        int bufferSize,
                        const juce::String& outputPulseSink)
{
    stop();

#if JUCE_LINUX
    if (outputPulseSink.isNotEmpty())
        setPulseDefaultSink(outputPulseSink);
#endif

    deviceManager.addAudioCallback(this);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = outputDeviceName;
    setup.sampleRate = std::max(8000.0, sampleRate);
    setup.bufferSize = juce::jlimit(16, 8192, bufferSize);

    setup.useDefaultInputChannels = false;
    setup.inputChannels = juce::BigInteger(3);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels = juce::BigInteger(1);

    auto error = deviceManager.setAudioDeviceSetup(setup, true);

    if (error.isNotEmpty())
    {
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        error = deviceManager.setAudioDeviceSetup(setup, true);
    }

    if (error.isNotEmpty())
    {
        deviceManager.removeAudioCallback(this);
        setStatus(error);
        return;
    }

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        currentSampleRate.store(device->getCurrentSampleRate());
        currentBufferSize.store(device->getCurrentBufferSizeSamples());

        initialiseBuffers(currentSampleRate.load(), currentBufferSize.load());

        setStatus("Audio aktif: " + device->getName()
                  + " @ " + juce::String((int) currentSampleRate.load())
                  + " Hz / " + juce::String(currentBufferSize.load()) + " samples");
    }
    else
    {
        deviceManager.removeAudioCallback(this);
        setStatus("Tidak ada device audio yang bisa dibuka");
    }
}

void AudioEngine::initialiseBuffers(double newSampleRate, int bufferSize)
{
    const auto rate = std::max(1000.0, newSampleRate);

    generatorCapture.assign(8192, 0.0f);
    capturedGeneratorSamples = 0;

    std::lock_guard<std::mutex> lock(bufferMutex);

    capacity = (int) std::max(4.0, rate * 8.0);
    ringBuffer.assign(3, std::vector<float>((size_t) capacity, 0.0f));

    writePosition.store(0);
    samplesFilled.store(0);
}

void AudioEngine::stop()
{
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();

    capturedChannels.store(0);
    samplesFilled.store(0);
    writePosition.store(0);

    std::lock_guard<std::mutex> lock(bufferMutex);
    ringBuffer.clear();
    capacity = 0;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const auto rate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    const auto block = device != nullptr ? device->getCurrentBufferSizeSamples() : 1024;

    currentSampleRate.store(rate);
    currentBufferSize.store(block);
    generator.prepare(rate);

    initialiseBuffers(rate, block);
}

void AudioEngine::audioDeviceStopped()
{
    capturedChannels.store(0);
    samplesFilled.store(0);
    writePosition.store(0);

    std::lock_guard<std::mutex> lock(bufferMutex);
    ringBuffer.clear();
    capacity = 0;
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    const auto channels = std::min(2, numInputChannels);
    capturedChannels.store(channels);

    // Render once: all speakers and the reference receive the exact same samples.
    const bool hasOutput = outputChannelData != nullptr && numOutputChannels > 0
                       && outputChannelData[0] != nullptr;
    if (hasOutput)
    {
        generator.process(outputChannelData[0], numSamples, 1.0f);
        for (int ch = 1; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy(outputChannelData[ch], outputChannelData[0], numSamples);
        const juce::ScopedLock lock(generatorLock);
        const auto count = std::min(numSamples, (int) generatorCapture.size());
        juce::FloatVectorOperations::copy(generatorCapture.data(), outputChannelData[0], count);
        capturedGeneratorSamples = count;
    }

    std::lock_guard<std::mutex> lock(bufferMutex);
    if (ringBuffer.size() < 3 || capacity <= 0)
        return;
    auto position = writePosition.load();
    for (int i = 0; i < numSamples; ++i)
    {
        ringBuffer[0][(size_t) position] = channels > 0 && inputChannelData[0] != nullptr ? inputChannelData[0][i] : 0.0f;
        ringBuffer[1][(size_t) position] = channels > 1 && inputChannelData[1] != nullptr ? inputChannelData[1][i] : 0.0f;
        ringBuffer[2][(size_t) position] = hasOutput ? outputChannelData[0][i] : 0.0f;
        position = (position + 1) % capacity;
    }
    writePosition.store(position);
    samplesFilled.store(std::min(capacity, samplesFilled.load() + numSamples));

}

void AudioEngine::getGeneratorOutput(std::vector<float>& target)
{
    const juce::ScopedLock lock(generatorLock);

    target.assign(generatorCapture.begin(),
                  generatorCapture.begin() + juce::jlimit(0, capturedGeneratorSamples,
                                                         (int) generatorCapture.size()));
}

void AudioEngine::getLatestBlock(int numSamples, std::vector<float>& ref,
                                 std::vector<float>& meas, std::vector<float>* generated)
{
    std::lock_guard<std::mutex> lock(bufferMutex);

    if (generated != nullptr)
        generated->clear();

    if (ringBuffer.size() < 3 || capacity <= 0)
    {
        ref.clear();
        meas.clear();
        return;
    }

    const auto available = samplesFilled.load();
    const auto count = std::min(numSamples, available);

    if (count <= 0)
    {
        ref.clear();
        meas.clear();
        return;
    }

    const auto start = (writePosition.load() - count + capacity) % capacity;

    ref.resize((size_t) count);
    meas.resize((size_t) count);
    if (generated != nullptr)
        generated->resize((size_t) count);

    for (int i = 0; i < count; ++i)
    {
        const auto index = (size_t) ((start + i) % capacity);
        ref[(size_t) i] = ringBuffer[0][index];
        meas[(size_t) i] = ringBuffer[1][index];
        if (generated != nullptr)
            (*generated)[(size_t) i] = ringBuffer[2][index];
    }
}
