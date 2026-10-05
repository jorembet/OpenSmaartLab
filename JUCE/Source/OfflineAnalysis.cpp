#include "OfflineAnalysis.h"

OfflineAnalysis::OfflineAnalysis() = default;
OfflineAnalysis::~OfflineAnalysis() = default;

juce::StringArray OfflineAnalysis::getMeasurementNames()
{
    return { "Spectrum", "RTA", "THD", "Transfer Function", "Impulse Response",
             "Reverberation", "SPL" };
}

juce::String OfflineAnalysis::measurementName (Measurement measurement)
{
    const auto names = getMeasurementNames();
    const auto index = (int) measurement;
    return index >= 0 && index < names.size() ? names[index] : juce::String();
}

juce::String OfflineAnalysis::measurementUnit (Measurement measurement)
{
    switch (measurement)
    {
        case Measurement::Rta:              return "dB";
        case Measurement::Distortion:       return "% / dB";
        case Measurement::TransferFunction: return "dB / deg / m";
        case Measurement::Reverberation:    return "s";
        case Measurement::Spl:              return "dB SPL";
        default:                            return "dB";
    }
}

std::vector<std::pair<OfflineAnalysis::Measurement, OfflineAnalysis::Report>>
OfflineAnalysis::Result::getReports() const
{
    return { { Measurement::Spectrum, spectrumReport },
             { Measurement::Rta, rtaReport },
             { Measurement::Distortion, distortionReport },
             { Measurement::TransferFunction, transferReport },
             { Measurement::ImpulseResponse, impulseReport },
             { Measurement::Reverberation, reverbReport },
             { Measurement::Spl, splReport } };
}

void OfflineAnalysis::runSpectrum (Result& result, const std::vector<float>& samples,
                                   const Settings& settings)
{
    spectrum.prepare (result.sampleRate, settings.fftSize);
    spectrum.setWindow (settings.window);
    spectrum.setOverlapPercent (settings.overlapPercent);
    spectrum.setAveraging (settings.averaging, settings.averagingSeconds);
    spectrum.reset();

    // Walked in blocks rather than pushed in one call. pushSamples hands out at most
    // getMaxFramesPerCall() frames per processAvailableFrames() call and counts the rest as
    // dropped, so a file pushed in one piece would report fewer frames analysed than it holds
    // and the frame count this class publishes would be short.
    constexpr int blockSize = 8192;
    size_t position = 0;

    while (position < samples.size())
    {
        const auto count = (int) std::min<size_t> (blockSize, samples.size() - position);
        spectrum.pushSamples (samples.data() + position, count);

        // Drained fully, with the RTA fed from each frame as it is produced. The limit is the
        // analyser's own, so a long block cannot leave frames behind.
        for (int guard = 0; guard < 512 && spectrum.hasFrameReady(); ++guard)
        {
            spectrum.processAvailableFrames();

            if (rta.getBandCount() > 0)
                rta.process (spectrum.getFrame());
        }

        position += (size_t) count;
    }

    for (int guard = 0; guard < 512 && spectrum.hasFrameReady(); ++guard)
    {
        spectrum.processAvailableFrames();

        if (rta.getBandCount() > 0)
            rta.process (spectrum.getFrame());
    }

    const auto& frame = spectrum.getFrame();

    result.spectrumFrames = frame.framesProcessed;
    result.spectrumDropped = frame.framesDropped;

    if (! frame.valid || frame.numBins < 4)
    {
        result.spectrumReport.skippedReason = "the file is shorter than one analysis frame";
        return;
    }

    result.frequency = frame.frequency;
    result.magnitudeDb = frame.magnitudeDb;
    result.powerDb = frame.powerDb;
    result.peakFrequency = spectrum.getPeakFrequency();
    result.peakMagnitudeDb = spectrum.getPeakMagnitude();

    // The average level comes from the mean of the power bins rather than the mean of the
    // decibels. Averaging decibels averages logarithms, which is not the same number as the
    // level of the averaged signal and reads low by several decibels.
    double powerSum = 0.0;
    int powerCount = 0;

    for (int bin = 0; bin < frame.numBins; ++bin)
    {
        if (frame.powerDb[(size_t) bin] > dsp::dbFloor + 0.01f)
        {
            powerSum += std::pow (10.0, frame.powerDb[(size_t) bin] / 10.0);
            ++powerCount;
        }
    }

    result.averageLevelDb = powerCount > 0
                          ? (float) (10.0 * std::log10 (powerSum / powerCount))
                          : dsp::dbFloor;

    // The noise floor is the median of the per-bin power, taken from the part of the spectrum
    // above the audio band. A mean would be pulled up by whatever is in the signal, and a
    // minimum would report the emptiest single bin, which is an accident of the window rather
    // than the noise.
    std::vector<float> noiseBinDb;

    for (int bin = 0; bin < frame.numBins; ++bin)
    {
        const auto hz = frame.frequency[(size_t) bin];

        if (hz > 20000.0f && frame.powerDb[(size_t) bin] > dsp::dbFloor + 0.01f)
            noiseBinDb.push_back (frame.powerDb[(size_t) bin]);
    }

    if (noiseBinDb.empty())
    {
        // Nothing above twenty kilohertz, which is the usual case at forty eight kilohertz.
        // The top octave of the spectrum is the next least useful place to look.
        for (int bin = frame.numBins / 2; bin < frame.numBins; ++bin)
            if (frame.powerDb[(size_t) bin] > dsp::dbFloor + 0.01f)
                noiseBinDb.push_back (frame.powerDb[(size_t) bin]);
    }

    if (! noiseBinDb.empty())
    {
        std::sort (noiseBinDb.begin(), noiseBinDb.end());
        result.noiseFloorDb = noiseBinDb[noiseBinDb.size() / 2];
    }

    result.spectrumReport.valid = true;
    result.spectrumReport.add ("Frames analysed", juce::String (frame.framesProcessed));
    result.spectrumReport.add ("Frames dropped", juce::String (frame.framesDropped));
    result.spectrumReport.add ("FFT size", juce::String (settings.fftSize));
    result.spectrumReport.add ("Window", dsp::SpectrumAnalyser::getWindowNames()[(int) settings.window]);
    result.spectrumReport.add ("Average level",
                               juce::String (result.averageLevelDb, 2) + " dB");
    result.spectrumReport.add ("Peak", juce::String (result.peakFrequency, 1)
                                       + " Hz at " + juce::String (result.peakMagnitudeDb, 2) + " dB");
    result.spectrumReport.add ("Noise floor", juce::String (result.noiseFloorDb, 2) + " dB");

    if (frame.framesDropped > 0)
        result.spectrumReport.add ("Note", "frames were dropped, so this covers less of the file");
}

void OfflineAnalysis::runDistortion (Result& result, const std::vector<float>& samples,
                                    const Settings& settings)
{
    if (! settings.measureDistortion)
    {
        result.distortionReport.skippedReason = "distortion measurement was switched off";
        return;
    }

    distortion.prepare (result.sampleRate, settings.fftSize);
    distortion.setSettings (settings.distortion);

    result.distortion = distortion.analyse (samples.data(), (int) samples.size());

    result.distortionReport.valid = result.distortion.valid;

    if (! result.distortion.valid)
    {
        result.distortionReport.skippedReason = "no stable fundamental was found";
        return;
    }

    result.distortionReport.add ("Fundamental",
                                 juce::String (result.distortion.fundamentalHz, 2) + " Hz at "
                                     + juce::String (result.distortion.fundamentalLevelDb, 2) + " dB");
    result.distortionReport.add ("THD", juce::String (result.distortion.thdPercent, 4) + " % ("
                                      + juce::String (result.distortion.thdDb, 2) + " dB)");
    result.distortionReport.add ("THD+N", juce::String (result.distortion.thdPlusNPercent, 4)
                                        + " % (" + juce::String (result.distortion.thdPlusNDb, 2) + " dB)");
    result.distortionReport.add ("SINAD", juce::String (result.distortion.sinadDb, 2) + " dB");
    result.distortionReport.add ("Noise", juce::String (result.distortion.noiseLevelDb, 2) + " dB");
    result.distortionReport.add ("Harmonics found",
                                 juce::String (result.distortion.harmonics.size()));
}

void OfflineAnalysis::runTransfer (Result& result, const std::vector<float>& channelA,
                                   const std::vector<float>& channelB,
                                   const Settings& settings)
{
    if (channelA.empty() || channelB.empty() || channelA.size() != channelB.size())
    {
        result.transferReport.skippedReason =
            "a transfer function needs two channels of equal length";
        return;
    }

    const auto& measurement = settings.measurementChannel == 0 ? channelA : channelB;
    const auto& reference = settings.measurementChannel == 0 ? channelB : channelA;

    transfer.prepare ((float) result.sampleRate, settings.fftSize);
    transfer.setAveraging (juce::jmax (1, settings.transferAverages));
    transfer.setAutomaticDelay (settings.automaticDelay);
    transfer.setDelayCompensation (settings.delayCompensation);
    transfer.setTemperature (settings.temperatureC);
    transfer.reset();

    // The live path pushes the captured pair into this same analyser with delay compensation
    // switched on, and the delay is only tracked when compensation is on, so it is switched on
    // here too. Configuring it differently would report a delay of zero for a file that plainly
    // contains one, which is what it did before this was matched to the live settings.
    constexpr int frameSize = 16384;

    for (size_t position = 0; position + frameSize <= measurement.size(); position += frameSize)
    {
        result.transfer = transfer.process (reference.data() + position,
                                            measurement.data() + position, frameSize);
    }

    result.transferReport.valid = result.transfer.valid;

    if (! result.transfer.valid)
    {
        result.transferReport.skippedReason =
            "the two channels hold nothing that corresponds to each other";
        return;
    }

    result.transferReport.add ("Magnitude",
                               juce::String (result.transfer.magnitudeDb.size()) + " bins");
    result.transferReport.add ("Delay", juce::String (result.transfer.delayMs, 3) + " ms ("
                                        + juce::String (result.transfer.delaySamples, 1) + " samples)");
    result.transferReport.add ("Distance", juce::String (result.transfer.delayDistanceM, 3) + " m at "
                                           + juce::String (result.transfer.temperatureC, 1) + " C");
    result.transferReport.add ("Average coherence", juce::String (result.transfer.averageCoherence, 4));
    result.transferReport.add ("Phase slope delay",
                               juce::String (result.transfer.phaseSlopeDelayMs, 3) + " ms from "
                                   + juce::String (result.transfer.phaseSlopeBins) + " bins");
    result.transferReport.add ("Delay trusted", result.transfer.delayTrusted ? "yes" : "no");
    result.transferReport.add ("Impulse peak", juce::String (result.transfer.impulsePeakIndex)
                                        + " samples");
}

void OfflineAnalysis::runReverberation (Result& result, const std::vector<float>& samples,
                                        const Settings& settings)
{
    if (samples.size() < 256)
    {
        result.reverbReport.skippedReason = "the file is too short to measure a decay";
        return;
    }

    result.acoustics = ImpulseResponse::analyse (samples.data(), (int) samples.size(),
                                                 (float) result.sampleRate);

    dsp::ReverbAnalyser::Settings reverbSettings;
    reverbSettings.resolution = settings.resolution == dsp::RtaAnalyser::Resolution::OneOctave
                              ? dsp::ReverbAnalyser::Resolution::Octave
                              : dsp::ReverbAnalyser::Resolution::ThirdOctave;

    reverb.setSettings (reverbSettings);
    result.reverb = reverb.analyse (samples.data(), (int) samples.size(), result.sampleRate);

    result.impulseReport.valid = result.acoustics.valid;

    if (! result.acoustics.valid)
    {
        result.impulseReport.skippedReason = "no usable impulse response was found in the file";
    }
    else
    {
        result.impulseReport.add ("Direct arrival",
                                  juce::String (result.acoustics.directArrivalMs, 2) + " ms at sample "
                                      + juce::String (result.acoustics.directArrivalSample));
        result.impulseReport.add ("C50", juce::String (result.acoustics.c50, 2) + " dB");
        result.impulseReport.add ("C80", juce::String (result.acoustics.c80, 2) + " dB");
        result.impulseReport.add ("D50", juce::String (result.acoustics.d50, 1) + " %");
        result.impulseReport.add ("Centre time",
                                  juce::String (result.acoustics.centreTime * 1000.0f, 1) + " ms");
    }

    result.reverbReport.valid = result.reverb.valid;

    if (! settings.measureReverberation)
    {
        result.reverbReport.skippedReason = "reverberation measurement was switched off";
    }
    else if (! result.reverb.valid)
    {
        result.reverbReport.skippedReason = "no decay could be measured: "
                                         + dsp::ReverbAnalyser::qualityToString (result.reverb.quality);
    }
    else
    {
        result.reverbReport.add ("EDT", juce::String (result.reverb.edt, 3) + " s");
        result.reverbReport.add ("T20", juce::String (result.reverb.t20, 3) + " s");
        result.reverbReport.add ("T30", juce::String (result.reverb.t30, 3) + " s");
        result.reverbReport.add ("RT60", juce::String (result.reverb.rt60, 3) + " s");
        result.reverbReport.add ("Decay range",
                                 juce::String (result.reverb.decayRangeDb, 1) + " dB");
        result.reverbReport.add ("Quality", dsp::ReverbAnalyser::qualityToString (result.reverb.quality));
        result.reverbReport.add ("Bands measured", juce::String (result.reverb.bandsValid)
                                        + " of " + juce::String (result.reverb.bands.size()));
    }
}

void OfflineAnalysis::runSpl (Result& result, const std::vector<float>& samples,
                              const Settings& settings)
{
    if (! settings.measureSpl)
    {
        result.splReport.skippedReason = "sound pressure level was switched off";
        return;
    }

    if (! (settings.calibration.isEnabled() && settings.calibration.hasMeasuredSensitivity()))
    {
        result.splReport.skippedReason =
            "no microphone calibration, so this is a level and not a sound pressure level";
    }

    spl.prepare (result.sampleRate);
    spl.setCalibration (settings.calibration);
    spl.setTimeWeighting (settings.splWeighting);
    spl.reset();
    spl.resetPeak();
    spl.resetLeq();

    spl.process (samples.data(), (int) samples.size());

    result.splZ = spl.getLevel (dsp::SplAnalyser::Weighting::Z);
    result.splA = spl.getLevel (dsp::SplAnalyser::Weighting::A);
    result.splC = spl.getLevel (dsp::SplAnalyser::Weighting::C);
    result.leqZ = spl.getLeq (dsp::SplAnalyser::Weighting::Z);
    result.leqA = spl.getLeq (dsp::SplAnalyser::Weighting::A);
    result.leqC = spl.getLeq (dsp::SplAnalyser::Weighting::C);
    result.peakZ = spl.getPeak (dsp::SplAnalyser::Weighting::Z);
    result.peakA = spl.getPeak (dsp::SplAnalyser::Weighting::A);
    result.peakC = spl.getPeak (dsp::SplAnalyser::Weighting::C);

    result.splReport.valid = true;
    result.splReport.add ("Z", juce::String (result.splZ, 1) + " / peak "
                              + juce::String (result.peakZ, 1)
                              + " / LEq " + juce::String (result.leqZ, 1) + " dB");
    result.splReport.add ("A", juce::String (result.splA, 1) + " / peak "
                              + juce::String (result.peakA, 1)
                              + " / LEq " + juce::String (result.leqA, 1) + " dB");
    result.splReport.add ("C", juce::String (result.splC, 1) + " / peak "
                              + juce::String (result.peakC, 1)
                              + " / LEq " + juce::String (result.leqC, 1) + " dB");
    result.splReport.add ("Time weighting",
                          dsp::SplAnalyser::getTimeWeightingNames()[(int) settings.splWeighting]);

    if (! (settings.calibration.isEnabled() && settings.calibration.hasMeasuredSensitivity()))
        result.splReport.add ("Calibration", "not calibrated, so these are relative levels");
}

bool OfflineAnalysis::readFile (const juce::File& file, std::vector<float>& channelA,
                                std::vector<float>& channelB, double& sampleRate,
                                int& numChannels, juce::String* errorOut)
{
    const auto refuse = [errorOut] (const juce::String& reason)
    {
        if (errorOut != nullptr)
            *errorOut = reason;

        return false;
    };

    if (! file.existsAsFile())
        return refuse ("That file does not exist");

    auto manager = std::make_unique<juce::AudioFormatManager>();
    manager->registerBasicFormats();

    // The manager tries each registered format itself, which is what lets a wav be opened
    // without the interface having to care that it is a wav.
    std::unique_ptr<juce::AudioFormatReader> reader (manager->createReaderFor (file));

    if (reader == nullptr)
        return refuse ("That file is not an audio file this build can read");

    sampleRate = reader->sampleRate;
    numChannels = (int) reader->numChannels;

    if (sampleRate <= 0.0 || numChannels < 1)
        return refuse ("That file reports no usable sample rate or channel count");

    const auto lengthInSamples = (int) reader->lengthInSamples;

    if (lengthInSamples <= 0)
        return refuse ("That file holds no audio");

    juce::AudioSampleBuffer buffer (numChannels, lengthInSamples);

    if (! reader->read (&buffer, 0, lengthInSamples, 0, true, true))
        return refuse ("That file could not be decoded");

    // readFile takes two channels because every measurement that needs a pair needs one, and
    // a mono file is by far the most common recording there is. Anything past two is dropped:
    // no measurement here uses it and holding it would cost memory for nothing.
    const auto take = juce::jmin (numChannels, 2);

    channelA.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + lengthInSamples);

    if (take > 1)
        channelB.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + lengthInSamples);
    else
        channelB.clear();

    return true;
}

bool OfflineAnalysis::isSupportedFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    return reader != nullptr;
}

OfflineAnalysis::Result OfflineAnalysis::analyse (const std::vector<float>& channelA,
                                                  const std::vector<float>& channelB,
                                                  double sampleRate,
                                                  const Settings& settings)
{
    Result result;

    if (channelA.empty())
    {
        result.error = "There are no samples to analyse";
        return result;
    }

    if (sampleRate <= 0.0)
    {
        result.error = "The sample rate must be greater than zero";
        return result;
    }

    result.sampleRate = sampleRate;
    result.numChannels = channelB.empty() ? 1 : 2;
    result.numFrames = (int64_t) channelA.size();
    result.durationSeconds = (double) channelA.size() / sampleRate;

    // The RTA is driven from the spectrum analyser's frames rather than from a pass of its own.
    // A second pass would be fed a second average of the same file, and the two would disagree
    // by however much the file changed between them, which for a moving signal is a lot.
    rta.prepare (sampleRate, settings.fftSize);
    rta.setResolution (settings.resolution);
    rta.setFrequencyRange (settings.rtaLowHz, settings.rtaHighHz);
    rta.setAveraging (settings.averaging, settings.averagingSeconds);
    rta.setIntegration (dsp::RtaAnalyser::Integration::Average);
    rta.reset();

    runSpectrum (result, channelA, settings);

    if (! result.spectrumReport.valid)
    {
        result.rtaReport.skippedReason = result.spectrumReport.skippedReason;
    }
    else
    {
        result.rtaBands = rta.getBands();
        result.rtaFullyResolved = rta.isFullyResolved();
        result.rtaReport.valid = true;
        result.rtaReport.add ("Bands", juce::String (rta.getBandCount()));
        result.rtaReport.add ("Resolution",
                              settings.resolution == dsp::RtaAnalyser::Resolution::OneOctave
                                  ? juce::String ("1/1 octave") : juce::String ("1/3 octave"));
        result.rtaReport.add ("Range", juce::String (settings.rtaLowHz, 0) + " Hz to "
                                          + juce::String (settings.rtaHighHz, 0) + " Hz");
        result.rtaReport.add ("All bands resolved", rta.isFullyResolved() ? "yes" : "no");

        if (! rta.isFullyResolved())
            result.rtaReport.add ("Unresolved bands",
                                  juce::String (rta.getUnresolvedBandCount())
                                      + " are narrower than one bin and are held flat");
    }

    runDistortion (result, channelA, settings);
    runTransfer (result, channelA, channelB, settings);
    runReverberation (result, channelA, settings);
    runSpl (result, channelA, settings);

    result.valid = true;
    return result;
}

OfflineAnalysis::Result OfflineAnalysis::analyseFile (const juce::File& file,
                                                      const Settings& settings)
{
    std::vector<float> channelA, channelB;
    double sampleRate = 0.0;
    int numChannels = 0;
    juce::String error;

    if (! readFile (file, channelA, channelB, sampleRate, numChannels, &error))
    {
        Result result;
        result.error = error;
        return result;
    }

    auto result = analyse (channelA, channelB, sampleRate, settings);

    // The file's own channel count is reported rather than the pair's, because a file with four
    // channels is a file this build read and threw two of them away.
    result.numChannels = numChannels;
    return result;
}