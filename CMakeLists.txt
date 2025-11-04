cmake_minimum_required(VERSION 3.16)
project(audio-lockstep CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where GStreamer (MSVC x64) is installed.
# You can pass -DGST_ROOT="C:/Program Files/gstreamer/1.0/msvc_x86_64"
# or we’ll fall back to the common env var from the MSI.
if(NOT DEFINED GST_ROOT AND DEFINED ENV{GSTREAMER_1_0_ROOT_X86_64})
  set(GST_ROOT "$ENV{GSTREAMER_1_0_ROOT_X86_64}")
endif()

if(NOT EXISTS "${GST_ROOT}")
  message(FATAL_ERROR "Set GST_ROOT to your GStreamer root (e.g. C:/Program Files/gstreamer/1.0/msvc_x86_64)")
endif()

# ----- Includes -----
# We include both GStreamer and GLib headers.
set(GST_INC_DIRS
  "${GST_ROOT}/include"
  "${GST_ROOT}/include/gstreamer-1.0"
  "${GST_ROOT}/include/glib-2.0"
  "${GST_ROOT}/lib/glib-2.0/include"         # glibconfig.h
  "${GST_ROOT}/lib/gstreamer-1.0/include"    # gstconfig.h (present in newer builds)
)

# ----- Libraries -----
# Link only the core libs we actually use.
# (If your linker complains, add gio-2.0 and/or gmodule-2.0 the same way.)
find_library(GSTREAMER_LIB NAMES gstreamer-1.0 HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(GOBJECT_LIB   NAMES gobject-2.0   HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(GLIB_LIB      NAMES glib-2.0      HINTS "${GST_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)

add_executable(audio-lockstep src/main.cpp)

target_include_directories(audio-lockstep PRIVATE ${GST_INC_DIRS})
target_link_libraries(audio-lockstep PRIVATE
  "${GSTREAMER_LIB}"
  "${GOBJECT_LIB}"
  "${GLIB_LIB}"
)

# Optional: give the VS debugger a PATH that includes GStreamer DLLs,
# so running from VS/VSCode "just works".
set_target_properties(audio-lockstep PROPERTIES
  VS_DEBUGGER_ENVIRONMENT "PATH=${GST_ROOT}\\bin;%PATH%")

if(MSVC)
  target_compile_options(audio-lockstep PRIVATE /utf-8 /EHsc)
  target_compile_definitions(audio-lockstep PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()