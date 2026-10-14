#!/usr/bin/env bash
# Every hardware-free check, in order. Exits non-zero if anything fails.
#
#   bash tests/run_all.sh
#
# Needs g++, Python 3.12 with cad/requirements.txt, and Node 20+. Zig is
# optional: without it the app tests run against the committed
# sim/mono_wallet.wasm instead of a fresh build.
set -u
cd "$(dirname "$0")/.."
mkdir -p build
# Run it, do not just look for it: on Windows "python3" is often the Microsoft
# Store alias, which is on PATH but only prints "Python was not found".
PY=${PYTHON:-}
if [ -z "$PY" ]; then
  for candidate in python3 python; do
    if "$candidate" -c "import sys" >/dev/null 2>&1; then PY=$candidate; break; fi
  done
fi
[ -n "$PY" ] || { echo "no working Python found; set PYTHON=/path/to/python"; exit 1; }
rc=0

step() {
  echo
  echo "=============================================================="
  echo " $1"
  echo "=============================================================="
}

step "1/4  firmware on a virtual clock: default, Find My and bench builds"
for v in "default:" "findmy:-DMONO_ENABLE_FINDMY=1" "bench:-DMONO_BENCH=1"; do
  name=${v%%:*}
  flags=${v#*:}
  if g++ -std=gnu++11 -Wall -Wextra -Werror $flags -x c++ -I sim/mocks \
         -o "build/test_firmware_$name" tests/test_firmware.cpp; then
    out=$("./build/test_firmware_$name"); code=$?
    if [ "$name" = default ]; then echo "$out"; else echo "  [$name build]$(echo "$out" | tail -n 2)"; fi
    [ $code -eq 0 ] || rc=1
  else
    echo "  the $name build failed to compile"; rc=1
  fi
done

step "2/4  CAD geometry and the digital mock-up"
"$PY" tests/test_cad.py --regen || rc=1

step "3/4  the firmware as WebAssembly, and the app's suites: against it, and against a fake Bluetooth stack"
if command -v zig >/dev/null 2>&1 || [ -n "${ZIG:-}" ]; then
  "$PY" sim/build_wasm.py || rc=1
else
  echo "  zig not found - testing the committed sim/mono_wallet.wasm"
fi
node --test "tests/app/*.test.mjs" || rc=1

step "4/4  the terminal simulator builds"
if g++ -std=gnu++11 -Wall -Wextra -DMONO_BENCH=1 -pthread -x c++ -I sim/mocks \
       -o build/mono_sim sim/mono_sim.cpp; then
  echo "  built build/mono_sim - run it, type HELP, then try !touch 1"
else
  echo "  the simulator failed to compile"; rc=1
fi

echo
[ $rc -eq 0 ] && echo "ALL GREEN" || echo "SOMETHING FAILED (see above)"
exit $rc
