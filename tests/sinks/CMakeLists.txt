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
# Uses the developer's own DV traces, which aren't committed (.gitignore).
if(EXISTS "${PROJECT_SOURCE_DIR}/traces/ep88-sample.bin" AND
   EXISTS "${PROJECT_SOURCE_DIR}/traces/ep88-ntsc-sample.bin")
    add_test(NAME test_dv_rewrap COMMAND test_dv_rewrap
        "${PROJECT_SOURCE_DIR}/traces/ep88-sample.bin"
        "${PROJECT_SOURCE_DIR}/traces/ep88-ntsc-sample.bin")
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
    "${PROJECT_SOURCE_DIR}/captures/20260925-hdv-5min.ts")
