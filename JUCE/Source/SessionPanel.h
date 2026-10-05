#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "TraceStore.h"
#include "Session.h"
#include "Session.h"
#include "AudioEngine.h"

/** The traces, the session file and the exports, on one panel.

    The panel is the whole of the data management side: which trace is being looked at, how it
    is frozen into one of the six, what is compared against what, and where the result goes. It
    holds the session, so opening one restores the device, the settings, the calibration and the
    curves together rather than one at a time. */
class SessionPanel : public juce::Component
{
public:
    explicit SessionPanel (AudioEngine& engine);
    ~SessionPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Copies the live spectrum and transfer curves in, so what is frozen is what is on screen. */
    void captureLive();

    Session& getSession() noexcept { return session; }

    /** Called when the rest of the application changes a setting the session records. */
    void syncSettings();

private:
    void freeze();
    void average();
    void computeDifference();
    void compare();
    void clearKind();
    void saveSession();
    void loadSession();
    void exportCurve();
    void exportTable();
    void refresh();

    juce::String describeTraces() const;
    juce::String describeSession() const;

    AudioEngine& engine;
    TraceStore traces;
    Session session;
    MeasurementExporter exporter;

    juce::ComboBox traceSelector;
    juce::ComboBox sourceSelector;

    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton averageButton { "Average" };
    juce::TextButton differenceButton { "Difference" };
    juce::TextButton compareButton { "Compare" };
    juce::TextButton clearButton { "Clear" };

    juce::TextButton saveSessionButton { "Simpan Session" };
    juce::TextButton loadSessionButton { "Buka Session" };

    juce::ComboBox exportKindSelector;
    juce::TextButton exportButton { "Export Curve" };
    juce::TextButton exportTableButton { "Export Tabel" };

    juce::Label traceInfoLabel;
    juce::Label compareInfoLabel;
    juce::Label sessionInfoLabel;
    juce::TextEditor notes;

    struct CurvePlot : juce::Component
    {
        void paint(juce::Graphics& g) override;
        std::vector<float> reference;
        std::vector<float> measurement;
        std::vector<float> difference;
        std::vector<float> frequency;
    };

    CurvePlot plot;

    juce::File lastFolder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionPanel)
};