#!/bin/bash
# Turns a raw recording (banjo_video_s) into an MP4 and a contact sheet of consecutive frames.
# Usage: tools/make_video.sh <capturas/video_Ns.rgba> [frames in the sheet, default 16] [step between them, default 1]
set -eu
RAW=$1; N=${2:-16}; STEP=${3:-1}
FFMPEG=${FFMPEG:-/c/ffmpeg/ffmpeg.exe}
read W H FPS COUNT < "${RAW%.rgba}.txt"
OUT="${RAW%.rgba}"
"$FFMPEG" -loglevel error -y -f rawvideo -pix_fmt rgb0 -s "${W}x${H}" -r "$FPS" -i "$RAW" \
  -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p "$OUT.mp4"
COLS=4; ROWS=$(( (N + COLS - 1) / COLS ))
"$FFMPEG" -loglevel error -y -f rawvideo -pix_fmt rgb0 -s "${W}x${H}" -r "$FPS" -i "$RAW" \
  -vf "select='not(mod(n\,$STEP))',scale=480:-1,drawtext=text='%{n}':x=8:y=8:fontsize=24:fontcolor=yellow:box=1:boxcolor=black@0.6,tile=${COLS}x${ROWS}" \
  -frames:v 1 "$OUT.sheet.png" 2>/dev/null || \
"$FFMPEG" -loglevel error -y -f rawvideo -pix_fmt rgb0 -s "${W}x${H}" -r "$FPS" -i "$RAW" \
  -vf "select='not(mod(n\,$STEP))',scale=480:-1,tile=${COLS}x${ROWS}" -frames:v 1 "$OUT.sheet.png"
echo "$OUT.mp4 ($COUNT frames at $FPS fps), sheet $OUT.sheet.png"
