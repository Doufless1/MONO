#!/usr/bin/env bash
# Every hardware-free check, in order. Exits non-zero if anything fails.
set -u
cd "$(dirname "$0")/.."
mkdir -p tests/build
rc=0

echo "=============================================================="
echo " 1/3  firmware logic"
echo "=============================================================="
if g++ -std=c++17 -x c++ -I tests/mocks -o tests/build/test_firmware.exe \
       tests/test_firmware.cpp; then
  ./tests/build/test_firmware.exe || rc=1
else
  echo "  firmware harness failed to compile"; rc=1
fi

echo
echo "=============================================================="
echo " 2/3  CAD geometry"
echo "=============================================================="
python mono_wallet_cad.py && python tests/test_cad.py --regen || rc=1

echo
echo "=============================================================="
echo " 3/3  wallet simulator builds"
echo "=============================================================="
if g++ -std=c++17 -x c++ -I tests/mocks -o tests/build/mono_sim.exe \
       tests/mono_sim.cpp; then
  echo "  built tests/build/mono_sim.exe"
  echo "  drive the app against it:  python tests/app_sim_server.py"
else
  echo "  simulator failed to compile"; rc=1
fi

echo
[ $rc -eq 0 ] && echo "ALL GREEN" || echo "SOMETHING FAILED (see above)"
exit $rc
