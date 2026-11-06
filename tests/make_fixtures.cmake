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

set(files dv-pal.dv dv-ntsc.dv dv-pal-32k.dv hdv.ts hdv-720p.ts hdv-split-a1.ts hdv-split-x.ts
          hdv-split-a2.ts hdv-split-b.ts hdv-repair.ts hdv-repair-closed.ts hdv-repair-jump.ts
          ep88-pal.bin ep88-ntsc.bin ep88-pal-ntsc.bin
          ep88-dv-hdv-dv.bin ep88-dv-stray.bin ep88-hdv-pid.bin ep88-pal-48k-32k.bin ep88-hdv-dv.bin)

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
# HDV 720p-like MPEG-2 TS, 0.3 s: 1280x720 progressive, 59.94 fps, 16:9 (square samples),
# 6-picture GOPs, MP2 audio; ffmpeg's own PIDs (PMT 0x1000, video 0x100, audio 0x101).
ffmpeg(hdv-720p.ts -f lavfi -i testsrc2=size=1280x720:rate=60000/1001:duration=0.3
       -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=0.35
       -pix_fmt yuv420p -c:v mpeg2video -b:v 6M -maxrate 6M -bufsize 2000000 -g 6 -bf 2 -aspect 16:9
       -color_primaries bt709 -color_trc bt709 -colorspace bt709 -timecode 00:10:00:00
       -c:a mp2 -b:a 384k -ac 2 -f mpegts)
# HDV scene split (test_replay_hdv_split puts them back to back, one tape): pieces encoded as
# hdv.ts, each with its own GOP timecode, the clock running on from one to the next.
#   a1  34 pictures from 01:00:00:00 (12-picture GOPs)
#   x   one 12-picture GOP with a stray timecode, 03:00:00:00 (must not split)
#   a2  36 pictures from 01:00:01:21, where a1's timecode would be after a1 and x
#   b   48 pictures from 05:00:00:00: the next scene, a new file from its first picture
function(hdv_split_piece out video_s audio_s timecode offset gop)
    ffmpeg(${out} -f lavfi -i testsrc2=size=1440x1080:rate=25:duration=${video_s}
           -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=${audio_s}
           -vf setfield=tff -pix_fmt yuv420p -c:v mpeg2video -b:v 25M -maxrate 25M -bufsize 7340032
           -g ${gop} -bf 2 -flags +ilme+ildct -aspect 16:9
           -color_primaries bt709 -color_trc bt709 -colorspace bt709 -timecode ${timecode}
           -c:a mp2 -b:a 384k -ac 2
           -mpegts_pmt_start_pid 0x81 -streamid 0:0x810 -streamid 1:0x814
           -output_ts_offset ${offset} -f mpegts)
endfunction()
hdv_split_piece(hdv-split-a1.ts 1.34 1.36 01:00:00:00 834 12)
hdv_split_piece(hdv-split-x.ts 0.46 0.48 03:00:00:00 835.36 100)
hdv_split_piece(hdv-split-a2.ts 1.42 1.44 01:00:01:21 835.84 12)
hdv_split_piece(hdv-split-b.ts 1.90 1.92 05:00:00:00 837.28 12)
# HDV repair (test_hdv_repair cuts, garbles and joins them): as hdv.ts, longer, with a 997 Hz
# tone (a whole number of 1 kHz periods fits in an MP2 frame, so a 1 kHz tone a frame early or
# late would look right).
#   repair         6 s, open GOPs (leading B pictures, as ffmpeg writes them)
#   repair-closed  3 s, closed GOPs
#   repair-jump    1.5 s, its clock at 5000 s, its timecode where repair-closed's ends (joined
#                  after it: a jump of the camera clock alone, as when a camera pauses)
function(hdv_repair_piece out seconds offset timecode flags)   # ARGN: more encoder options
    ffmpeg(${out} -f lavfi -i testsrc2=size=1440x1080:rate=25:duration=${seconds}
           -f lavfi -i sine=frequency=997:sample_rate=48000:duration=${seconds}
           -vf setfield=tff -pix_fmt yuv420p -c:v mpeg2video -b:v 25M -maxrate 25M -bufsize 7340032
           -g 12 -bf 2 -flags ${flags} ${ARGN} -aspect 16:9
           -color_primaries bt709 -color_trc bt709 -colorspace bt709 -timecode ${timecode}
           -c:a mp2 -b:a 384k -ac 2
           -mpegts_pmt_start_pid 0x81 -streamid 0:0x810 -streamid 1:0x814
           -output_ts_offset ${offset} -f mpegts)
endfunction()
hdv_repair_piece(hdv-repair.ts 6 834 00:20:00:00 +ilme+ildct)
hdv_repair_piece(hdv-repair-closed.ts 3 900 00:20:00:00 +ilme+ildct+cgop -sc_threshold 1000000000)
hdv_repair_piece(hdv-repair-jump.ts 1.5 5000 00:20:03:00 +ilme+ildct)
foreach(ts hdv.ts hdv-720p.ts hdv-split-a1.ts hdv-split-x.ts hdv-split-a2.ts hdv-split-b.ts
           hdv-repair.ts hdv-repair-closed.ts hdv-repair-jump.ts)
    file(SIZE "${OUT}/${ts}" ts_size)
    math(EXPR ts_rest "${ts_size} % 188")
    if(ts_size EQUAL 0 OR NOT ts_rest EQUAL 0)
        message(FATAL_ERROR "${OUT}/${ts} is ${ts_size} bytes, not whole TS packets")
    endif()
endforeach()

# Raw EP 0x88 dumps of two of the DV files, as the device would deliver them
make_ep88(ep88-pal.bin dv-pal-32k.dv)
make_ep88(ep88-ntsc.bin dv-ntsc.dv)
# Tapes that mix formats (test_reassembler_mixed, test_replay_mixed_*), one stream each:
#   pal-ntsc      PAL (50 frames) then NTSC (30)
#   dv-hdv-dv     PAL DV (14 frames), HDV 1080i (50 pictures), NTSC DV (30 frames)
#   dv-stray      PAL DV, a burst of 300 HDV TS packets (too short to switch), PAL DV
#   hdv-pid       HDV 1080i on video PID 0x810, then 720p on PID 0x100 (another PMT too)
#   pal-48k-32k   PAL DV with 48 kHz audio (50 frames), then 32 kHz (14)
#   hdv-dv        HDV 1080i (50 pictures), then PAL DV (50 frames): a capture that starts on HDV
make_ep88(ep88-pal-ntsc.bin dv-pal.dv dv-ntsc.dv)
make_ep88(ep88-dv-hdv-dv.bin dv-pal-32k.dv hdv.ts dv-ntsc.dv)
make_ep88(ep88-dv-stray.bin dv-pal-32k.dv hdv.ts@300 dv-pal-32k.dv)
make_ep88(ep88-hdv-pid.bin hdv.ts hdv-720p.ts)
make_ep88(ep88-pal-48k-32k.bin dv-pal.dv dv-pal-32k.dv)
make_ep88(ep88-hdv-dv.bin hdv.ts dv-pal.dv)

file(WRITE "${stamp}" "${key}")
message(STATUS "Test fixtures made in ${OUT}")
