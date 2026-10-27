cmake_minimum_required(VERSION 3.22)

project(SnareStack VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SnareStack is a macOS plugin (AU for Logic Pro, plus VST3 for other Mac DAWs).
# SNARESTACK_DEV_LINUX lets the plugin compile on Linux for headless testing only.
option(SNARESTACK_DEV_LINUX "Allow a headless Linux build for running the tests" OFF)
if(NOT APPLE AND NOT SNARESTACK_DEV_LINUX)
    message(FATAL_ERROR "SnareStack builds on macOS. (For headless tests on Linux, pass -DSNARESTACK_DEV_LINUX=ON.)")
endif()

if(APPLE)
    # one universal binary for Apple Silicon and Intel Macs, macOS 11 Big Sur or later
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build a universal binary")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# JUCE: use a local checkout if you have one (-DJUCE_DIR=/path/to/JUCE),
# otherwise it's downloaded automatically.
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(SNARESTACK_FORMATS AU VST3 Standalone)
else()
    set(SNARESTACK_FORMATS VST3) # dev/test build only
endif()

# On a Mac, `cmake --build` also installs the plugins into ~/Library/Audio/Plug-Ins
option(SNARESTACK_INSTALL_AFTER_BUILD "Copy the built plugins into ~/Library/Audio/Plug-Ins" ON)
if(APPLE AND SNARESTACK_INSTALL_AFTER_BUILD)
    set(SNARESTACK_COPY TRUE)
else()
    set(SNARESTACK_COPY FALSE)
endif()

juce_add_plugin(SnareStack
    PRODUCT_NAME "SnareStack"
    COMPANY_NAME "SnareStack"
    BUNDLE_ID "com.snarestack.snarestack"
    PLUGIN_MANUFACTURER_CODE Snst
    PLUGIN_CODE Sstk
    FORMATS ${SNARESTACK_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${SNARESTACK_COPY}
    VST3_CATEGORIES Fx Drum
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(SnareStack PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/BuiltinSamples.cpp)

target_compile_definitions(SnareStack PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0)

# headless Linux test builds don't need the optional X11/ALSA dev packages
if(SNARESTACK_DEV_LINUX AND NOT APPLE)
    target_compile_definitions(SnareStack PUBLIC
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XINPUT=0
        JUCE_ALSA=0 JUCE_JACK=0)
endif()

target_link_libraries(SnareStack
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Detector unit test (no JUCE needed)
add_executable(detector_test tests/detector_test.cpp)
target_include_directories(detector_test PRIVATE Source)
enable_testing()
add_test(NAME detector_test COMMAND detector_test)

# Headless end-to-end test of the processor (links the plugin's shared code)
option(SNARESTACK_BUILD_HOST_TEST "Build the headless processor test" OFF)
if(SNARESTACK_BUILD_HOST_TEST)
    juce_add_console_app(SnareStackHostTest PRODUCT_NAME "SnareStackHostTest")
    target_sources(SnareStackHostTest PRIVATE tests/host_test.cpp)
    # reuse the plugin's compiled JUCE modules + settings instead of compiling them twice
    target_include_directories(SnareStackHostTest PRIVATE $<TARGET_PROPERTY:SnareStack,INCLUDE_DIRECTORIES>)
    target_compile_definitions(SnareStackHostTest PRIVATE $<TARGET_PROPERTY:SnareStack,COMPILE_DEFINITIONS>)
    target_link_libraries(SnareStackHostTest PRIVATE SnareStack juce::juce_recommended_config_flags)
    add_test(NAME host_test COMMAND SnareStackHostTest)
endif()
