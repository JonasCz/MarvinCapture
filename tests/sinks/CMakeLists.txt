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

add_executable(test_dv_rewrap test_dv_rewrap.c)
target_link_libraries(test_dv_rewrap PRIVATE pinnacle_sinks pinnacle_engine_pure)
# Uses the small local fixtures in tests/data/, which aren't committed (.gitignore).
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/data/ep88-pal.bin" AND
   EXISTS "${PROJECT_SOURCE_DIR}/tests/data/ep88-ntsc.bin")
    add_test(NAME test_dv_rewrap COMMAND test_dv_rewrap
        "${PROJECT_SOURCE_DIR}/tests/data/ep88-pal.bin"
        "${PROJECT_SOURCE_DIR}/tests/data/ep88-ntsc.bin")
endif()

add_executable(test_analog_sinks test_analog_sinks.c)
target_link_libraries(test_analog_sinks PRIVATE pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_analog_sinks PRIVATE m)
endif()
add_test(NAME test_analog_sinks COMMAND test_analog_sinks)

add_executable(test_hdv_rewrap test_hdv_rewrap.c)
target_link_libraries(test_hdv_rewrap PRIVATE pinnacle_sinks)
add_test(NAME test_hdv_rewrap COMMAND test_hdv_rewrap
    "${PROJECT_SOURCE_DIR}/tests/data/hdv.ts")

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
# Linux) and sink_ffv1's compression levels; the DV recordings that are there
add_executable(test_hwenc_sink test_hwenc_sink.c)
target_link_libraries(test_hwenc_sink PRIVATE pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hwenc_sink PRIVATE m)
endif()
set(_pin_hwenc_dv "")
foreach(_f ep88-pal.bin ep88-ntsc.bin dv-pal.dv dv-ntsc.dv)
    if(EXISTS "${PROJECT_SOURCE_DIR}/tests/data/${_f}")
        list(APPEND _pin_hwenc_dv "${PROJECT_SOURCE_DIR}/tests/data/${_f}")
    endif()
endforeach()
add_test(NAME test_hwenc_sink COMMAND test_hwenc_sink ${_pin_hwenc_dv})
set_tests_properties(test_hwenc_sink PROPERTIES TIMEOUT 300)
