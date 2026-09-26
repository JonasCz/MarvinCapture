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

# Top-level build. Supersedes src/Makefile (kept working for now, see its
# header) as the build that also targets Windows: this project is built on
# Linux (the capture rig) with plain gcc/make today, and on Windows with
# MSYS2 UCRT64 gcc from here on, since a WinUI 3 GUI is coming and its native
# core needs to build there too. See docs and HANDOFF.md for the hardware
# background; this file only wires up the build.

cmake_minimum_required(VERSION 3.20)
project(pinnacle-oss C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The static core/engine/sinks libraries are linked into the shared
# pinnacle-oss-core library, which needs them built as PIC on ELF.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_EXTENSIONS ON)   # gnu11, not c11: strict c11 hides POSIX
                              # declarations (nanosleep, clock_gettime, ...)
                              # on glibc/mingw unless _DEFAULT_SOURCE or
                              # similar is defined; gnu11 defines it for us,
                              # matching src/Makefile's -std=gnu11.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

find_package(Threads REQUIRED)

# --- pinnacle_core: the hardware layer (device, cfg, 1394, stream, analog,
# DV/HDV reassembly, logging), everything under src/core. Static on purpose:
# it's linked straight into each CLI today, and later into the shared
# pinnacle-oss-core library and pinctl by a future phase, unchanged. ---
file(GLOB PINNACLE_CORE_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.c)

add_library(pinnacle_core STATIC ${PINNACLE_CORE_SRCS})
target_include_directories(pinnacle_core PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(pinnacle_core PUBLIC PkgConfig::LIBUSB Threads::Threads)

# --- CLIs. Kept building (and kept, full stop -- see the plan) as low-level
# test tools once a session-oriented pinctl exists. ---
add_executable(pincli src/cli/pincli.c)
target_link_libraries(pincli PRIVATE pinnacle_core)

add_executable(pindeck src/cli/pindeck.c)
target_link_libraries(pindeck PRIVATE pinnacle_core)

add_executable(pinanalog src/cli/pinanalog.c)
target_link_libraries(pinanalog PRIVATE pinnacle_core)

add_executable(pinlist src/cli/pinlist.c)
target_link_libraries(pinlist PRIVATE pinnacle_core)

# --- Hardware-free regression tests. ---
enable_testing()
add_subdirectory(tests)

# --- Session engine (Phase 3+): pinnacle_engine_pure (hardware-free; the
# rest of pinnacle_engine is defined further down, after src/sinks, since it
# needs pinnacle_sinks while tests/sinks (added by src/sinks below) needs
# pinnacle_engine_pure -- see src/engine/CMakeLists.txt's comment). ---
if(EXISTS ${CMAKE_SOURCE_DIR}/src/engine/CMakeLists.txt)
    add_subdirectory(src/engine)
endif()

# --- File-output layer (Phase 6): pin_writer + the pin_sink_t muxers. ---
add_subdirectory(src/sinks)

# --- pinnacle_engine: pin_session/pin_deck/pin_preview, the rest of the
# session engine (see src/engine/pin_session.[ch] etc). Needs libusb (via
# pinnacle_core), the sinks and the hardware-free engine modules, all of
# which now exist. ---
if(EXISTS ${CMAKE_SOURCE_DIR}/src/engine/pin_session.c)
    add_library(pinnacle_engine STATIC
        src/engine/pin_session.c
        src/engine/pin_deck.c
        src/engine/pin_preview.c
    )
    target_include_directories(pinnacle_engine PUBLIC ${CMAKE_SOURCE_DIR}/src/engine)
    target_link_libraries(pinnacle_engine PUBLIC pinnacle_core pinnacle_sinks
        pinnacle_engine_pure Threads::Threads)
    # This target is only ever linked into the pinnacle-oss-core DLL, never
    # a consumer of it, so pin_api.h's functions must not be seen as
    # dllimport here (PIN_API => dllexport) -- see src/api/pin_api.h.
    target_compile_definitions(pinnacle_engine PUBLIC PIN_BUILDING_LIBRARY)
endif()

# --- Public API: the pinnacle-oss-core shared library (pin_api.c, a thin
# wrapper over src/engine) and pinctl, the API-only CLI/integration test
# that links nothing but it. ---
if(EXISTS ${CMAKE_SOURCE_DIR}/src/api/pin_api.c)
    add_library(pinnacle-oss-core SHARED src/api/pin_api.c)
    target_compile_definitions(pinnacle-oss-core PRIVATE PIN_BUILDING_LIBRARY)
    target_include_directories(pinnacle-oss-core PUBLIC ${CMAKE_SOURCE_DIR}/src/api)
    target_link_libraries(pinnacle-oss-core PRIVATE pinnacle_engine pinnacle_engine_pure
        pinnacle_sinks pinnacle_core Threads::Threads)
    if(WIN32)
        # winpthread and libgcc statically linked in where possible, so the
        # only extra runtime DLL the GUI needs to ship is libusb-1.0.dll.
        target_link_options(pinnacle-oss-core PRIVATE -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        # Front-ends load it by this exact name; MinGW would add "lib".
        set_target_properties(pinnacle-oss-core PROPERTIES PREFIX "")
        # Put libusb-1.0.dll next to the core so the build directory is a
        # complete runtime set a GUI (or pinctl) can load from.
        find_file(PIN_LIBUSB_DLL libusb-1.0.dll PATHS ENV PATH NO_DEFAULT_PATH)
        if(PIN_LIBUSB_DLL)
            add_custom_command(TARGET pinnacle-oss-core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PIN_LIBUSB_DLL} $<TARGET_FILE_DIR:pinnacle-oss-core>)
        endif()
    else()
        target_link_options(pinnacle-oss-core PRIVATE -static-libgcc)
        set_target_properties(pinnacle-oss-core PROPERTIES C_VISIBILITY_PRESET hidden)
    endif()

    add_executable(pinctl src/cli/pinctl.c)
    target_link_libraries(pinctl PRIVATE pinnacle-oss-core)

    # Hardware-free ctest coverage of the session engine, driven through
    # pin_api.h against the replay (virtual device) source -- see
    # tests/engine_replay/replay_test_util.h.
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/engine_replay/CMakeLists.txt)
        add_subdirectory(${CMAKE_SOURCE_DIR}/tests/engine_replay ${CMAKE_BINARY_DIR}/tests/engine_replay)
    endif()
endif()

