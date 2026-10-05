#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OfflineAnalysis.h"
#include "MicrophoneCalibration.h"

/** Opens a wav file and runs every measurement over it.

    The readings on this panel come out of the same analysers the live meters use, so a file and
    a live input of the same audio give the same numbers. That is the point of the panel: it is a
    way of measuring a capture, not a second instrument. */
class OfflinePanel : public juce::Component
{
public:
    OfflinePanel();
    ~OfflinePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** The calibration the live path is using, so a file is read the same way. */
    void setCalibration (const MicrophoneCalibration& profile);

    void setFftSize (int size);
    void setResolution (bool thirdOctave);
    void clear();

private:
    void chooseFile();
    void runAnalysis();
    void refresh();

    juce::TextButton openButton { "Buka WAV" };
    juce::TextButton analyseButton { "Jalankan Analisis" };
    juce::TextButton clearButton { "Bersihkan" };
    juce::ComboBox fftSelector;
    juce::ComboBox resolutionSelector;

    juce::Label fileLabel { {}, "Belum ada file" };
    juce::Label summaryLabel;
    juce::TextEditor notes;

    /** The measurement report, shown read only.

        A plain read-only editor rather than a code component: it scrolls, it selects text so a
        reading can be copied out of it, and it does not pretend the report is something that can
        be edited. */
    juce::TextEditor reportEditor;

    OfflineAnalysis analysis;
    OfflineAnalysis::Result result;
    MicrophoneCalibration calibration;
    juce::File currentFile;
    bool haveResult = false;
    bool busy = false;

    struct SpectrumPlot : juce::Component
    {
        void paint(juce::Graphics& g) override;
        std::vector<float> frequency;
        std::vector<float> magnitudeDb;
        float averageLevelDb = -200.0f;
        float noiseFloorDb = -200.0f;
    };

    SpectrumPlot spectrumPlot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OfflinePanel)
};
