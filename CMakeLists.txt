cmake_minimum_required(VERSION 3.22)
project(Centsible VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CENTSIBLE_BUILD_PLUGIN "Build the VST3 + Standalone plugin (needs JUCE)" ON)
option(CENTSIBLE_BUILD_CLI "Build the command-line analyzer (needs JUCE audio formats)" ON)

enable_testing()

if(CENTSIBLE_BUILD_PLUGIN OR CENTSIBLE_BUILD_CLI)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

add_subdirectory(core)      # pure DSP library: no JUCE, no GUI
add_subdirectory(tests)     # synthetic suite (plain C++) + real-audio suite
if(CENTSIBLE_BUILD_CLI)
    add_subdirectory(cli)
endif()
if(CENTSIBLE_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
