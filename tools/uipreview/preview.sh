#!/usr/bin/env bash
# Renders the panel preview under Wine inside Xvfb and saves screenshots.
#   tools/uipreview/preview.sh start                 launch the preview window
#   tools/uipreview/preview.sh ctl "tab=3 mx=300 my=200 down=0 click=0"
#   tools/uipreview/preview.sh shot OUT.png
#   tools/uipreview/preview.sh stop
set -euo pipefail
export DISPLAY="${PREVIEW_DISPLAY:-:97}"
export WINEPREFIX="${PREVIEW_PREFIX:-$HOME/.cache/ts2fx-prefix}"
export WINEDEBUG=-all
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
case "${1:-}" in
  start)
    pgrep -f "Xvfb $DISPLAY" >/dev/null || { setsid nohup Xvfb "$DISPLAY" -screen 0 1100x760x24 +extension GLX -nolisten tcp >/tmp/uipreview-xvfb.log 2>&1 </dev/null & sleep 2; }
    echo "tab=3 mx=-100 my=-100" > /tmp/uipreview.ctl
    setsid nohup wine "$ROOT/build/uipreview.exe" >/tmp/uipreview.out 2>&1 </dev/null &
    sleep "${2:-8}"
    ;;
  ctl) echo "$2" > /tmp/uipreview.ctl ;;
  shot) sleep 1; magick import -display "$DISPLAY" -window root "$2" ;;
  stop) echo "exit=1" > /tmp/uipreview.ctl; sleep 1; pkill -f "Xvfb $DISPLAY" || true ;;
  *) sed -n 2,6p "$0" ;;
esac
