#!/usr/bin/env bash
# Builds the co-op harness (real src/coop_net.cpp) and runs Host + Guest under Wine
# with two fake user profiles to validate TCP transport + neighborhood transfer.
set -euo pipefail
ROOT=/home/bruno/PROJETOS/TS
OUT=$ROOT/build/coop_test
T=/tmp/ts2coop_test
mkdir -p "$OUT"
cd "$OUT"

CF=(--target=i686-pc-windows-msvc -ffreestanding -std=c++20 -O1 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -mno-sse2 -I "$ROOT/include")
clang++ "${CF[@]}" -c "$ROOT/tools/coop_test/harness.cpp" -o harness.obj
clang++ "${CF[@]}" -c "$ROOT/src/coop_net.cpp" -o coop_net.obj
cat > k32x.def <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
    SetEnvironmentVariableA@8
    ExitProcess@4
    GetCommandLineA@0
EOF
llvm-dlltool -m i386 -k -d k32x.def -l k32x.lib
lld-link /SUBSYSTEM:CONSOLE /MACHINE:X86 /NODEFAULTLIB /ENTRY:HarnessMain /OUT:harness.exe \
    harness.obj coop_net.obj k32x.lib ../kernel32.lib ../user32.lib ../ws2_32.lib ../msvcrt.lib

# ---- fake profiles ---------------------------------------------------------
rm -rf "$T"
HOST_N="$T/host/Documents/EA Games/The Sims 2/Neighborhoods/N001"
GUEST_N="$T/guest/Documents/EA Games/The Sims 2/Neighborhoods/N001"
mkdir -p "$HOST_N/Lots" "$HOST_N/Characters" "$HOST_N/Thumbnails" "$GUEST_N"
head -c 3000000 /dev/urandom > "$HOST_N/Lots/N001_Lot5.package"
head -c 70000   /dev/urandom > "$HOST_N/N001_Neighborhood.package"
head -c 123     /dev/urandom > "$HOST_N/Characters/C001.package"
: > "$HOST_N/Characters/empty.package"
head -c 999     /dev/urandom > "$HOST_N/Thumbnails/skip_me.png"
echo "guest-original" > "$GUEST_N/old_guest_save.package"

export WINEDEBUG=-all
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine}
wine harness.exe host  'Z:\tmp\ts2coop_test\host'  &
HP=$!
sleep 1
wine harness.exe guest 'Z:\tmp\ts2coop_test\guest' &
GP=$!
wait $HP $GP || true

echo "=== HOST LOG ===";  cat "$T/host/harness.log"
echo "=== GUEST LOG ==="; cat "$T/guest/harness.log"

echo "=== VERIFY ==="
fail=0
check() { if cmp -s "$1" "$2"; then echo "OK   $3"; else echo "FAIL $3"; fail=1; fi; }
GN="$T/guest/Documents/EA Games/The Sims 2/Neighborhoods/N001"
check "$HOST_N/Lots/N001_Lot5.package"        "$GN/Lots/N001_Lot5.package"        "Lot package (3 MB)"
check "$HOST_N/N001_Neighborhood.package"     "$GN/N001_Neighborhood.package"     "Neighborhood package"
check "$HOST_N/Characters/C001.package"       "$GN/Characters/C001.package"       "Character package"
[ -e "$GN/Characters/empty.package" ] && echo "OK   empty file created" || { echo "FAIL empty file"; fail=1; }
[ ! -e "$GN/Thumbnails" ] && echo "OK   Thumbnails skipped" || { echo "FAIL Thumbnails copied"; fail=1; }
[ ! -e "$GN/old_guest_save.package" ] && echo "OK   guest old save replaced" || { echo "FAIL old save still in N001"; fail=1; }
BK=$(ls -d "$T"/guest/Documents/"EA Games"/"The Sims 2"/CoopBackup/N001_* 2>/dev/null | head -1 || true)
if [ -n "$BK" ] && [ -e "$BK/old_guest_save.package" ]; then echo "OK   guest backup kept: $BK"; else echo "FAIL backup missing"; fail=1; fi

# ---- furniture sync: both sides must end with identical poses ----
tr -d '\r' < "$T/host/harness.log"  > "$T/host.txt"
tr -d '\r' < "$T/guest/harness.log" > "$T/guest.txt"
grep '^OBJ ' "$T/host.txt"  > "$T/host_objs.txt"  || true
grep '^OBJ ' "$T/guest.txt" > "$T/guest_objs.txt" || true
if [ -s "$T/host_objs.txt" ] && cmp -s "$T/host_objs.txt" "$T/guest_objs.txt"; then
    echo "OK   furniture poses identical on host and guest"
else
    echo "FAIL furniture poses differ:"; diff "$T/host_objs.txt" "$T/guest_objs.txt" || true; fail=1
fi
grep -q '^OBJ 3: 6,9,1,3,00001003$' "$T/guest.txt" && echo "OK   snapshot overwrote guest's different object #3" || { echo "FAIL snapshot #3"; fail=1; }
grep -q '^OBJ 5: 50,51,1,3,00001005$' "$T/guest.txt" && echo "OK   host move (#5) reached guest" || { echo "FAIL host move #5"; fail=1; }
grep -q '^OBJ 7: 70,71,2,5,00001007$' "$T/host.txt"  && echo "OK   guest move (#7) reached host" || { echo "FAIL guest move #7"; fail=1; }
grep -q '^OBJ 10: 110,120,1,2,ABCD1234$' "$T/guest.txt" && echo "OK   host Buy/Build create (#10) reached guest" || { echo "FAIL create #10"; fail=1; }
grep -q '^OBJ 4: DELETED$' "$T/guest.txt" && echo "OK   host Buy/Build delete (#4) reached guest" || { echo "FAIL delete #4"; fail=1; }
grep -q '^OBJ 0: 7,8,1,6,00001000$' "$T/guest.txt" && echo "OK   host Sim pose reached guest" || { echo "FAIL host Sim -> guest"; fail=1; }
grep -q '^OBJ 1: 20,21,2,4,00001001$' "$T/host.txt" && echo "OK   guest Sim pose reached host" || { echo "FAIL guest Sim -> host"; fail=1; }
grep -q '^MOTIVES sim0=85 sim1=92$' "$T/guest.txt" && echo "OK   host Sim motives reached guest" || { echo "FAIL host motives -> guest"; fail=1; }
grep -q '^MOTIVES sim0=85 sim1=92$' "$T/host.txt" && echo "OK   guest Sim motives reached host" || { echo "FAIL guest motives -> host"; fail=1; }
grep -q 'lastInter=7:5' "$T/host.txt" && echo "OK   guest Pie Menu interaction reached host" || { echo "FAIL interaction sync"; fail=1; }
grep -q 'desync=0' "$T/guest.txt" && echo "OK   deterministic lot CRC (desync=0)" || { echo "FAIL desync > 0"; fail=1; }
exit $fail
