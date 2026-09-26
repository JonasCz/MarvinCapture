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

# Runs replay_reassembler over one trace sample and checks its output's
# sha256 against the frozen Phase-1 baseline. Invoked by ctest (see
# tests/CMakeLists.txt) as:
#   cmake -DEXE=<replay_reassembler> -DINPUT=<trace file> -DOUTPUT=<scratch file>
#         -DEXPECTED=<sha256> -P check_baseline.cmake

if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "replay_reassembler not found at ${EXE}")
endif()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "trace sample not found at ${INPUT} -- tests/data/ep88-*.bin are "
                         "gitignored (large recordings); see tests/data/README.md")
endif()

execute_process(
    COMMAND "${EXE}" "${INPUT}" "${OUTPUT}"
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "replay_reassembler exited with status ${rc}")
endif()

file(SHA256 "${OUTPUT}" actual)
if(NOT actual STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "output for ${INPUT} is not byte-identical to the Phase-1 "
                         "baseline\n  expected sha256 ${EXPECTED}\n  got      sha256 ${actual}")
endif()

message(STATUS "OK: ${INPUT} -> ${actual}")
