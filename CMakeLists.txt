cmake_minimum_required(VERSION 3.20)
project(AppSrcFeeder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------- MSVC niceties ----------------
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  add_compile_options(/W4 /EHsc /permissive- /utf-8 /MP)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------- Configurable root ----------------
# For manual fallback linking (no pkg-config)
set(GSTREAMER_ROOT "C:/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC root")

# ---------------- Try pkg-config first ------------
include(CheckCXXSourceCompiles)
find_package(PkgConfig QUIET)

set(HAVE_GST_PKG OFF)
if(PkgConfig_FOUND)
  # Point pkg-config at the MSVC SDK's .pc files if needed
  if(NOT DEFINED ENV{PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_PATH} "${GSTREAMER_ROOT}/lib/pkgconfig")
  endif()

  pkg_check_modules(GST QUIET IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gobject-2.0
    glib-2.0
  )
  if(GST_FOUND)
    set(HAVE_GST_PKG ON)
  endif()
endif()

add_executable(appsrc_feeder main.cpp)

if(HAVE_GST_PKG)
  # ---------- pkg-config path (preferred) ----------
  target_link_libraries(appsrc_feeder PRIVATE PkgConfig::GST)
else()
  # ---------- manual path fallback ----------
  # Basic sanity check
  if(NOT EXISTS "${GSTREAMER_ROOT}/include" OR NOT EXISTS "${GSTREAMER_ROOT}/lib")
    message(FATAL_ERROR "GSTREAMER_ROOT does not look valid: ${GSTREAMER_ROOT}")
  endif()

  target_include_directories(appsrc_feeder PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    "${GSTREAMER_ROOT}/include"
  )

  target_link_directories(appsrc_feeder PRIVATE
    "${GSTREAMER_ROOT}/lib"
  )

  # MSVC import libs are *.lib in the MSVC SDK
  target_link_libraries(appsrc_feeder PRIVATE
    gstreamer-1.0
    gstapp-1.0
    gstbase-1.0
    gobject-2.0
    glib-2.0
    # If you hit unresolved externals, uncomment Win32 libs below:
    # ws2_32 winmm shlwapi ole32 uuid
  )
endif()

# Helpful note after build
add_custom_command(TARGET appsrc_feeder POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo "Add ${GSTREAMER_ROOT}/bin to PATH before running."
)