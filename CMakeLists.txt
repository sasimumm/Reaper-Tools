cmake_minimum_required(VERSION 3.20)
project(reascribe LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(WDL ${CMAKE_SOURCE_DIR}/vendor/WDL/WDL)

if(NOT APPLE)
  message(FATAL_ERROR "Only macOS is supported so far")
endif()

add_library(reaper_reascribe MODULE
  src/main.cpp
  src/nsgt.cpp
  ${WDL}/fft.c
  ${WDL}/swell/swell-modstub.mm
)
set_target_properties(reaper_reascribe PROPERTIES PREFIX "" SUFFIX ".dylib")
target_include_directories(reaper_reascribe PRIVATE vendor/reaper-sdk/sdk ${WDL})
target_compile_definitions(reaper_reascribe PRIVATE SWELL_PROVIDED_BY_APP)
target_link_libraries(reaper_reascribe PRIVATE "-framework Cocoa")

set(USERPLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
add_custom_target(deploy
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:reaper_reascribe> ${USERPLUGINS}/
  DEPENDS reaper_reascribe
  COMMENT "Copying to REAPER UserPlugins (restart REAPER to load)")

add_executable(nsgt_test tests/nsgt_test.cpp src/nsgt.cpp ${WDL}/fft.c)
target_include_directories(nsgt_test PRIVATE ${WDL})
target_compile_options(nsgt_test PRIVATE -UNDEBUG)
