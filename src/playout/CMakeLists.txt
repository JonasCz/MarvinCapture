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

# MarvinPlayout: DV/HDV playout and the virtual deck (docs/playout.md).
#
#   pinnacle_playout_pure  every pout_*.c except pout_hw_*.c: no libusb, unit-tested
#   MarvinPlayout          the program: pout_hw_*.c + marvin_playout.c, linked
#                          statically against the core (it does not use marvin-core)
#   tests/playout/test_*.c one ctest test each

file(GLOB _pout_all CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pout_*.c)
file(GLOB _pout_hw CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pout_hw_*.c)
set(_pout_pure ${_pout_all})
if(_pout_hw)
    list(REMOVE_ITEM _pout_pure ${_pout_hw})
endif()
if(NOT _pout_pure)
    return()
endif()

add_library(pinnacle_playout_pure STATIC ${_pout_pure})
target_include_directories(pinnacle_playout_pure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# pinnacle_core for dv_reassembler (the --dry-run loopback output) and pin_log;
# none of the pure files call libusb.
target_link_libraries(pinnacle_playout_pure PUBLIC pinnacle_engine_pure pinnacle_core)
target_compile_features(pinnacle_playout_pure PUBLIC c_std_11)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/marvin_playout.c)
    add_executable(MarvinPlayout marvin_playout.c ${_pout_hw})
    target_link_libraries(MarvinPlayout PRIVATE pinnacle_playout_pure pinnacle_core
        pinnacle_engine_pure Threads::Threads)
    if(UNIX AND NOT APPLE)
        target_link_libraries(MarvinPlayout PRIVATE m)
    endif()
    if(WIN32)
        target_link_options(MarvinPlayout PRIVATE -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    # Next to MarvinCaptureCLI, marvin-core and firmware/ (the top-level build puts them all in
    # the build directory; the program looks for firmware/ next to itself, see
    # pout_hw_find_bitstream()), and libusb-1.0.dll beside it on Windows.
    set_target_properties(MarvinPlayout PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(ANDROID)
        # 16 KB pages, as for marvin-core and MarvinCaptureCLI (see the top-level CMakeLists.txt)
        target_link_options(MarvinPlayout PRIVATE -Wl,-z,max-page-size=16384)
    endif()
    if(APPLE AND DEFINED _pin_libusb_id AND DEFINED _pin_libusb_name)
        # The same libusb copy marvin-core bundles (top-level CMakeLists.txt): point this
        # program at it too instead of Homebrew's, so dist/ needs no Homebrew at run time.
        add_custom_command(TARGET MarvinPlayout POST_BUILD
            COMMAND install_name_tool -change ${_pin_libusb_id}
                @loader_path/${_pin_libusb_name} $<TARGET_FILE:MarvinPlayout>
            COMMAND codesign --force --sign - $<TARGET_FILE:MarvinPlayout>
            VERBATIM)
    endif()
endif()

if(PIN_BUILD_TESTS)
    file(GLOB _pout_tests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/playout/test_*.c)
    foreach(_src IN LISTS _pout_tests)
        get_filename_component(_name ${_src} NAME_WE)
        add_executable(${_name} ${_src})
        target_link_libraries(${_name} PRIVATE pinnacle_playout_pure pinnacle_core pinnacle_engine_pure)
        if(UNIX AND NOT APPLE)
            target_link_libraries(${_name} PRIVATE m)
        endif()
        # Generated DV/HDV fixtures (tests/data/README.md): the test finds them in
        # PIN_TEST_DATA and skips (exit 77) when they are missing.
        add_test(NAME ${_name} COMMAND ${_name})
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT "PIN_TEST_DATA=${PIN_TEST_DATA}"
            FIXTURES_REQUIRED pin_test_data
            SKIP_RETURN_CODE 77)
    endforeach()
endif()
