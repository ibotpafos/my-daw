cmake_minimum_required(VERSION 3.28)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DAW_VERSION)
string(STRIP "${DAW_VERSION}" DAW_VERSION)
if(NOT DAW_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain X.Y.Z")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(MyDAW VERSION ${DAW_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(DAW_SANITIZERS "Address and undefined behavior sanitizers" OFF)
option(DAW_THREAD_SANITIZER "Thread sanitizer" OFF)
set(DAW_VST3_SDK_DIR "${CMAKE_SOURCE_DIR}/build/dependencies/vst3sdk" CACHE PATH
    "Pinned VST3 SDK checkout; bootstrap it explicitly with scripts/bootstrap-vst3-sdk.sh")
option(DAW_BUILD_VST3_SCAN_HELPER "Build the isolated VST3 scanner helper when the pinned SDK is present" OFF)
option(DAW_BUILD_VST3_RUNTIME_HELPER "Build the managed isolated VST3 runtime helper when the pinned SDK is present" OFF)
if(DAW_SANITIZERS AND DAW_THREAD_SANITIZER)
  message(FATAL_ERROR "Choose ASan/UBSan or TSan, not both")
endif()
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
add_library(daw_core STATIC
  engine/audio/analysis.cpp
  engine/audio/clip.cpp
  engine/audio/import_job.cpp
  engine/audio/export.cpp
  engine/audio/loudness.cpp
  engine/audio/input.cpp
  engine/audio/recording.cpp
  engine/audio/renderer.cpp
  engine/domain/session.cpp
  engine/export/dawproject_model.cpp
  engine/export/dawproject_xml.cpp
  engine/export/dawproject_export.cpp
  engine/jobs/limiter.cpp
  engine/plugins/plugin_descriptor.cpp
  engine/storage/project_package.cpp
  engine/storage/save_job.cpp
  engine/storage/storage.cpp
  engine/storage/zip_package.cpp
  engine/bridge/daw.cpp
)
target_include_directories(daw_core PUBLIC engine/bridge PRIVATE engine)
target_compile_features(daw_core PUBLIC cxx_std_20)
if(TARGET SQLite3::SQLite3)
  target_link_libraries(daw_core PUBLIC SQLite3::SQLite3)
else()
  target_link_libraries(daw_core PUBLIC SQLite::SQLite3)
endif()
target_link_libraries(daw_core PUBLIC Threads::Threads)
target_compile_options(daw_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(DAW_SANITIZERS)
  target_compile_options(daw_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(daw_core PUBLIC -fsanitize=address,undefined)
endif()
if(DAW_THREAD_SANITIZER)
  target_compile_options(daw_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(daw_core PUBLIC -fsanitize=thread)
endif()

include(cmake/DawPlatform.cmake)
include(CTest)
include(cmake/DawTests.cmake)
