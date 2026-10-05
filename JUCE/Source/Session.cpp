#include "Session.h"
#include <cstring>
#include <juce_data_structures/juce_data_structures.h>
#include "WavRecorder.h"

namespace
{
    const juce::Identifier traceKindId ("kind");
    const juce::Identifier traceSourceId ("source");
    const juce::Identifier traceXId ("x");
    const juce::Identifier traceYId ("y");
    const juce::Identifier traceNameId ("name");

    /** Floats are written with enough digits to come back the same.

        Six significant figures is the most a spreadsheet will hold without showing noise, and
        writing fewer would mean a curve exported and read back is not the curve that was
        measured. A difference of a hundredth of a decibel is not a measurement. */
    juce::String numberToText (float value)
    {
        if (! std::isfinite (value))
            return "";

        return juce::String (value, 8);
    }

    /** Arrays of floats are stored as the raw bytes in hex rather than as written-out numbers.

        A sixteen thousand point spectrum is a long way to type and a text array of it makes a
        session file enormous and slow to read. Hex costs two characters a byte, keeps the
        samples exact, and survives a file being edited or moved between machines without a
        question of what the encoding was. The endianness is stated in the header and the file
        is written little endian to match. */
    juce::String floatsToHex (const std::vector<float>& values)
    {
        static const char* digits = "0123456789abcdef";

        std::string bytes;

        // Two characters for every byte, so the buffer is twice the size of the samples. Sized
        // by the sample count alone it was half what it needed to be, and every trace came back
        // truncated rather than whole.
        bytes.resize (values.size() * sizeof (float) * 2);

        for (std::size_t i = 0; i < values.size(); ++i)
        {
            static_assert (sizeof (float) == 4, "the hex format assumes a four byte float");

            std::uint32_t bits = 0;
            std::memcpy (&bits, &values[i], sizeof (bits));

            // Eight characters to a float, four bytes at two characters each. Stepping by four
            // instead of eight made each value overwrite the upper half of the one before it,
            // so a saved curve came back as zeros with only its last point intact.
            for (int byte = 0; byte < 4; ++byte)
            {
                const auto value = (bits >> (byte * 8)) & 0xffu;
                bytes[i * 8 + (std::size_t) byte * 2] = digits[value >> 4];
                bytes[i * 8 + (std::size_t) byte * 2 + 1] = digits[value & 0x0fu];
            }
        }

        return juce::String (bytes.data(), (int) bytes.size());
    }

    bool hexToFloats (const juce::String& hex, std::vector<float>& values, int expectedCount)
    {
        if (expectedCount <= 0 || hex.length() != expectedCount * 8)
            return false;

        values.resize ((std::size_t) expectedCount);

        const auto text = hex.toStdString();

        for (int i = 0; i < expectedCount; ++i)
        {
            std::uint32_t bits = 0;

            for (int byte = 0; byte < 4; ++byte)
            {
                const auto high = std::strchr ("0123456789abcdef", text[(std::size_t) i * 8 + (std::size_t) byte * 2]);
                const auto low  = std::strchr ("0123456789abcdef", text[(std::size_t) i * 8 + (std::size_t) byte * 2 + 1]);

                if (high == nullptr || low == nullptr)
                    return false;

                bits |= (std::uint32_t) ((high - "0123456789abcdef") * 16
                                         + (low - "0123456789abcdef")) << (byte * 8);
            }

            std::memcpy (&values[(std::size_t) i], &bits, sizeof (float));
        }

        return true;
    }

    /** Turns an xml element back into a value tree.

        The value tree's own readFromStream is a binary format, which would have meant a session
        file nobody can read without this program. Reading the xml back by hand keeps the file
        openable in a text editor, which is the property the format exists for: a disputed
        measurement starts with somebody reading what was actually recorded.

        Properties are stored as their text, which is what they were written as. Numbers come back
        through fromString, so a value that was written as a float reads as a float. */
    bool valueTreeFromXml (const juce::XmlElement* element, juce::ValueTree& treeOut)
    {
        if (element == nullptr)
            return false;

        const auto tag = element->getTagName();

        if (tag.isEmpty())
            return false;

        treeOut = juce::ValueTree (tag);

        for (int attribute = 0; attribute < element->getNumAttributes(); ++attribute)
        {
            const auto name = element->getAttributeName (attribute);
            const auto value = element->getAttributeValue (attribute);

            if (name.isEmpty())
                continue;

            // Everything comes back as the text it was written as. Guessing from the shape of
            // the text whether it holds a number is not possible: the letter e turns up in the
            // middle of ordinary words like "system" and "universal", so a value whose name
            // says it is a device name was being read as a number and came back as zero. The
            // readers below ask for the type they want, and a value that holds text converts
            // cleanly when they do.
            treeOut.setProperty (name, value, nullptr);
        }

        for (int child = 0; child < element->getNumChildElements(); ++child)
        {
            juce::ValueTree childTree;

            if (valueTreeFromXml (element->getChildElement (child), childTree))
                treeOut.appendChild (childTree, nullptr);
        }

        return true;
    }

    juce::ValueTree traceToTree (TraceStore::Kind kind, TraceStore::Source source,
                                 const TraceStore::Trace& trace)
    {
        juce::ValueTree tree ("trace");
        tree.setProperty (traceKindId, (int) kind, nullptr);
        tree.setProperty (traceSourceId, (int) source, nullptr);
        tree.setProperty (traceNameId, trace.sourceName, nullptr);

        if (! trace.valid)
            return tree;

        tree.setProperty (traceXId, floatsToHex (trace.x), nullptr);
        tree.setProperty (traceYId, floatsToHex (trace.y), nullptr);
        tree.setProperty ("count", juce::String ((int) trace.x.size()), nullptr);

        return tree;
    }

    bool traceFromTree (const juce::ValueTree& tree, TraceStore::Trace& traceOut)
    {
        traceOut.sourceName = tree.getProperty (traceNameId).toString();
        traceOut.clear();

        const auto count = (int) tree.getProperty ("count");

        // The two decoded arrays must both come back at the declared length, or the trace is
        // refused rather than half loaded. A curve with an x axis of one length and a y axis of
        // another is not a curve, and plotting it would read past the end of one of them.
        if (! hexToFloats (tree.getProperty (traceXId).toString(), traceOut.x, count))
            return false;

        if (! hexToFloats (tree.getProperty (traceYId).toString(), traceOut.y, count))
        {
            traceOut.clear();
            return false;
        }

        traceOut.valid = true;
        return true;
    }
}

Session::Session()
{
    createdAt = juce::Time::getCurrentTime().toString (true, true);
    applicationVersion = "1.0";
}

void Session::clear()
{
    inputDevice.clear();
    outputDevice.clear();
    outputPulseSink.clear();
    inputChannel = 0;
    outputChannel = 1;
    measurementChannel = 0;
    referenceChannel = 1;
    sampleRate = 48000.0;
    bufferSize = 1024;
    fftSize = 16384;
    averaging = 1;
    averagingSeconds = 3.0f;
    window = 1;
    overlapPercent = 50.0f;
    thirdOctave = false;
    rtaLowHz = 20.0f;
    rtaHighHz = 20000.0f;
    automaticDelay = true;
    delayCompensation = true;
    temperatureC = 20.0f;
    calibration = MicrophoneCalibration();
    traces.clearAll();
    notes.clear();

    createdAt = juce::Time::getCurrentTime().toString (true, true);
    applicationVersion = "1.0";
}

juce::String Session::describe() const
{
    return inputDevice.isNotEmpty() ? inputDevice : juce::String ("no device")
         + "  |  " + juce::String ((int) sampleRate) + " Hz"
         + "  |  " + juce::String (bufferSize) + " buffer"
         + "  |  FFT " + juce::String (fftSize)
         + "  |  " + (thirdOctave ? juce::String ("1/3") : juce::String ("1/1")) + " octave"
         + "  |  " + createdAt;
}

juce::ValueTree Session::toValueTree() const
{
    juce::ValueTree root ("OpenSmaartLabSession");

    root.setProperty ("version", formatVersion, nullptr);
    root.setProperty ("createdAt", createdAt, nullptr);
    root.setProperty ("application", applicationVersion, nullptr);

    juce::ValueTree device ("device");
    device.setProperty ("input", inputDevice, nullptr);
    device.setProperty ("output", outputDevice, nullptr);
    device.setProperty ("pulseSink", outputPulseSink, nullptr);
    device.setProperty ("inputChannel", inputChannel, nullptr);
    device.setProperty ("outputChannel", outputChannel, nullptr);
    device.setProperty ("measurementChannel", measurementChannel, nullptr);
    device.setProperty ("referenceChannel", referenceChannel, nullptr);
    device.setProperty ("sampleRate", sampleRate, nullptr);
    device.setProperty ("bufferSize", bufferSize, nullptr);

    juce::ValueTree settings ("settings");
    settings.setProperty ("fftSize", fftSize, nullptr);
    settings.setProperty ("averaging", averaging, nullptr);
    settings.setProperty ("averagingSeconds", averagingSeconds, nullptr);
    settings.setProperty ("window", window, nullptr);
    settings.setProperty ("overlapPercent", overlapPercent, nullptr);
    settings.setProperty ("thirdOctave", thirdOctave, nullptr);
    settings.setProperty ("rtaLowHz", rtaLowHz, nullptr);
    settings.setProperty ("rtaHighHz", rtaHighHz, nullptr);
    settings.setProperty ("automaticDelay", automaticDelay, nullptr);
    settings.setProperty ("delayCompensation", delayCompensation, nullptr);
    settings.setProperty ("temperatureC", temperatureC, nullptr);

    root.appendChild (device, nullptr);
    root.appendChild (settings, nullptr);
    root.setProperty ("calibration", calibration.toJson(), nullptr);

    for (int kind = 0; kind < TraceStore::numKinds; ++kind)
    {
        const auto kindValue = (TraceStore::Kind) kind;

        for (int source = 0; source < TraceStore::numSources; ++source)
        {
            const auto sourceValue = (TraceStore::Source) source;
            const auto& trace = traces.get (kindValue, sourceValue);

            if (trace.valid)
                root.appendChild (traceToTree (kindValue, sourceValue, trace), nullptr);
        }
    }

    juce::ValueTree state ("state");
    state.setProperty ("averagedIn", traces.getAveragesFolded (TraceStore::Kind::Average), nullptr);
    root.appendChild (state, nullptr);

    root.setProperty ("notes", notes, nullptr);

    return root;
}

bool Session::fromValueTree (const juce::ValueTree& root, juce::String* errorOut)
{
    const auto refuse = [errorOut] (const juce::String& reason)
    {
        if (errorOut != nullptr)
            *errorOut = reason;

        return false;
    };

    if (! root.hasType (juce::Identifier ("OpenSmaartLabSession")))
        return refuse ("That file is not an OpenSmaartLab session");

    const auto version = (int) root.getProperty ("version");

    // Refused rather than half-read. A newer format read by an older build would produce a
    // session that looks complete and has silently dropped whatever the older build did not
    // know about, which is the one outcome worse than refusing.
    if (version > formatVersion)
        return refuse ("That session was written by a newer version of the application (format "
                       + juce::String (version) + ", this build reads up to "
                       + juce::String (formatVersion) + ")");

    createdAt = root.getProperty ("createdAt").toString();
    applicationVersion = root.getProperty ("application").toString();
    notes = root.getProperty ("notes").toString();

    if (auto device = root.getChildWithName ("device"); device.isValid())
    {
        inputDevice = device.getProperty ("input").toString();
        outputDevice = device.getProperty ("output").toString();
        outputPulseSink = device.getProperty ("pulseSink").toString();
        inputChannel = (int) device.getProperty ("inputChannel");
        outputChannel = (int) device.getProperty ("outputChannel");
        measurementChannel = (int) device.getProperty ("measurementChannel");
        referenceChannel = (int) device.getProperty ("referenceChannel");
        sampleRate = (double) device.getProperty ("sampleRate");
        bufferSize = (int) device.getProperty ("bufferSize");

        if (sampleRate <= 0.0)
            sampleRate = 48000.0;

        if (bufferSize <= 0)
            bufferSize = 1024;
    }

    if (auto settings = root.getChildWithName ("settings"); settings.isValid())
    {
        fftSize = (int) settings.getProperty ("fftSize");
        averaging = (int) settings.getProperty ("averaging");
        averagingSeconds = (float) settings.getProperty ("averagingSeconds");
        window = (int) settings.getProperty ("window");
        overlapPercent = (float) settings.getProperty ("overlapPercent");
        thirdOctave = (bool) settings.getProperty ("thirdOctave");
        rtaLowHz = (float) settings.getProperty ("rtaLowHz");
        rtaHighHz = (float) settings.getProperty ("rtaHighHz");
        automaticDelay = (bool) settings.getProperty ("automaticDelay");
        delayCompensation = (bool) settings.getProperty ("delayCompensation");
        temperatureC = (float) settings.getProperty ("temperatureC");

        // Every one of these comes back from a file somebody else wrote, so each is put back in
        // range rather than trusted. A transform size that is not a power of two would fail
        // inside the FFT, and a negative buffer size would fail inside the device.
        fftSize = juce::jlimit (256, 65536, fftSize);
        averaging = juce::jmax (1, averaging);
        averagingSeconds = juce::jlimit (0.01f, 300.0f, averagingSeconds);
        overlapPercent = juce::jlimit (0.0f, 99.0f, overlapPercent);
        bufferSize = juce::jmax (16, bufferSize);
        rtaLowHz = juce::jmax (1.0f, rtaLowHz);
        rtaHighHz = juce::jmax (rtaLowHz, rtaHighHz);
        temperatureC = juce::jlimit (-40.0f, 60.0f, temperatureC);
    }

    // A calibration that will not parse leaves the existing profile alone rather than clearing
    // it. Replacing a working calibration with a default because the saved one was damaged would
    // silently change every sound pressure level the session goes on to report.
    {
        const auto saved = root.getProperty ("calibration").toString();

        if (saved.isNotEmpty())
        {
            MicrophoneCalibration restored;

            if (restored.fromJson (saved))
                calibration = restored;
        }
    }

    traces.clearAll();

    int tracedCount = 0;

    for (int child = 0; child < root.getNumChildren(); ++child)
    {
        const auto tree = root.getChild (child);

        if (tree.getType() != juce::Identifier ("trace"))
            continue;

        const auto kindIndex = (int) tree.getProperty (traceKindId);
        const auto sourceIndex = (int) tree.getProperty (traceSourceId);

        if (! TraceStore::isValidKind (kindIndex) || ! TraceStore::isValidSource (sourceIndex))
            continue;

        TraceStore::Trace trace;

        if (traceFromTree (tree, trace))
        {
            traces.setStored ((TraceStore::Kind) kindIndex, (TraceStore::Source) sourceIndex, trace);
            ++tracedCount;
        }
    }

    if (auto state = root.getChildWithName ("state"); state.isValid())
        traces.setAveragesFolded (TraceStore::Kind::Average,
                                  (int) state.getProperty ("averagedIn"));

    return true;
}

bool Session::save (const juce::File& file, juce::String* errorOut) const
{
    const auto root = toValueTree();

    auto parent = file.getParentDirectory();

    if (! parent.exists() && parent.createDirectory().failed())
    {
        if (errorOut != nullptr)
            *errorOut = "Could not create the folder to save into";

        return false;
    }

    // Written through the xml document rather than a stream helper, because the point of the
    // format is that it stays readable. What lands on disk is a document someone can open in a
    // text editor and understand when a measurement is disputed.
    const auto xml = root.createXml();

    if (xml == nullptr || ! xml->writeToFile (file, {}, "UTF-8", 0))
    {
        if (errorOut != nullptr)
            *errorOut = "Could not write the session file";

        return false;
    }

    return true;
}

bool Session::load (const juce::File& file, juce::String* errorOut)
{
    if (! file.existsAsFile())
    {
        if (errorOut != nullptr)
            *errorOut = "That session file does not exist";

        return false;
    }

    // The file is read as text and handed straight to the value tree parser, rather than being
    // opened as a document and put back together afterwards. Taking the document apart and
    // putting it back together is a step that can lose the thing being read, and a session that
    // loads as an empty one is the worst possible outcome of that.
    juce::String text = file.loadFileAsString().trim();

    // The declaration line an xml writer puts at the top is not part of the value tree format,
    // and the parser reads the first line as the tree's type. Left in, every session comes back
    // with the type "<?xml version="1.0"..." and every load is refused as not a session.
    if (text.startsWith ("<?xml"))
    {
        if (const auto end = text.indexOf ("?>"); end > 0)
            text = text.substring (end + 2).trim();
    }

    if (text.isEmpty())
    {
        if (errorOut != nullptr)
            *errorOut = "That file is not a readable session";

        return false;
    }

    const auto document = juce::XmlDocument::parse (text);

    if (document == nullptr)
    {
        if (errorOut != nullptr)
            *errorOut = "That file is not a readable session";

        return false;
    }

    juce::ValueTree root;

    if (! valueTreeFromXml (document.get(), root))
    {
        if (errorOut != nullptr)
            *errorOut = "That file is not a readable session";

        return false;
    }

    return fromValueTree (root, errorOut);
}