#!/bin/bash
# Automated PC test run: minimized window, muted audio, fresh log (--log_file appends otherwise).
# Usage: tools/run_pc_test.sh <seconds> <log name> [extra --cvar=value ...]
# Runs in app/out/pc-me; the log lands there. Exit status 3 = the run hit an unhandled guest access violation.
set -u
DIR="$(cd "$(dirname "$0")/../app/out/pc-me" && pwd)"
SECONDS_RUN=$1; LOG=$2; shift 2
cd "$DIR" || exit 1
rm -f "$LOG"
timeout "$SECONDS_RUN" ./renut.exe --log_file="$LOG" --audio_mute=true --window_start_minimized=true "$@" >/dev/null 2>&1
if grep -q "access violation" "$LOG" 2>/dev/null; then exit 3; fi
exit 0
