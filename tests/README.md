# Testing MONO without the hardware

Three layers, none of which need a board, a sensor, a servo or a battery.

| Layer | What runs | What it proves |
|---|---|---|
| `test_firmware.cpp` | the real `mono_firmware.ino`, compiled for your PC against mock hardware, on a virtual clock | protocol, settings, persistence, every state transition |
| `test_cad.py` | measurements taken off the generated STLs | printability, envelope, part mating, component fit |
| `mono_sim.cpp` + `app_sim_server.py` | the same firmware at wall-clock speed, with the real app talking to it in a browser | the app and the firmware actually agree |

## Run everything

```bash
bash tests/run_all.sh
```

## Run one layer

Firmware logic (instant — the virtual clock skips the 30-second lockouts):

```bash
g++ -std=c++17 -x c++ -I tests/mocks -o tests/build/test_firmware.exe tests/test_firmware.cpp && ./tests/build/test_firmware.exe
```

Geometry (add `--regen` to re-run the generator and prove the CONFIG block is live):

```bash
python mono_wallet_cad.py && python tests/test_cad.py --regen
```

The app against a simulated wallet:

```bash
g++ -std=c++17 -x c++ -I tests/mocks -o tests/build/mono_sim.exe tests/mono_sim.cpp && python tests/app_sim_server.py
```

Then open <http://127.0.0.1:8765/>. The page is `mono_app/index.html` byte for byte, with
one injected `<script>` that swaps `navigator.bluetooth` for a shim wired to the
simulator. A bench panel bottom-right stands in for the physical world: press a
finger to the sensor, shake it, walk away.

You can also drive the wallet straight from a terminal:

```bash
./tests/build/mono_sim.exe
```

Type `HELP`, `STATUS`, `ENROLL`. Lines starting with `!` are the bench, not the
wallet — `!finger 1`, `!press 7`, `!lift`, `!tamper on`, `!link 0`, `!batt 3.4`, `!peek`.

## How the firmware runs on a PC

`tests/mocks/` provides `Arduino.h`, `bluefruit.h`, `Adafruit_Fingerprint.h`,
`Servo.h`, `Adafruit_LittleFS.h` and `LSM6DS3.h`. The sketch is `#include`d
**unmodified** — if it stops compiling here, it has stopped compiling.

Two details make the tests meaningful rather than decorative:

- **`delay()` advances the clock.** A 6-second relock or a 30-second lockout costs
  no real time in the test suite, but the simulator points `g_sleepHook` at a real
  sleep so a human at the app sees the wallet behave at its true pace. Blocking
  code blocks in both, which is why the beep loops show up as latency.
- **The mocks record physical effects.** Servo angles, buzzer hits, LED state and
  the advertising interval are all captured, so a test can assert that a tamper
  really did drive the latch shut and switch the radio to the fast beacon —
  not merely that it printed a message saying so.

## What this cannot tell you

Everything analogue. Real BLE timing and range, R502 UART behaviour and match
quality against actual fingers, servo torque against a real latch, current draw,
and whether the printed parts fit once the plastic has shrunk. The mocks answer
"is the logic right"; only the bench answers "does it work".
