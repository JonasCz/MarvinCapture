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

# Top-level build: the native core (marvin-core), MarvinCaptureCLI and the ctest
# suite. scripts/build.ps1 (Windows, then also the GUI) and scripts/build.sh
# (macOS/Linux) drive this; see docs/building.md.

cmake_minimum_required(VERSION 3.20)
project(pinnacle-oss C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The static core/engine/sinks libraries are linked into the shared
# marvin-core library, which needs them built as PIC on ELF.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_EXTENSIONS ON)   # gnu11, not c11: strict c11 hides POSIX
                              # declarations (nanosleep, clock_gettime, ...)
                              # on glibc/mingw unless _DEFAULT_SOURCE or
                              # similar is defined; gnu11 defines it for us,
                              # matching the -std=gnu11 the project has always used.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# The ctest suite (unit and replay tests, and the test-only executables). Off for a cross
# build whose binaries cannot run on the build machine: scripts/build-android-native.sh.
option(PIN_BUILD_TESTS "Build the ctest suite" ON)

# The release version: "version" of the top-level VERSION file (JSON; the same file
# is published for the update check, see src/engine/pin_update_net.c).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)
file(READ ${CMAKE_SOURCE_DIR}/VERSION _pin_version_json)
string(JSON PIN_APP_VERSION GET "${_pin_version_json}" version)
add_compile_definitions(PIN_APP_VERSION="${PIN_APP_VERSION}")
message(STATUS "MarvinCapture version ${PIN_APP_VERSION}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

find_package(Threads REQUIRED)

# --- pinnacle_core: the hardware layer (device, cfg, 1394, stream, analog,
# DV/HDV reassembly, logging), everything under src/core. Static on purpose:
# it's linked into the shared marvin-core library. ---
file(GLOB PINNACLE_CORE_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.c)

add_library(pinnacle_core STATIC ${PINNACLE_CORE_SRCS})
target_include_directories(pinnacle_core PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(pinnacle_core PUBLIC PkgConfig::LIBUSB Threads::Threads)
if(APPLE OR ANDROID)
    # The sources include <libusb-1.0/libusb.h>; libusb-1.0.pc only adds
    # .../include/libusb-1.0, which on Linux and MSYS2 sits under a default
    # include dir anyway but not on macOS (Homebrew's /opt/homebrew/include)
    # or for Android (third_party/libusb-android-<arch>/include).
    foreach(_dir IN LISTS LIBUSB_INCLUDE_DIRS)
        get_filename_component(_parent "${_dir}" DIRECTORY)
        target_include_directories(pinnacle_core PUBLIC "${_parent}")
    endforeach()
endif()
if(WIN32)
    # MMCSS, for the analog read loop's thread priority
    target_link_libraries(pinnacle_core PUBLIC avrt powrprof)
endif()

# --- Hardware-free regression tests. ---
if(PIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

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
        src/engine/pin_closer.c
        src/engine/pin_deck.c
        src/engine/pin_preview.c
        src/engine/pin_hdv_audio.c
        src/engine/pin_script_run.c
        src/engine/pin_devices_wait.c
        src/engine/pin_update_net.c
    )
    target_include_directories(pinnacle_engine PUBLIC ${CMAKE_SOURCE_DIR}/src/engine)
    target_link_libraries(pinnacle_engine PUBLIC pinnacle_core pinnacle_sinks
        pinnacle_engine_pure Threads::Threads)
    if(WIN32)
        # pin_devices_wait.c: CM_Register_Notification (device hotplug);
        # pin_update_net.c: WinHTTP.
        target_link_libraries(pinnacle_engine PUBLIC cfgmgr32 winhttp)
    else()
        # pin_session.c: dladdr() finds the core library's own directory.
        target_link_libraries(pinnacle_engine PUBLIC ${CMAKE_DL_LIBS})
    endif()
    # This target is only ever linked into the marvin-core DLL, never
    # a consumer of it, so pin_api.h's functions must not be seen as
    # dllimport here (PIN_API => dllexport) -- see src/api/pin_api.h.
    target_compile_definitions(pinnacle_engine PUBLIC PIN_BUILDING_LIBRARY)

    if(PIN_BUILD_TESTS)
        # pin_api.c: the engine calls back into it (pin_strerror)
        add_executable(test_pin_event_queue tests/engine/test_pin_event_queue.c src/api/pin_api.c)
        target_link_libraries(test_pin_event_queue PRIVATE pinnacle_engine)
        add_test(NAME test_pin_event_queue COMMAND test_pin_event_queue)

        add_executable(test_pin_closer tests/engine/test_pin_closer.c src/api/pin_api.c)
        target_link_libraries(test_pin_closer PRIVATE pinnacle_engine pinnacle_sinks)
        add_test(NAME test_pin_closer COMMAND test_pin_closer)

        add_executable(test_pin_deck tests/engine/test_pin_deck.c src/api/pin_api.c)
        target_link_libraries(test_pin_deck PRIVATE pinnacle_engine)
        add_test(NAME test_pin_deck COMMAND test_pin_deck)

        # the ready-made texts / enable rules that read a status snapshot or device info (pin_api.c)
        add_executable(test_pin_ui_api tests/engine/test_pin_ui_api.c src/api/pin_api.c)
        target_link_libraries(test_pin_ui_api PRIVATE pinnacle_engine)
        add_test(NAME test_pin_ui_api COMMAND test_pin_ui_api)

        # the calls for Android: pin_usb_model, pin_open("fd:N"), pin_set_app_dirs, format list and
        # warnings per platform, pin_set_cpu_keep_busy (pin_api.c)
        add_executable(test_pin_platform_api tests/engine/test_pin_platform_api.c src/api/pin_api.c)
        target_link_libraries(test_pin_platform_api PRIVATE pinnacle_engine)
        add_test(NAME test_pin_platform_api COMMAND test_pin_platform_api)
    endif()
endif()

# --- Public API: the marvin-core shared library (pin_api.c, a thin
# wrapper over src/engine) and MarvinCaptureCLI, the command-line front end
# that links nothing but it. ---
if(EXISTS ${CMAKE_SOURCE_DIR}/src/api/pin_api.c)
    add_library(marvin-core SHARED src/api/pin_api.c)
    target_compile_definitions(marvin-core PRIVATE PIN_BUILDING_LIBRARY)
    target_include_directories(marvin-core PUBLIC ${CMAKE_SOURCE_DIR}/src/api)
    target_link_libraries(marvin-core PRIVATE pinnacle_engine pinnacle_engine_pure
        pinnacle_sinks pinnacle_core Threads::Threads)
    if(WIN32)
        # winpthread and libgcc statically linked in where possible, so the
        # only extra runtime DLL the GUI needs to ship is libusb-1.0.dll.
        target_link_options(marvin-core PRIVATE -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        # Front-ends load it by this exact name; MinGW would add "lib".
        set_target_properties(marvin-core PROPERTIES PREFIX "")
        # Put libusb-1.0.dll next to the core so the build directory is a
        # complete runtime set a GUI (or MarvinCaptureCLI) can load from.
        find_file(PIN_LIBUSB_DLL libusb-1.0.dll PATHS ENV PATH NO_DEFAULT_PATH)
        if(PIN_LIBUSB_DLL)
            add_custom_command(TARGET marvin-core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PIN_LIBUSB_DLL} $<TARGET_FILE_DIR:marvin-core>)
        endif()
    else()
        if(ANDROID)
            # -Bsymbolic as below. The NDK has no libgcc to link statically (its runtime is
            # compiler-rt and libunwind, linked in already). --as-needed: FFmpeg's .pc lists
            # libandroid and libmediandk for MediaCodec even when that is not built in, which
            # would leave two unused dependencies. libusb is the static libusb-1.0.a of
            # scripts/build-android-native.sh (no shared one is installed there), so it ends
            # up inside libmarvin-core.so. --exclude-libs: export pin_api.h only, not the
            # static libraries' symbols (FFmpeg, libusb, the core's internals), so nothing
            # else in the app's process can bind to them and --gc-sections can drop what
            # the core does not use.
            target_link_options(marvin-core PRIVATE -Wl,-Bsymbolic -Wl,--as-needed
                -Wl,--exclude-libs,ALL)
        elseif(NOT APPLE)
            # Apple clang has no libgcc and rejects the flag.
            # -Bsymbolic: the static FFmpeg's x86 assembly references its own
            # tables (ff_pw_32 ...) PC-relative, which ld refuses in a shared
            # object while those symbols are preemptible; FFmpeg links its own
            # shared libraries the same way.
            target_link_options(marvin-core PRIVATE -static-libgcc -Wl,-Bsymbolic)
        endif()
        set_target_properties(marvin-core PROPERTIES C_VISIBILITY_PRESET hidden)
    endif()
    if(APPLE)
        # Bundle libusb like libusb-1.0.dll on Windows: copy Homebrew's
        # dylib next to the core and point the core at that copy
        # (@loader_path), so the build directory and dist/ don't need
        # Homebrew at run time. install_name_tool breaks the signature,
        # which arm64 requires, hence the ad-hoc re-signing.
        find_library(PIN_LIBUSB_DYLIB NAMES usb-1.0 HINTS ${LIBUSB_LIBRARY_DIRS} NO_DEFAULT_PATH)
        if(NOT PIN_LIBUSB_DYLIB)
            message(FATAL_ERROR "libusb-1.0 dylib not found in ${LIBUSB_LIBRARY_DIRS}")
        endif()
        # The name the core records for it, e.g.
        # /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib
        execute_process(COMMAND otool -D ${PIN_LIBUSB_DYLIB}
            OUTPUT_VARIABLE _pin_libusb_id OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX REPLACE "^[^\n]*\n" "" _pin_libusb_id "${_pin_libusb_id}")
        get_filename_component(_pin_libusb_name "${_pin_libusb_id}" NAME)
        set(_pin_libusb_copy $<TARGET_FILE_DIR:marvin-core>/${_pin_libusb_name})
        add_custom_command(TARGET marvin-core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${PIN_LIBUSB_DYLIB} ${_pin_libusb_copy}
            COMMAND chmod u+w ${_pin_libusb_copy}
            COMMAND install_name_tool -id @rpath/${_pin_libusb_name} ${_pin_libusb_copy}
            COMMAND install_name_tool -change ${_pin_libusb_id}
                @loader_path/${_pin_libusb_name} $<TARGET_FILE:marvin-core>
            COMMAND codesign --force --sign - ${_pin_libusb_copy}
            COMMAND codesign --force --sign - $<TARGET_FILE:marvin-core>
            VERBATIM)
    endif()

    # The FPGA bitstreams go in firmware/ next to the core library, where
    # the core looks for them first (see pin_set_firmware_dir()).
    add_custom_command(TARGET marvin-core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:marvin-core>/firmware
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/firmware/fpga-ohci.bin
            ${CMAKE_SOURCE_DIR}/firmware/fpga-capture.bin
            ${CMAKE_SOURCE_DIR}/firmware/fx2-marvin.bin
            ${CMAKE_SOURCE_DIR}/firmware/README.md
            $<TARGET_FILE_DIR:marvin-core>/firmware)

    add_executable(MarvinCaptureCLI src/cli/marvin_capture_cli.c)
    target_link_libraries(MarvinCaptureCLI PRIVATE marvin-core)
    if(ANDROID)
        # 16 KB pages (Android 15+ devices may use them; Google Play requires it): the NDK
        # links this way by default since r28, kept explicit so an older NDK cannot quietly
        # produce 4 KB-aligned segments.
        target_link_options(marvin-core PRIVATE -Wl,-z,max-page-size=16384)
        target_link_options(MarvinCaptureCLI PRIVATE -Wl,-z,max-page-size=16384)
    endif()
    if(NOT WIN32)
        # Find the core library next to the executable (build tree and
        # build/<os>-<arch>/dist alike), not via an absolute build path.
        if(APPLE)
            set(_pin_cli_rpath "@loader_path")
        else()
            set(_pin_cli_rpath "$ORIGIN")
        endif()
        if(ANDROID)
            # CMake's Android platform switches rpaths off ("Android ignores RPATH"), but
            # its linker honours DT_RUNPATH since API 24: the CLI in Termux finds
            # libmarvin-core.so next to itself.
            set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "-Wl,-rpath,")
            set(CMAKE_EXECUTABLE_RUNTIME_C_FLAG "-Wl,-rpath,")
            target_link_options(MarvinCaptureCLI PRIVATE -Wl,--enable-new-dtags)
        endif()
        set_target_properties(MarvinCaptureCLI PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "${_pin_cli_rpath}")
    endif()

    # MarvinPlayout: DV/HDV playout and the virtual deck, a separate program
    # (src/playout, docs/playout.md).
    add_subdirectory(src/playout)

    # Hardware-free ctest coverage of the session engine, driven through
    # pin_api.h against the replay (virtual device) source -- see
    # tests/engine_replay/replay_test_util.h.
    if(PIN_BUILD_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/engine_replay/CMakeLists.txt)
        add_subdirectory(${CMAKE_SOURCE_DIR}/tests/engine_replay ${CMAKE_BINARY_DIR}/tests/engine_replay)
    endif()
endif()

