# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
# for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# File-output layer: pin_writer (bounded queue + writer thread) and the
# pin_sink_t implementations (raw passthrough, analog AVI, analog FFV1/MKV,
# DV/HDV rewrap). Links the minimal static FFmpeg built by
# scripts/build-ffmpeg.sh (see third_party/README.md).

if(WIN32)
    set(PIN_FFMPEG_OS "windows")
elseif(APPLE)
    set(PIN_FFMPEG_OS "macos")
else()
    set(PIN_FFMPEG_OS "linux")
endif()

set(PIN_FFMPEG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT PIN_FFMPEG_ARCH)
    set(PIN_FFMPEG_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
if(NOT PIN_FFMPEG_ARCH AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Some Windows generator/toolchain combinations (seen with MSYS2 UCRT64
    # gcc + Ninja) leave both CMAKE_SYSTEM_PROCESSOR and
    # CMAKE_HOST_SYSTEM_PROCESSOR blank. x86_64 is the only target this
    # project's scripts/build-ffmpeg.sh and CI produce today; override with
    # -DPIN_FFMPEG_PREFIX=... for anything else (e.g. arm64 macOS/Windows).
    set(PIN_FFMPEG_ARCH "x86_64")
endif()
if(PIN_FFMPEG_ARCH MATCHES "^(AMD64|amd64|x86_64)$")
    set(PIN_FFMPEG_ARCH "x86_64")
elseif(PIN_FFMPEG_ARCH MATCHES "^(aarch64|arm64|ARM64)$")
    set(PIN_FFMPEG_ARCH "arm64")
endif()

# The cache entry stays empty unless the user sets it; the default is
# resolved on every configure. Caching the computed default instead went
# wrong on the in-build re-runs Ninja does after a CMakeLists change, where
# the processor variables can come back blank and the cache then held a
# useless path for good.
set(PIN_FFMPEG_PREFIX "" CACHE PATH
    "Prefix of the minimal static FFmpeg (empty: third_party/ffmpeg-<os>-<arch> from scripts/build-ffmpeg.sh)")
if(NOT PIN_FFMPEG_PREFIX)
    set(PIN_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-${PIN_FFMPEG_OS}-${PIN_FFMPEG_ARCH}")
endif()

set(_pin_ffmpeg_pkgconfig "${PIN_FFMPEG_PREFIX}/lib/pkgconfig")
if(NOT EXISTS "${_pin_ffmpeg_pkgconfig}")
    message(FATAL_ERROR
        "src/sinks needs the minimal FFmpeg built by scripts/build-ffmpeg.sh at "
        "${PIN_FFMPEG_PREFIX} (looked for ${_pin_ffmpeg_pkgconfig}). Run "
        "scripts/build-ffmpeg.sh, or set -DPIN_FFMPEG_PREFIX=... to an existing build.")
endif()

find_package(PkgConfig REQUIRED)
set(_pin_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${_pin_ffmpeg_pkgconfig}")
# pkg_check_modules() always also computes a "<prefix>_STATIC_*" variant
# from `pkg-config --static`; that's the one this project needs, since this
# FFmpeg build is a static-only install and its .pc files list the extra
# system libs (bcrypt/ole32/user32/atomic on Windows) only under --static.
# There is no separate STATIC keyword to request just that half on the
# FindPkgConfig module shipped with the CMake used here, so PIN_FFMPEG_STATIC_*
# is used directly below instead of an IMPORTED_TARGET (which would wire up
# the non-static flags).
pkg_check_modules(PIN_FFMPEG REQUIRED libavformat libavcodec libavutil)
set(ENV{PKG_CONFIG_PATH} "${_pin_saved_pkg_config_path}")

add_library(pinnacle_sinks STATIC
    pin_writer.c
    pin_sink_factory.c
    sink_sar.c
    sink_raw.c
    sink_nut.c
    pin_stdout.c
    sink_avi.c
    sink_ffv1.c
    sink_rewrap.c
    sink_lazy.c
)
target_include_directories(pinnacle_sinks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${PIN_FFMPEG_STATIC_INCLUDE_DIRS})
target_compile_options(pinnacle_sinks PRIVATE ${PIN_FFMPEG_STATIC_CFLAGS_OTHER})
target_link_directories(pinnacle_sinks PUBLIC ${PIN_FFMPEG_STATIC_LIBRARY_DIRS})
target_link_libraries(pinnacle_sinks PUBLIC pinnacle_core Threads::Threads
    ${PIN_FFMPEG_STATIC_LIBRARIES})
set(_pin_ffmpeg_ldflags ${PIN_FFMPEG_STATIC_LDFLAGS_OTHER})
if(APPLE)
    # macOS: libavutil.pc lists "-framework CoreFoundation -framework ...";
    # as separate list items CMake de-duplicates the repeated "-framework"
    # and the linker then sees bare "CoreVideo" etc. Keep each pair together.
    string(REPLACE "-framework;" "-Wl,-framework," _pin_ffmpeg_ldflags "${_pin_ffmpeg_ldflags}")
endif()
target_link_options(pinnacle_sinks PUBLIC ${_pin_ffmpeg_ldflags})

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/sinks/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/sinks ${CMAKE_BINARY_DIR}/tests/sinks)
endif()
