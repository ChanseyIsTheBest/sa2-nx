#!/bin/bash
# Records the emulated renderer (D3D12, the reference) and the native renderer with the same scripted button presses,
# then builds a side-by-side video and a contact sheet of matching moments.
# Usage: tools/compare_runs.sh <name> <seconds to record> <input script> [extra native cvars...]
#   e.g. tools/compare_runs.sh menu 150 "60:A,66:A,72:A,78:A"
# Output: app/out/pc-me/ref/videos/<name>_emulated.mp4, <name>_native.mp4, <name>_sbs_small.mp4, <name>_sheet.png
set -u
NAME=$1; SECONDS_REC=$2; SCRIPT=$3; shift 3
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/app/out/pc-me/ref/videos"
F=/c/ffmpeg/ffmpeg.exe
mkdir -p "$OUT"
RUN=$((SECONDS_REC + 30))
"$ROOT/tools/run_pc_test.sh" "$RUN" "${NAME}_emu.log" --disable_msaa=false --input_script="$SCRIPT" \
  --banjo_record_s=1 --banjo_record_seconds="$SECONDS_REC" --banjo_record_name="${NAME}_emulated"
"$ROOT/tools/run_pc_test.sh" "$RUN" "${NAME}_nat.log" --masseffect_renderer_native=true --disable_msaa=false \
  --input_script="$SCRIPT" --banjo_record_s=1 --banjo_record_seconds="$SECONDS_REC" \
  --banjo_record_name="${NAME}_native" "$@"
CAP="$ROOT/app/out/pc-me/capturas"
mv -f "$CAP/${NAME}_emulated.mp4" "$CAP/${NAME}_native.mp4" "$OUT/"
"$F" -loglevel error -y -i "$OUT/${NAME}_emulated.mp4" -i "$OUT/${NAME}_native.mp4" \
  -filter_complex "[0]scale=640:360,fps=30[a];[1]scale=640:360,fps=30[b];[a][b]hstack" \
  -c:v h264_nvenc -b:v 750k -maxrate 900k -bufsize 1500k -an "$OUT/${NAME}_sbs_small.mp4"
STEP=$(( SECONDS_REC / 8 ))
"$F" -loglevel error -y -i "$OUT/${NAME}_emulated.mp4" -vf "fps=1/$STEP,scale=320:-1,tile=8x1" -frames:v 1 "$OUT/${NAME}_row_emu.png"
"$F" -loglevel error -y -i "$OUT/${NAME}_native.mp4" -vf "fps=1/$STEP,scale=320:-1,tile=8x1" -frames:v 1 "$OUT/${NAME}_row_nat.png"
"$F" -loglevel error -y -i "$OUT/${NAME}_row_emu.png" -i "$OUT/${NAME}_row_nat.png" -filter_complex vstack \
  "$OUT/${NAME}_sheet.png"
echo "$OUT/${NAME}_sbs_small.mp4 $OUT/${NAME}_sheet.png"
