#!/usr/bin/env bash
# Runs TWO real The Sims 2 instances (host + guest) under Wine on private Xvfb displays, each with its own
# private copy of the Wine prefix and of Documents/EA Games (so the user's real saves are never touched),
# and lets the plugin's TS2COOP_AUTO driver host/join and exercise object + Sim sync.
#
#   real2.sh setup            create the two private prefixes (one time, ~4 GB with reflinks)
#   real2.sh start [loop|net] launch host+guest, boot menus into the lot (loop = 127.0.0.1 same-PC mode,
#                             net = TS2_COOP_TEST=1: behaves like two PCs, incl. autosave + neighborhood transfer)
#   real2.sh shot             screenshots of both displays -> build/shots/coop_{host,guest}.png
#   real2.sh logs             tail of both plugin logs
#   real2.sh stop
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GAME_DIR="${GAME_DIR:-/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin}"
BASE="${COOP_BASE:-$HOME/.cache/ts2coop}"
TOOLS="${TS2_TOOLS:-/home/bruno/PROJETOS/TheSimsRust/tools}"
SRC_PREFIX="${SRC_PREFIX:-$HOME/.wine-ts2trace}"
SRC_DOCS="${SRC_DOCS:-$HOME/Documentos/EA Games}"
declare -A DISP=( [host]=:98 [guest]=:97 )
declare -A PFX=( [host]="$BASE/host/prefix" [guest]="$BASE/guest/prefix" )

cmd="${1:-}"; shift || true
case "$cmd" in
  setup)
    for r in host guest; do
      if [[ ! -d "${PFX[$r]}" ]]; then
        mkdir -p "$BASE/$r"
        cp -a --reflink=auto "$SRC_PREFIX" "${PFX[$r]}"
      fi
      D="${PFX[$r]}/drive_c/users/bruno/Documents"
      [[ -L "$D" ]] && rm "$D"
      mkdir -p "$D"
      [[ -d "$D/EA Games" ]] || cp -a --reflink=auto "$SRC_DOCS" "$D/EA Games"
      echo "ready: ${PFX[$r]}"
    done
    ;;
  start)
    mode="${1:-loop}"
    for r in host guest; do
      pgrep -f "Xvfb ${DISP[$r]}" >/dev/null || { setsid nohup Xvfb "${DISP[$r]}" -screen 0 1280x1024x24 +extension GLX -nolisten tcp >/tmp/coop-xvfb-$r.log 2>&1 </dev/null & }
    done
    sleep 2
    rm -f "$GAME_DIR"/ts2se_host_*.log "$GAME_DIR"/ts2se_guest_*.log
    for r in host guest; do
      ( cd "$GAME_DIR"
        export DISPLAY="${DISP[$r]}" WINEPREFIX="${PFX[$r]}" WINEDEBUG=-all TS2_LOGTAG=$r TS2COOP_AUTO=$([[ $r == host ]] && echo host || echo join) TS2COOP_FORCEBUY=1
        [[ "$mode" == net ]] && export TS2_COOP_TEST=1
        setsid nohup wine Sims2EP9.exe >/tmp/coop-game-$r.out 2>&1 </dev/null & )
    done
    echo "both started ($mode); booting menus (~3-5 min)"
    sleep "${BOOT_INTRO_WAIT:-25}"
    for r in host guest; do
      ( export DISPLAY="${DISP[$r]}" TS2_TOOLS="$TOOLS"
        setsid nohup python3 -u "$ROOT/tools/coop_test/enter_lot.py" >/tmp/coop-boot-$r.log 2>&1 </dev/null & )
    done
    ;;
  shot)
    mkdir -p "$ROOT/build/shots"
    for r in host guest; do magick import -display "${DISP[$r]}" -window root "$ROOT/build/shots/coop_$r.png"; done
    ;;
  logs)
    for r in host guest; do echo "=== $r ==="; ls -t "$GAME_DIR"/ts2se_${r}_*.log 2>/dev/null | head -1 | xargs -r -d '\n' tail -n "${1:-40}"; done
    ;;
  stop)
    pkill -f Sims2EP9 || true
    for r in host guest; do WINEPREFIX="${PFX[$r]}" wineserver -k 2>/dev/null || true; done
    pkill -f "Xvfb :98" || true; pkill -f "Xvfb :97" || true
    pkill -f fxtest/boot.py || true
    ;;
  *) sed -n 2,11p "$0"; exit 2 ;;
esac
