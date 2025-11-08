cmake_minimum_required(VERSION 3.16)
project(audio-lockstep LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# Detect platform and compiler
# ------------------------------------------------------------------------------
message(STATUS "Compiler ID: ${CMAKE_CXX_COMPILER_ID}")

if (MSVC)
  message(STATUS "Detected MSVC toolchain")

  # Default GStreamer install path for MSVC
  if(NOT DEFINED GST_ROOT AND DEFINED ENV{GSTREAMER_1_0_ROOT_X86_64})
    set(GST_ROOT "$ENV{GSTREAMER_1_0_ROOT_X86_64}")
  endif()

  if(NOT EXISTS "${GST_ROOT}")
    message(FATAL_ERROR "Set GST_ROOT to your GStreamer MSVC root (e.g. C:/gstreamer/1.0/msvc_x86_64)")
  endif()

  set(GST_LIB_SUFFIX "")
  set(GST_LIB_EXT "")
else()
  message(STATUS "Detected MinGW or GCC toolchain")

  # Default path for MinGW GStreamer (MSYS2)
  if(NOT DEFINED GST_ROOT)
    set(GST_ROOT "C:/msys64/mingw64")
  endif()

  set(GST_LIB_SUFFIX "lib")
  set(GST_LIB_EXT ".dll.a")
endif()

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
set(GST_INC_DIRS
  "${GST_ROOT}/include"
  "${GST_ROOT}/include/gstreamer-1.0"
  "${GST_ROOT}/include/glib-2.0"
  "${GST_ROOT}/lib/glib-2.0/include"
  "${GST_ROOT}/lib/gstreamer-1.0/include"
)

include_directories(${GST_INC_DIRS})

# ------------------------------------------------------------------------------
# Find libraries
# ------------------------------------------------------------------------------
if (MSVC)
  find_library(GSTREAMER_LIB NAMES gstreamer-1.0 HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  find_library(GOBJECT_LIB   NAMES gobject-2.0   HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  find_library(GLIB_LIB      NAMES glib-2.0      HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
else()
  find_library(GSTREAMER_LIB NAMES ${GST_LIB_SUFFIX}gstreamer-1.0${GST_LIB_EXT} HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  find_library(GOBJECT_LIB   NAMES ${GST_LIB_SUFFIX}gobject-2.0${GST_LIB_EXT}   HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  find_library(GLIB_LIB      NAMES ${GST_LIB_SUFFIX}glib-2.0${GST_LIB_EXT}      HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
endif()

# ------------------------------------------------------------------------------
# Target
# ------------------------------------------------------------------------------
add_executable(audio-lockstep src/main.cpp)

target_link_libraries(audio-lockstep PRIVATE
  "${GSTREAMER_LIB}"
  "${GOBJECT_LIB}"
  "${GLIB_LIB}"
)

# ------------------------------------------------------------------------------
# MSVC-specific tweaks
# ------------------------------------------------------------------------------
if(MSVC)
  set_target_properties(audio-lockstep PROPERTIES
    VS_DEBUGGER_ENVIRONMENT "PATH=${GST_ROOT}\\bin;%PATH%")
  target_compile_options(audio-lockstep PRIVATE /utf-8 /EHsc)
  target_compile_definitions(audio-lockstep PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(audio-lockstep PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()
