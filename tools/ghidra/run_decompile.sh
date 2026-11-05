#!/usr/bin/env bash
# Pipeline retomavel: Sims2EP9.exe -> Ghidra headless -> C decompilado + metadados.
#   1) importar + analisar + salvar projeto       (marcador: docs/ghidra/.analysis_done)
#   2) renomear funcoes via RTTI/vtables + salvar (marcador: docs/ghidra/.names_done; faz backup antes)
#   3) exportar metadados (CSV/JSON/.h)           (marcador: export/_DONE)
#   4) decompilar tudo, retomavel por blocos      (marcador: decomp_named/_DONE)
#   5) pos-processar (por classe, indice, README) (sempre roda)
#
# Rodar:                ./tools/ghidra/run_decompile.sh
# Depois de reiniciar:  rode o MESMO comando; ele continua de onde parou.
# Acompanhar:           tail -f docs/ghidra/run.log docs/ghidra/decomp_named/_progress.log
# Parar:                pkill -f run_decompile.sh; pkill -f Ghidra-Headless
set -uo pipefail

EXE="${EXE:-/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/docs/ghidra"
PROJ="$OUT/project"
NAME="Sims2EP9"
HL=/opt/ghidra/support/analyzeHeadless
SCRIPTS="$ROOT/tools/ghidra"
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-16G}"

unlock() { if ! pgrep -f "Ghidra-Headless" >/dev/null; then rm -f "$PROJ/$NAME.lock" "$PROJ/$NAME.lock~"; fi; }
proc() { "$HL" "$PROJ" "$NAME" -process "$(basename "$EXE")" -noanalysis -scriptPath "$SCRIPTS" "$@"; }

pipeline() {
  mkdir -p "$PROJ"
  unlock

  echo "=== $(date) | 1: importar + analisar ==="
  if [ -f "$OUT/.analysis_done" ]; then echo "ja feito."; else
    "$HL" "$PROJ" "$NAME" -import "$EXE" -overwrite || { echo "ERRO etapa 1"; exit 1; }
    touch "$OUT/.analysis_done"
  fi

  echo "=== $(date) | 2: renomear via RTTI ==="
  if [ -f "$OUT/.names_done" ]; then echo "ja feito."; else
    unlock
    [ -d "$OUT/backup/project.pre_names" ] || { mkdir -p "$OUT/backup" && cp -a "$PROJ" "$OUT/backup/project.pre_names"; }
    proc -postScript ApplyNames.java || { echo "ERRO etapa 2"; exit 2; }
    touch "$OUT/.names_done"; rm -f "$OUT/export/_DONE"
  fi

  echo "=== $(date) | 3: exportar metadados ==="
  if [ -f "$OUT/export/_DONE" ]; then echo "ja feito."; else
    unlock; mkdir -p "$OUT/export"
    proc -readOnly -postScript ExportAll.java "$OUT/export" || { echo "ERRO etapa 3"; exit 3; }
  fi

  echo "=== $(date) | 4: decompilar (retomavel) ==="
  if [ -f "$OUT/decomp_named/_DONE" ]; then echo "ja feito."; else
    unlock; mkdir -p "$OUT/decomp_named"
    proc -readOnly -postScript ExportDecompiled.java "$OUT/decomp_named" 60 || { echo "ERRO etapa 4 (rode de novo para retomar)"; exit 4; }
  fi

  echo "=== $(date) | 5: pos-processar ==="
  python3 "$SCRIPTS/postprocess.py" "$OUT" || { echo "ERRO etapa 5"; exit 5; }
  echo "=== $(date) | FIM ==="
}

if [ "${1:-}" = "--child" ]; then pipeline; exit; fi

if pgrep -f "run_decompile.sh --child" >/dev/null; then echo "Ja esta rodando."; exit 1; fi
mkdir -p "$OUT"
setsid nohup nice -n 10 "$0" --child >> "$OUT/run.log" 2>&1 < /dev/null &
echo "PID $! | log: $OUT/run.log"
