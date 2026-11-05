#!/usr/bin/env bash
# Builds the co-op harness (real src/coop_net.cpp) and runs Host + Guest under Wine
# with two fake user profiles to validate TCP transport + neighborhood transfer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
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

# ---- two processes, one lot each (the guest's save differs on purpose) ----
T=/tmp/ts2coop_test
rm -rf "$T"
mkdir -p "$T/host" "$T/guest"
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
tr -d '\r' < "$T/host/harness.log"  > "$T/host.txt"
tr -d '\r' < "$T/guest/harness.log" > "$T/guest.txt"
grep -E '^(OBJ|SIM) ' "$T/host.txt"  | sort > "$T/host_objs.txt"  || true
grep -E '^(OBJ|SIM) ' "$T/guest.txt" | sort > "$T/guest_objs.txt" || true
ok()  { echo "OK   $1"; }
bad() { echo "FAIL $1"; fail=1; }
if [ -s "$T/host_objs.txt" ] && cmp -s "$T/host_objs.txt" "$T/guest_objs.txt"; then
    ok "furniture + Sims identical on host and guest (slot independent)"
else
    bad "lots differ:"; diff "$T/host_objs.txt" "$T/guest_objs.txt" || true
fi
g() { grep -q "$2" "$T/$1.txt"; }
g guest '^OBJ 00001003: 6,9,1,3$'  && ok "snapshot fixed guest's displaced object #3" || bad "snapshot #3"
g guest '^OBJ 00001008: 16,24,1,0$' && ok "snapshot created the object the guest lacked (#8)" || bad "snapshot create #8"
! g guest '^OBJ EEEE0001' && ok "snapshot removed the guest's extra object" || bad "extra object not removed"
g guest '^OBJ 00001006: 12,18,1,6$' && g guest '^OBJ 00001007: 70,71,2,5$' \
    && ok "swapped slots mapped (host move of #5 / guest move of #7 land on the right objects)" || bad "slot mapping"
g guest '^OBJ 00001005: 50,51,1,3$' && ok "host move reached guest" || bad "host move #5"
g host  '^OBJ 00001007: 70,71,2,5$' && ok "guest move reached host (through the slot map)" || bad "guest move #7"
g guest '^OBJ ABCD1234: 110,120,1,2$' && ok "host purchase reached guest" || bad "host purchase"
! g guest '^OBJ 00001004' && ok "host sale reached guest" || bad "host sale"
g host  '^OBJ BEEF0001: 14,15,1,1$' && g guest '^OBJ BEEF0001: 14,15,1,1$' && ok "guest purchase reached host, later guest move followed (ack mapping)" || bad "guest purchase / ack"
g guest '^SIM 101: 22,23,1,6' && ok "host Sim position/walk reached guest" || bad "host Sim -> guest"
! g host '^SIM 202: 30,31,2,4' && ok "host stays authoritative (a Guest-side Sim move is not pushed to the Host)" || bad "guest Sim leaked to host"
g guest '^SIM 101: .* motive=85$' && g guest '^SIM 202: .* motive=20$' && ok "Host motives of every Sim reached the Guest (matched by NeighborID)" || bad "motives"
g host 'lastInter=7:' && ok "guest Pie Menu interaction reached host" || bad "interaction sync"
g guest 'desync=0' && ok "deterministic lot CRC (desync=0)" || bad "desync > 0"
exit $fail
