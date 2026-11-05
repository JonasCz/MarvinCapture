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

# Makes the synthetic test fixtures (tests/data/README.md) with the ffmpeg command-line
# tool and make_ep88 (tests/make_ep88.c) into the build tree. ctest runs it as the
# fixture-setup test pin_test_data (tests/CMakeLists.txt) before any test that needs them:
#   cmake -DFFMPEG=<ffmpeg, empty if none> -DMAKE_EP88=<make_ep88> -DOUT=<dir> -P make_fixtures.cmake
#
# Without ffmpeg it only says so and the tests that need the files skip themselves, unless
# the environment variable CI is set (GitHub Actions sets CI=true): then it fails, so a CI
# run cannot pass by skipping. The files are made again only when this script, make_ep88
# or the ffmpeg path changed (OUT/fixtures.stamp).

if(NOT FFMPEG)
    if(NOT "$ENV{CI}" STREQUAL "")
        message(FATAL_ERROR "ffmpeg was not found when this build was configured, and CI is set: the "
                            "tests that need generated fixtures must not skip. Install ffmpeg "
                            "(docs/building.md) and configure again.")
    endif()
    message("SKIP: ffmpeg was not found when this build was configured: no generated fixtures, "
            "the tests that need them skip (tests/data/README.md)")
    return()
endif()

set(files dv-pal.dv dv-ntsc.dv dv-pal-32k.dv hdv.ts ep88-pal.bin ep88-ntsc.bin)

file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" script_hash)
file(SHA256 "${MAKE_EP88}" tool_hash)
set(key "${FFMPEG} ${script_hash} ${tool_hash}")
set(stamp "${OUT}/fixtures.stamp")
set(complete 1)
foreach(f IN LISTS files)
    if(NOT EXISTS "${OUT}/${f}")
        set(complete 0)
    endif()
endforeach()
if(complete AND EXISTS "${stamp}")
    file(READ "${stamp}" old_key)
    if(old_key STREQUAL key)
        message(STATUS "Test fixtures in ${OUT} are up to date")
        return()
    endif()
endif()
file(REMOVE "${stamp}")
file(MAKE_DIRECTORY "${OUT}")

function(ffmpeg out)
    execute_process(COMMAND "${FFMPEG}" -hide_banner -nostdin -loglevel error -y ${ARGN} "${OUT}/${out}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${FFMPEG} failed (${rc}) making ${OUT}/${out}")
    endif()
endfunction()

function(make_ep88 out)
    set(inputs "")
    foreach(f IN LISTS ARGN)
        list(APPEND inputs "${OUT}/${f}")
    endforeach()
    execute_process(COMMAND "${MAKE_EP88}" "${OUT}/${out}" ${inputs} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "make_ep88 failed (${rc}) making ${OUT}/${out}")
    endif()
endfunction()

# Checks a file came out the size the tests count on (another ffmpeg version could cut a
# frame differently).
function(expect_size f size)
    file(SIZE "${OUT}/${f}" got)
    if(NOT got EQUAL size)
        message(FATAL_ERROR "${OUT}/${f} is ${got} bytes, expected ${size}: check its ffmpeg command")
    endif()
endfunction()

# DV: a moving test pattern and a tone, with a fixed recording date/time and timecode so the
# bytes are the same every time. Each length is set by the video source's duration, a little
# short of the next frame (the audio runs a little longer, so the last frame has its audio).
# ffmpeg's DV muxer writes 16-bit stereo audio only, at 48 kHz, or 32 kHz at 25 fps.
set(dv -c:v dvvideo -c:a pcm_s16le -ac 2 -metadata creation_time=2026-01-01T12:00:00Z
       -timecode 00:10:00:00 -f dv)
# PAL, 48 kHz, 50 frames (2 s)
ffmpeg(dv-pal.dv -f lavfi -i testsrc2=size=720x576:rate=25:duration=1.98
       -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=2.1 -pix_fmt yuv420p ${dv})
expect_size(dv-pal.dv 7200000)
# NTSC, 48 kHz, 30 frames (1 s): 3,600,000 bytes, a whole number of PAL frames too, so
# only the DSF bit tells it is NTSC
ffmpeg(dv-ntsc.dv -f lavfi -i testsrc2=size=720x480:rate=30000/1001:duration=0.99
       -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1.1 -pix_fmt yuv411p ${dv})
expect_size(dv-ntsc.dv 3600000)
# PAL, 32 kHz, 14 frames (0.56 s): the source of ep88-pal.bin, short like a 2 MiB dump of
# a real camera (the replay tests count on its end coming soon)
ffmpeg(dv-pal-32k.dv -f lavfi -i testsrc2=size=720x576:rate=25:duration=0.55
       -f lavfi -i sine=frequency=1000:sample_rate=32000:duration=0.65 -pix_fmt yuv420p ${dv})
expect_size(dv-pal-32k.dv 2016000)

# HDV-like MPEG-2 TS, 2 s: 1440x1080 interlaced top field first, 25 fps, 12-picture GOPs
# with a GOP timecode, BT.709, MP2 audio, and the PIDs of a real camera (PMT 0x81, video
# 0x810, audio 0x814); its clock starts at 834 s, as a camera's does, not at 0.
# What ffmpeg cannot make: the camera's aux stream on PID 0x811 (recording date/time).
ffmpeg(hdv.ts -f lavfi -i testsrc2=size=1440x1080:rate=25:duration=1.98
       -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=2
       -vf setfield=tff -pix_fmt yuv420p -c:v mpeg2video -b:v 25M -maxrate 25M -bufsize 7340032
       -g 12 -bf 2 -flags +ilme+ildct -aspect 16:9
       -color_primaries bt709 -color_trc bt709 -colorspace bt709 -timecode 00:10:00:00
       -c:a mp2 -b:a 384k -ac 2
       -mpegts_pmt_start_pid 0x81 -streamid 0:0x810 -streamid 1:0x814 -output_ts_offset 834 -f mpegts)
file(SIZE "${OUT}/hdv.ts" ts_size)
math(EXPR ts_rest "${ts_size} % 188")
if(ts_size EQUAL 0 OR NOT ts_rest EQUAL 0)
    message(FATAL_ERROR "${OUT}/hdv.ts is ${ts_size} bytes, not whole TS packets")
endif()

# Raw EP 0x88 dumps of two of the DV files, as the device would deliver them
make_ep88(ep88-pal.bin dv-pal-32k.dv)
make_ep88(ep88-ntsc.bin dv-ntsc.dv)

file(WRITE "${stamp}" "${key}")
message(STATUS "Test fixtures made in ${OUT}")
