cmake_minimum_required(VERSION 3.22)

project(CashSynth VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.17-security
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------- plugin
set(CASH_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND CASH_FORMATS AU)
endif()

juce_add_plugin(CashSynth
    COMPANY_NAME                "Cashroy"
    BUNDLE_ID                   com.cashroy.cashsynth
    PLUGIN_MANUFACTURER_CODE    Chru
    PLUGIN_CODE                 Csyn
    PRODUCT_NAME                "Cash Synth"
    FORMATS                     ${CASH_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    VST3_CATEGORIES             Instrument Synth
    COPY_PLUGIN_AFTER_BUILD     TRUE)

set(CASH_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Parameters.cpp
    Source/SynthVoice.cpp
    Source/SynthEngine.cpp
    Source/CashLookAndFeel.cpp
    Source/AdvancedPanel.cpp
    Source/ParameterMap.cpp
    Source/CashCode.cpp
    Source/Mutator.cpp
    Source/Ledger.cpp
    Source/Vault.cpp
    Source/Stash.cpp
    Source/StashBar.cpp
    Source/BillArt.cpp
    Source/BillComponents.cpp
    Source/MoneyTray.cpp
    Source/SoundPrint.cpp
    Source/CashMatch.cpp
    Source/MatchPanel.cpp
    Source/Tempo.cpp
    Source/Effects.cpp)

target_sources(CashSynth PRIVATE ${CASH_SOURCES})

# Bill background: greyscale public-domain $100 scan (see CREDITS.md). Tinted in code.
juce_add_binary_data(CashAssets
    HEADER_NAME BinaryData.h
    NAMESPACE   CashAssets
    SOURCES     Resources/hundred_obverse_gray.jpg)
target_include_directories(CashSynth PUBLIC Source)

target_compile_definitions(CashSynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(CashSynth
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        CashAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

# Warnings as errors on our own sources only (not JUCE modules).
if(MSVC)
    set(CASH_WARNINGS /W4 /WX)
else()
    set(CASH_WARNINGS -Wall -Wextra -Werror)
endif()
set_source_files_properties(${CASH_SOURCES} PROPERTIES COMPILE_OPTIONS "${CASH_WARNINGS}")

# ----------------------------------------------------------------- tests
option(CASH_BUILD_TESTS "Build unit tests" ON)
if(CASH_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.1
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    set(CASH_TEST_SOURCES Tests/ProcessorTests.cpp Tests/EngineTests.cpp Tests/CashTests.cpp Tests/UiTests.cpp Tests/StateTests.cpp Tests/MatchTests.cpp Tests/LeadTests.cpp Tests/AutomationTests.cpp Tests/MatchV2Tests.cpp Tests/TempoTests.cpp Tests/EffectsTests.cpp)
    add_executable(CashSynthTests ${CASH_TEST_SOURCES})
    target_link_libraries(CashSynthTests PRIVATE CashSynth Catch2::Catch2WithMain)
    # Reuse the plugin's compiled JUCE code; only borrow its headers and module defines.
    target_include_directories(CashSynthTests PRIVATE $<TARGET_PROPERTY:CashSynth,INCLUDE_DIRECTORIES>)
    target_compile_definitions(CashSynthTests PRIVATE $<TARGET_PROPERTY:CashSynth,COMPILE_DEFINITIONS>)
    set_source_files_properties(${CASH_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${CASH_WARNINGS}")

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(CashSynthTests)
endif()
