cmake_minimum_required(VERSION 3.22)

project(OpenSmaartLab VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 7.0.12
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(OpenSmaartLab
    PRODUCT_NAME "OpenSmaartLab"
    COMPANY_NAME "OpenSmaartLab"
    NEEDS_CURL FALSE
)


# Sources shared by the application and the UI checks. Both targets need them, and
# keeping one list is what stops a new file being added to one and not the other.
set(OpenSmaartLabSharedSources
    Source/AudioEngine.cpp
    Source/DSP.cpp
    Source/DspWorker.cpp
    Source/FFTDisplay.cpp
    Source/GeneratorDisplay.cpp
    Source/ImpedanceDisplay.cpp
    Source/ImpulseResponse.cpp
    Source/InputLevelPanel.cpp
    Source/LevelMeter.cpp
    Source/MainComponent.cpp
    Source/SessionPanel.cpp
    Source/RecorderPanel.cpp
    Source/OfflinePanel.cpp
    Source/MicrophoneCalibration.cpp
    Source/RTASnapshot.cpp
    Source/ReverbDisplay.cpp
    Source/RtaAnalyser.cpp
    Source/RtaDisplay.cpp
    Source/SPLMeter.cpp
    Source/SignalGenerator.cpp
    Source/SpectrumAnalyser.cpp
    Source/SpectrumAnalyserDisplay.cpp
    Source/SplAnalyser.cpp
    Source/DistortionAnalyser.cpp
    Source/DistortionDisplay.cpp
    Source/AdvancedMeasurements.cpp
    Source/ReverbAnalyser.cpp
    Source/WavRecorder.cpp
    Source/OfflineAnalysis.cpp
    Source/TraceStore.cpp
    Source/MeasurementExporter.cpp
    Source/Session.cpp
    Source/TransferFunction.cpp
    Source/TransferFunctionDisplay.cpp
    Source/EqDesigner.cpp
)

target_sources(OpenSmaartLab PRIVATE
    Source/Main.cpp
    Source/OfflineAnalysis.cpp
    ${OpenSmaartLabSharedSources}
)

target_compile_definitions(OpenSmaartLab PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_HOST_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(OpenSmaartLab PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
)

include(CTest)

target_include_directories(OpenSmaartLab PRIVATE Source)

juce_add_console_app(OpenSmaartLabAudioChecks PRODUCT_NAME "OpenSmaartLabAudioChecks")

target_include_directories(OpenSmaartLabAudioChecks PRIVATE Source)

target_sources(OpenSmaartLabAudioChecks PRIVATE
    tests/audio_checks.cpp
    Source/AudioEngine.cpp
    Source/WavRecorder.cpp
    Source/DSP.cpp
    Source/MicrophoneCalibration.cpp
    Source/RTASnapshot.cpp
    Source/SignalGenerator.cpp
    Source/TransferFunction.cpp
)

target_compile_definitions(OpenSmaartLabAudioChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabAudioChecks PRIVATE
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME audio_checks COMMAND OpenSmaartLabAudioChecks)



juce_add_console_app(OpenSmaartLabUiChecks PRODUCT_NAME "OpenSmaartLabUiChecks")

target_include_directories(OpenSmaartLabUiChecks PRIVATE Source)

target_sources(OpenSmaartLabUiChecks PRIVATE
    tests/ui_checks.cpp
    ${OpenSmaartLabSharedSources}
)

target_compile_definitions(OpenSmaartLabUiChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0
)

target_link_libraries(OpenSmaartLabUiChecks PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
)

add_test(NAME ui_checks COMMAND OpenSmaartLabUiChecks)

# ---- Phase 1: realtime audio foundation ----
# Level metering and the capture ring are measured against known test signals, and the
# callback is checked for allocations, so this target links only what those need.
juce_add_console_app(OpenSmaartLabLevelChecks PRODUCT_NAME "OpenSmaartLabLevelChecks")

target_include_directories(OpenSmaartLabLevelChecks PRIVATE Source)

target_sources(OpenSmaartLabLevelChecks PRIVATE
    tests/level_checks.cpp
    Source/AudioEngine.cpp
    Source/WavRecorder.cpp
    Source/DSP.cpp
    Source/LevelMeter.cpp
    Source/SignalGenerator.cpp
)

target_compile_definitions(OpenSmaartLabLevelChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabLevelChecks PRIVATE
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME level_checks COMMAND OpenSmaartLabLevelChecks)

# Stereo input metering is fed through the same off-thread worker as the live spectrum,
# but these checks synthesize both channels and need no audio device.
juce_add_console_app(OpenSmaartLabDspWorkerChecks PRODUCT_NAME "OpenSmaartLabDspWorkerChecks")

target_include_directories(OpenSmaartLabDspWorkerChecks PRIVATE Source)

target_sources(OpenSmaartLabDspWorkerChecks PRIVATE
    tests/dsp_worker_checks.cpp
    Source/DSP.cpp
    Source/DspWorker.cpp
    Source/LevelMeter.cpp
    Source/RtaAnalyser.cpp
    Source/SpectrumAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabDspWorkerChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabDspWorkerChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME dsp_worker_checks COMMAND OpenSmaartLabDspWorkerChecks)

# ---- Phase 2: FFT spectrum analyser ----
# The analyser is verified with an internal sine generator at known frequencies and
# levels, so it needs no audio device at all.
juce_add_console_app(OpenSmaartLabSpectrumChecks PRODUCT_NAME "OpenSmaartLabSpectrumChecks")

target_include_directories(OpenSmaartLabSpectrumChecks PRIVATE Source)

target_sources(OpenSmaartLabSpectrumChecks PRIVATE
    tests/spectrum_checks.cpp
    Source/DSP.cpp
    Source/LevelMeter.cpp
    Source/SpectrumAnalyser.cpp
    Source/RtaAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabSpectrumChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabSpectrumChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME spectrum_checks COMMAND OpenSmaartLabSpectrumChecks)

juce_add_console_app(OpenSmaartLabSplMeterChecks PRODUCT_NAME "OpenSmaartLabSplMeterChecks")

target_include_directories(OpenSmaartLabSplMeterChecks PRIVATE Source)

target_sources(OpenSmaartLabSplMeterChecks PRIVATE
    tests/spl_meter_checks.cpp
    Source/DSP.cpp
    Source/MicrophoneCalibration.cpp
    Source/SPLMeter.cpp
    Source/SplAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabSplMeterChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabSplMeterChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME spl_meter_checks COMMAND OpenSmaartLabSplMeterChecks)

# ---- Phase 3: octave band analyser ----
# The bands are integrated from a real spectrum, so this needs the spectrum analyser but
# no audio device.
juce_add_console_app(OpenSmaartLabRtaChecks PRODUCT_NAME "OpenSmaartLabRtaChecks")

target_include_directories(OpenSmaartLabRtaChecks PRIVATE Source)

target_sources(OpenSmaartLabRtaChecks PRIVATE
    tests/rta_checks.cpp
    Source/DSP.cpp
    Source/RtaAnalyser.cpp
    Source/SpectrumAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabRtaChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabRtaChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME rta_checks COMMAND OpenSmaartLabRtaChecks)

# ---- Phase 5: dual channel transfer function ----
# Both channels are generated internally, so the checks need the analyser and the delay
# finder but no audio device. The GUI is left out on purpose: these are statements about
# arithmetic, and a display that draws a wrong number correctly is still wrong.
juce_add_console_app(OpenSmaartLabTransferFunctionChecks PRODUCT_NAME "OpenSmaartLabTransferFunctionChecks")

target_include_directories(OpenSmaartLabTransferFunctionChecks PRIVATE Source)

target_sources(OpenSmaartLabTransferFunctionChecks PRIVATE
    tests/transfer_function_checks.cpp
    Source/DSP.cpp
    Source/TransferFunction.cpp
)

target_compile_definitions(OpenSmaartLabTransferFunctionChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabTransferFunctionChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME transfer_function_checks COMMAND OpenSmaartLabTransferFunctionChecks)

# ---- Phase 6: phase and delay ----
# The delays checked here are synthetic and their answers are known exactly, so these are
# statements about arithmetic rather than about a particular room.
juce_add_console_app(OpenSmaartLabPhaseDelayChecks PRODUCT_NAME "OpenSmaartLabPhaseDelayChecks")

target_include_directories(OpenSmaartLabPhaseDelayChecks PRIVATE Source)

target_sources(OpenSmaartLabPhaseDelayChecks PRIVATE
    tests/phase_delay_checks.cpp
    Source/DSP.cpp
    Source/TransferFunction.cpp
)

target_compile_definitions(OpenSmaartLabPhaseDelayChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabPhaseDelayChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME phase_delay_checks COMMAND OpenSmaartLabPhaseDelayChecks)

# ---- Phase 8: signal generator ----
# Every waveform is rendered exactly as the audio callback renders it and then measured through
# the spectrum analyser, so what is checked is the signal that reaches the converter.
juce_add_console_app(OpenSmaartLabGeneratorChecks PRODUCT_NAME "OpenSmaartLabGeneratorChecks")

target_include_directories(OpenSmaartLabGeneratorChecks PRIVATE Source)

target_sources(OpenSmaartLabGeneratorChecks PRIVATE
    tests/generator_checks.cpp
    Source/DSP.cpp
    Source/SignalGenerator.cpp
    Source/SpectrumAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabGeneratorChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabGeneratorChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME generator_checks COMMAND OpenSmaartLabGeneratorChecks)

# ---- Phase 9: microphone calibration and SPL ----
# The calibrator is synthetic: a signal of a known level in, a number of the known level out.
# No audio device is involved, so the whole chain is arithmetic and can be checked exactly.
juce_add_console_app(OpenSmaartLabCalibrationChecks PRODUCT_NAME "OpenSmaartLabCalibrationChecks")

target_include_directories(OpenSmaartLabCalibrationChecks PRIVATE Source)

target_sources(OpenSmaartLabCalibrationChecks PRIVATE
    tests/calibration_checks.cpp
    Source/DSP.cpp
    Source/MicrophoneCalibration.cpp
    Source/SplAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabCalibrationChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabCalibrationChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME calibration_checks COMMAND OpenSmaartLabCalibrationChecks)

# ---- Phase 10: distortion analyser ----
# Built from sines of known amplitude, so the expected THD is a number that can be worked out
# on paper rather than a number the analyser agrees with itself about.
juce_add_console_app(OpenSmaartLabDistortionChecks PRODUCT_NAME "OpenSmaartLabDistortionChecks")

target_include_directories(OpenSmaartLabDistortionChecks PRIVATE Source)

target_sources(OpenSmaartLabDistortionChecks PRIVATE
    tests/distortion_checks.cpp
    Source/DSP.cpp
    Source/DistortionAnalyser.cpp
    Source/DistortionDisplay.cpp
    Source/AdvancedMeasurements.cpp
    Source/ReverbAnalyser.cpp
    Source/WavRecorder.cpp
)

target_compile_definitions(OpenSmaartLabDistortionChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabDistortionChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME distortion_checks COMMAND OpenSmaartLabDistortionChecks)

# ---- Phase 11: intermodulation, crosstalk and polarity ----
# Every signal in here is generated and then measured, so each figure has a value worked out by
# hand to be compared against.
juce_add_console_app(OpenSmaartLabAdvancedMeasurementsChecks
    PRODUCT_NAME "OpenSmaartLabAdvancedMeasurementsChecks")

target_include_directories(OpenSmaartLabAdvancedMeasurementsChecks PRIVATE Source)

target_sources(OpenSmaartLabAdvancedMeasurementsChecks PRIVATE
    tests/advanced_measurements_checks.cpp
    Source/DSP.cpp
    Source/AdvancedMeasurements.cpp
    Source/ReverbAnalyser.cpp
    Source/WavRecorder.cpp
)

target_compile_definitions(OpenSmaartLabAdvancedMeasurementsChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabAdvancedMeasurementsChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME advanced_measurements_checks COMMAND OpenSmaartLabAdvancedMeasurementsChecks)

# ---- Phase 12: reverberation analysis ----
# Reference signals are synthetic rooms built to a known reverberation time, so every expected
# value is derived from the envelope that went in.
juce_add_console_app(OpenSmaartLabReverbChecks PRODUCT_NAME "OpenSmaartLabReverbChecks")

target_include_directories(OpenSmaartLabReverbChecks PRIVATE Source)

target_sources(OpenSmaartLabReverbChecks PRIVATE
    tests/reverb_checks.cpp
    Source/DSP.cpp
    Source/ReverbAnalyser.cpp
    Source/WavRecorder.cpp
)

target_compile_definitions(OpenSmaartLabReverbChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabReverbChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME reverb_checks COMMAND OpenSmaartLabReverbChecks)

# ---- Phase 13: recording and offline analysis ----
# The round trip is checked through the real recorder and the real file reader rather than
# through a buffer, because the failure this covers only appears once bytes have been written
# and read back.
juce_add_console_app(OpenSmaartLabRecordingChecks PRODUCT_NAME "OpenSmaartLabRecordingChecks")

target_include_directories(OpenSmaartLabRecordingChecks PRIVATE Source)

target_sources(OpenSmaartLabRecordingChecks PRIVATE
    tests/recording_checks.cpp
    Source/DSP.cpp
    Source/WavRecorder.cpp
    Source/OfflineAnalysis.cpp
    Source/TraceStore.cpp
    Source/MeasurementExporter.cpp
    Source/Session.cpp
    Source/SpectrumAnalyser.cpp
    Source/RtaAnalyser.cpp
    Source/DistortionAnalyser.cpp
    Source/SplAnalyser.cpp
    Source/MicrophoneCalibration.cpp
    Source/TransferFunction.cpp
    Source/ImpulseResponse.cpp
    Source/ReverbAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabRecordingChecks PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabRecordingChecks PRIVATE
    juce::juce_data_structures
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME recording_checks COMMAND OpenSmaartLabRecordingChecks)

# ---- Phase 14: traces, sessions and exports ----
# The round trip is the claim under test: a session written here is read back by a different
# object in the same run, and every value that gave the measurement its meaning is compared.
juce_add_console_app(OpenSmaartLabSessionChecks PRODUCT_NAME "OpenSmaartLabSessionChecks")

target_include_directories(OpenSmaartLabSessionChecks PRIVATE Source)

target_sources(OpenSmaartLabSessionChecks PRIVATE
    tests/session_checks.cpp
    Source/DSP.cpp
    Source/WavRecorder.cpp
    Source/OfflineAnalysis.cpp
    Source/TraceStore.cpp
    Source/MeasurementExporter.cpp
    Source/Session.cpp
    Source/MicrophoneCalibration.cpp
    Source/SpectrumAnalyser.cpp
    Source/RtaAnalyser.cpp
    Source/DistortionAnalyser.cpp
    Source/SplAnalyser.cpp
    Source/TransferFunction.cpp
    Source/ImpulseResponse.cpp
    Source/ReverbAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabSessionChecks PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabSessionChecks PRIVATE
    juce::juce_data_structures
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME session_checks COMMAND OpenSmaartLabSessionChecks)

# ---- Phase 15: final integration ----
# Every measurement held against a number that can be worked out in advance, plus the stress,
# performance and numerical safety sections. A test that compares a reading with itself proves
# nothing, so nothing here does.
juce_add_console_app(OpenSmaartLabValidationChecks PRODUCT_NAME "OpenSmaartLabValidationChecks")

target_include_directories(OpenSmaartLabValidationChecks PRIVATE Source)

target_sources(OpenSmaartLabValidationChecks PRIVATE
    tests/validation_checks.cpp
    Source/DSP.cpp
    Source/LevelMeter.cpp
    Source/SpectrumAnalyser.cpp
    Source/RtaAnalyser.cpp
    Source/DistortionAnalyser.cpp
    Source/AdvancedMeasurements.cpp
    Source/SplAnalyser.cpp
    Source/MicrophoneCalibration.cpp
    Source/TransferFunction.cpp
    Source/ImpulseResponse.cpp
    Source/ReverbAnalyser.cpp
)

target_compile_definitions(OpenSmaartLabValidationChecks PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabValidationChecks PRIVATE
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME validation_checks COMMAND OpenSmaartLabValidationChecks)

# ---- Phase 4: auto-EQ designer ----
# A fit is judged by adding its filters back onto the measurement, so these checks need no
# audio and no display: they are arithmetic with a known answer.
juce_add_console_app(OpenSmaartLabEqChecks PRODUCT_NAME "OpenSmaartLabEqChecks")

target_include_directories(OpenSmaartLabEqChecks PRIVATE Source)

target_sources(OpenSmaartLabEqChecks PRIVATE
    tests/eq_checks.cpp
    Source/EqDesigner.cpp
)

target_compile_definitions(OpenSmaartLabEqChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabEqChecks PRIVATE
    juce::juce_dsp
    juce::juce_gui_basics
)

add_test(NAME eq_checks COMMAND OpenSmaartLabEqChecks)
