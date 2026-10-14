# Testing MONO without the hardware

Six suites. None of them needs a board, a sensor, a servo or a battery.

| Layer | What runs | What it proves |
|---|---|---|
| `test_firmware.cpp` | the real `mono_firmware.ino`, compiled for your PC against the virtual hardware in `sim/mocks/`, on a virtual clock — in three builds | protocol, security policy, settings, persistence, every state transition, and that nothing blocks |
| `test_cad.py` | measurements taken off the generated meshes, plus `cad/dmu.py`'s digital mock-up | printability, envelopes, mating, clashes, access paths, fasteners, and that the committed STLs are current |
| `app/protocol.test.mjs` | the app's pure protocol layer, in Node | parsing, presets, the AUTH answer against a Python-computed vector |
| `app/session.test.mjs` | the app's session layer against **the firmware compiled to WebAssembly** | pairing, HMAC authentication, enrolment, unlock, alarms, walk-away and reconnect — the app and the wallet actually agree |
| `app/transport.test.mjs` | the Web Bluetooth transport against a fake GATT stack, as strict as Chrome's | one write in flight at a time, 20-byte chunks, no lost first line, streaming decode, lost versus closed |
| `app/shell.test.mjs` | the manifest and the service worker's file list | the app installs, and every module it is made of is cached for offline use |

## Run everything

```bash
bash tests/run_all.sh
```

## Run one layer

Firmware — instant, because the virtual clock skips the 30-second lockouts.
Built with `-std=gnu++11`, the board toolchain's own standard:

```bash
g++ -std=gnu++11 -Wall -Wextra -x c++ -I sim/mocks -o build/test_firmware tests/test_firmware.cpp && ./build/test_firmware
```

Add `-DMONO_ENABLE_FINDMY=1` for the Find My time-slicing tests, or
`-DMONO_BENCH=1` for the bench build's trusted console.

Geometry (`--regen` also proves `N_CARDS_B` really drives the pocket):

```bash
pip install -r cad/requirements.txt
python tests/test_cad.py --regen
python cad/dmu.py                     # the mock-up's own report
```

The app against the firmware:

```bash
python sim/build_wasm.py              # needs Zig; or use the committed sim/mono_wallet.wasm
node --test "tests/app/*.test.mjs"
```

## Drive the wallet yourself

From a terminal, at wall-clock speed:

```bash
g++ -std=gnu++11 -DMONO_BENCH=1 -pthread -x c++ -I sim/mocks -o build/mono_sim sim/mono_sim.cpp
./build/mono_sim
```

Type `HELP`, `STATUS`, `ENROLL`. A line starting with `@` is sent by the phone
over BLE (`@HELLO`, `@PAIR`); a line starting with `!` is the physical world —
`!touch 1` (your finger), `!touch 2` (someone else's), `!lift`, `!shake on`,
`!knock`, `!link 1`, `!link 0` (walk away), `!link bye`, `!batt 3.4`, `!peek`.
Output lines say where they went: `U` the USB console, `B` the phone, `S` the bench.

Or in a browser, with the app and a 3D twin of the wallet:

```bash
python tools/serve.py                 # then http://localhost:8080/
```

## How the firmware runs on a PC

`sim/mocks/` provides `Arduino.h`, `bluefruit.h`, `Adafruit_Fingerprint.h`,
`Servo.h`, `Adafruit_LittleFS.h`, `LSM6DS3.h` and `nrf_soc.h`. The sketch is
`#include`d **unmodified** — if it stops compiling here, it has stopped compiling.

What makes the tests meaningful rather than decorative:

- **`delay()` advances the clock**, and a test can hook it: one test records
  the longest single `delay()` across a ring, an alarm, an unlock and an
  enrolment, and fails if it ever exceeds 20 ms.
- **The mocks record physical effects** — servo angles, whether the servo is
  powered, every beep, the LED pin, the advertising interval, every line sent
  to each transport — so a test asserts that a tamper really drove the latch
  shut and sped the beacon up, not merely that it printed so.
- **Fingers have identities.** Enrol identity 1, touch with identity 1, it opens;
  touch with identity 2, it rejects.
- **Knocks are scheduled on the clock** (`g_imu.jolt(t, 30)`), so their length
  does not depend on the loop — which is how a test proves that tamper
  detection no longer does either.
- **The app's session runs on the wallet's clock** in `session.test.mjs`, so
  the two advance together: deterministic, and a 60-second walk-away takes
  milliseconds.

## What this cannot tell you

Everything analogue: real BLE timing and range, R502 match quality against
actual fingers, servo torque against a real latch, current draw, and whether
the printed parts fit once the plastic has shrunk. The mocks answer "is the
logic right"; only the bench answers "does it work".
