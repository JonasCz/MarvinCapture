#!/bin/bash
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.
F=${1:-/home/jonas/long.dv}
echo "=== ffprobe streams ==="
ffprobe -v error -show_entries stream=index,codec_name,width,height,r_frame_rate,sample_rate,channels -of default=noprint_wrappers=1 "$F"
echo
echo "=== full decode (errors only) ==="
ffmpeg -v error -i "$F" -f null - 2> /tmp/fferr.txt
echo "exit=$?"
echo "error lines: $(wc -l < /tmp/fferr.txt)"
head -20 /tmp/fferr.txt
echo
echo "=== frame count per stream ==="
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of default=noprint_wrappers=1 "$F"
