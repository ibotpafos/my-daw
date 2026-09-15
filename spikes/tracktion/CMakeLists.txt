cmake_minimum_required(VERSION 3.28)

project(MyDAWTracktionProof VERSION 0.1.0 LANGUAGES C CXX)

set(TRACKTION_ENGINE_ROOT "" CACHE PATH "Path to an exact Tracktion Engine checkout")
if(NOT TRACKTION_ENGINE_ROOT)
  message(FATAL_ERROR "Set TRACKTION_ENGINE_ROOT to the pinned checkout")
endif()
if(NOT EXISTS "${TRACKTION_ENGINE_ROOT}/modules/tracktion_engine/tracktion_engine.h")
  message(FATAL_ERROR "TRACKTION_ENGINE_ROOT is not a Tracktion Engine checkout")
endif()
if(NOT EXISTS "${TRACKTION_ENGINE_ROOT}/modules/juce/CMakeLists.txt")
  message(FATAL_ERROR "Initialise Tracktion's pinned JUCE submodule first")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)

add_subdirectory("${TRACKTION_ENGINE_ROOT}/modules/juce" juce EXCLUDE_FROM_ALL)
add_subdirectory("${TRACKTION_ENGINE_ROOT}/modules" tracktion EXCLUDE_FROM_ALL)

juce_add_console_app(mydaw_tracktion_proof
  PRODUCT_NAME "My DAW Tracktion Proof")
juce_generate_juce_header(mydaw_tracktion_proof)

target_sources(mydaw_tracktion_proof PRIVATE main.cpp)
target_compile_definitions(mydaw_tracktion_proof PRIVATE
  JUCE_MODAL_LOOPS_PERMITTED=1
  JUCE_PLUGINHOST_AU=1
  JUCE_PLUGINHOST_VST3=1
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0
  TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
  TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
  TRACKTION_LOG_DEVICES=0)
target_link_libraries(mydaw_tracktion_proof PRIVATE
  tracktion::tracktion_core
  tracktion::tracktion_engine
  tracktion::tracktion_graph
  juce::juce_audio_devices
  juce::juce_audio_processors
  juce::juce_audio_utils)

if(APPLE)
  target_compile_options(mydaw_tracktion_proof PRIVATE -fno-aligned-allocation)
endif()
