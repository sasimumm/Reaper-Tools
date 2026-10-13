cmake_minimum_required(VERSION 3.20)
project(reatrace LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clang-tidy
# Bounds-checked operator[] on std::vector, std::span, std::string etc. (libc++'s cheap checks), also in the plugin.
add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
set(WDL ${CMAKE_SOURCE_DIR}/vendor/WDL/WDL)

# Warnings on our own sources only (vendor code and SDK headers are left alone: they are SYSTEM includes).
# -Wno-format-nonliteral: the panel's slider formats come from its table.
option(REATRACE_WERROR "Treat warnings in ReaTrace's own sources as errors" ON)
set(REATRACE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow-all -Wconversion -Wsign-conversion
  -Wimplicit-fallthrough -Wcomma -Wformat=2 -Wno-format-nonliteral -Wundef -Wcast-qual -Wextra-semi -Wnon-virtual-dtor
  -Wmissing-prototypes -Wzero-as-null-pointer-constant -Wnewline-eof -Wunreachable-code-aggressive -Wswitch-enum -Wdeprecated
  -Wdouble-promotion -Wold-style-cast -Wunsafe-buffer-usage)
if(REATRACE_WERROR)
  list(APPEND REATRACE_WARNINGS -Werror)
endif()
file(GLOB_RECURSE REATRACE_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.mm ${CMAKE_SOURCE_DIR}/tests/*.cpp)
set_source_files_properties(${REATRACE_SOURCES} PROPERTIES COMPILE_OPTIONS "${REATRACE_WARNINGS}")

if(NOT APPLE)
  message(FATAL_ERROR "Only macOS is supported so far")
endif()

add_library(reaper_reatrace MODULE
  src/reatrace/analysis.cpp
  src/reatrace/main.cpp
  src/reatrace/model.cpp
  src/reatrace/settings.cpp
  src/reatrace/dsp/basicpitch.mm
  src/reatrace/dsp/contour.cpp
  src/reatrace/dsp/estimate.cpp
  src/reatrace/dsp/nsgt.cpp
  src/reatrace/notes/commands.cpp
  src/reatrace/notes/editor.cpp
  src/reatrace/notes/lyrics.cpp
  src/reatrace/notes/notesync.cpp
  src/reatrace/notes/takesync.cpp
  src/reatrace/playback/isolate.cpp
  src/reatrace/playback/isolatefx.cpp
  src/reatrace/playback/preview.cpp
  src/reatrace/ui/paint.cpp
  src/reatrace/ui/panel.cpp
  src/reatrace/ui/theme.cpp
  src/reatrace/ui/toolbar.cpp
  src/reatrace/ui/window.cpp
  ${WDL}/fft.c
  ${WDL}/swell/swell-modstub.mm
)
set_target_properties(reaper_reatrace PROPERTIES PREFIX "" SUFFIX ".dylib")
set_source_files_properties(src/reatrace/dsp/basicpitch.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
target_include_directories(reaper_reatrace SYSTEM PRIVATE vendor/reaper-sdk/sdk ${WDL})
# includes are relative to src/reatrace ("dsp/nsgt.h") or src ("shared/guard.h")
target_include_directories(reaper_reatrace PRIVATE src/reatrace src)
target_compile_definitions(reaper_reatrace PRIVATE SWELL_PROVIDED_BY_APP)
target_link_libraries(reaper_reatrace PRIVATE "-framework Cocoa" "-framework CoreML" "-framework Accelerate")

# ReaChords: chord items, a separate extension; ReaTrace's chords lane reads them through its API.
add_library(reaper_reachords MODULE
  src/reachords/main.cpp
  src/reachords/chords.cpp
  ${WDL}/swell/swell-modstub.mm
)
set_target_properties(reaper_reachords PROPERTIES PREFIX "" SUFFIX ".dylib")
target_include_directories(reaper_reachords SYSTEM PRIVATE vendor/reaper-sdk/sdk ${WDL})
target_include_directories(reaper_reachords PRIVATE src)
target_compile_definitions(reaper_reachords PRIVATE SWELL_PROVIDED_BY_APP)
target_link_libraries(reaper_reachords PRIVATE "-framework Cocoa")

# basic-pitch's CoreML model, compiled; installed as UserPlugins/reatrace/nmp.mlmodelc
set(BASIC_PITCH_MODEL ${CMAKE_BINARY_DIR}/nmp.mlmodelc)
add_custom_command(OUTPUT ${BASIC_PITCH_MODEL}/coremldata.bin
  COMMAND xcrun coremlcompiler compile ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage ${CMAKE_BINARY_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage/Data/com.apple.CoreML/model.mlmodel
    ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage/Data/com.apple.CoreML/weights/weight.bin
    ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage/Manifest.json
  COMMENT "Compiling the basic-pitch CoreML model")
add_custom_target(basic_pitch_model ALL DEPENDS ${BASIC_PITCH_MODEL}/coremldata.bin)

set(USERPLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
add_custom_target(deploy
  COMMAND ${CMAKE_COMMAND} -E make_directory ${USERPLUGINS}/reatrace
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:reaper_reatrace> $<TARGET_FILE:reaper_reachords> ${USERPLUGINS}/
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${USERPLUGINS}/reatrace/nmp.mlmodelc
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${BASIC_PITCH_MODEL} ${USERPLUGINS}/reatrace/nmp.mlmodelc
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/LICENSE ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/NOTICE ${USERPLUGINS}/reatrace/
  DEPENDS reaper_reatrace reaper_reachords basic_pitch_model
  COMMENT "Copying to REAPER UserPlugins (restart REAPER to load)")

add_executable(nsgt_test tests/nsgt_test.cpp src/reatrace/dsp/nsgt.cpp ${WDL}/fft.c)
target_include_directories(nsgt_test SYSTEM PRIVATE ${WDL})
target_compile_options(nsgt_test PRIVATE -UNDEBUG)

add_executable(editor_test tests/editor_test.cpp src/reatrace/notes/editor.cpp)

add_executable(notesync_test tests/notesync_test.cpp src/reatrace/notes/notesync.cpp)
target_compile_options(notesync_test PRIVATE -UNDEBUG)

add_executable(model_test tests/model_test.cpp src/reatrace/model.cpp src/reatrace/dsp/nsgt.cpp ${WDL}/fft.c)
target_include_directories(model_test SYSTEM PRIVATE ${WDL})
target_compile_options(model_test PRIVATE -UNDEBUG)
target_compile_options(editor_test PRIVATE -UNDEBUG)

add_executable(contour_test tests/contour_test.cpp src/reatrace/dsp/contour.cpp src/reatrace/dsp/nsgt.cpp ${WDL}/fft.c)
target_include_directories(contour_test SYSTEM PRIVATE ${WDL})
target_compile_options(contour_test PRIVATE -UNDEBUG)

add_executable(estimate_test tests/estimate_test.cpp src/reatrace/dsp/estimate.cpp src/reatrace/dsp/basicpitch.mm src/reatrace/dsp/nsgt.cpp ${WDL}/fft.c)
target_include_directories(estimate_test SYSTEM PRIVATE ${WDL})
target_compile_options(estimate_test PRIVATE -UNDEBUG)
target_compile_definitions(estimate_test PRIVATE BASIC_PITCH_MODEL="${BASIC_PITCH_MODEL}")
target_link_libraries(estimate_test PRIVATE "-framework Foundation" "-framework CoreML" "-framework Accelerate")
add_dependencies(estimate_test basic_pitch_model)

# The isolate JSFX run through EEL2 (portable interpreter, not REAPER's JIT; same language).
set(EEL2 ${WDL}/eel2)
add_library(eel2 STATIC ${EEL2}/nseel-caltab.c ${EEL2}/nseel-compiler.c ${EEL2}/nseel-eval.c
  ${EEL2}/nseel-lextab.c ${EEL2}/nseel-ram.c ${EEL2}/nseel-yylex.c ${EEL2}/nseel-cfunc.c)
target_compile_definitions(eel2 PUBLIC EEL_TARGET_PORTABLE NSEEL_LOOPFUNC_SUPPORT_MAXLEN=0)
target_compile_options(eel2 PRIVATE -w -fsigned-char)
target_include_directories(eel2 SYSTEM PUBLIC ${WDL})
add_executable(isolate_test tests/isolate_test.cpp src/reatrace/playback/isolatefx.cpp)
target_link_libraries(isolate_test PRIVATE eel2)
target_compile_options(isolate_test PRIVATE -UNDEBUG)

# the self-checks include ReaTrace's headers the same way
foreach(t nsgt_test editor_test notesync_test model_test contour_test estimate_test isolate_test)
  target_include_directories(${t} PRIVATE src/reatrace src)
endforeach()

# Sanitized self-checks: cmake -B build-asan -DREATRACE_SANITIZE=address,undefined (or thread), in a
# separate build directory; the plugin itself is not sanitized (it runs inside REAPER).
set(REATRACE_SANITIZE "" CACHE STRING "Sanitizers for the self-check programs, e.g. address,undefined or thread")
if(REATRACE_SANITIZE)
  foreach(t nsgt_test editor_test notesync_test model_test contour_test estimate_test isolate_test)
    target_compile_options(${t} PRIVATE -fsanitize=${REATRACE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    target_link_options(${t} PRIVATE -fsanitize=${REATRACE_SANITIZE})
  endforeach()
endif()

# clang-tidy over our sources (.clang-tidy has the checks); Xcode has none, Homebrew's llvm does.
find_program(CLANG_TIDY clang-tidy HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
if(CLANG_TIDY)
  # Homebrew's clang-tidy doesn't know the macOS SDK
  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_custom_target(lint
    COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet --extra-arg=-isysroot${MACOS_SDK} ${REATRACE_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-tidy"
    VERBATIM)
endif()

# clang-format (from Homebrew's llvm; style in .clang-format) over our sources and headers
find_program(CLANG_FORMAT clang-format HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
if(CLANG_FORMAT)
  file(GLOB_RECURSE REATRACE_HEADERS ${CMAKE_SOURCE_DIR}/src/*.h)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${REATRACE_SOURCES} ${REATRACE_HEADERS}
    COMMENT "clang-format" VERBATIM)
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT} --dry-run --Werror ${REATRACE_SOURCES} ${REATRACE_HEADERS}
    COMMENT "clang-format check" VERBATIM)
endif()
