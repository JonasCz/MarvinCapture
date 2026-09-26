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

add_executable(replay_reassembler replay_reassembler.c)
target_link_libraries(replay_reassembler PRIVATE pinnacle_core)

# One ctest per trace sample in tests/baseline_sha256.txt, each running
# replay_reassembler over that sample and checking the output's sha256
# against the frozen Phase-1 baseline (see check_baseline.cmake). This is
# the regression net for the dv_reassembler refactor: whatever changes
# around it, this output must stay byte-identical.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/baseline_sha256.txt" baseline_lines)
foreach(line IN LISTS baseline_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    separate_arguments(fields UNIX_COMMAND "${line}")
    list(GET fields 0 sample_name)
    list(GET fields 1 sample_hash)

    set(sample_path "${PROJECT_SOURCE_DIR}/traces/${sample_name}")
    # The traces are the developer's own recordings and aren't committed
    # (.gitignore); without them there's nothing to compare against.
    if(NOT EXISTS "${sample_path}")
        message(STATUS "traces/${sample_name} not present: skipping its baseline test")
        continue()
    endif()
    set(out_path "${CMAKE_CURRENT_BINARY_DIR}/${sample_name}.out")

    add_test(
        NAME "reassembler_baseline_${sample_name}"
        COMMAND "${CMAKE_COMMAND}"
                -DEXE=$<TARGET_FILE:replay_reassembler>
                -DINPUT=${sample_path}
                -DOUTPUT=${out_path}
                -DEXPECTED=${sample_hash}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_baseline.cmake"
    )
endforeach()

# pinnacle_lock: hardware-free (see tests/test_lock.c), but it does spawn a
# real child process (fork on POSIX, CreateProcess on Windows) to exercise
# cross-process BUSY detection and crash cleanup, so give it more headroom
# than a default ctest timeout before calling it hung.
add_executable(test_lock test_lock.c)
target_link_libraries(test_lock PRIVATE pinnacle_core)
add_test(NAME pinnacle_lock COMMAND test_lock)
set_tests_properties(pinnacle_lock PROPERTIES TIMEOUT 60)
