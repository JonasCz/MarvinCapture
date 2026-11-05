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

# Runs replay_reassembler over a raw EP 0x88 dump that make_ep88 made from a .dv file and
# checks the output is byte-identical to that .dv file: both layers of framing (type-9
# messages, OHCI isochronous records) and the junk in front must come off without losing,
# adding or shifting a byte. Invoked by ctest (see tests/CMakeLists.txt) as:
#   cmake -DEXE=<replay_reassembler> -DINPUT=<dump> -DSOURCE=<the .dv it was made from>
#         -DOUTPUT=<scratch file> -P check_reassembler.cmake

if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "replay_reassembler not found at ${EXE}")
endif()
if(NOT EXISTS "${INPUT}" OR NOT EXISTS "${SOURCE}")
    if(NOT "$ENV{CI}" STREQUAL "")
        message(FATAL_ERROR "${INPUT} not present, and CI is set (the fixtures are made with "
                            "ffmpeg at test time, see tests/data/README.md)")
    endif()
    message("SKIP: ${INPUT} not present (made with ffmpeg at test time, see tests/data/README.md)")
    return()
endif()

execute_process(COMMAND "${EXE}" "${INPUT}" "${OUTPUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "replay_reassembler exited with status ${rc}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUTPUT}" "${SOURCE}" RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    file(SIZE "${OUTPUT}" out_size)
    file(SIZE "${SOURCE}" src_size)
    message(FATAL_ERROR "reassembling ${INPUT} did not give back ${SOURCE}:\n"
                        "  output ${out_size} bytes, source ${src_size} bytes")
endif()

message(STATUS "OK: ${INPUT} reassembles to ${SOURCE}, byte for byte")
