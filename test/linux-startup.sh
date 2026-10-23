#!/usr/bin/env bash
set -euo pipefail
APPIMAGE_EXTRACT_AND_RUN=1 "$1" > /tmp/nordssh-startup.log 2>&1 &
app_pid=$!
trap 'kill "$app_pid" 2>/dev/null || true; wait "$app_pid" 2>/dev/null || true' EXIT
for i in $(seq 1 30); do
  if ! kill -0 "$app_pid" 2>/dev/null; then cat /tmp/nordssh-startup.log; exit 1; fi
  if xdotool search --name '^NordSSH$' >/dev/null 2>&1; then
    echo 'PASS: Linux AppImage created a native NordSSH window under Xvfb (not a full UI test).'
    exit 0
  fi
  sleep 1
done
cat /tmp/nordssh-startup.log
exit 1
