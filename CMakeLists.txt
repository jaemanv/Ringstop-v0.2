cmake_minimum_required(VERSION 3.22)
project(Ringstop VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # no VC++ redistributable needed

# Download JUCE automatically (no manual install needed)
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Ringstop
    COMPANY_NAME              "Ringstop Audio"      # change to your company
    PLUGIN_MANUFACTURER_CODE  Rgsa                  # 4 chars, at least one uppercase
    PLUGIN_CODE               Rgst                  # 4 chars, unique per plugin
    FORMATS                   VST3 Standalone
    PRODUCT_NAME              "Ringstop"
    BUNDLE_ID                 com.ringstopaudio.ringstop
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    VST3_CATEGORIES           Fx Restoration)

target_sources(Ringstop PRIVATE
    Source/FeedbackDetector.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Ringstop PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Ringstop
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Optional offline test:  cmake -B build -DRINGSTOP_BUILD_TESTS=ON
option(RINGSTOP_BUILD_TESTS "Build the offline detection test" OFF)
if (RINGSTOP_BUILD_TESTS)
    add_executable(RingstopTest Tests/OfflineTest.cpp)
    target_link_libraries(RingstopTest PRIVATE Ringstop)
    target_include_directories(RingstopTest PRIVATE
        $<TARGET_PROPERTY:Ringstop,INCLUDE_DIRECTORIES>)
    target_compile_definitions(RingstopTest PRIVATE
        $<TARGET_PROPERTY:Ringstop,COMPILE_DEFINITIONS>)
endif()
