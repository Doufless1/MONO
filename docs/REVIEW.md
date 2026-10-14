# Engineering review

Two passes over the same prototype. The **first** (2026-08-22, kept below as
it was written) ran the v2 firmware, CAD and app without hardware and found
eleven issues. The **second** (2026-10-13) went looking for what that pass
missed, reproduced every suspicion against the real code before calling it a
bug, and then rebuilt the three parts around the fixes.

Every bug in this document was **reproduced first** — against the v2 firmware
in the host harness, against v1's own meshes, or in a browser. Every fix that
can be checked by a program is **pinned by a test** whose name is given, and
the test suites print the same IDs (`[REVIEW F7]`) next to the assertions that
guard them. The few that only a person can judge — how a screen reads, what a
browser asks — say how they were checked instead.

| Area | Found | Fixed | Pinned by |
|---|---:|---:|---|
| Firmware | 22 | 21 (+1 by design) | `tests/test_firmware.cpp` — 213 / 219 / 212 assertions in three builds |
| Companion app | 17 | 17 | `tests/app/*.test.mjs` — 36 tests: 13 against the real firmware as WebAssembly, 7 against a fake Bluetooth stack |
| CAD | 17 | 17 | `tests/test_cad.py` + `cad/dmu.py` — 61 checks measured off the meshes |
| Repository | 3 | 3 | CI |

Severity: **high** — wrong, unsafe or destroys data · **medium** — wrong in a
way a user meets · **low** — wrong in a corner · **design** — a decision, made explicit.

---

## Firmware

### F1 — Any phone in radio range could open the wallet · high, security
`UNLOCK` was accepted from any connected client, with no pairing and no
authentication (first pass, F1). The same was true of `SET servolock`, which
could retune the latch so it never closed, and of `ENROLL`.

**Fix.** A trust model with three ways in, documented in [SECURITY.md](SECURITY.md):
a factory-fresh wallet pairs its first phone at once; after that a new phone
asks with `PAIR` and the **owner's finger on the sensor confirms it**; a paired
phone proves itself on every connection with **HMAC-SHA256 over a fresh
128-bit nonce**. Everything except `HELP`, `HELLO`, `AUTH` and `PAIR` needs a
trusted session. The USB-C port is on the outside of the wallet, so the USB
console is trusted only while the wallet is unowned or for a minute after the
owner's finger opened it (`MONO_BENCH` restores bench convenience).
**Pinned:** *an unpaired phone cannot UNLOCK*, *a recorded AUTH replayed against
a new nonce fails*, *a key one bit wrong is refused*, *a stranger's finger does
not confirm it*, *the USB port on the outside of the wallet cannot UNLOCK it*,
and the SHA-256 / HMAC suite (NIST and RFC 4231 vectors).

### F2 — A wake line that never clears was rescanned forever · medium, battery
With the R502's touch line held high (a damp sensor, a resting thumb) the wallet
cycled scan → sleep → scan at full current: 4 times per 30 s, continuously
(first pass, F2). A resting **wrong** thumb also burned a try every ~4 s.
**Fix.** The sensor re-arms only once the line has gone low.
**Pinned:** *a wake line that never clears is not rescanned again and again*,
*a wrong thumb left resting on the sensor costs one try, not several*.

### F3 — A freshly enrolled finger was scanned immediately · low
The finger still on the platen after `ENROLL ok` opened the wallet at once
(first pass, F3). **Fix:** same re-arm. **Pinned:** *the freshly enrolled finger
is not rescanned until it lifts*.

### F4 — `LOCK` silently cancelled a tamper lockdown · high
Reproduced: in lockdown, `LOCK` took the state from `5` to `0` — alarm gone —
and left the radio on the 20 ms alarm beacon. It also cut a brute-force
lockout short. **Fix:** `LOCK` locks the latch and nothing else; ending an alarm
is `CLEAR`'s job (or the owner's finger, or `UNLOCK` from the paired phone).
**Pinned:** *LOCK does not quietly cancel the alarm*, *LOCK does not end a lockout early*.

### F5 — Unlimited wrong fingers during a lockdown · high, security
Reproduced: 12 wrong fingers during a tamper lockdown left `failCount = 0`, no
lockout, no event. Lockdown is exactly when a thief is trying fingers.
**Fix:** the scan path is shared by every state, so rejects count everywhere.
**Pinned:** *wrong fingers during lockdown count toward a lockout*.

### F6 — USB and BLE shared one line buffer · medium
Reproduced: a BLE `STA`, then a USB `LOCK⏎`, then BLE `TUS⏎` produced
`ERR unknown STALOCK` and `ERR unknown TUS` — both commands destroyed.
**Fix:** a line buffer per transport; a reply goes to the port that asked.
**Pinned:** *USB and BLE lines arriving interleaved stay intact*.

### F7 — Tamper detection depended on what the main loop was doing · medium
v2 sampled the IMU once per loop pass. Asleep, a pass took 60 ms; rejecting a
finger, under a millisecond. Reproduced: the **same 3 ms knock** was ignored
while asleep and raised a full lockdown while a finger was being rejected.
**Fix:** a fixed 50 Hz sampling grid, counting rising edges — three separate
knocks, not three samples of one. **Pinned:** *three 30 ms knocks raise the
alarm in every state*, *the same 3 ms knocks get the same verdict in every state*,
*a sustained push is one knock, not three*. The IMU mock schedules knocks on the
clock, so a knock's physical length no longer depends on the loop either.

### F8 — `SET` echoed values it did not store, and accepted garbage · low
Reproduced: `SET relock 999999` replied `SET ok relock=999999` while storing
60000; `SET relock banana` stored 1000 and said `ok`.
**Fix:** whole-string number parsing; the reply carries the stored value.
**Pinned:** *over-range relock clamps, and the reply says what was stored*,
*a non-number is refused, not stored as 0*.

### F9 — An overlong line ran its prefix · medium
Reproduced: 310 characters of junk starting with `DELETEALL ` wiped every
fingerprint. **Fix:** a line that overflows the buffer is refused whole
(`ERR toolong`). **Pinned:** *a 310-character line is refused whole*, *its
DELETEALL prefix never runs*.

### F10 — The battery read drove P0.14 HIGH · medium, hardware safety
v2 set `VBAT_ENABLE` HIGH after each read to save ~3 µA. Seeed's own wiki warns
that with P0.14 high, P0.31 can reach its 3.6 V limit and be damaged
([Seeed](https://wiki.seeedstudio.com/battery_charging_considerations/)).
**Fix:** driven LOW once at boot and left there. **Pinned:** *battery divider
enable is driven LOW … and stays LOW through every battery read*.

### F11 — Opening the wallet then enrolling froze the auto-relock · medium
Reproduced: 15 s into an enrolment the latch was still open (relock due at 6 s).
**Fix:** the relock timer is its own subsystem. **Pinned:** *the wallet relocks
on time even while enrolling*.

### F12 — The alarm blocked the loop, and stopped after one burst · medium
`beep()` was `tone()` plus `delay()`: 1.25 s blocked per tamper, 1.6 s per
`RING` (first pass, F5). And the app tells the owner "it is beeping … stop it
from here", while the firmware beeped six times and went quiet.
**Fix:** a scheduled sound player with priorities, a deferred servo detach, and
an alarm that bursts every 2.5 s until a minute after the last shake.
**Pinned:** *through a ring, an alarm, an unlock and an enrolment, no delay()
exceeded 20 ms*, *a STATUS sent behind a RING is answered at once (v2: 1.6 s later)*,
*it keeps beeping in bursts*, *it goes quiet a minute after the last shake*.

### F13 — A stranger connecting silenced the separation alarm · high, security
Any connection reset the leash, so a thief could stop the "you left your
wallet" alarm with nRF Connect. **Fix:** only the paired phone, after it has
authenticated, stands the leash down. **Pinned:** *a stranger connecting does
not silence the alarm*.

### F14 — After an alarm the radio could beacon at 20 ms forever · high, battery
Reproduced: after a separation alert, with the phone back, advertising stayed
at 20 ms (`adv=32`) — until a reboot. `LOCK` and `UNLOCK` during a lockdown did
the same. **Fix:** the advertising interval is **derived from the state on
every pass** instead of being switched by whoever raised an alarm.
**Pinned:** *… and puts the beacon back to normal*, *CLEAR … the radio returns to
the normal interval*.

### F15 — Tapping Disconnect sounded the walk-away alarm · medium
Reproduced: a graceful disconnect (reason 0x13) produced `EVT separation warn`
and `EVT separation alert`. **Fix:** only a lost link (supervision timeout
0x08, LL timeout 0x22, failed to establish 0x3E) arms the leash, and the app
says `BYE` before hanging up. **Pinned:** *tapping Disconnect in the app is not
walking away*, *after BYE, even a dropped link is not an alarm*.

### F16 — Enrolling after a delete overwrote someone else's finger · high, data loss
The next id was `templateCount + 1`. Reproduced: with fingers {2, 3}, a new
finger was stored as id 3 — finger 3 silently gone. **Fix:** a slot map probed
once at boot (`loadModel`), kept current on every store and delete; the first
free id is used; `LIST` names the ids. **Pinned:** *after a delete, a new finger
takes the free slot*, *finger 3 is untouched*, *finger 3 still opens the wallet*.

### F17 — Find My time-slicing never restored the wallet's address · medium, latent
Switching to the beacon changed the BLE address; switching back never restored
it, so a bonded phone would stop recognising the wallet. And each app slice
reset the alarm beacon to normal. Latent: Find My is compiled out by default.
**Fix:** the identity address is saved at boot and restored; the interval is
derived. **Pinned** (in the `MONO_ENABLE_FINDMY=1` build): *… with the wallet's
own address restored*, *during an alarm, the app slices use the fast interval*.

### F18 — A smudged image counted as a wrong finger · low
`image2Tz` failing (a poor capture) was treated as a reject and burned a try.
**Fix:** a poor image is a retry; only a readable, unenrolled finger is a
reject. **Pinned:** *a smudged image is not held against you as a wrong finger*.

### F19 — The power note blamed the wrong thing · low, documentation
v2's note called `delay()` a busy-wait. On this core it is FreeRTOS
`vTaskDelay()` ([source](https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/cores/nRF5/delay.c)).
The real costs are the R502's always-on supply, 50 Hz IMU polling and the
loop spinning flat out in states v2 never slept in. **Fix:** the note is
rewritten, and `idle()` now sleeps in every state.

### F20 — `HELP` broke the protocol's own rule · low
Every reply starts with the command name — except v2's `HELP`, which replied
`CMDS …`. **Fix:** the table-driven dispatcher generates `HELP …`.

### F21 — Find My is wired but inert · design (first pass, F4)
The key is still 28 zero bytes and the beacon is compiled out. Unchanged and
expected: paste a real key and set `MONO_ENABLE_FINDMY 1`.

### F22 — `PIN_LED` redefined a macro the board's variant owns · low, build
v2's `#define PIN_LED 5` collides with the Seeed variant, which defines
`PIN_LED` as the onboard RGB LED's red channel and `LED_BUILTIN` through it.
On the board that is a "PIN_LED redefined" warning, and inside the sketch
`LED_BUILTIN` quietly became D5. The host mocks defined no such macro, so
nothing noticed. **Fix.** MONO's LED is `PIN_STATUS_LED`, and the mock
`Arduino.h` defines the variant's pin macros, so a collision now fails the
`-Werror` host build in CI. **Pinned:** that build — the old line is an error.

A hardening note without a reproduction: v2 declared `enum State` after the
sketch's first function. The Arduino IDE inserts generated prototypes before
the first function, where such a type is not yet visible — a known cause of
"not declared in this scope" errors. v3 declares every type up front; the
`board` CI job compiles the sketch with the real toolchain.

---

## Companion app

The app was rebuilt as ES modules — `protocol.js` (pure), `crypto.js`,
`transport.js` (Web Bluetooth), `session.js` (the conversation), `app.js`
(screens) — so the parts that can be tested in Node are.

| | Issue | Severity | Fix | Pinned by |
|---|---|---|---|---|
| **A1** | Lock/Unlock/Clear left the screen showing the opposite of reality (first pass) | high | Fixed structurally: the firmware **pushes STATUS** on every change | *UNLOCK resolves only when the wallet says it opened; the relock arrives as a push* |
| **A2** | Overlapping GATT writes: STATUS and GET fired back to back; Chrome rejects the second ("GATT operation already in progress") | high | One write queue in `transport.js` | *writes go out one at a time: STATUS and GET sent back to back both arrive*, against a fake GATT stack as strict as Chrome's |
| **A3** | Writes were not chunked to the default 20-byte ATT payload | medium | 20-byte chunks | *a long line goes out in 20-byte pieces* |
| **A4** | The notification listener was attached after notifications started (first lines lost); a non-streaming UTF-8 decoder | medium | Listener first, streaming decoder | *the listener is in place before notifications start*, *lines and characters split across notifications arrive whole* |
| **A5** | "Hold to open" announced **Open** without the wallet's word — even while disconnected | high | "Opening…" until `UNLOCK ok`; an error if it never comes | *UNLOCK resolves only when …*, *a wallet that never answers times the request out* |
| **A6** | Ring, settings, enrolment and removal acted optimistically while disconnected | medium | Controls disabled while the wallet is away; settings show a spinner until stored | *a preset choice is stored by the wallet and reflected back* |
| **A7** | Removing a finger dropped it from the list first; offline it came back as "Finger N" | medium | The list follows `LIST` from the wallet | *enrolling through the app … the list updates itself* |
| **A8** | The roster assumed template ids 1..N — wrong after any delete | medium | `LIST ids=` | *GET, LIST and HELLO* |
| **A9** | Stopping the alarm opened a sheet titled "Hold to open" | low | Its own copy | verified in the browser |
| **A10** | "Never" for the walk-away alarm meant "after an hour" | medium | Real off switch (`SET sepwarn 0`) | *"Never" really switches the leash off* |
| **A11** | No reconnect: walking back into range left the app "Out of range" until you re-picked the device | high | Backoff reconnect on the same device; never after a deliberate hang-up | *walking away … reconnects by itself*, *hanging up on purpose says BYE* |
| **A12** | A 2.7 s splash on every launch; 2 s even with reduced motion | low | ~1.5 s once per session; instant on return and under reduced motion | measured in Chrome: gone 1.3 s after first paint; invisible within 0.3 s under reduced motion |
| **A13** | A `data:` manifest with no icons: not installable, no offline support | low | Real manifest, icons, service worker | *every module the app is made of is in the offline shell*, *every icon the manifest names exists, at the size it claims* |
| **A14** | Raw failure reasons ("The wallet reported: no_match"); "30 hours ago" | low | Sentences; days | *failure reasons become sentences*, *time since, in words* |
| **A15** | Location was requested on first connect, out of context | medium, privacy | An opt-in switch that explains itself | by inspection: the only call that asks is behind the switch, which starts off |
| **A16** | Icon-only buttons without labels; the hold pad unusable by keyboard; no focus management | medium, a11y | Labels, Space/Enter press-and-hold, focus moved and restored, Escape | verified in the browser |
| **A17** | Safari/Firefox users saw a dead app | medium | A demo wallet: the firmware as WebAssembly, no Bluetooth needed | the session tests drive the same wallet; the demo GIF is recorded in Chrome (Safari and Firefox not yet tried) |

## Simulator

**S1 — `mono_sim` exited before answering the last lines of a piped script.**
When stdin ended it broke out of its loop before running the firmware on the
lines it had just read. Fixed: it drains, then keeps running for half a second.

---

## CAD

Every finding below was measured off v1's own meshes, rebuilt in assembled
coordinates, before anything was changed.

### Model A — looks

| | Issue | Severity | Evidence | Fix |
|---|---|---|---|---|
| **C6** | **The halves could not close.** The pin holes were cut from z = −0.05 to +1.1: 0.05 mm into the back half and the rest into thin air | high | 1.81 mm³ pin/half interference | Holes cut on the right side, 1.22 mm deep |
| **C7** | The card cavity broke through the outer skin around both top corners | medium | no skin over 67 of 90° of each corner arc | Cavity clipped to the outline inset |
| **C8** | The 1.0 mm-deep latch channel and USB slot holed through the 0.76 mm walls into the card cavity | medium | no solid between outside and cavity at both | 0.74 mm of skin remains |
| **C9** | **Root cause of C5, C7 and C8: the concept was drawn around 52.02 × 84.02 mm cards.** A real 53.98 mm ISO card cannot fit its outline — at best 0.24 mm of skin over its corners; with room for pins, it pokes out | design | measured | Model A holds concept-size display cards (`A4_display_card.stl`) and gets 1.74 mm walls; `A_CARDS = "iso"` restores the old trade-off. Real cards are Model B's job |
| **C5** | 0.76 mm walls (first pass) | — | | Now 1.74 mm, a consequence of C9 |
| **C4** | Two stale comments (first pass) | cosmetic | | Gone with the rewrite |

### Model B — works

| | Issue | Severity | Evidence | Fix |
|---|---|---|---|---|
| **C10** | **The USB-C slot was blind** behind a 0.5 mm membrane — and led to the LiPo; the XIAO sat ~30 mm away | high | solid at x = −27.25 | XIAO against the −X wall; the slot is **derived from its position**, through the wall, with an outside relief so a real plug engages 6.25 mm |
| **C11** | **The latch slot was blind** behind 0.5 mm, so no servo pin could reach it | high | solid at x = +27.25 | A window from the servo into the channel |
| **C12** | **No septum**: cards and electronics shared one open void, 12 mm deep | high | empty from z = −5 to +7 | A 1 mm septum; B1 is a closed card sleeve |
| **C13** | The tongue hung into the card exit at the top edge, and met its groove with zero clearance | medium | tongue at (0, 94.4, −0.5); 0.000 mm³ contact | The tongue runs round the sides and bottom only; groove 0.2 mm deeper |
| **C2** | The mouth (47.5 mm) was narrower than a card (first pass) | high | | A 54.6 mm full-width mouth, tight top corners, a thumb notch and 0.35 mm snap nubs |
| **C3** | The servo pocket stood a 16 mm servo up in a 7 mm bay (first pass) | high | | Servo lies flat (6.5 mm vertical), relocated beside the latch |
| **C1** | `N_CARDS_B` did nothing (first pass) | medium | 6 → 12 left the pocket at 5.05 mm | Drives the whole stack-up: 6 → 12 cards deepens the measured pocket 5.06 → 9.62 mm |
| **C14** | The tray covered the status-LED window; the buzzer had no pocket and no way out | medium | | LED and buzzer press against the front wall through tray holes; a 3-slot grille |
| **C15** | The R502 sat 2.5 mm behind the face under a 19 mm hole — narrower than its 19.5 mm sensing window | medium | | A stepped bore: the whole window shows through a 20.1 mm aperture, 0.6 mm under the finger well; a lip retains the 22 mm body |
| **C16** | The generator wrote STLs into the current directory, wherever that was | low | | Always `cad/stl/`, next to the script |
| **C17** | v1's geometry suite checked several claims with arithmetic on the CONFIG constants, not the meshes — which is how C6, C10 and C11 passed it | process | | Every check measures a mesh. `cad/dmu.py` assembles everything and checks 226 solid pairs for clashes plus the access paths |

---

## Repository

- **R1** — a 0-byte file named `git` at the root (a mistyped redirect). Removed.
- **R2** — no CI. Now: three firmware builds, a compile for the real board,
  CAD, and the app against the firmware, on every push.
- **R3** — generated artifacts could silently go stale. Now each has a check:
  the STLs against a fresh build, the WebAssembly against a fresh build.

---

## What none of this can tell you

Real BLE range and timing; R502 match quality against real fingers; servo
torque against a real latch; current draw; print fit after shrinkage; whether
`R502_T` (5.5 mm, unverified) matches your module. The mocks answer "is the
logic right"; only the bench answers "does it work".

---

<details>
<summary><b>The first pass, as written on 2026-08-22</b> (re-verified 2026-08-23)</summary>

Firmware **110/110** logic assertions passed, CAD **71/72** geometry checks
passed, and the companion app talked to a simulated wallet end to end. The
full write-up was `FINDINGS.md` at the repository root; its last version is in
commit `989a5dc`.

- **A1** — Lock, Unlock and Clear alarm left the UI showing the opposite of
  reality: `handleLine()` refreshed status after `ENROLL ok`, `DELETE`,
  `DELETEALL` and four events, but not after `LOCK ok`, `UNLOCK ok` or
  `CLEAR ok`. Fixed in the ONYX rebuild.
- **F1** — `UNLOCK` over the radio opened the wallet with no authentication.
- **F2** — A wake line that never clears kept the wallet awake indefinitely.
- **F3** — A freshly enrolled finger was re-scanned immediately.
- **F4** — Find My wired but inert (placeholder key).
- **F5** — The wallet went deaf for over a second during its own alarms.
- **C1** — `N_CARDS_B` did nothing.
- **C2** — The card mouth was narrower than a card, in both models.
- **C3** — The servo pocket may leave the servo standing up in a 7 mm bay.
- **C4** — Two stale comments.
- **C5** — Model A walls were 0.76 mm.
- Housekeeping: a 0-byte file named `git` at the repo root.

</details>
