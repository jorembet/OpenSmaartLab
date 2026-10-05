#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace dsp
{
    /** FFT based spectrum analysis with correct window normalisation.

        The class is a self contained measurement core: it is fed the captured audio, and
        it owns the framing, the window, the normalisation and the averaging. Nothing
        about it depends on the audio device, so it can be driven by the live capture or
        by a synthetic signal in a test with the same code path.

        Two normalisations are produced on purpose, because they answer different
        questions and one window cannot serve both:

        - Magnitude is normalised by the window's coherent gain, 2|X| / (N * CG). A sine
          at a bin centre therefore reads its own amplitude for every window, so a 0 dBFS
          tone reads 0 dBFS whichever window is selected. This is what the display shows.

        - Power is normalised by the window's energy, 2|X|^2 / sum(w^2). That is the only
          normalisation under which the level does not depend on the window's shape, so it
          is the one to use for noise floors and for comparing windows. Summed over the
          positive frequencies it returns the variance of the signal.
    */
    class SpectrumAnalyser
    {
    public:
        enum class Window
        {
            Rectangular = 0,
            Hann,
            Hamming,
            Blackman,
            BlackmanHarris,
            FlatTop
        };

        enum class Averaging
        {
            Off = 0,
            Exponential
        };

        /** One analysed frame. The vectors are owned by the analyser and reused, so read
            them before the next frame is processed rather than keeping a reference. */
        struct Frame
        {
            std::vector<float> frequency;
            /** Peak amplitude per bin, 0 dBFS being full scale. */
            std::vector<float> magnitude;
            std::vector<float> magnitudeDb;
            /** Window independent energy per bin. See the class comment. */
            std::vector<float> power;
            std::vector<float> powerDb;
            /** magnitudeDb after averaging; equals magnitudeDb when averaging is off. */
            std::vector<float> averagedDb;

            int fftSize = 0;
            int numBins = 0;
            int hopSize = 0;
            int framesProcessed = 0;
            /** Frames dropped because the caller pushed faster than they were analysed. */
            int framesDropped = 0;
            double sampleRate = 48000.0;
            bool valid = false;
        };

        SpectrumAnalyser();
        ~SpectrumAnalyser();

        /** Allocates every buffer. Nothing in pushSamples or processAvailableFrames
            allocates, so this has to be called before audio is analysed. */
        void prepare (double newSampleRate, int newFftSize);

        void setWindow (Window newWindow);
        Window getWindow() const { return windowType; }

        /** 0 for no overlap, 50 for 50 % overlap. Fractional hops are allowed but are
            rounded, so the frame rate is always an integer number of samples. */
        void setOverlapPercent (float percent);

        void setAveraging (Averaging newMode, float timeConstantSeconds);

        /** How much faster a rise in level is followed than a fall.

            One is a plain symmetric average, which is what this was before. Raising it makes a
            level that rises show up almost at once while a level that still eases down over the
            set time constant, so a signal arriving or a measurement starting appears
            immediately without the curve flickering under noise.

            A symmetric average cannot give both: fast enough to feel responsive and it
            flickers, smooth enough to read and it feels broken. This is the asymmetry that
            every hardware display uses for the same reason. */
        void setAttackRatio (float ratio)
        {
            attackRatio = juce::jlimit (1.0f, 32.0f, ratio);

            // Recomputed here rather than only in setAveraging, so the ratio and the coefficient
            // cannot end up out of step whichever order the caller sets them in.
            attackAlpha = juce::jmin (1.0f, averagingAlpha * attackRatio);
        }
        float getAttackRatio() const noexcept { return attackRatio; }

        /** Appends captured audio. Only new samples may be appended: the analyser treats
            its input as one continuous stream and frames it itself. */
        void pushSamples (const float* samples, int numSamples);

        /** Analyses every frame the pushed samples completed. Cheap when there are none,
            which is the usual case when the caller pushes less than a hop at a time. */
        void processAvailableFrames();

        /** True when at least one complete frame is waiting. */
        bool hasFrameReady() const;

        void reset();

        const Frame& getFrame() const { return frame; }

        float getCoherentGain() const { return coherentGain; }
        float getNoiseBandwidth() const { return noiseBandwidth; }
        int getMaxFramesPerCall() const noexcept { return maxFramesPerCall; }

        /** Frequency and level of the strongest bin above 20 Hz, the pair a measurement
            is read from. */
        float getPeakFrequency() const;
        float getPeakMagnitude() const { return peakMagnitude; }

        static juce::StringArray getWindowNames();
        static juce::StringArray getAveragingNames();

        /** dBFS of a linear amplitude, with the same floor the whole app uses. */
        static float amplitudeToDb (float amplitude);

        /** The largest loss a tone can suffer between bins for a window, in dB. A tone
            that is not on a bin centre always reads low by this much at worst, so it is
            the tolerance a correct implementation can be checked against. */
        static float worstCaseScallopingLossDb (Window window);

    private:
        void rebuildWindowTables();
        void analyseFrame();
        void updatePeakTracking();

        double sampleRate = 48000.0;
        int fftSize = 1024;
        int numBins = 513;
        int hopSize = 512;
        float overlapPercent = 50.0f;

        Window windowType = Window::Hann;
        Averaging averagingMode = Averaging::Off;
        float averagingTimeConstant = 1.0f;
        float averagingAlpha = 1.0f;
        float attackAlpha = 1.0f;
        float attackRatio = 4.0f;

        juce::dsp::FFT fft { 10 };
        /** The window table is kept here rather than inside a WindowingFunction, so the
            coherent gain and the energy can be measured from the very samples that are
            applied to the audio. */
        std::vector<float> windowTable;
        std::vector<float> scratch;
        std::vector<float> windowed;

        float coherentGain = 1.0f;
        float windowEnergy = 1.0f;
        /** sum(w^2) / (N * CG^2): the equivalent noise bandwidth in bins. */
        float noiseBandwidth = 1.0f;

        std::vector<float> fifo;
        int fifoCapacity = 0;
        int64_t fifoWrite = 0;
        int64_t fifoFilled = 0;

        Frame frame;
        std::vector<float> averagedMagnitude;
        bool averagePrimed = false;

        int maxFramesPerCall = 8;

        int peakBin = 0;
        float peakMagnitude = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyser)
    };
}
