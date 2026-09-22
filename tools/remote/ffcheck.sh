#!/bin/bash
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
