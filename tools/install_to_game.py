#!/usr/bin/env python3
"""
Instala ou remove o proxy ijl15.dll (TS2SE + Multiplayer Co-op + Engine QoL)
no diretório TSBin do The Sims 2 EP9 (/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/).
Faz backup seguro do ijl15.dll original como ijl15_orig.dll.
"""
import os
import shutil
import sys

GAME_TSBIN = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin"
ORIG_DLL   = os.path.join(GAME_TSBIN, "ijl15.dll")
BACKUP_DLL = os.path.join(GAME_TSBIN, "ijl15_orig.dll")
BUILT_DLL  = "/home/bruno/PROJETOS/TS/build/ijl15.dll"

def install():
    if not os.path.exists(BUILT_DLL):
        print(f"[ERRO] {BUILT_DLL} nao encontrado. Execute tools/build_ijl15_dll.py primeiro.")
        sys.exit(1)
    if not os.path.exists(GAME_TSBIN):
        print(f"[ERRO] Diretorio do jogo nao encontrado: {GAME_TSBIN}")
        sys.exit(1)

    if os.path.exists(ORIG_DLL) and not os.path.exists(BACKUP_DLL):
        shutil.copy2(ORIG_DLL, BACKUP_DLL)
        print(f"[Backup] {ORIG_DLL} -> {BACKUP_DLL}")

    shutil.copy2(BUILT_DLL, ORIG_DLL)
    print(f"[Instalado] {BUILT_DLL} -> {ORIG_DLL}")
    print("Pronto! Ao iniciar Sims2EP9.exe, pressione [F10] (ou [INSERT]) para abrir o painel TS2SE + Co-op + QoL.")

def uninstall():
    if os.path.exists(BACKUP_DLL):
        shutil.copy2(BACKUP_DLL, ORIG_DLL)
        os.remove(BACKUP_DLL)
        print(f"[Restaurado] {BACKUP_DLL} -> {ORIG_DLL}")
    else:
        print("[Aviso] Backup ijl15_orig.dll nao encontrado.")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("--uninstall", "uninstall", "restore"):
        uninstall()
    else:
        install()
