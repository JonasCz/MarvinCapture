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

# Hardware-free ctest coverage for the session engine (pin_session/pin_deck/
# pin_preview), driven only through pin_api.h against the replay (virtual
# device) source -- see replay_test_util.h. Pulled in by the top-level
# CMakeLists.txt only once marvin-core exists (needs pinnacle_engine,
# pinnacle_sinks, pinnacle_core already built).

if(NOT TARGET marvin-core)
    message(FATAL_ERROR "tests/engine_replay needs the marvin-core target")
endif()

# The recordings are made at test time by the fixture-setup test pin_test_data
# (tests/CMakeLists.txt, tests/data/README.md) into PIN_TEST_DATA.
set(PIN_TRACE_PAL "${PIN_TEST_DATA}/ep88-pal.bin")    # PAL DV, raw EP 0x88 dump
set(PIN_HDV_CAPTURE "${PIN_TEST_DATA}/hdv.ts")

# On Windows, marvin-core is a DLL; these test executables live one
# directory below it (tests/engine_replay/), so the loader won't find it on
# the default search path without help. Harmless to also add it as
# LD_LIBRARY_PATH on Linux/macOS, where it's normally unnecessary.
#
# ENVIRONMENT_MODIFICATION (not plain ENVIRONMENT) prepends to whatever the
# variable already is at test-run time instead of replacing it outright --
# necessary here because PATH's own value is itself a ';'-separated list, so
# splicing a full copy of it into ENVIRONMENT's own ';'-separated value list
# corrupts the list (silently drops every property after it).
function(pin_engine_replay_test_env name)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:marvin-core>"
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:marvin-core>"
        "DYLD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:marvin-core>")
endfunction()

function(pin_engine_replay_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE marvin-core)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    pin_engine_replay_test_env(${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# 1a: replay capture output is a subset of what dv_reassembler makes of the dump.
pin_engine_replay_test(test_replay_dv_baseline
    $<TARGET_FILE:replay_reassembler> "${PIN_TRACE_PAL}")

# 1b: dv-avi / dv-mov via replay, checked with libavformat.
add_executable(test_replay_dv_formats test_replay_dv_formats.c)
target_link_libraries(test_replay_dv_formats PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_dv_formats COMMAND test_replay_dv_formats "${PIN_TRACE_PAL}")
pin_engine_replay_test_env(test_replay_dv_formats)
set_tests_properties(test_replay_dv_formats PROPERTIES TIMEOUT 30)

# 1c: --passes 2 produces base.dv and base-pass-2.dv.
pin_engine_replay_test(test_replay_multipass "${PIN_TRACE_PAL}")

# 1e: preview-dump equivalent (720x576, chroma shift 1,1, DAR 4:3 for PAL DV).
pin_engine_replay_test(test_replay_preview "${PIN_TRACE_PAL}")

# 1e': the preview's jitter buffer (present_time on an even grid, lock_due).
pin_engine_replay_test(test_replay_preview_pacing "${PIN_TRACE_PAL}")

# 1f: pin_script_run() against the replay device (the command-line sequencer).
pin_engine_replay_test(test_pin_script_run "${PIN_TRACE_PAL}")
set_tests_properties(test_pin_script_run PROPERTIES TIMEOUT 120)

# 1g: pin_check_output() collision detection and free space, plus the
# replay device's serial == "REPLAY" (task 3).
pin_engine_replay_test(test_pin_check_output "${PIN_TRACE_PAL}")

# Task 3/4: HDV .ts replay and HDV scene split (no spurious cuts).
pin_engine_replay_test(test_replay_hdv "${PIN_HDV_CAPTURE}" "${PIN_TEST_DATA}/hdv-720p.ts")

# HDV scene split: a GOP timecode jump starts a new file at the jump's first picture (PAT,
# PMT, sequence header), a stray GOP does not, no picture lost or doubled; both files decode.
add_executable(test_replay_hdv_split test_replay_hdv_split.c)
target_link_libraries(test_replay_hdv_split PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_hdv_split COMMAND test_replay_hdv_split
    "${PIN_TEST_DATA}/hdv-split-a1.ts" "${PIN_TEST_DATA}/hdv-split-x.ts"
    "${PIN_TEST_DATA}/hdv-split-a2.ts" "${PIN_TEST_DATA}/hdv-split-b.ts")
pin_engine_replay_test_env(test_replay_hdv_split)
set_tests_properties(test_replay_hdv_split PROPERTIES TIMEOUT 60)

# Unplug during a capture (replay fakes the USB NO_DEVICE failure): files finalised,
# stop reason DEVICE_LOST, session in ERROR, a new session opens afterwards.
pin_engine_replay_test(test_replay_unplug "${PIN_TEST_DATA}/dv-ntsc.dv" "${PIN_HDV_CAPTURE}")

# the H.264 / HEVC formats through the engine (FFV1 standing in for a missing encoder)
add_executable(test_replay_hwenc test_replay_hwenc.c)
target_link_libraries(test_replay_hwenc PRIVATE marvin-core pinnacle_sinks)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_replay_hwenc PRIVATE m)
endif()
add_test(NAME test_replay_hwenc COMMAND test_replay_hwenc "${PIN_TEST_DATA}/dv-pal.dv")
pin_engine_replay_test_env(test_replay_hwenc)
set_tests_properties(test_replay_hwenc PROPERTIES TIMEOUT 60)

# Tapes that mix formats: PAL -> NTSC as raw DV and 48 -> 32 kHz as DV AVI (two files each,
# libavformat reads the AVIs); DV -> HDV -> DV to files (.dv, .m2t, .dv; as DV AVI each file
# with its own kind's aspect and the title) and to a pipe (ends at the change, with the reason);
# HDV -> DV as HDV MKV and DV AVI.
add_executable(test_replay_mixed_dv test_replay_mixed_dv.c)
target_link_libraries(test_replay_mixed_dv PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_mixed_dv COMMAND test_replay_mixed_dv "${PIN_TEST_DATA}")
pin_engine_replay_test_env(test_replay_mixed_dv)
set_tests_properties(test_replay_mixed_dv PROPERTIES TIMEOUT 60)
# DV -> HDV -> DV (and HDV -> DV) with per-kind aspect and the title: libavformat reads the files.
add_executable(test_replay_mixed_kind test_replay_mixed_kind.c)
target_link_libraries(test_replay_mixed_kind PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_mixed_kind COMMAND test_replay_mixed_kind "${PIN_TEST_DATA}")
pin_engine_replay_test_env(test_replay_mixed_kind)
set_tests_properties(test_replay_mixed_kind PROPERTIES TIMEOUT 90)

# The output drive gets full, fails or is slow (the test seams PIN_TEST_FREE_SPACE,
# PIN_TEST_WRITE_FAIL_AFTER, PIN_TEST_SINK_DELAY_MS, PIN_TEST_WRITER_QUEUE): DISK_FULL,
# WRITE_ERROR, DISK_SLOW, and the file finished (libavformat reads it).
add_executable(test_replay_disk test_replay_disk.c)
target_link_libraries(test_replay_disk PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_disk COMMAND test_replay_disk "${PIN_TEST_DATA}/dv-pal.dv")
pin_engine_replay_test_env(test_replay_disk)
set_tests_properties(test_replay_disk PROPERTIES TIMEOUT 60)

# FAT32 (the seam PIN_TEST_FAT32_LIMIT, with PIN_TEST_WRITE_FAIL_AFTER as the drive's refusal):
# DV and HDV go on in a new file before the limit; raw DV, DV AVI and HDV files all finished.
add_executable(test_replay_fat32 test_replay_fat32.c)
target_link_libraries(test_replay_fat32 PRIVATE marvin-core pinnacle_sinks)
add_test(NAME test_replay_fat32 COMMAND test_replay_fat32 "${PIN_TEST_DATA}/dv-pal.dv" "${PIN_HDV_CAPTURE}")
pin_engine_replay_test_env(test_replay_fat32)
set_tests_properties(test_replay_fat32 PROPERTIES TIMEOUT 60)

# Pipe output end to end (check_cli_pipe.cmake): MarvinCaptureCLI --capture - into ffmpeg for DV
# and HDV, a reader that exits early (PIPE_CLOSED, exit 4) and one that cannot keep up
# (PIPE_SLOW, exit 4).
add_test(NAME test_cli_pipe
    COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:MarvinCaptureCLI>" "-DFFMPEG=${PIN_TEST_FFMPEG}"
            "-DDATA=${PIN_TEST_DATA}" -P "${PROJECT_SOURCE_DIR}/tests/check_cli_pipe.cmake")
set_tests_properties(test_cli_pipe PROPERTIES TIMEOUT 120)

# All of the above replay the generated recordings.
set_tests_properties(test_replay_dv_baseline test_replay_dv_formats test_replay_multipass
    test_replay_preview test_replay_preview_pacing test_pin_script_run test_pin_check_output
    test_replay_hdv test_replay_hdv_split test_replay_unplug test_replay_hwenc
    test_replay_mixed_dv test_replay_mixed_kind test_replay_disk test_replay_fat32 test_cli_pipe
    PROPERTIES FIXTURES_REQUIRED pin_test_data)

# The preview colour matrix every GUI shader relies on, and pin_fit_rect.
pin_engine_replay_test(test_pin_color_matrix)

# dv_audio.c's real SMPTE 314M/IEC 61834 deshuffle, validated sample-for-
# sample against FFmpeg's own "dv" demuxer: on the generated NTSC 48 kHz and
# PAL 32 kHz files, and on a camera's recording with 32 kHz 12-bit audio when
# tests/data/dv-ntsc-32k.dv is there (optional: ffmpeg cannot write 12-bit DV
# audio). Links pinnacle_engine_pure directly (for dv_audio_extract()) and
# pinnacle_sinks (for libavformat/libavcodec, already wired up there) rather
# than marvin-core: dv_audio_extract() isn't part of the public pin_api.h
# ABI, so it isn't exported from that DLL on Windows.
if(TARGET pinnacle_sinks AND TARGET pinnacle_engine_pure)
    add_executable(test_dv_audio_vs_libavformat test_dv_audio_vs_libavformat.c)
    target_link_libraries(test_dv_audio_vs_libavformat PRIVATE pinnacle_sinks pinnacle_engine_pure)
    add_test(NAME test_dv_audio_vs_libavformat COMMAND test_dv_audio_vs_libavformat
        "${PIN_TEST_DATA}/dv-ntsc.dv" "${PIN_TEST_DATA}/dv-pal-32k.dv"
        --optional "${PROJECT_SOURCE_DIR}/tests/data/dv-ntsc-32k.dv")
    set_tests_properties(test_dv_audio_vs_libavformat PROPERTIES TIMEOUT 120
        FIXTURES_REQUIRED pin_test_data)
endif()

# Task: pin_devices_wait()/pin_devices_wake() and pin_set_replay_file()
# replacing the old settings-key replay mechanism.
add_executable(test_pin_devices_wait test_pin_devices_wait.c)
target_link_libraries(test_pin_devices_wait PRIVATE marvin-core Threads::Threads)
add_test(NAME test_pin_devices_wait COMMAND test_pin_devices_wait)
pin_engine_replay_test_env(test_pin_devices_wait)
set_tests_properties(test_pin_devices_wait PROPERTIES TIMEOUT 30)
