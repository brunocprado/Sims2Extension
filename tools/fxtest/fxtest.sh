#!/usr/bin/env bash
# Test harness for the post-fx plugin: runs the real game under Wine inside a private Xvfb display.
#
#   tools/fxtest/fxtest.sh install            back up the game's current ijl15.dll once, install build/ijl15.dll
#   tools/fxtest/fxtest.sh restore            put the user's original ijl15.dll back
#   tools/fxtest/fxtest.sh start              boot game (detached) and navigate menus into a lot
#   tools/fxtest/fxtest.sh shot OUT.png       full resolution screenshot of the game display
#   tools/fxtest/fxtest.sh cfg "k=v" ...      rewrite ts2fx.cfg in the game dir (hot reloaded by the plugin)
#   tools/fxtest/fxtest.sh logs               tail the plugin log / list probe files
#   tools/fxtest/fxtest.sh stop
#
# Uses only display :99 and the Wine prefix ~/.wine-ts2trace (same as TheSimsRust/tools/game-run.sh).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GAME_DIR="${GAME_DIR:-/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin}"
export DISPLAY="${TS2_DISPLAY:-:99}"
export WINEPREFIX="${TS2_WINEPREFIX:-$HOME/.wine-ts2trace}"
export WINEDEBUG="${WINEDEBUG:--all}"
BACKUP="$ROOT/build/ijl15_user_backup.dll"
TOOLS="${TS2_TOOLS:-/home/bruno/PROJETOS/TheSimsRust/tools}"
export TS2_TOOLS="$TOOLS"

cmd="${1:-}"; shift || true
case "$cmd" in
  install)
    [[ -f "$BACKUP" ]] || cp -v "$GAME_DIR/ijl15.dll" "$BACKUP"
    cp -v "$ROOT/build/ijl15.dll" "$GAME_DIR/ijl15.dll"
    ;;
  restore)
    cp -v "$BACKUP" "$GAME_DIR/ijl15.dll"
    ;;
  cfg)
    : > "$GAME_DIR/ts2fx.cfg"
    for kv in "$@"; do echo "$kv" >> "$GAME_DIR/ts2fx.cfg"; done
    cat "$GAME_DIR/ts2fx.cfg"
    ;;
  start)
    [[ -d "$WINEPREFIX" ]] || cp -a --reflink=auto "$HOME/.wine" "$WINEPREFIX"
    pgrep -x Xvfb >/dev/null || { setsid nohup Xvfb "$DISPLAY" -screen 0 1280x1024x24 +extension GLX -nolisten tcp \
        >/tmp/ts2-xvfb.log 2>&1 </dev/null & sleep 2; }
    cd "$GAME_DIR"
    setsid nohup wine Sims2EP9.exe >/tmp/ts2-game.out 2>&1 </dev/null &
    echo "game started on $DISPLAY"
    sleep "${BOOT_INTRO_WAIT:-20}"
    python3 "$HERE/boot.py"
    ;;
  shot)
    magick import -display "$DISPLAY" -window root "$1"
    ;;
  logs)
    ls -t "$GAME_DIR"/ts2se_*.log | head -1 | xargs tail -n 40
    ls -la "$GAME_DIR"/ts2fx_probe_* 2>/dev/null || true
    ;;
  stop)
    pkill -f Sims2EP9 || true
    wineserver -k 2>/dev/null || true
    pkill -x Xvfb || true
    ;;
  *) sed -n 2,13p "$0"; exit 2 ;;
esac
