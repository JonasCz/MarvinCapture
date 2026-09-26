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
# CMakeLists.txt only once pinnacle-oss-core exists (needs pinnacle_engine,
# pinnacle_sinks, pinnacle_core already built).

if(NOT TARGET pinnacle-oss-core)
    message(FATAL_ERROR "tests/engine_replay needs the pinnacle-oss-core target")
endif()

set(PIN_TRACES_DIR "${PROJECT_SOURCE_DIR}/traces")
set(PIN_TRACE_PAL "${PIN_TRACES_DIR}/ep88-sample.bin")           # PAL DV
set(PIN_TRACE_NTSC "${PIN_TRACES_DIR}/ep88-ntsc-sample.bin")     # NTSC DV
set(PIN_HDV_CAPTURE "${PROJECT_SOURCE_DIR}/captures/20260925-hdv-5min.ts")

# On Windows, pinnacle-oss-core is a DLL; these test executables live one
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
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:pinnacle-oss-core>"
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:pinnacle-oss-core>"
        "DYLD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:pinnacle-oss-core>")
endfunction()

function(pin_engine_replay_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE pinnacle-oss-core)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    pin_engine_replay_test_env(${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# The PAL trace is the developer's own recording and isn't committed
# (.gitignore); the tests replaying it only exist when it's there.
if(EXISTS "${PIN_TRACE_PAL}")
    # 1a: replay capture output is a subset of the frozen Phase-1 baseline.
    pin_engine_replay_test(test_replay_dv_baseline
        $<TARGET_FILE:replay_reassembler> "${PIN_TRACE_PAL}")

    # 1b: dv-avi / dv-mov via replay, checked with libavformat.
    add_executable(test_replay_dv_formats test_replay_dv_formats.c)
    target_link_libraries(test_replay_dv_formats PRIVATE pinnacle-oss-core pinnacle_sinks)
    add_test(NAME test_replay_dv_formats COMMAND test_replay_dv_formats "${PIN_TRACE_PAL}")
    pin_engine_replay_test_env(test_replay_dv_formats)
    set_tests_properties(test_replay_dv_formats PROPERTIES TIMEOUT 30)

    # 1c: --passes 2 produces base.dv and base-pass-2.dv.
    pin_engine_replay_test(test_replay_multipass "${PIN_TRACE_PAL}")
endif()

# 1d: synthetic scene split (persistent jump + single-frame glitch).
add_executable(test_replay_scene_split test_replay_scene_split.c)
target_link_libraries(test_replay_scene_split PRIVATE pinnacle-oss-core pinnacle_engine_pure)
add_test(NAME test_replay_scene_split COMMAND test_replay_scene_split)
pin_engine_replay_test_env(test_replay_scene_split)
set_tests_properties(test_replay_scene_split PROPERTIES TIMEOUT 30)

# 1e: preview-dump equivalent (720x576, chroma shift 1,1, DAR 4:3 for PAL DV).
if(EXISTS "${PIN_TRACE_PAL}")
    pin_engine_replay_test(test_replay_preview "${PIN_TRACE_PAL}")
endif()

# 1f: pin_launch_parse(), no session/replay involved.
pin_engine_replay_test(test_pin_launch_parse)

# 1g: pin_check_output() collision detection and free space, plus the
# replay device's serial == "REPLAY" (task 3).
if(EXISTS "${PIN_TRACE_PAL}")
    pin_engine_replay_test(test_pin_check_output "${PIN_TRACE_PAL}")
endif()

# Task 3/4: HDV .ts replay and HDV scene split (no spurious cuts). Skips
# gracefully (exit 0) if the real capture isn't present -- see the test's
# own header comment and captures/README.md.
pin_engine_replay_test(test_replay_hdv "${PIN_HDV_CAPTURE}")

# The preview colour matrix every GUI shader relies on, and pin_fit_rect.
pin_engine_replay_test(test_pin_color_matrix)
