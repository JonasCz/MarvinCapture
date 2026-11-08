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

# --- Test fixtures ---------------------------------------------------------------------
# The recordings the replay and sink tests run on are made at test time, not committed:
# synthetic DV and HDV files from the ffmpeg command-line tool, and raw EP 0x88 dumps that
# make_ep88 wraps around two of the DV files. The fixture-setup test pin_test_data writes
# them into <build>/tests/data (make_fixtures.cmake, tests/data/README.md); every test that
# reads them has FIXTURES_REQUIRED pin_test_data and gets their paths from PIN_TEST_DATA.
# Without ffmpeg those tests print SKIP and pass, except when the environment variable CI
# is set: then they fail. -DPIN_TEST_FFMPEG=OFF configures as if there were no ffmpeg.
add_executable(make_ep88 make_ep88.c)

find_program(PIN_TEST_FFMPEG ffmpeg DOC "ffmpeg command-line tool that makes the test fixtures")
if(PIN_TEST_FFMPEG)
    message(STATUS "Test fixtures: made with ${PIN_TEST_FFMPEG}")
    set(_pin_test_ffmpeg "${PIN_TEST_FFMPEG}")
else()
    message(WARNING "ffmpeg (the command-line tool) not found: the tests that need generated "
                    "DV/HDV fixtures will skip, or fail when the environment variable CI is set. "
                    "Install ffmpeg (docs/building.md) and configure again.")
    set(_pin_test_ffmpeg "")
endif()
set(PIN_TEST_DATA "${CMAKE_BINARY_DIR}/tests/data")
set(PIN_TEST_DATA "${PIN_TEST_DATA}" PARENT_SCOPE)
add_test(NAME pin_test_data
    COMMAND "${CMAKE_COMMAND}" "-DFFMPEG=${_pin_test_ffmpeg}" "-DMAKE_EP88=$<TARGET_FILE:make_ep88>"
            "-DOUT=${PIN_TEST_DATA}" -P "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.cmake")
set_tests_properties(pin_test_data PROPERTIES FIXTURES_SETUP pin_test_data TIMEOUT 120)

add_executable(replay_reassembler replay_reassembler.c)
target_link_libraries(replay_reassembler PRIVATE pinnacle_core)

# dv_reassembler end to end: each generated EP 0x88 dump, reassembled, must give back the
# .dv file it was made from, byte for byte (check_reassembler.cmake).
foreach(_pair "pal;dv-pal-32k.dv" "ntsc;dv-ntsc.dv")
    list(GET _pair 0 _std)
    list(GET _pair 1 _src)
    add_test(NAME reassembler_ep88_${_std}
        COMMAND "${CMAKE_COMMAND}"
                -DEXE=$<TARGET_FILE:replay_reassembler>
                -DINPUT=${PIN_TEST_DATA}/ep88-${_std}.bin
                -DSOURCE=${PIN_TEST_DATA}/${_src}
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ep88-${_std}.out
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_reassembler.cmake")
    set_tests_properties(reassembler_ep88_${_std} PROPERTIES FIXTURES_REQUIRED pin_test_data)
endforeach()

# dv_reassembler on tapes that mix formats (PAL / NTSC, DV / HDV, a stray burst, an HDV
# video PID change), from the mixed dumps make_fixtures.cmake makes.
add_executable(test_reassembler_mixed test_reassembler_mixed.c)
target_link_libraries(test_reassembler_mixed PRIVATE pinnacle_core)
add_test(NAME test_reassembler_mixed COMMAND test_reassembler_mixed "${PIN_TEST_DATA}")
set_tests_properties(test_reassembler_mixed PROPERTIES FIXTURES_REQUIRED pin_test_data TIMEOUT 60)

# dv_reassembler on a stream that loses sequences and whole frames in transit (synthetic, no
# fixture files).
add_executable(test_reassembler_loss test_reassembler_loss.c)
target_link_libraries(test_reassembler_loss PRIVATE pinnacle_core)
add_test(NAME test_reassembler_loss COMMAND test_reassembler_loss)
set_tests_properties(test_reassembler_loss PROPERTIES TIMEOUT 60)

# The analog assembler on synthetic packets (includes pinnacle_analog.c for its static functions).
add_executable(test_analog_assembler test_analog_assembler.c)
target_link_libraries(test_analog_assembler PRIVATE pinnacle_core)
add_test(NAME test_analog_assembler COMMAND test_analog_assembler)
set_tests_properties(test_analog_assembler PROPERTIES TIMEOUT 60)

# pinnacle_lock: hardware-free (see tests/test_lock.c), but it does spawn a
# real child process (fork on POSIX, CreateProcess on Windows) to exercise
# cross-process BUSY detection and crash cleanup, so give it more headroom
# than a default ctest timeout before calling it hung.
add_executable(test_lock test_lock.c)
target_link_libraries(test_lock PRIVATE pinnacle_core)
add_test(NAME pinnacle_lock COMMAND test_lock)
set_tests_properties(pinnacle_lock PROPERTIES TIMEOUT 60)

add_executable(test_fx2 test_fx2.c)
target_link_libraries(test_fx2 PRIVATE pinnacle_core)
add_test(NAME pinnacle_fx2 COMMAND test_fx2)

# dv_reassembler on streams that lose arbitrary packets, and on header-like video data
# (synthetic, no fixture files).
add_executable(test_reassembler_holes test_reassembler_holes.c)
target_link_libraries(test_reassembler_holes PRIVATE pinnacle_core)
add_test(NAME test_reassembler_holes COMMAND test_reassembler_holes)
set_tests_properties(test_reassembler_holes PROPERTIES TIMEOUT 300)
