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

# Hardware-free engine modules: no libusb, no FFmpeg, pure C11 + libc (+ a
# few platform APIs, guarded by #ifdef, for settings paths / atomic save).
# The top-level CMakeLists.txt add_subdirectory()s this file and calls
# enable_testing() itself; this file only adds the tests via add_test().

add_library(pinnacle_engine_pure STATIC
    dv_subcode.c
    dv_audio.c
    hdv_aux.c
    dv_error.c
    hdv_error.c
    pin_estimate.c
    pin_scene.c
    pin_split.c
    pin_naming.c
    pin_settings.c
    pin_cmdline.c
    pin_audio_resample.c
)

target_include_directories(pinnacle_engine_pure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pinnacle_engine_pure PUBLIC c_std_11)

if(WIN32)
    target_link_libraries(pinnacle_engine_pure PUBLIC shell32 ole32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(pinnacle_engine_pure PUBLIC m)
endif()

set(PINNACLE_ENGINE_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/engine)

function(pinnacle_engine_test name)
    add_executable(${name} ${PINNACLE_ENGINE_TEST_DIR}/${name}.c)
    target_link_libraries(${name} PRIVATE pinnacle_engine_pure)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pinnacle_engine_test(test_dv_subcode)
pinnacle_engine_test(test_dv_audio)
pinnacle_engine_test(test_hdv_aux)
pinnacle_engine_test(test_pin_scene)
pinnacle_engine_test(test_pin_split)
pinnacle_engine_test(test_pin_naming)
pinnacle_engine_test(test_pin_settings)
pinnacle_engine_test(test_pin_cmdline)
pinnacle_engine_test(test_pin_audio_resample)
pinnacle_engine_test(test_dv_error)
pinnacle_engine_test(test_hdv_error)
pinnacle_engine_test(test_pin_estimate)
foreach(t test_dv_error test_hdv_error)
    target_compile_definitions(${t} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/data")
endforeach()

# pinnacle_engine itself (pin_session/pin_deck/pin_preview) is defined in the
# top-level CMakeLists.txt, not here: it needs pinnacle_sinks, and
# tests/sinks needs pinnacle_engine_pure (defined above) to already exist,
# so neither add_subdirectory() order (src/engine first or src/sinks first)
# lets both live in the same file without one seeing the other too early.
