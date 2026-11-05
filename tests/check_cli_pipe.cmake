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

# Pipe output end to end: MarvinCaptureCLI --capture - from the replay device into ffmpeg,
# the way a user pipes it (docs/cli.md). execute_process() connects the commands with a pipe on
# every platform, no shell needed. Checks:
#   - DV: ffmpeg decodes every frame it gets (whole 720x576 frames), at least the 1 s waited
#     for; both programs exit 0;
#   - HDV: the same for the MPEG-TS (1440x1080 pictures, from a GOP on);
#   - a reader that exits early (ffmpeg -frames:v 5): the capture stops, "the program reading
#     the output exited", exit 4;
#   - a reader that cannot keep up (the test seams PIN_TEST_SINK_DELAY_MS and
#     PIN_TEST_WRITER_QUEUE make the queue in front of the pipe small and slow): "can't keep up",
#     exit 4, and ffmpeg still reads a clean stream.
# Invoked by ctest (tests/engine_replay/CMakeLists.txt) as:
#   cmake -DCLI=<MarvinCaptureCLI> -DFFMPEG=<ffmpeg, empty if none> -DDATA=<fixture dir>
#         -P check_cli_pipe.cmake

function(skip_or_fail why)
    if(NOT "$ENV{CI}" STREQUAL "")
        message(FATAL_ERROR "${why}, and CI is set (the fixtures are made with ffmpeg at test "
                            "time, see tests/data/README.md)")
    endif()
    message("SKIP: ${why} (see tests/data/README.md)")
endfunction()

if(NOT FFMPEG)
    skip_or_fail("no ffmpeg")
    return()
endif()
foreach(f dv-pal.dv hdv.ts)
    if(NOT EXISTS "${DATA}/${f}")
        skip_or_fail("${DATA}/${f} not present")
        return()
    endif()
endforeach()

set(ff "${FFMPEG}" -hide_banner -nostdin -v error)

# Video frames ffmpeg decoded (lines of stream 0 in its framecrc output), and how many of them
# had size bytes.
function(count_frames out size n_var n_size_var)
    string(REGEX MATCHALL "\n0,[^\n]*" lines "${out}")
    list(LENGTH lines n)
    set(n_size 0)
    foreach(l IN LISTS lines)
        if(l MATCHES ",[ ]*${size},")
            math(EXPR n_size "${n_size} + 1")
        endif()
    endforeach()
    set(${n_var} ${n} PARENT_SCOPE)
    set(${n_size_var} ${n_size} PARENT_SCOPE)
endfunction()

# DV: 1 s (25 frames) to stdout, decoded by ffmpeg
execute_process(
    COMMAND "${CLI}" -d "${DATA}/dv-pal.dv" --capture - --wait captured=00:00:01
    COMMAND ${ff} -f dv -i - -map 0:v -f framecrc -
    RESULTS_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
count_frames("${out}" 622080 n n_whole)
if(NOT rc STREQUAL "0;0" OR n LESS 25 OR n GREATER 50 OR NOT n EQUAL n_whole)
    message(FATAL_ERROR "DV to a pipe: exit codes ${rc} (want 0;0), ${n} frame(s) decoded "
                        "(want 25-50), ${n_whole} of them whole\n${err}")
endif()
message(STATUS "OK: DV to a pipe, ffmpeg decoded ${n} frames")

# HDV: the MPEG-TS, from the first GOP on
execute_process(
    COMMAND "${CLI}" -d "${DATA}/hdv.ts" --capture - --wait captured=00:00:01
    COMMAND ${ff} -f mpegts -i - -map 0:v -f framecrc -
    RESULTS_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
count_frames("${out}" 2332800 n n_whole)
if(NOT rc STREQUAL "0;0" OR n LESS 12 OR NOT n EQUAL n_whole)
    message(FATAL_ERROR "HDV to a pipe: exit codes ${rc} (want 0;0), ${n} picture(s) decoded "
                        "(want 12 or more), ${n_whole} of them whole\n${err}")
endif()
message(STATUS "OK: HDV to a pipe, ffmpeg decoded ${n} pictures")

# The reader exits after 5 frames, long before the replay file's end (2 s): PIPE_CLOSED, exit 4.
# (The wallclock bound only keeps a failure from hanging: a capture that ends normally first
# leaves the captured= wait unmet.)
execute_process(
    COMMAND "${CLI}" -d "${DATA}/dv-pal.dv" --capture - --wait-any captured=00:01:00,wallclock=00:00:10
    COMMAND ${ff} -probesize 150000 -analyzeduration 0 -f dv -i - -frames:v 5 -f null -
    RESULTS_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
if(NOT rc STREQUAL "4;0" OR NOT err MATCHES "the program reading the output exited")
    message(FATAL_ERROR "a reader that exits: exit codes ${rc} (want 4;0)\n${err}")
endif()
message(STATUS "OK: a reader that exits ends the capture, exit 4")

# The reader cannot keep up: a 1 MB queue drained at 5 frames a second fills at once.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env PIN_TEST_SINK_DELAY_MS=200 PIN_TEST_WRITER_QUEUE=1000000
            "${CLI}" -d "${DATA}/dv-pal.dv" --capture - --wait captured=00:00:01
    COMMAND ${ff} -f dv -i - -map 0:v -f framecrc -
    RESULTS_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
count_frames("${out}" 622080 n n_whole)
if(NOT rc STREQUAL "4;0" OR NOT err MATCHES "can't keep up" OR NOT n EQUAL n_whole)
    message(FATAL_ERROR "a slow reader: exit codes ${rc} (want 4;0), ${n} frame(s) decoded, "
                        "${n_whole} of them whole\n${err}")
endif()
message(STATUS "OK: a reader that cannot keep up ends the capture, exit 4 (${n} frames went out)")
