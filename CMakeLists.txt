cmake_minimum_required(VERSION 3.28)
project(MyDAW VERSION 1.43.0 LANGUAGES C CXX)
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
add_library(daw_core STATIC engine/audio/analysis.cpp engine/audio/clip.cpp engine/audio/import_job.cpp engine/audio/export.cpp engine/audio/input.cpp engine/audio/recording.cpp engine/audio/renderer.cpp engine/domain/session.cpp engine/export/dawproject_model.cpp engine/export/dawproject_xml.cpp engine/export/dawproject_export.cpp engine/jobs/limiter.cpp engine/plugins/plugin_descriptor.cpp engine/storage/save_job.cpp engine/storage/storage.cpp engine/storage/zip_package.cpp engine/bridge/daw.cpp)
target_include_directories(daw_core PUBLIC engine/bridge PRIVATE engine)
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
if(APPLE)
  enable_language(OBJCXX)
  target_sources(daw_core PRIVATE engine/audio/clip_resampler.cpp)
  set_property(TARGET daw_core PROPERTY OBJCXX_STANDARD 20)
  target_sources(daw_core PRIVATE engine/platform/macos/input.cpp engine/platform/macos/midi_input.cpp engine/platform/macos/output.cpp engine/platform/macos/duplex.cpp engine/platform/macos/storage.mm)
  target_sources(daw_core PRIVATE engine/platform/macos/effect.mm engine/platform/macos/plugin_parameters.cpp)
  if(EXISTS "${DAW_VST3_SDK_DIR}/CMakeLists.txt")
    target_sources(daw_core PRIVATE
      engine/platform/macos/vst3_effect.cpp
      engine/platform/macos/vst3_runtime.cpp
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
      "${DAW_VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp")
    target_include_directories(daw_core PRIVATE "${DAW_VST3_SDK_DIR}" "${DAW_VST3_SDK_DIR}/pluginterfaces" "${DAW_VST3_SDK_DIR}/base/source")
    # The SDK's legacy UTF conversion uses a libc++ API deprecated by current
    # macOS. Keep our host warnings fatal while scoping the compatibility flag
    # to that pinned upstream implementation.
    set_source_files_properties(
      engine/platform/macos/vst3_effect.cpp
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp"
      PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations;-Wno-unnecessary-virtual-specifier")
    set_source_files_properties("${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(daw_core PUBLIC "-framework Cocoa" "-framework CoreFoundation")
  else()
    target_sources(daw_core PRIVATE engine/audio/vst3_effect_stub.cpp)
  endif()
  set_source_files_properties(engine/platform/macos/storage.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(daw_core PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMIDI" "-framework Foundation")
  # The scanner is intentionally independent from daw_core: it only supervises
  # disposable helper processes and never loads third-party Audio Units itself.
  add_library(daw_au_scanner STATIC engine/platform/macos/au_scanner.cpp engine/platform/macos/au_scan_cache.cpp engine/platform/macos/vst3_scanner.cpp engine/platform/macos/vst3_scan_cache.cpp)
  target_include_directories(daw_au_scanner PUBLIC engine)
  target_compile_options(daw_au_scanner PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(daw_core PUBLIC daw_au_scanner)
  add_executable(daw_au_scan_helper tools/au_scan_helper.cpp)
  target_compile_options(daw_au_scan_helper PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(daw_au_scan_helper PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework Foundation")

  # The VST3 SDK is deliberately an explicit, pinned developer dependency. A
  # normal My DAW build never fetches it; bootstrap-vst3-sdk.sh owns that step.
  # Keep the scanner out of daw_core so an untrusted plug-in can later be
  # supervised as a disposable process, like the AU scanner.
  if(DAW_BUILD_VST3_SCAN_HELPER)
    if(NOT EXISTS "${DAW_VST3_SDK_DIR}/CMakeLists.txt")
      message(FATAL_ERROR
        "DAW_BUILD_VST3_SCAN_HELPER requires the pinned SDK at ${DAW_VST3_SDK_DIR}. "
        "Run scripts/bootstrap-vst3-sdk.sh; normal builds leave this option OFF.")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/vst3_scan_helper.cpp")
      add_executable(daw_vst3_scan_helper
        tools/vst3_scan_helper.cpp
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp"
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm"
        "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp")
      target_sources(daw_vst3_scan_helper PRIVATE
        "${DAW_VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp")
      set_source_files_properties(
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
      # The pinned SDK's UTF-16 adapter still uses std::wstring_convert, which
      # current Apple libc++ marks deprecated. Keep warnings fatal for our
      # helper while isolating this upstream compatibility warning to its file.
      set_source_files_properties(
        "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
        "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
      target_include_directories(daw_vst3_scan_helper PRIVATE
        "${DAW_VST3_SDK_DIR}"
        "${DAW_VST3_SDK_DIR}/pluginterfaces"
        "${DAW_VST3_SDK_DIR}/base/source")
      target_compile_options(daw_vst3_scan_helper PRIVATE -Wall -Wextra -Wpedantic -Werror)
      target_link_libraries(daw_vst3_scan_helper PRIVATE "-framework Cocoa" "-framework CoreFoundation" "-framework Foundation")
    else()
      message(STATUS "Pinned VST3 SDK found; daw_vst3_scan_helper is deferred until tools/vst3_scan_helper.cpp exists")
    endif()
  endif()
  if(DAW_BUILD_VST3_RUNTIME_HELPER)
    if(NOT EXISTS "${DAW_VST3_SDK_DIR}/CMakeLists.txt")
      message(FATAL_ERROR "DAW_BUILD_VST3_RUNTIME_HELPER requires the pinned VST3 SDK at ${DAW_VST3_SDK_DIR}")
    endif()
    add_executable(daw_vst3_runtime_helper
      tools/vst3_runtime_helper.cpp
      engine/domain/session.cpp
      engine/platform/macos/vst3_effect.cpp
      engine/platform/macos/vst3_runtime.cpp
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
      "${DAW_VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp"
      engine/plugins/plugin_descriptor.cpp)
    set_source_files_properties("${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(engine/platform/macos/vst3_effect.cpp
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp"
      "${DAW_VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp"
      PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations;-Wno-unnecessary-virtual-specifier")
    target_include_directories(daw_vst3_runtime_helper PRIVATE engine "${DAW_VST3_SDK_DIR}" "${DAW_VST3_SDK_DIR}/pluginterfaces" "${DAW_VST3_SDK_DIR}/base/source")
    target_compile_options(daw_vst3_runtime_helper PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(daw_vst3_runtime_helper PRIVATE "-framework Cocoa" "-framework CoreFoundation" "-framework Foundation")
    target_compile_definitions(daw_core PRIVATE DAW_VST3_RUNTIME_AVAILABLE=1)
  endif()
else()
  target_sources(daw_core PRIVATE engine/audio/clip_resampler_stub.cpp engine/audio/input_stub.cpp engine/audio/output_stub.cpp engine/audio/duplex_stub.cpp engine/audio/effect_stub.cpp engine/audio/vst3_effect_stub.cpp engine/audio/plugin_parameters_stub.cpp)
endif()
include(CTest)
if(BUILD_TESTING)
  add_executable(daw_storage_jobs tests/storage_jobs.cpp)
  target_include_directories(daw_storage_jobs PRIVATE engine)
  target_link_libraries(daw_storage_jobs PRIVATE daw_core)
  add_test(NAME storage_jobs_crash COMMAND daw_storage_jobs)
  add_executable(daw_audio_tests tests/audio_tests.cpp)
  target_include_directories(daw_audio_tests PRIVATE engine)
  target_link_libraries(daw_audio_tests PRIVATE daw_core)
  add_test(NAME audio_render_storage COMMAND daw_audio_tests)
  add_executable(daw_import_jobs tests/import_jobs.cpp)
  target_include_directories(daw_import_jobs PRIVATE engine)
  target_link_libraries(daw_import_jobs PRIVATE daw_core)
  add_test(NAME background_wav_import COMMAND daw_import_jobs)
  add_executable(daw_aiff_import_tests tests/aiff_import_tests.cpp)
  target_include_directories(daw_aiff_import_tests PRIVATE engine)
  target_link_libraries(daw_aiff_import_tests PRIVATE daw_core)
  add_test(NAME aiff_import COMMAND daw_aiff_import_tests)
  add_executable(daw_export_tests tests/export_tests.cpp)
  target_include_directories(daw_export_tests PRIVATE engine)
  target_link_libraries(daw_export_tests PRIVATE daw_core)
  add_test(NAME offline_wav_export COMMAND daw_export_tests)
  add_executable(daw_recording_recovery tests/recording_recovery.cpp)
  target_include_directories(daw_recording_recovery PRIVATE engine)
  target_link_libraries(daw_recording_recovery PRIVATE daw_core)
  add_test(NAME recording_crash_recovery COMMAND daw_recording_recovery)
  if(APPLE)
    if(DAW_BUILD_VST3_RUNTIME_HELPER)
      add_executable(daw_vst3_runtime_isolation tests/vst3_runtime_isolation.cpp)
      target_include_directories(daw_vst3_runtime_isolation PRIVATE engine)
      target_link_libraries(daw_vst3_runtime_isolation PRIVATE daw_core)
      add_test(NAME vst3_runtime_isolation COMMAND daw_vst3_runtime_isolation)
    endif()
    add_executable(daw_au_host_probe tests/au_host_probe.cpp)
    target_include_directories(daw_au_host_probe PRIVATE engine)
    target_link_libraries(daw_au_host_probe PRIVATE daw_core)
    add_test(NAME approved_au_host COMMAND daw_au_host_probe)
    add_executable(daw_hardware_smoke tests/hardware_smoke.cpp)
    target_include_directories(daw_hardware_smoke PRIVATE engine)
    target_link_libraries(daw_hardware_smoke PRIVATE daw_core)
    add_executable(daw_duplex_hardware_smoke tests/duplex_hardware_smoke.cpp)
    target_include_directories(daw_duplex_hardware_smoke PRIVATE engine)
    target_link_libraries(daw_duplex_hardware_smoke PRIVATE daw_core)
    # MIDI capture tests are hardware-free by design: the packet parser, the
    # drop-oldest ring and host-time framing are pure logic, and open() without
    # a device must reject. Real controller capture stays a manual gate.
    add_executable(daw_midi_input_tests tests/midi_input_tests.cpp)
    target_include_directories(daw_midi_input_tests PRIVATE engine)
    target_link_libraries(daw_midi_input_tests PRIVATE daw_core)
    add_test(NAME midi_input COMMAND daw_midi_input_tests)
    # Device-dependent smoke/audition tests are intentionally NOT registered as
    # CTests: they require a real output/input device and are run manually on a
    # physical machine (see scripts/test-audio-preview.sh for the AU/VST3 browser
    # audition, and run daw_hardware_smoke / daw_duplex_hardware_smoke directly).
    # The shared CI runner is headless and has no audio device, so registering them
    # would break ctest on GitHub macOS runners.
    #
    # Note: daw_hardware_smoke and daw_duplex_hardware_smoke are built above but
    # deliberately kept out of CTest for the same reason (physical audio gate).
  endif()
  add_executable(daw_core_tests tests/core_tests.cpp)
  target_link_libraries(daw_core_tests PRIVATE daw_core)
  target_include_directories(daw_core_tests PRIVATE engine)
  add_test(NAME session_storage_bridge COMMAND daw_core_tests)
  add_executable(daw_c_header_test tests/c_header.c)
  target_link_libraries(daw_c_header_test PRIVATE daw_core)
  add_test(NAME pure_c_bridge COMMAND daw_c_header_test)
  add_executable(daw_dawproject_export_tests tests/dawproject_export_tests.cpp)
  target_include_directories(daw_dawproject_export_tests PRIVATE engine)
  target_link_libraries(daw_dawproject_export_tests PRIVATE daw_core)
  add_test(NAME dawproject_export COMMAND daw_dawproject_export_tests)
  add_executable(daw_midi_model_tests tests/midi_model_tests.cpp)
  target_include_directories(daw_midi_model_tests PRIVATE engine)
  target_link_libraries(daw_midi_model_tests PRIVATE daw_core)
  add_test(NAME midi_model COMMAND daw_midi_model_tests)
  add_executable(daw_midi_bridge_tests tests/midi_bridge_tests.cpp)
  target_include_directories(daw_midi_bridge_tests PRIVATE engine)
  target_link_libraries(daw_midi_bridge_tests PRIVATE daw_core)
  add_test(NAME midi_bridge COMMAND daw_midi_bridge_tests)
  add_executable(daw_tempo_model_tests tests/tempo_model_tests.cpp)
  target_include_directories(daw_tempo_model_tests PRIVATE engine)
  target_link_libraries(daw_tempo_model_tests PRIVATE daw_core)
  add_test(NAME tempo_model COMMAND daw_tempo_model_tests)
endif()
