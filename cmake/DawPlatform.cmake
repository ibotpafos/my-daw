if(APPLE)
  enable_language(OBJCXX)
  target_sources(daw_core PRIVATE engine/audio/clip_resampler.cpp)
  set_property(TARGET daw_core PROPERTY OBJCXX_STANDARD 20)
  target_sources(daw_core PRIVATE engine/platform/macos/input.cpp engine/platform/macos/midi_input.cpp engine/platform/macos/output.cpp engine/platform/macos/duplex.cpp engine/platform/macos/storage.mm)
  target_sources(daw_core PRIVATE engine/platform/macos/effect.mm engine/platform/macos/plugin_parameters.cpp)
  if(EXISTS "${DAW_VST3_SDK_DIR}/CMakeLists.txt")
    # AppleClang does not support every upstream Clang warning. Probe the
    # positive form: some compilers silently accept unknown -Wno-* options.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-Wunnecessary-virtual-specifier" DAW_HAS_VIRTUAL_SPECIFIER_WARNING)
    set(DAW_VST3_COMPATIBILITY_OPTIONS "-Wno-deprecated-declarations")
    if(DAW_HAS_VIRTUAL_SPECIFIER_WARNING)
      list(APPEND DAW_VST3_COMPATIBILITY_OPTIONS "-Wno-unnecessary-virtual-specifier")
    endif()
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
      PROPERTIES COMPILE_OPTIONS "${DAW_VST3_COMPATIBILITY_OPTIONS}")
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
      PROPERTIES COMPILE_OPTIONS "${DAW_VST3_COMPATIBILITY_OPTIONS}")
    target_include_directories(daw_vst3_runtime_helper PRIVATE engine "${DAW_VST3_SDK_DIR}" "${DAW_VST3_SDK_DIR}/pluginterfaces" "${DAW_VST3_SDK_DIR}/base/source")
    target_compile_options(daw_vst3_runtime_helper PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(daw_vst3_runtime_helper PRIVATE "-framework Cocoa" "-framework CoreFoundation" "-framework Foundation")
    target_compile_definitions(daw_core PRIVATE DAW_VST3_RUNTIME_AVAILABLE=1)
  endif()
  # Helpers do not link daw_core and therefore do not inherit its sanitizer
  # flags. Instrument their own process paths during the SDK acceptance gate.
  if(DAW_SANITIZERS)
    foreach(helper IN ITEMS daw_vst3_scan_helper daw_vst3_runtime_helper)
      if(TARGET ${helper})
        target_compile_options(${helper} PRIVATE -fsanitize=address,undefined
          -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        target_link_options(${helper} PRIVATE -fsanitize=address,undefined)
      endif()
    endforeach()
  endif()
else()
  # Offline conversion uses an established DSP library instead of a custom
  # resampler or a stub that makes otherwise portable import tests fail.
  # Install explicitly; a normal configure/build never downloads dependencies.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SampleRate REQUIRED IMPORTED_TARGET "samplerate>=0.2.2")
  target_link_libraries(daw_core PRIVATE PkgConfig::SampleRate)
  target_sources(daw_core PRIVATE
    engine/audio/clip_resampler_libsamplerate.cpp
    engine/audio/input_stub.cpp
    engine/audio/output_stub.cpp
    engine/audio/duplex_stub.cpp
    engine/audio/effect_stub.cpp
    engine/audio/vst3_effect_stub.cpp
    engine/audio/plugin_parameters_stub.cpp)
endif()
