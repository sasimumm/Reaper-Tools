cmake_minimum_required(VERSION 3.20)
project(reatrace LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Bounds-checked operator[] on std::vector, std::span, std::string etc. (libc++'s cheap checks), also in the plugin.
add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
set(WDL ${CMAKE_SOURCE_DIR}/vendor/WDL/WDL)

if(NOT APPLE)
  message(FATAL_ERROR "Only macOS is supported so far")
endif()

add_library(reaper_reatrace MODULE
  src/main.cpp
  src/nsgt.cpp
  src/editor.cpp
  src/window.cpp
  src/contour.cpp
  src/estimate.cpp
  src/basicpitch.mm
  ${WDL}/fft.c
  ${WDL}/swell/swell-modstub.mm
)
set_target_properties(reaper_reatrace PROPERTIES PREFIX "" SUFFIX ".dylib")
set_source_files_properties(src/basicpitch.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
target_include_directories(reaper_reatrace PRIVATE vendor/reaper-sdk/sdk ${WDL})
target_compile_definitions(reaper_reatrace PRIVATE SWELL_PROVIDED_BY_APP)
target_link_libraries(reaper_reatrace PRIVATE "-framework Cocoa" "-framework CoreML" "-framework Accelerate")

# basic-pitch's CoreML model, compiled; installed as UserPlugins/reatrace/nmp.mlmodelc
set(BASIC_PITCH_MODEL ${CMAKE_BINARY_DIR}/nmp.mlmodelc)
add_custom_command(OUTPUT ${BASIC_PITCH_MODEL}/coremldata.bin
  COMMAND xcrun coremlcompiler compile ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage ${CMAKE_BINARY_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/nmp.mlpackage/Data/com.apple.CoreML/model.mlmodel
  COMMENT "Compiling the basic-pitch CoreML model")
add_custom_target(basic_pitch_model ALL DEPENDS ${BASIC_PITCH_MODEL}/coremldata.bin)

set(USERPLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
add_custom_target(deploy
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:reaper_reatrace> ${USERPLUGINS}/
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${USERPLUGINS}/reatrace/nmp.mlmodelc
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${BASIC_PITCH_MODEL} ${USERPLUGINS}/reatrace/nmp.mlmodelc
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/LICENSE ${CMAKE_SOURCE_DIR}/vendor/basic-pitch/NOTICE ${USERPLUGINS}/reatrace/
  DEPENDS reaper_reatrace basic_pitch_model
  COMMENT "Copying to REAPER UserPlugins (restart REAPER to load)")

add_executable(nsgt_test tests/nsgt_test.cpp src/nsgt.cpp ${WDL}/fft.c)
target_include_directories(nsgt_test PRIVATE ${WDL})
target_compile_options(nsgt_test PRIVATE -UNDEBUG)

add_executable(editor_test tests/editor_test.cpp src/editor.cpp)
target_compile_options(editor_test PRIVATE -UNDEBUG)

add_executable(contour_test tests/contour_test.cpp src/contour.cpp src/nsgt.cpp ${WDL}/fft.c)
target_include_directories(contour_test PRIVATE ${WDL})
target_compile_options(contour_test PRIVATE -UNDEBUG)

add_executable(estimate_test tests/estimate_test.cpp src/estimate.cpp src/basicpitch.mm src/nsgt.cpp ${WDL}/fft.c)
target_include_directories(estimate_test PRIVATE ${WDL})
target_compile_options(estimate_test PRIVATE -UNDEBUG)
target_compile_definitions(estimate_test PRIVATE BASIC_PITCH_MODEL="${BASIC_PITCH_MODEL}")
target_link_libraries(estimate_test PRIVATE "-framework Foundation" "-framework CoreML" "-framework Accelerate")
add_dependencies(estimate_test basic_pitch_model)
