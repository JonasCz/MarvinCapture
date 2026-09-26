#!/bin/bash
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.
#
# Checks a pinanalog AVI: streams, a full decode, video frames against audio
# samples (1920 per PAL frame when nothing was lost), audio levels, and a
# still at the given time for a visual check.
F=${1:?usage: avicheck.sh file.avi [still-time-seconds]}
T=${2:-5}
echo "=== streams ==="
ffprobe -v error -show_entries stream=index,codec_name,codec_tag_string,width,height,pix_fmt,r_frame_rate,sample_rate,channels,duration -of compact "$F"
echo "=== full decode (errors only) ==="
ffmpeg -v error -i "$F" -f null - 2> /tmp/avierr.txt
echo "exit=$? error lines: $(wc -l < /tmp/avierr.txt)"
head -5 /tmp/avierr.txt
echo "=== counts ==="
V=$(ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$F")
A=$(ffmpeg -v error -i "$F" -map 0:a -f s16le - | wc -c)
echo "video frames: $V"
echo "audio samples: $((A / 4))  ($(python3 -c "print(f'{$A/4/$V:.4f}')" 2>/dev/null) per frame)"
echo "=== audio levels ==="
ffmpeg -v info -i "$F" -map 0:a -af volumedetect -f null - 2>&1 | grep -E 'mean_volume|max_volume'
echo "=== still at ${T}s -> ${F%.avi}-still.png ==="
ffmpeg -v error -y -ss "$T" -i "$F" -frames:v 1 "${F%.avi}-still.png" && ls -la "${F%.avi}-still.png"
