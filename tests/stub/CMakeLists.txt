# pinnacle-oss-core STUB — a hardware-free fake of src/api/pin_api.h,
# built as pinnacle-oss-core.dll (the same name the real core library will
# use) so the Windows GUI can be developed and tested with zero hardware.
#
# Standalone: this file does not depend on anything else in the repo
# except the frozen ABI header src/api/pin_api.h (read-only, never built
# from here). Build with:
#
#   C:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:$PATH; \
#     cd '/c/Users/Jonas/Desktop/pinnacle-500-usb-open-driver/tests/stub' && \
#     cmake -G Ninja -B build-stub && cmake --build build-stub"
#
# Output: tests/stub/build-stub/pinnacle-oss-core.dll (a bin/ subdir depending
# on the generator; the build step below reports the exact path).

cmake_minimum_required(VERSION 3.16)
project(pinnacle_oss_core_stub C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)

add_library(pinnacle-oss-core SHARED
    pin_stub_util.c
    pin_stub_session.c
    pin_stub_worker.c
    pin_stub_deck.c
    pin_stub_capture.c
    pin_stub_formats.c
    pin_stub_preview.c
    pin_stub_settings.c
    pin_stub_launch.c
)

target_include_directories(pinnacle-oss-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/api
)

target_compile_definitions(pinnacle-oss-core PRIVATE PIN_BUILDING_LIBRARY)

target_compile_options(pinnacle-oss-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(WIN32)
    # winpthreads (via UCRT64 gcc's -pthread) covers pthread_*, clock_gettime,
    # nanosleep. Also link the Win32 APIs used for real disk-free-space /
    # FAT32 detection and settings persistence.
    target_link_libraries(pinnacle-oss-core PRIVATE Threads::Threads kernel32)
    target_compile_options(pinnacle-oss-core PRIVATE -pthread)
    target_link_options(pinnacle-oss-core PRIVATE -pthread)
else()
    target_link_libraries(pinnacle-oss-core PRIVATE Threads::Threads m)
endif()

set_target_properties(pinnacle-oss-core PROPERTIES
    OUTPUT_NAME "pinnacle-oss-core"
    PREFIX ""
)
