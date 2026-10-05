/* Checks for recording to a wav file and analysing it again afterwards.

   The claims under test are the ones the recorder and the offline path make to each other:
   that what goes in comes back out at the right depth and rate, that a file can be reopened and
   read, and that analysing the file gives the same answer as analysing the same samples live.
   The last one is the important one, and it is only meaningful because both paths call the same
   analysers, so a disagreement here would mean the plumbing between them is wrong rather than
   the maths being different. */

#include "WavRecorder.h"
#include "OfflineAnalysis.h"
#include <cstdio>
#include <iostream>

namespace
{
    int failures = 0;
    int checks = 0;

    void report (bool condition, const char* message, const juce::String& detail = {})
    {
        ++checks;

        if (condition)
        {
            std::cout << "PASS: " << message << "\n";
            return;
        }

        ++failures;
        std::cout << "FAIL: " << message << "  [" << detail << "]\n";
    }

    /** Sample rate for these checks. High enough that the fractional frame positions in the
        delay test land on different samples, which is what makes that test worth running. */
    constexpr double rate = 48000.0;

    juce::File scratchFile (const juce::String& name)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("opensmaartlab_" + name);
    }

    struct Sine
    {
        double phase = 0.0;
        float next (double hz, double sampleRate)
        {
            const auto value = (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * hz / sampleRate;

            if (phase > juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            return value;
        }
    };

    /** A two channel signal: a tone on the left and the same tone delayed on the right, so a
        transfer function has something real to find. */
    void tonePair (double hz, int64_t delaySamples, double seconds,
                   std::vector<float>& left, std::vector<float>& right)
    {
        const auto count = (int64_t) (seconds * rate);
        left.assign ((size_t) count, 0.0f);
        right.assign ((size_t) count, 0.0f);

        Sine a, b;

        for (int64_t i = 0; i < count; ++i)
        {
            left[(size_t) i] = a.next (hz, rate) * 0.5f;

            const auto j = i - delaySamples;

            if (j >= 0)
                right[(size_t) i] = b.next (hz, rate) * 0.25f;
        }
    }

    /** Broadband noise, deterministic so a failure can be reproduced.

        A pure tone cannot measure a delay. Its transfer function is one bin, its impulse
        response is a spike somewhere and nothing to either side of it, and a delay read off
        that is an artefact of the window rather than a property of the signal. Noise has a
        real impulse response and a delay that can be measured from it. */
    struct Noise
    {
        std::uint32_t state = 0x12345678u;

        float next()
        {
            state = state * 1664525u + 1013904223u;
            return ((float) (state >> 8) / (float) (1 << 24)) * 2.0f - 1.0f;
        }
    };

    /** Two channels of noise, the second one the first shifted by a known number of samples. */
    void noisePair (int delaySamples, double seconds, double amplitude,
                    std::vector<float>& left, std::vector<float>& right)
    {
        const auto count = (int) (seconds * rate);
        left.assign ((size_t) count, 0.0f);
        right.assign ((size_t) count, 0.0f);

        Noise a, b;

        for (int i = 0; i < count; ++i)
        {
            left[(size_t) i] = a.next() * (float) amplitude;

            const auto j = i - delaySamples;

            if (j >= 0)
                right[(size_t) i] = b.next() * (float) amplitude;
        }
    }

    /** Feeds a signal through the recorder the way the audio callback does, then pumps it to
        disk the way the message thread does. */
    bool recordThroughCallback (WavRecorder& recorder, const std::vector<float>& left,
                                const std::vector<float>& right)
    {
        constexpr int blockSize = 512;
        size_t position = 0;

        while (position < left.size())
        {
            const auto count = (int) std::min<size_t> (blockSize, left.size() - position);

            const float* planes[2] = { left.data() + position, right.data() + position };
            recorder.pushFromAudioThread (planes, count);
            recorder.pump();

            position += (size_t) count;
        }

        return true;
    }

    /** Reads a wav back and reports the rate, the channel count and the peak of each channel. */
    struct ReadBack
    {
        double sampleRate = 0.0;
        int numChannels = 0;
        int64_t length = 0;
        float peakA = 0.0f, peakB = 0.0f;
        bool valid = false;
    };

    ReadBack readBack (const juce::File& file)
    {
        ReadBack out;

        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::FileInputStream> (file);

        if (stream == nullptr || stream->failedToOpen())
            return out;

        // The stream is handed over by pointer and the reader takes ownership of it, so the
        // test reads the file the same way any other reader would rather than through a
        // shortcut that could differ from what the application does.
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (stream.release(), true));

        if (reader == nullptr)
            return out;

        out.sampleRate = reader->sampleRate;
        out.numChannels = (int) reader->numChannels;
        out.length = (int64_t) reader->lengthInSamples;

        juce::AudioSampleBuffer buffer (out.numChannels, (int) out.length);

        if (! reader->read (&buffer, 0, (int) out.length, 0, true, true))
            return out;

        for (int64_t i = 0; i < out.length; ++i)
        {
            out.peakA = std::max (out.peakA, std::abs (buffer.getSample (0, (int) i)));

            if (out.numChannels > 1)
                out.peakB = std::max (out.peakB, std::abs (buffer.getSample (1, (int) i)));
        }

        out.valid = true;
        return out;
    }
}

// Test 1: a recorded file reopens at the rate and depth it was written at, with the signal
// still in it. This is the record and reopen half of the acceptance, and it is checked at all
// three depths because each one takes a different path through the writer.
void testRecordAndReopen()
{
    std::vector<float> left, right;
    tonePair (1000.0, 0, 1.0, left, right);

    const juce::Array<WavRecorder::BitDepth> depths { WavRecorder::BitDepth::Int16,
                                                     WavRecorder::BitDepth::Int24,
                                                     WavRecorder::BitDepth::Float32 };

    const juce::Array<juce::String> names { "16-bit", "24-bit", "32-bit float" };

    for (int index = 0; index < depths.size(); ++index)
    {
        const auto depth = depths[index];
        const auto name = names[index];

        auto file = scratchFile ("depth_" + juce::String (index) + ".wav");
        file.deleteFile();

        WavRecorder recorder;
        juce::String error;

        const auto opened = recorder.startRecording (file, rate, 2, depth, &error);

        report (opened, ("a " + name + " recording must be able to open").toRawUTF8(), error);

        if (! opened)
            continue;

        recordThroughCallback (recorder, left, right);

        report (recorder.save(), ("a " + name + " recording must save").toRawUTF8(),
                recorder.getLastError());

        const auto read = readBack (file);

        report (read.valid, ("a " + name + " file must reopen").toRawUTF8());
        report (read.sampleRate == rate, ("a " + name + " file must keep its sample rate").toRawUTF8(),
                juce::String (read.sampleRate));
        report (read.numChannels == 2, ("a " + name + " file must keep both channels").toRawUTF8(),
                juce::String (read.numChannels));

        // The whole point of a capture is that what was measured is still in the file. A file
        // that reopens at the right rate with silence in it has lost the measurement, so the
        // peak is checked rather than the header.
        report (read.peakA > 0.4f && read.peakA < 0.55f,
                ("a " + name + " file must keep the left channel's level").toRawUTF8(),
                juce::String (read.peakA, 4));

        report (read.peakB > 0.2f && read.peakB < 0.28f,
                ("a " + name + " file must keep the right channel's level").toRawUTF8(),
                juce::String (read.peakB, 4));

        // Sixteen bit quantises, and a quantisation step of one in 32768 is far larger than
        // anything here, so the level is allowed to move by that much and no more.
        if (depth == WavRecorder::BitDepth::Int16)
            report (std::abs (read.peakA - 0.5f) < 1.0f / 32768.0f,
                    "a 16-bit file must be quantised to its own step and no further",
                    juce::String (std::abs (read.peakA - 0.5f), 8));
        else
            report (std::abs (read.peakA - 0.5f) < 1.0e-5f,
                    ("a " + name + " file must keep the level exactly").toRawUTF8(),
                    juce::String (std::abs (read.peakA - 0.5f), 8));

        report (recorder.getRecordedSamples() == left.size(),
                ("a " + name + " recording must write every sample it was given").toRawUTF8(),
                juce::String (recorder.getRecordedSamples()) + " of "
                    + juce::String (left.size()));

        file.deleteFile();
    }
}

// Test 2: pause and stop. The recording must be one continuous waveform, so nothing may be
// inserted into the gap a pause leaves.
void testPauseAndStop()
{
    std::vector<float> left, right;
    tonePair (500.0, 0, 2.0, left, right);

    auto file = scratchFile ("pause.wav");
    file.deleteFile();

    WavRecorder recorder;
    recorder.startRecording (file, rate, 2, WavRecorder::BitDepth::Float32);

    // A second of audio, a pause, then the rest of it. Looped by position rather than by block
    // index, because counting whole blocks threw away the final partial one: a second at
    // 48000 is ninety three blocks of 512 and a remainder, and the remainder was never fed in.
    // The recorder was then asked to keep audio it had never been given.
    constexpr int blockSize = 512;
    const auto firstHalf = (int) rate;

    const auto feedUpTo = [&recorder, &left, &right, blockSize] (int limit)
    {
        for (int position = 0; position < limit; position += blockSize)
        {
            const auto count = std::min (blockSize, limit - position);
            const float* planes[2] = { left.data() + position, right.data() + position };
            recorder.pushFromAudioThread (planes, count);
            recorder.pump();
        }
    };

    feedUpTo (firstHalf);

    recorder.pause();

    report (recorder.isPaused(), "pausing must be reported as paused");
    report (recorder.getRecordedSamples() == firstHalf,
            "everything captured before the pause must still be written",
            juce::String (recorder.getRecordedSamples()) + " against " + juce::String (firstHalf));

    // Audio arriving while paused is discarded, and the recording continues from the resume
    // point rather than from where the pause began.
    feedUpTo ((int) left.size());

    report (recorder.getRecordedSamples() == firstHalf,
            "audio arriving while paused must not be written into the file",
            juce::String (recorder.getRecordedSamples()));

    recorder.resume();
    report (recorder.isRecording(), "resuming must be reported as recording");

    // Fed again from the resume point only. Feeding the whole file again would put the audio
    // that was already written in front of the pause back into the file, and the recording would
    // come out longer than the signal that went in.
    for (int position = firstHalf; position < (int) left.size(); position += blockSize)
    {
        const auto count = std::min (blockSize, (int) left.size() - position);
        const float* planes[2] = { left.data() + position, right.data() + position };
        recorder.pushFromAudioThread (planes, count);
        recorder.pump();
    }

    recorder.save();

    const auto read = readBack (file);

    report (read.valid, "a paused recording must still save");
    // The first second was kept, and the second was fed again after the resume, so the file
    // holds the whole of the signal and no silence has been inserted in place of the pause.
    report (read.length == (int64_t) left.size(),
            "the file must be as long as the audio that was kept, with no silence inserted",
            juce::String (read.length) + " against " + juce::String (left.size()));
    report (read.peakA > 0.4f, "a paused recording must still hold its signal",
            juce::String (read.peakA, 4));

    // Stopping twice, and saving twice, must be harmless. An interface that lets a user press
    // stop after stop should not be able to lose the file.
    report (recorder.save(), "saving again must be harmless", recorder.getLastError());
    recorder.stop();
    report (recorder.getState() == WavRecorder::State::Stopped, "stopping must be reported stopped");

    file.deleteFile();
}

// Test 3: a refused recording must say why and leave nothing behind.
void testRefusals()
{
    WavRecorder recorder;
    juce::String error;

    report (! recorder.startRecording (juce::File(), rate, 2, WavRecorder::BitDepth::Int24, &error),
            "recording into no file must be refused");
    report (error.isNotEmpty(), "a refusal must say why");

    error.clear();
    report (! recorder.startRecording (scratchFile ("bad_rate.wav"), 22050.0, 2,
                                      WavRecorder::BitDepth::Int24, &error),
            "a sample rate outside the supported list must be refused", error);

    error.clear();
    report (! recorder.startRecording (scratchFile ("bad_channels.wav"), rate, 7,
                                      WavRecorder::BitDepth::Int24, &error),
            "a channel count this build cannot record must be refused", error);

    auto file = scratchFile ("twice.wav");
    file.deleteFile();

    report (recorder.startRecording (file, rate, 1, WavRecorder::BitDepth::Float32, &error),
            "the first recording must open", error);

    error.clear();
    report (! recorder.startRecording (file, rate, 1, WavRecorder::BitDepth::Float32, &error),
            "a second recording on top of the first must be refused", error);

    // An unsaved recording has to go when it is discarded, or a run of tests leaves files
    // behind that look like measurements somebody took.
    recorder.discard();
    report (! file.existsAsFile(), "a discarded recording must not leave its file behind");

    report (WavRecorder::getSupportedSampleRates().size() == 4,
            "the four documented sample rates must be offered",
            juce::String (WavRecorder::getSupportedSampleRates().size()));
    report (WavRecorder::isSampleRateSupported (192000.0),
            "192 kHz must be offered for hardware that supports it");
    report (! WavRecorder::isSampleRateSupported (22050.0),
            "a rate this build does not claim must not be offered");
}

// Test 4: analysing a file must give the same answer as analysing the same samples live. This
// is the claim the whole offline path rests on, and it is checked on a number of readings
// rather than one, because two code paths can agree on a peak and disagree on everything else.
void testOfflineMatchesLive()
{
    std::vector<float> left, right;
    tonePair (1000.0, 37, 3.0, left, right);

    auto file = scratchFile ("offline.wav");
    file.deleteFile();

    WavRecorder recorder;
    recorder.startRecording (file, rate, 2, WavRecorder::BitDepth::Float32);
    recordThroughCallback (recorder, left, right);
    recorder.save();

    OfflineAnalysis offline;

    OfflineAnalysis::Settings settings;
    settings.fftSize = 16384;
    settings.calibration = MicrophoneCalibration::daytonImm6c();
    settings.calibration.setEnabled (true);

    const auto fromFile = offline.analyseFile (file, settings);

    report (fromFile.valid, "a saved file must analyse", fromFile.error);
    report (fromFile.sampleRate == rate, "the analysis must use the file's own sample rate",
            juce::String (fromFile.sampleRate));
    report (fromFile.numChannels == 2, "both channels must be read back",
            juce::String (fromFile.numChannels));

    // The same samples, handed straight in rather than through a file.
    OfflineAnalysis direct;
    const auto directResult = direct.analyse (left, right, rate, settings);

    report (directResult.valid, "the same samples must analyse directly", directResult.error);

    report (std::abs (fromFile.peakFrequency - directResult.peakFrequency) < 1.0,
            "the peak frequency must agree between file and direct",
            juce::String (fromFile.peakFrequency, 2) + " against "
                + juce::String (directResult.peakFrequency, 2));

    report (std::abs (fromFile.peakMagnitudeDb - directResult.peakMagnitudeDb) < 0.05f,
            "the peak level must agree between file and direct",
            juce::String (fromFile.peakMagnitudeDb, 3) + " against "
                + juce::String (directResult.peakMagnitudeDb, 3));

    report (std::abs (fromFile.averageLevelDb - directResult.averageLevelDb) < 0.05f,
            "the average level must agree between file and direct",
            juce::String (fromFile.averageLevelDb, 3) + " against "
                + juce::String (directResult.averageLevelDb, 3));

    report (std::abs (fromFile.splA - directResult.splA) < 0.05f,
            "the A weighted level must agree between file and direct",
            juce::String (fromFile.splA, 3) + " against " + juce::String (directResult.splA, 3));

    // The RTA bands have to match bin for bin, not merely in number.
    const auto bandCount = (int) std::min (fromFile.rtaBands.size(), directResult.rtaBands.size());

    // One octave from twenty hertz to twenty kilohertz is about eleven bands, not the sixty
    // odd a third octave gives, and the low bands below the first analysable frequency are
    // left out rather than reported as resolved.
    report (bandCount >= 8, "the RTA must have produced bands", juce::String (bandCount));

    if (bandCount > 0)
    {
        report (fromFile.rtaBands[(size_t) bandCount - 1].frequency >= 16000.0f,
                "the RTA must reach the top of the configured range",
                juce::String (fromFile.rtaBands[(size_t) bandCount - 1].frequency, 1));

        // Consecutive octave centres must be a factor of two apart. A set of bands that is not
        // an octave series means the band edges moved somewhere they should not have.
        bool octaveSeries = true;

        for (int band = 1; band < bandCount; ++band)
        {
            const auto ratio = fromFile.rtaBands[(size_t) band].frequency
                             / std::max (1.0f, fromFile.rtaBands[(size_t) band - 1].frequency);

            if (ratio < 1.7f || ratio > 2.4f)
                octaveSeries = false;
        }

        report (octaveSeries, "the RTA bands must be an octave series");
    }

    float worstBandDifference = 0.0f;

    for (int band = 0; band < bandCount; ++band)
    {
        const auto difference = std::abs (fromFile.rtaBands[(size_t) band].levelDb
                                            - directResult.rtaBands[(size_t) band].levelDb);
        worstBandDifference = std::max (worstBandDifference, difference);
    }

    report (worstBandDifference < 0.05f,
            "every RTA band must agree between file and direct",
            juce::String (worstBandDifference, 4) + " dB");

    // The transfer function has to find the delay that was put into the signal, and has to
    // report the same one whichever way the samples arrived.
    report (fromFile.transfer.valid, "the transfer function must be measurable from the file",
            fromFile.transferReport.skippedReason);
    report (directResult.transfer.valid, "the transfer function must be measurable directly",
            directResult.transferReport.skippedReason);

    if (fromFile.transfer.valid && directResult.transfer.valid)
    {
        report (std::abs (fromFile.transfer.delayMs - directResult.transfer.delayMs) < 0.05f,
                "the delay must agree between file and direct",
                juce::String (fromFile.transfer.delayMs, 3) + " ms against "
                    + juce::String (directResult.transfer.delayMs, 3) + " ms");

    }

    // Every measurement must either have an answer or a stated reason. A blank report is the
    // one outcome that cannot be acted on.
    for (const auto& entry : fromFile.getReports())
    {
        const auto name = OfflineAnalysis::measurementName (entry.first);
        const auto& entryReport = entry.second;

        report (entryReport.valid || entryReport.skippedReason.isNotEmpty(),
                ("the " + name + " report must answer or say why not").toRawUTF8(),
                entryReport.skippedReason);

        if (entryReport.valid)
            report (! entryReport.lines.isEmpty(),
                    ("the " + name + " report must carry readings").toRawUTF8());
    }

    file.deleteFile();
}

// Test 5: the delay a broadband signal carries must survive the round trip. This is the check
// that says the recording is a measurement and not a copy of a sound, and it is the only one
// that can be made on broadband, for the reason given on noisePair.
void testDelaySurvivesRecording()
{
    constexpr int delaySamples = 37;

    std::vector<float> left, right;
    noisePair (delaySamples, 3.0, 0.25, left, right);

    auto file = scratchFile ("delay.wav");
    file.deleteFile();

    WavRecorder recorder;
    recorder.startRecording (file, rate, 2, WavRecorder::BitDepth::Float32);
    recordThroughCallback (recorder, left, right);
    recorder.save();

    OfflineAnalysis offline;
    OfflineAnalysis::Settings settings;
    settings.fftSize = 16384;
    settings.automaticDelay = true;

    const auto fromFile = offline.analyseFile (file, settings);
    OfflineAnalysis direct;
    const auto directResult = direct.analyse (left, right, rate, settings);

    report (fromFile.transfer.valid, "broadband noise must give a usable transfer function",
            fromFile.transferReport.skippedReason);
    report (directResult.transfer.valid, "the same noise must give one directly",
            directResult.transferReport.skippedReason);

    if (fromFile.transfer.valid)
    {
        const auto expectedMs = 1000.0 * delaySamples / rate;

        report (std::abs (std::abs (fromFile.transfer.delaySamples) - (float) delaySamples) < 1.5f,
                "the delay in the file must be the delay that was put there",
                juce::String (fromFile.transfer.delaySamples, 2) + " samples against "
                    + juce::String (delaySamples));

        // The channel that was shifted is the reference here, so the reference arrives late and
        // the sign is negative. The convention is that a negative delay means the measurement
        // arrived first, and a test that only checked the size would not notice it being
        // reversed.
        report (fromFile.transfer.delaySamples < 0.0f,
                "a reference that arrives late must report a negative delay",
                juce::String (fromFile.transfer.delaySamples, 2) + " samples");

        report (std::abs (std::abs (fromFile.transfer.delayMs) - expectedMs) < 0.05f,
                "the delay in the file must be the delay that was put there in milliseconds",
                juce::String (fromFile.transfer.delayMs, 4) + " ms against "
                    + juce::String (expectedMs, 4) + " ms");

        report (fromFile.transfer.averageCoherence > 0.9f,
                "noise against itself must show high coherence",
                juce::String (fromFile.transfer.averageCoherence, 4));

        report (std::abs (fromFile.transfer.magnitudeDb[100]
                            - directResult.transfer.magnitudeDb[100]) < 0.2f,
                "the magnitude must agree between file and direct",
                juce::String (fromFile.transfer.magnitudeDb[100], 3) + " against "
                    + juce::String (directResult.transfer.magnitudeDb[100], 3));
    }

    file.deleteFile();
}

// Test 6: files the analyser has to refuse, and readings that must come out finite.
void testOfflineEdgeCases()
{
    OfflineAnalysis offline;
    OfflineAnalysis::Settings settings;

    {
        const auto result = offline.analyseFile (juce::File::getSpecialLocation (
                            juce::File::tempDirectory).getChildFile ("opensmaartlab_nothing_here.wav"),
                                                 settings);
        report (! result.valid, "a file that does not exist must be refused");
        report (result.error.isNotEmpty(), "refusing a missing file must say why", result.error);
    }

    {
        std::vector<float> empty;
        const auto result = offline.analyse (empty, {}, rate, settings);
        report (! result.valid, "an empty signal must be refused");
        report (result.error.isNotEmpty(), "refusing an empty signal must say why");
    }

    {
        const auto result = offline.analyse ({ 1.0f }, {}, 0.0, settings);
        report (! result.valid, "a zero sample rate must be refused");
    }

    // A file far too short to hold a frame cannot produce a spectrum, and must say so instead
    // of reporting a flat line as though it were a measurement.
    {
        std::vector<float> tiny (512, 0.25f);
        const auto result = offline.analyse (tiny, {}, rate, settings);
        report (! result.spectrumReport.valid,
                "a file shorter than one frame must not report a spectrum",
                result.spectrumReport.skippedReason);
        report (result.spectrumReport.skippedReason.isNotEmpty(),
                "a file too short to measure must say why");
    }

    // Silence in, nothing measurable out, and above all no NaN anywhere in what is reported.
    {
        std::vector<float> silence ((size_t) (rate * 2), 0.0f);
        const auto result = offline.analyse (silence, {}, rate, settings);

        const bool finite = std::isfinite (result.averageLevelDb)
                         && std::isfinite (result.noiseFloorDb)
                         && std::isfinite (result.peakMagnitudeDb)
                         && std::isfinite (result.peakFrequency)
                         && std::isfinite (result.splZ) && std::isfinite (result.splA)
                         && std::isfinite (result.leqZ);

        report (finite, "a silent file must report finite numbers and never NaN",
                juce::String (result.averageLevelDb) + " / " + juce::String (result.noiseFloorDb));
    }

    // A mono file has no pair, so the transfer function must decline and say so rather than
    // compare the channel against itself and report a perfect one.
    {
        std::vector<float> mono ((size_t) (rate * 2), 0.1f);
        const auto result = offline.analyse (mono, {}, rate, settings);

        report (! result.transfer.valid, "a single channel cannot give a transfer function");
        report (result.transferReport.skippedReason.isNotEmpty(),
                "declining a mono file must say why");
    }

    report (OfflineAnalysis::getMeasurementNames().size() == 7,
            "every measurement the panel offers must be runnable",
            juce::String (OfflineAnalysis::getMeasurementNames().size()));
}

int main()
{
    std::cout << "--- record and reopen ---\n";
    testRecordAndReopen();

    std::cout << "--- pause and stop ---\n";
    testPauseAndStop();

    std::cout << "--- refusals ---\n";
    testRefusals();

    std::cout << "--- offline agrees with live ---\n";
    testOfflineMatchesLive();

    std::cout << "--- delay survives recording ---\n";
    testDelaySurvivesRecording();

    std::cout << "--- edge cases ---\n";
    testOfflineEdgeCases();

    std::cout << "\n" << (checks - failures) << " of " << checks << " checks passed\n";

    if (failures > 0)
        std::cout << failures << " check(s) failed\n";

    return failures == 0 ? 0 : 1;
}