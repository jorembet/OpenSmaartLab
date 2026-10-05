#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WavRecorder.h"
#include "AudioEngine.h"

/** The record, stop, pause and save controls, and what the recorder is doing.

    The panel owns a recorder and hands it to the engine, which is what connects it to the
    audio callback. Everything here runs on the message thread; the only thing that crosses onto
    the audio thread is the recorder's lock free push, and the engine is told about the recorder
    once rather than on every block. */
class RecorderPanel : public juce::Component,
                      private juce::Timer
{
public:
    explicit RecorderPanel (AudioEngine& engine);
    ~RecorderPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setSampleRate (double rate);

    /** Asks the recorder to give up whatever it has open, without deleting anything saved. */
    void stopAndRelease();

private:
    void timerCallback() override;

    void refresh();
    void updateButtonStates();
    juce::String describeDuration() const;

    void start();
    void pauseOrResume();
    void stop();
    void save();
    void chooseLocation();

    AudioEngine& engine;
    WavRecorder recorder;

    juce::TextButton recordButton { "REC" };
    juce::TextButton stopButton { "STOP" };
    juce::TextButton pauseButton { "PAUSE" };
    juce::TextButton saveButton { "SAVE" };
    juce::TextButton locationButton { "Pilih Folder Rekaman" };

    juce::ComboBox depthSelector;
    juce::ComboBox rateSelector;

    juce::Label stateLabel { {}, "Idle" };
    juce::Label detailLabel;
    juce::Label adviceLabel;

    /** Peak level while recording, so the panel shows that something is arriving rather than
        only that the file is open. A recorder that is armed and silent is the usual state when
        a microphone is muted, and it looks exactly like a recorder that is working. */
    juce::Label levelLabel { {}, "-inf dBFS" };

    double sampleRate = 48000.0;
    juce::File chosenFolder;
    juce::File lastSaved;
    bool armed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecorderPanel)
};