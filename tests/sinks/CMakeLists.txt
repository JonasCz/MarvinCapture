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

# Hardware-free ctest coverage for src/sinks. Pulled in by
# src/sinks/CMakeLists.txt (which already has pinnacle_sinks -> pinnacle_core
# -> the minimal static FFmpeg wired up). Each test also needs
# pinnacle_engine_pure for dv_subcode_parse_frame(); that target is only
# defined once src/engine/CMakeLists.txt has run, which the top-level
# CMakeLists.txt add_subdirectory()s before src/sinks.

if(NOT TARGET pinnacle_engine_pure)
    message(FATAL_ERROR "tests/sinks needs pinnacle_engine_pure (src/engine); "
                         "add_subdirectory(src/engine) must run before src/sinks")
endif()

# The tests that read recordings use the ones made at test time by the fixture-setup test
# pin_test_data (tests/CMakeLists.txt, tests/data/README.md) in PIN_TEST_DATA.
add_executable(test_dv_rewrap test_dv_rewrap.c)
target_link_libraries(test_dv_rewrap PRIVATE pinnacle_sinks pinnacle_engine_pure)
add_test(NAME test_dv_rewrap COMMAND test_dv_rewrap
    "${PIN_TEST_DATA}/ep88-pal.bin" "${PIN_TEST_DATA}/ep88-ntsc.bin")
set_tests_properties(test_dv_rewrap PROPERTIES FIXTURES_REQUIRED pin_test_data)

add_executable(test_analog_sinks test_analog_sinks.c)
target_link_libraries(test_analog_sinks PRIVATE pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_analog_sinks PRIVATE m)
endif()
add_test(NAME test_analog_sinks COMMAND test_analog_sinks)

# avi_writer's OpenDML layout over a dozen RIFF segments (a small test segment limit),
# read back through its indexes with libavformat
add_executable(test_avi_odml test_avi_odml.c)
target_link_libraries(test_avi_odml PRIVATE pinnacle_sinks)
add_test(NAME test_avi_odml COMMAND test_avi_odml)

add_executable(test_hdv_rewrap test_hdv_rewrap.c)
target_link_libraries(test_hdv_rewrap PRIVATE pinnacle_sinks)
add_test(NAME test_hdv_rewrap COMMAND test_hdv_rewrap "${PIN_TEST_DATA}/hdv.ts" "${PIN_TEST_DATA}/hdv-720p.ts")
set_tests_properties(test_hdv_rewrap PROPERTIES FIXTURES_REQUIRED pin_test_data)

# HDV to MOV / MKV with repair of damaged GOPs, on streams damaged like tape dropouts
add_executable(test_hdv_repair test_hdv_repair.c)
target_link_libraries(test_hdv_repair PRIVATE pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_repair PRIVATE m)
endif()
add_test(NAME test_hdv_repair COMMAND test_hdv_repair
    "${PIN_TEST_DATA}/hdv-repair.ts" "${PIN_TEST_DATA}/hdv-repair-closed.ts"
    "${PIN_TEST_DATA}/hdv-repair-jump.ts" "${PIN_TEST_DATA}/hdv-720p.ts" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_hdv_repair PROPERTIES FIXTURES_REQUIRED pin_test_data)

# The two shared stages of the HDV pipeline (hdv_demux, hdv_decode) on their own
add_executable(test_hdv_stages test_hdv_stages.c)
target_link_libraries(test_hdv_stages PRIVATE pinnacle_sinks)
add_test(NAME test_hdv_stages COMMAND test_hdv_stages "${PIN_TEST_DATA}/hdv-repair.ts")
set_tests_properties(test_hdv_stages PROPERTIES FIXTURES_REQUIRED pin_test_data)

add_executable(test_stdout_sinks test_stdout_sinks.c)
target_link_libraries(test_stdout_sinks PRIVATE pinnacle_sinks)
add_test(NAME test_stdout_sinks COMMAND test_stdout_sinks)

add_executable(test_lazy_sink test_lazy_sink.c)
target_link_libraries(test_lazy_sink PRIVATE pinnacle_sinks)
add_test(NAME test_lazy_sink COMMAND test_lazy_sink)

# sink_hwenc's pixel conversions
add_executable(test_hwenc_convert test_hwenc_convert.c)
target_link_libraries(test_hwenc_convert PRIVATE pinnacle_sinks)
add_test(NAME test_hwenc_convert COMMAND test_hwenc_convert)

# sink_hwenc end to end (with FFV1 standing in for the encoder where there is none, as on
# Linux) and sink_ffv1's compression levels; DV from an EP 0x88 dump and from a .dv file
add_executable(test_hwenc_sink test_hwenc_sink.c)
target_link_libraries(test_hwenc_sink PRIVATE pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hwenc_sink PRIVATE m)
endif()
add_test(NAME test_hwenc_sink COMMAND test_hwenc_sink
    "${PIN_TEST_DATA}/ep88-pal.bin" "${PIN_TEST_DATA}/dv-ntsc.dv")
set_tests_properties(test_hwenc_sink PROPERTIES TIMEOUT 300 FIXTURES_REQUIRED pin_test_data)
