#include "WavRecorder.h"

WavRecorder::WavRecorder() = default;

WavRecorder::~WavRecorder()
{
    discard();
}

juce::String WavRecorder::getFormatName (BitDepth depth)
{
    switch (depth)
    {
        case BitDepth::Int16:   return "WAV";
        case BitDepth::Int24:   return "WAV";
        case BitDepth::Float32: return "WAV";
    }

    return "WAV";
}

juce::StringArray WavRecorder::getBitDepthNames()
{
    // A depth is named by what it holds, not by how many bytes it takes, because that is the
    // number the choice is actually about.
    return { "16-bit", "24-bit", "32-bit float" };
}

juce::StringArray WavRecorder::getSupportedSampleRates()
{
    return { "44100", "48000", "96000", "192000" };
}

bool WavRecorder::isSampleRateSupported (double sampleRate)
{
    for (const auto& rate : getSupportedSampleRates())
        if (rate.getDoubleValue() == sampleRate)
            return true;

    return false;
}

double WavRecorder::getRecordedSeconds() const noexcept
{
    if (sampleRate <= 0.0)
        return 0.0;

    return (double) getRecordedSamples() / sampleRate;
}

bool WavRecorder::startRecording (const juce::File& target, double newSampleRate,
                                  int newNumChannels, BitDepth newBitDepth,
                                  juce::String* errorOut)
{
    const auto refuse = [errorOut] (const juce::String& reason)
    {
        if (errorOut != nullptr)
            *errorOut = reason;

        return false;
    };

    if (isOpen() || getState() != State::Idle)
        return refuse ("A recording is already open");

    if (newSampleRate <= 0.0)
        return refuse ("The sample rate must be greater than zero");

    // A wav format reader needs a whole number of bits per sample. A rate that does not divide
    // evenly would be written as a different rate on the way back in, and the file would then
    // play at a different speed to the one that was recorded.
    if (! isSampleRateSupported (newSampleRate))
        return refuse ("Unsupported sample rate");

    if (newNumChannels < 1 || newNumChannels > 2)
        return refuse ("Only one or two channels can be recorded");

    if (target == juce::File())
        return refuse ("No file was given to record into");

    auto parent = target.getParentDirectory();

    if (! parent.exists())
    {
        if (auto result = parent.createDirectory(); result.failed())
            return refuse ("Could not create the folder to record into");
    }

    teardown();

    juce::WavAudioFormat wav;

    // Bits per sample, not bytes per frame. This wav writer picks integer pcm below thirty two
    // bits and IEEE float at thirty two, so naming the depth this way is what puts a float file
    // on disk rather than a thirty two bit integer one that would clip anything above full scale.
    const int bitsPerSample = newBitDepth == BitDepth::Int16 ? 16
                             : newBitDepth == BitDepth::Int24 ? 24
                                                              : 32;

    auto fileStream = std::make_unique<juce::FileOutputStream> (target);

    if (fileStream == nullptr || fileStream->failedToOpen())
        return refuse ("Could not open the file to record into");

    // Written straight to the file rather than through a buffering stream. This JUCE build has
    // no buffered output stream, and the rewrite of the wav header that every block causes is a
    // seek on the message thread. That is a cost worth paying here precisely because it is not
    // on the audio thread, which is the only place a seek would have been a fault.
    stream = std::move (fileStream);

    auto created = wav.createWriterFor (stream.get(), (double) newSampleRate,
                                        (unsigned int) newNumChannels, bitsPerSample,
                                        juce::StringPairArray(), 0);

    if (created == nullptr)
        return refuse ("Could not create a wav writer for the chosen format");

    // The writer takes ownership of the stream and deletes it in its own destructor, so the
    // ownership is handed over here and this recorder keeps no second pointer to it. Holding
    // both freed it twice: once through the writer's destructor and once through here.
    stream.release();
    writer.reset (created);

    const auto capacity = juce::jmax (1024, juce::roundToInt (newSampleRate * ringSeconds));

    if (! ring.prepare (newNumChannels, capacity))
        return refuse ("Could not allocate the recording buffer");

    readCursor = ring.getWritePosition();
    readGeneration = ring.getGeneration();

    targetFile = target;
    sampleRate = newSampleRate;
    numChannels = newNumChannels;
    bitDepth = newBitDepth;
    lastError.clear();

    recordedSamples.store (0);
    pausedSamples.store (0);
    droppedSamples.store (0);
    finishing.store (false);
    savedToDisk.store (false, std::memory_order_release);

    // Recording last. Before this the callback has nothing to write into, and publishing the
    // open state first would let it write into a ring that is about to be reallocated.
    state.store (State::Recording, std::memory_order_release);
    return true;
}

void WavRecorder::pushFromAudioThread (const float* const* planes, int numSamples) noexcept
{
    if (planes == nullptr || numSamples <= 0)
        return;

    if (state.load (std::memory_order_acquire) != State::Recording)
        return;

    const auto written = ring.write (planes, numSamples);

    if (written < numSamples)
        droppedSamples.fetch_add (numSamples - written, std::memory_order_relaxed);
}

void WavRecorder::pause()
{
    if (state.load (std::memory_order_acquire) == State::Recording)
    {
        // The samples already in the ring still belong to the recording, so they are written
        // out before the state changes. Recording does not stop until the audio that was
        // captured before the button was pressed is safely on disk.
        pump();
        state.store (State::Paused, std::memory_order_release);
    }
}

void WavRecorder::resume()
{
    if (state.load (std::memory_order_acquire) != State::Paused)
        return;

    // Everything that arrived while paused is thrown away rather than written, and the reader
    // is moved up to it so the recording continues from the resume point as one continuous
    // waveform. Inserting the gap as silence would be worse: it would put a step in the
    // waveform that reads as a real reflection.
    readCursor = ring.getWritePosition();
    readGeneration = ring.getGeneration();

    state.store (State::Recording, std::memory_order_release);
}

void WavRecorder::stop()
{
    const auto current = state.load (std::memory_order_acquire);

    if (current == State::Idle || current == State::Stopped)
        return;

    // Hold the callback off first, so the ring cannot grow while it is being drained.
    state.store (State::Stopped, std::memory_order_release);
    pump();

    // Anything that arrived between the drain above and the state change is still the audio
    // that was captured before the stop, so it is collected too.
    pump();
}

void WavRecorder::pump()
{
    if (writer == nullptr)
        return;

    if (ring.getGeneration() != readGeneration)
    {
        readCursor = ring.getWritePosition();
        readGeneration = ring.getGeneration();
    }

    const auto pending = ring.getWritePosition() - readCursor;

    if (pending <= 0)
        return;

    // Never move more than a block at a time. Draining the whole ring in one call would write
    // megabytes on a single message thread turn, which is the stall this design exists to
    // avoid.
    constexpr int maxFramesPerPump = 16384;

    const auto frames = (int) std::min<int64_t> (pending, maxFramesPerPump);

    pumpScratch.resize ((size_t) frames * (size_t) numChannels);

    const auto read = ring.read (readCursor, pumpScratch.data(), numChannels, frames);

    if (read <= 0)
        return;

    // The ring hands back one plane per channel and the buffer is handed over as planes, not
    // interleaved. That is not a preference. A format writer reinterprets the pointers it is
    // given as one pointer per channel and writes numChannels channels from them, so a single
    // channel holding interleaved samples is read as if it were the first plane: every other
    // sample landed in the wrong channel and the rest of the frame came from whatever happened
    // to follow the buffer. The file that came out had the right sample rate and the right
    // channel count and the wrong audio in it, which is the worst kind of wrong.
    if (interleaved.getNumChannels() != numChannels || interleaved.getNumSamples() < frames)
        interleaved.setSize (numChannels, std::max (frames, 16384), false, false, false);

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* destination = interleaved.getWritePointer (channel);
        const auto* source = pumpScratch.data() + (size_t) channel * (size_t) frames;

        juce::FloatVectorOperations::copy (destination, source, (size_t) read);
    }

    const auto success = writer->writeFromAudioSampleBuffer (interleaved, 0, read);

    if (state.load (std::memory_order_acquire) == State::Paused)
        pausedSamples.fetch_add (read, std::memory_order_relaxed);
    else
        recordedSamples.fetch_add (read, std::memory_order_relaxed);
}

bool WavRecorder::save()
{
    if (writer == nullptr)
        return ! targetFile.exists() == false; // nothing open: whatever is on disk already stands

    stop();

    writer.reset();
    teardown();

    if (state.load (std::memory_order_acquire) != State::Stopped)
        state.store (State::Stopped, std::memory_order_release);

    savedToDisk.store (true, std::memory_order_release);
    return lastError.isEmpty();
}

void WavRecorder::discard()
{
    const auto wasOpen = writer != nullptr || state.load (std::memory_order_acquire) != State::Idle;
    const auto wasSaved = savedToDisk.load (std::memory_order_acquire);
    const auto file = targetFile;

    state.store (State::Idle, std::memory_order_release);
    writer.reset();
    teardown();

    // Only a file this recorder created, and only one that never reached save(), is removed.
    // A saved recording is left exactly where it was put, which is what makes closing the
    // application after saving safe.
    if (wasOpen && ! wasSaved && file != juce::File() && file.exists())
        file.deleteFile();
}

void WavRecorder::teardown()
{
    writer.reset();
    stream.reset();

    readCursor = 0;
    readGeneration = 0;
    pumpScratch.clear();
    pumpScratch.shrink_to_fit();
    // Released by assignment rather than by asking for a zero sized buffer. Setting an audio
    // buffer to zero channels and zero samples is not a shape it accepts, and asking for it
    // walks off the end of the allocation it just freed.
    interleaved = juce::AudioBuffer<float>();
}