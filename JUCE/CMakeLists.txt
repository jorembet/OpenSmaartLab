cmake_minimum_required(VERSION 3.22)

project(OpenSmaartLab VERSION 1.0.0)

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

target_sources(OpenSmaartLab PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/DSP.cpp
    Source/FFTDisplay.cpp
    Source/MicrophoneCalibration.cpp
    Source/RTASnapshot.cpp
    Source/GeneratorDisplay.cpp
    Source/ReverbDisplay.cpp
    Source/SPLMeter.cpp
    Source/TransferFunction.cpp
    Source/ImpulseResponse.cpp
    Source/SignalGenerator.cpp
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
    Source/DSP.cpp
    Source/MicrophoneCalibration.cpp
    Source/RTASnapshot.cpp
    Source/SignalGenerator.cpp
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
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/DSP.cpp
    Source/FFTDisplay.cpp
    Source/MicrophoneCalibration.cpp
    Source/RTASnapshot.cpp
    Source/GeneratorDisplay.cpp
    Source/ReverbDisplay.cpp
    Source/SPLMeter.cpp
    Source/TransferFunction.cpp
    Source/ImpulseResponse.cpp
    Source/SignalGenerator.cpp
)

target_compile_definitions(OpenSmaartLabUiChecks PRIVATE
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
)

target_link_libraries(OpenSmaartLabUiChecks PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
)

add_test(NAME ui_checks COMMAND OpenSmaartLabUiChecks)
