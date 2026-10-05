#include "AudioEngine.h"

#include <cmath>
#include <cstdlib>
#include <memory>

#if JUCE_LINUX
 #include <alsa/asoundlib.h>
#endif

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


class AudioEngine::InputGainControl
{
public:
    ~InputGainControl() { close(); }

    void openForDevice (const juce::String& deviceName)
    {
        close();

#if JUCE_LINUX
        const auto cardPrefix = cardPrefixForDevice (deviceName);
        if (cardPrefix.isEmpty())
            return;

        const auto cardId = findCardId (cardPrefix);
        if (cardId.isEmpty())
            return;

        if (snd_mixer_open (&mixer, 0) < 0
            || snd_mixer_attach (mixer, ("hw:" + cardId).toRawUTF8()) < 0
            || snd_mixer_selem_register (mixer, nullptr, nullptr) < 0
            || snd_mixer_load (mixer) < 0)
        {
            close();
            return;
        }

        int bestScore = 0;
        for (auto* candidate = snd_mixer_first_elem (mixer);
             candidate != nullptr; candidate = snd_mixer_elem_next (candidate))
        {
            if (! snd_mixer_selem_is_active (candidate)
                || ! snd_mixer_selem_has_capture_volume (candidate))
                continue;

            const auto score = controlNameScore (snd_mixer_selem_get_name (candidate));
            if (score > bestScore)
            {
                long minimum = 0, maximum = 0, current = 0;
                if (snd_mixer_selem_get_capture_dB_range (candidate, &minimum, &maximum) < 0
                    || ! readCaptureDb (candidate, current))
                    continue;

                const auto safeMinimum = juce::jmax (-96.0f, (float) minimum / 100.0f);
                const auto safeMaximum = juce::jmin (60.0f, (float) maximum / 100.0f);
                if (safeMaximum <= safeMinimum)
                    continue;

                bestScore = score;
                element = candidate;
                minimumDb = safeMinimum;
                maximumDb = safeMaximum;
                currentDb = juce::jlimit (minimumDb, maximumDb, (float) current / 100.0f);
            }
        }

        if (element == nullptr)
            close();
#else
        juce::ignoreUnused (deviceName);
#endif
    }

    void close()
    {
#if JUCE_LINUX
        element = nullptr;
        if (mixer != nullptr)
        {
            snd_mixer_close (mixer);
            mixer = nullptr;
        }
#endif
        minimumDb = maximumDb = currentDb = 0.0f;
    }

    HardwareInputGainInfo getInfo() const
    {
        HardwareInputGainInfo info;
        info.available = isAvailable();
        info.minimumDb = minimumDb;
        info.maximumDb = maximumDb;
        auto reportedDb = currentDb;
#if JUCE_LINUX
        long current = 0;
        if (info.available && readCaptureDb (element, current))
            reportedDb = (float) current / 100.0f;
#endif
        info.currentDb = info.available ? juce::jlimit (minimumDb, maximumDb, reportedDb)
                                        : reportedDb;
        return info;
    }

    bool setGainDb (float requestedDb)
    {
#if JUCE_LINUX
        if (! isAvailable() || ! std::isfinite (requestedDb))
            return false;

        const auto clamped = juce::jlimit (minimumDb, maximumDb, requestedDb);
        const auto alsaDb = std::lround ((double) clamped * 100.0);
        if (snd_mixer_selem_set_capture_dB_all (element, alsaDb, 0) < 0)
            return false;

        long applied = 0;
        if (! readCaptureDb (element, applied))
            return false;

        currentDb = (float) applied / 100.0f;
        return true;
#else
        juce::ignoreUnused (requestedDb);
        return false;
#endif
    }

private:
#if JUCE_LINUX
    static juce::String cardPrefixForDevice (const juce::String& deviceName)
    {
        const auto lower = deviceName.toLowerCase();
        if (lower.contains ("pipewire") || lower.contains ("pulse")
            || lower.startsWith ("default") || lower.startsWith ("sysdefault"))
            return {};

        const auto hw = lower.indexOf ("hw:");
        if (hw >= 0)
        {
            auto card = deviceName.substring (hw + 3).upToFirstOccurrenceOf (",", false, false).trim();
            if (card.startsWithIgnoreCase ("CARD="))
                card = card.substring (5);
            return card;
        }

        return alsaCardName (deviceName);
    }

    static juce::String findCardId (const juce::String& cardPrefix)
    {
        int cardNumber = -1;
        while (snd_card_next (&cardNumber) == 0 && cardNumber >= 0)
        {
            snd_ctl_t* control = nullptr;
            const auto controlName = "hw:" + juce::String (cardNumber);
            if (snd_ctl_open (&control, controlName.toRawUTF8(), SND_CTL_NONBLOCK) < 0)
                continue;

            snd_ctl_card_info_t* cardInfo = nullptr;
            snd_ctl_card_info_alloca (&cardInfo);
            juce::String id;
            juce::String name;
            if (snd_ctl_card_info (control, cardInfo) >= 0)
            {
                id = snd_ctl_card_info_get_id (cardInfo);
                name = snd_ctl_card_info_get_name (cardInfo);
            }
            snd_ctl_close (control);

            if (cardPrefix.equalsIgnoreCase (id) || cardPrefix.equalsIgnoreCase (name)
                || cardPrefix.startsWithIgnoreCase (name + ","))
                return id.isNotEmpty() ? id : juce::String (cardNumber);
        }

        return {};
    }

    static int controlNameScore (const char* rawName)
    {
        const auto name = juce::String (rawName).toLowerCase();
        if (name == "capture")          return 100;
        if (name.contains ("capture"))  return 90;
        if (name.contains ("mic"))      return 80;
        if (name.contains ("input"))    return 70;
        if (name.contains ("adc"))      return 60;
        if (name.contains ("line"))     return 50;
        return 0;
    }

    static bool readCaptureDb (snd_mixer_elem_t* captureElement, long& value)
    {
        for (int channel = SND_MIXER_SCHN_FRONT_LEFT;
             channel <= SND_MIXER_SCHN_LAST; ++channel)
        {
            const auto id = (snd_mixer_selem_channel_id_t) channel;
            if (snd_mixer_selem_has_capture_channel (captureElement, id)
                && snd_mixer_selem_get_capture_dB (captureElement, id, &value) >= 0)
                return true;
        }
        return false;
    }

    snd_mixer_t* mixer = nullptr;
    snd_mixer_elem_t* element = nullptr;
#endif
    float minimumDb = 0.0f;
    float maximumDb = 0.0f;
    float currentDb = 0.0f;

    bool isAvailable() const
    {
#if JUCE_LINUX
        return mixer != nullptr && element != nullptr && maximumDb > minimumDb;
#else
        return false;
#endif
    }
};


AudioEngine::AudioEngine()
    : inputGainControl (std::make_unique<InputGainControl>())
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

    // A device whose name says it is for playback cannot capture a microphone, whatever the
    // driver reports for its channel counts. ALSA's "default" is the trap: when PipeWire owns
    // the sound server it resolves to whatever is playing, it sorts to the front of the list
    // because "D" comes first, and it opens without complaint while capturing nothing. It
    // then became the default selection, so starting a session produced a running app with a
    // silent reference and no measurement at all. Such devices are ranked last instead of
    // being hidden, because a loopback cable through "default" is a legitimate thing to want.
    auto looksPlaybackOnly = [] (const juce::String& name)
    {
        const auto lower = name.toLowerCase();

        return lower.contains("output") || lower.contains("playback")
            || lower.contains("monitor") || lower.startsWith("default");
    };

    std::sort(candidates.begin(), candidates.end(), [&] (const Candidate& a, const Candidate& b)
    {
        const auto auxiliaryA = looksAuxiliary(a.choice.name);
        const auto auxiliaryB = looksAuxiliary(b.choice.name);

        if (auxiliaryA != auxiliaryB)
            return auxiliaryB;

        const auto playbackA = looksPlaybackOnly(a.choice.name);
        const auto playbackB = looksPlaybackOnly(b.choice.name);

        if (playbackA != playbackB)
            return playbackB;

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

HardwareInputGainInfo AudioEngine::getHardwareInputGainInfo() const
{
    return inputGainControl != nullptr ? inputGainControl->getInfo()
                                       : HardwareInputGainInfo();
}

bool AudioEngine::setHardwareInputGainDb (float gainDb)
{
    return inputGainControl != nullptr && inputGainControl->setGainDb (gainDb);
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
    startingOrStopping = true;
    stop();

#if JUCE_LINUX
    if (outputPulseSink.isNotEmpty())
        setPulseDefaultSink(outputPulseSink);
#endif

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = outputDeviceName;
    setup.sampleRate = std::max (8000.0, sampleRate);
    setup.bufferSize = juce::jlimit (16, 8192, bufferSize);

    // Two input channels and two output channels are opened, and the routing decides what
    // each of them carries. Opening only one output would leave the left/right routing
    // with nothing to separate.
    setup.useDefaultInputChannels = false;
    setup.inputChannels = juce::BigInteger ((1u << requestedInputChannels) - 1u);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels = juce::BigInteger ((1u << requestedOutputChannels) - 1u);

    // The callback is registered after the device is open, so it can never run against
    // storage that has not been allocated yet. Opening the device first also means the
    // reported sample rate and buffer size are the ones the device really accepted
    // rather than the ones that were asked for.
    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
    {
        // A device that cannot give the exact channel layout still works as a plain
        // capture, so the default layout is tried before giving up on it entirely.
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        error = deviceManager.setAudioDeviceSetup (setup, true);
    }

    if (error.isNotEmpty())
    {
        deviceManager.closeAudioDevice();
        startingOrStopping = false;
        hasLastSetup = false;
        setStatus ("Gagal membuka perangkat: " + error);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        startingOrStopping = false;
        hasLastSetup = false;
        setStatus ("Tidak ada device audio yang bisa dibuka");
        return;
    }

    if (inputGainControl != nullptr)
        inputGainControl->openForDevice (inputDeviceName);

    deviceManager.addAudioCallback (this);

    const auto actualRate = device->getCurrentSampleRate();
    const auto actualBlock = device->getCurrentBufferSizeSamples();

    currentSampleRate.store (actualRate);
    currentBufferSize.store (actualBlock);
    currentSampleRateInt.store ((int) actualRate);
    currentBufferSizeInt.store (actualBlock);

    // The channel count is published from the opened device rather than waiting for the
    // first callback, because the assignment selectors are filled in immediately after a
    // start and cannot ask the audio thread how many channels arrived.
    capturedChannels.store (juce::jlimit (0, requestedInputChannels,
                                           (int) device->getInputChannelNames().size()),
                            std::memory_order_relaxed);

    // The device thread may not have reached audioDeviceAboutToStart yet, so the capture
    // storage is prepared here as well. It is idempotent, so whichever happens first
    // wins and the other call re-prepares the same layout.
    initialiseBuffers (actualRate, actualBlock);

    lastInputDeviceName = inputDeviceName;
    lastOutputDeviceName = outputDeviceName;
    lastPulseSink = outputPulseSink;
    lastRequestedSampleRate = setup.sampleRate;
    lastRequestedBufferSize = setup.bufferSize;
    hasLastSetup = true;
    deviceLost.store (false, std::memory_order_release);

    startingOrStopping = false;

    // What the user gets back is what the device really does, so a sample rate that was
    // refused is visible here instead of only in the selector.
    setStatus ("Audio aktif: " + device->getName()
               + " @ " + juce::String ((int) actualRate)
               + " Hz / " + juce::String (actualBlock) + " samples / "
               + juce::String (getCapturedChannelCount()) + " kanal masuk");

    if (std::abs (actualRate - setup.sampleRate) > 0.5)
        setStatus ("Sample rate " + juce::String ((int) setup.sampleRate)
                   + " Hz tidak tersedia, perangkat memakai " + juce::String ((int) actualRate) + " Hz");
}

void AudioEngine::initialiseBuffers(double newSampleRate, int bufferSize)
{
    const auto rate = std::max (1000.0, newSampleRate);

    // Eight seconds of history: long enough that a slow analysis frame still finds
    // contiguous audio, short enough to keep the memory cost trivial. The storage is
    // allocated here and only here, while the device is stopped, so the audio thread
    // never sees it change underneath itself.
    releaseBuffers();

    capture.prepare (requestedInputChannels + 1, (int) std::max (4.0, rate * 8.0));

    latestCursor = newSampleCursor = 0;
    latestGeneration = newSampleGeneration = capture.getGeneration();

    buffersReady.store (capture.isReady(), std::memory_order_release);
}

void AudioEngine::releaseBuffers()
{
    // Clearing the flag before the storage goes away is what makes this safe to call
    // while the device is running: the callback sees it and writes nothing.
    buffersReady.store (false, std::memory_order_release);

    capture.reset();
    capturedChannels.store (0);
    callbackCount.store (0);
    deviceLost.store (false, std::memory_order_release);
}

void AudioEngine::syncReader(uint64_t& cursorGeneration, int64_t& cursor) const
{
    const auto generation = capture.getGeneration();

    if (cursorGeneration != generation)
    {
        cursorGeneration = generation;
        cursor = capture.getWritePosition();
    }
}

void AudioEngine::stop()
{
    startingOrStopping = true;
    if (inputGainControl != nullptr)
        inputGainControl->close();
    deviceManager.removeAudioCallback (this);

    if (auto* device = deviceManager.getCurrentAudioDevice())
        device->stop();

    deviceManager.closeAudioDevice();

    releaseBuffers();
    startingOrStopping = false;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const auto rate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    const auto block = device != nullptr ? device->getCurrentBufferSizeSamples() : 1024;

    currentSampleRate.store (rate);
    currentBufferSize.store (block);
    currentSampleRateInt.store ((int) rate);
    currentBufferSizeInt.store (block);

    // The device thread prepares the generator and the capture storage before the audio
    // thread starts, so reallocating here cannot race with a running callback.
    generator.prepare (rate);
    initialiseBuffers (rate, block);
}

void AudioEngine::audioDeviceStopped()
{
    // The device thread must not call the GUI or reopen a device, so the loss is only
    // recorded here and acted on by pollDeviceHealth() from the message thread.
    releaseBuffers();

    if (! startingOrStopping)
        deviceLost.store (true, std::memory_order_release);
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    const auto channels = std::min (requestedInputChannels, numInputChannels);
    capturedChannels.store (channels, std::memory_order_relaxed);

    // Render once into the first channel that carries the signal, then copy it to the
    // other enabled side so both get the exact same samples. A channel left out of the
    // routing is silenced rather than left holding whatever was there before.
    const auto routing = generatorRouting.load (std::memory_order_relaxed);
    auto hasOutput = false;
    auto firstEnabled = -1;

    if (outputChannelData != nullptr && numOutputChannels > 0)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;

            // A mono device has no side to pick, so it always carries the signal.
            const auto mono = numOutputChannels < 2;
            const auto enabled = mono
                              || routing == OutputRouting::Both
                              || (routing == OutputRouting::Left && ch == 0)
                              || (routing == OutputRouting::Right && ch == 1);

            if (enabled)
            {
                if (firstEnabled < 0)
                    firstEnabled = ch;
            }
            else
            {
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
            }
        }
    }

    if (firstEnabled >= 0)
    {
        hasOutput = true;
        generator.process (outputChannelData[firstEnabled], numSamples, 1.0f);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (ch == firstEnabled || outputChannelData[ch] == nullptr)
                continue;

            const auto enabled = numOutputChannels < 2
                              || routing == OutputRouting::Both
                              || (routing == OutputRouting::Left && ch == 0)
                              || (routing == OutputRouting::Right && ch == 1);

            if (enabled)
                juce::FloatVectorOperations::copy (outputChannelData[ch],
                                                   outputChannelData[firstEnabled], numSamples);
        }
    }

    // Capture is the last thing the callback does and it is a memcpy into storage that
    // was allocated when the device opened: no allocation, no lock, no file access, no
    // logging, and nothing that can block. The generator output rides along as the third
    // plane of the same ring, which is what removes the lock that used to guard a copy
    // of it.
    if (buffersReady.load (std::memory_order_acquire))
    {
        const float* planes[3] = { nullptr, nullptr, nullptr };

        if (inputChannelData != nullptr && channels > 0)
            planes[0] = inputChannelData[0];

        if (inputChannelData != nullptr && channels > 1)
            planes[1] = inputChannelData[1];

        if (hasOutput && firstEnabled >= 0)
            planes[2] = outputChannelData[firstEnabled];

        capture.write (planes, numSamples);

        // A recorder, when one is armed, is fed from the same pointers and by the same rule:
        // one call that copies into a ring and returns. Recording must not be the thing that
        // makes the audio thread late, because a late audio thread is an overrun and an
        // overrun loses the audio the recording was supposed to keep.
        if (auto* target = recorder.load (std::memory_order_acquire); target != nullptr)
            target->pushFromAudioThread (planes, numSamples);
    }

    callbackCount.fetch_add (1, std::memory_order_relaxed);
}

juce::String AudioEngine::channelName(int channelIndex)
{
    switch (channelIndex)
    {
        case 0:  return "Kiri (Ch1)";
        case 1:  return "Kanan (Ch2)";
        default: return "Ch " + juce::String(channelIndex + 1);
    }
}

void AudioEngine::getGeneratorOutput(std::vector<float>& target, int numSamples)
{
    // A graph needs the newest window each time it paints. A consuming cursor falls
    // behind when the UI refreshes less often than the audio callback, which made the
    // displayed waveform increasingly stale. Use a local snapshot cursor instead.
    const auto count = numSamples > 0 ? numSamples
                                      : juce::jmax (0, currentBufferSizeInt.load());
    if (count <= 0)
    {
        target.clear();
        return;
    }

    target.resize ((size_t) count);
    juce::FloatVectorOperations::clear (target.data(), count);

    if (! capture.isReady())
        return;

    const auto write = capture.getWritePosition();
    const auto available = (int) std::min<int64_t> (count, write);
    if (available <= 0)
        return;

    const auto padding = count - available;
    auto cursor = write - available;
    capture.readPlane (cursor, 2, target.data() + padding, available);
}

void AudioEngine::getLatestBlock(int numSamples, std::vector<float>& left,
                                 std::vector<float>& right, std::vector<float>* generated)
{
    if (numSamples <= 0 || ! capture.isReady())
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    syncReader (latestGeneration, latestCursor);

    const auto write = capture.getWritePosition();
    const auto available = std::max<int64_t> (0, write - latestCursor);
    const auto count = (int) std::min<int64_t> (numSamples, available);

    if (count <= 0)
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    // A window can only be given from audio the ring still holds. Asking for more than
    // exists means the window slides to the newest data, which is what a live display
    // wants and what keeps the caller from reading past the write position.
    if (available > count)
        latestCursor = write - count;

    // Reused between calls so the steady state does not allocate: only the requested
    // window times the plane count is held here.
    if ((int) captureScratch.size() < count * 3)
        captureScratch.assign ((size_t) count * 3, 0.0f);

    const auto read = capture.read (latestCursor, captureScratch.data(), 3, count);

    if (read <= 0)
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    left.assign (captureScratch.begin(), captureScratch.begin() + read);
    right.assign (captureScratch.begin() + read, captureScratch.begin() + 2 * read);

    if (generated != nullptr)
        generated->assign (captureScratch.begin() + 2 * read,
                           captureScratch.begin() + 3 * read);
}

bool AudioEngine::readLatestFrame (int frameSize, std::vector<float>& left,
                                   std::vector<float>& right, std::vector<float>* generated)
{
    left.clear();
    right.clear();
    if (generated != nullptr)
        generated->clear();

    if (frameSize <= 0 || ! capture.isReady())
        return false;

    const auto write = capture.getWritePosition();
    if (write < frameSize)
        return false;

    // This is a snapshot reader: use a local cursor so repeated UI refreshes overlap and
    // always include the newest microphone samples. The stream reader remains independent.
    auto cursor = write - frameSize;
    const auto required = (size_t) frameSize * 3;
    if (captureScratch.size() < required)
        captureScratch.resize (required);

    const auto read = capture.read (cursor, captureScratch.data(), 3, frameSize);
    if (read != frameSize)
        return false;

    left.assign (captureScratch.begin(), captureScratch.begin() + frameSize);
    right.assign (captureScratch.begin() + frameSize, captureScratch.begin() + frameSize * 2);
    if (generated != nullptr)
        generated->assign (captureScratch.begin() + frameSize * 2,
                           captureScratch.begin() + frameSize * 3);

    return true;
}

void AudioEngine::readFromRing (uint64_t& cursorGeneration, int64_t& cursor,
                                std::vector<float>& left, std::vector<float>& right,
                                std::vector<float>* generated)
{
    if (! capture.isReady())
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    syncReader (cursorGeneration, cursor);

    // Everything captured since this reader last looked, in arrival order. A stream
    // analyser needs exactly this: consecutive windows must not repeat the same audio,
    // or the frames it averages are the same frames counted many times.
    const auto pending = std::max<int64_t> (0, capture.getWritePosition() - cursor);

    if (pending <= 0)
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    const auto capacity = capture.getCapacity();

    if (pending > capacity)
        cursor = capture.getWritePosition() - capacity;

    const auto want = (int) std::min<int64_t> (pending, capacity);

    if ((int) captureScratch.size() < want * 3)
        captureScratch.assign ((size_t) want * 3, 0.0f);

    const auto read = capture.read (cursor, captureScratch.data(), 3, want);

    if (read <= 0)
    {
        left.clear();
        right.clear();

        if (generated != nullptr)
            generated->clear();

        return;
    }

    left.assign (captureScratch.begin(), captureScratch.begin() + read);
    right.assign (captureScratch.begin() + read, captureScratch.begin() + 2 * read);

    if (generated != nullptr)
        generated->assign (captureScratch.begin() + 2 * read,
                           captureScratch.begin() + 3 * read);

}

void AudioEngine::readNewSamples(std::vector<float>& left, std::vector<float>& right,
                                 std::vector<float>* generated)
{
    readFromRing (newSampleGeneration, newSampleCursor, left, right, generated);
}

bool AudioEngine::readFrame (int frameSize, std::vector<float>& left,
                             std::vector<float>& right, std::vector<float>* generated)
{
    left.clear();
    right.clear();

    if (generated != nullptr)
        generated->clear();

    if (frameSize <= 0 || ! capture.isReady())
        return false;

    // The ring was reallocated, so whatever was half collected describes a device that no
    // longer exists. Mixing it with the new audio would put a discontinuity in the middle of
    // a frame, which the transform turns into a broadband artefact that looks like a fault
    // in the room.
    if (frameAccumGeneration != capture.getGeneration())
    {
        frameAccumGeneration = capture.getGeneration();
        frameLeftAccum.clear();
        frameRightAccum.clear();
        frameGeneratedAccum.clear();
    }

    std::vector<float> newLeft, newRight, newGenerated;
    readFromRing (frameGeneration, frameCursor, newLeft, newRight, &newGenerated);

    if (! newLeft.empty())
    {
        frameLeftAccum.insert (frameLeftAccum.end(), newLeft.begin(), newLeft.end());
        frameRightAccum.insert (frameRightAccum.end(), newRight.begin(), newRight.end());
        frameGeneratedAccum.insert (frameGeneratedAccum.end(), newGenerated.begin(),
                                    newGenerated.end());
    }

    // Behind by more than two frames means the reader was starved, most likely because the
    // message thread stalled. Holding the stale audio would only push the frame further away,
    // so the oldest is dropped and the newest kept.
    const auto limit = (size_t) frameSize * 2;

    if (frameLeftAccum.size() > limit)
    {
        const auto excess = frameLeftAccum.size() - limit;
        frameLeftAccum.erase (frameLeftAccum.begin(),
                              frameLeftAccum.begin() + (ptrdiff_t) excess);
        frameRightAccum.erase (frameRightAccum.begin(),
                               frameRightAccum.begin() + (ptrdiff_t) excess);
        frameGeneratedAccum.erase (frameGeneratedAccum.begin(),
                                   frameGeneratedAccum.begin() + (ptrdiff_t) excess);
    }

    if (frameLeftAccum.size() < (size_t) frameSize)
        return false;

    // The oldest whole frame is handed over and consumed, so every sample is transformed
    // exactly once and the remainder is kept for the next call. Handing out overlapping
    // windows instead would report more averaging than actually happened.
    left.assign (frameLeftAccum.begin(), frameLeftAccum.begin() + frameSize);
    right.assign (frameRightAccum.begin(), frameRightAccum.begin() + frameSize);

    if (generated != nullptr)
        generated->assign (frameGeneratedAccum.begin(),
                           frameGeneratedAccum.begin() + frameSize);

    frameLeftAccum.erase (frameLeftAccum.begin(), frameLeftAccum.begin() + frameSize);
    frameRightAccum.erase (frameRightAccum.begin(), frameRightAccum.begin() + frameSize);
    frameGeneratedAccum.erase (frameGeneratedAccum.begin(),
                               frameGeneratedAccum.begin() + frameSize);

    return true;
}

void AudioEngine::resetFrameReader()
{
    frameLeftAccum.clear();
    frameRightAccum.clear();
    frameGeneratedAccum.clear();
}

int AudioEngine::getPendingSampleCount() const
{
    if (! capture.isReady())
        return 0;

    return (int) std::max<int64_t> (0, capture.getWritePosition() - newSampleCursor);
}

int64_t AudioEngine::getCaptureOverrunCount() const
{
    return capture.getOverrunCount();
}

bool AudioEngine::reconnect()
{
    if (! hasLastSetup)
    {
        setStatus ("Belum ada pengaturan perangkat untuk disambung ulang");
        return false;
    }

    start (lastInputDeviceName, lastOutputDeviceName, lastRequestedSampleRate,
           lastRequestedBufferSize, lastPulseSink);

    if (isRunning())
    {
        setStatus ("Audio tersambung kembali: " + juce::String ((int) currentSampleRate.load())
                   + " Hz / " + juce::String (currentBufferSize.load()) + " samples");
        return true;
    }

    return false;
}

bool AudioEngine::pollDeviceHealth()
{
    if (! deviceLost.load (std::memory_order_acquire))
        return false;

    deviceLost.store (false, std::memory_order_release);

    // Everything below runs on the message thread, which is the only thread allowed to
    // reopen a device or touch the GUI.
    if (onDeviceLost != nullptr)
        onDeviceLost();

    setStatus ("Perangkat audio terputus"
               + juce::String (autoReconnect.load() ? ", mencoba menyambung ulang" : ""));

    if (autoReconnect.load())
        reconnect();

    return true;
}
