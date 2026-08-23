# MONO prototype — what the hardware-free test run found

Ran on 2026-08-22, re-verified 2026-08-23 against the ONYX app rebuild.
Firmware **110/110** logic assertions pass, CAD **71/72** geometry checks pass,
and the companion app talks to a simulated wallet end to end. Nothing here
blocks ordering parts.

**Status since first run:** the companion app was rebuilt as the ONYX phone UI
(commits `a874c9a`, `f001353`). A1 below is **fixed** in that rebuild and
re-verified at runtime. Everything under Firmware and CAD is unchanged and
still open — those files were not touched.

---

## App

### A1 — Lock, Unlock and Clear alarm leave the UI showing the opposite of reality
**Severity: high. FIXED in the ONYX rebuild — verified against the simulator on
2026-08-23:** the app now refreshes on `LOCK ok` / `UNLOCK ok` / `CLEAR ok` and on
`EVT lockdown`, and the screen tracks the wallet without a manual refresh
(`UNLOCK` → "Open", `LOCK` → "Locked", shake → "Locked down" with the alarm card
shown). The original report is kept below for the record.

`handleLine()` refreshes the status tiles after `ENROLL ok`, `DELETE`, `DELETEALL`
and four `EVT` types — but not after `LOCK ok`, `UNLOCK ok` or `CLEAR ok`.
The firmware does the right thing every time; the app simply never asks again.

Reproduced in the browser against the simulator:

```
after Unlock button          Asleep/Locked     <- wallet is actually open
after manual Refresh status  Unlocked/Open     <- only now does it catch up
after Lock button            Unlocked/Open     <- wallet is actually shut
after manual Refresh status  Asleep/Locked
```

The same gap hides `EVT lockdown cleared`, so clearing a tamper alarm with your
finger also leaves `LOCKDOWN` on screen.

One line, in [mono_app/index.html](mono_app/index.html:382):

```js
// before
else if (["DELETE","DELETEALL"].includes(head)) send("STATUS");

// after
else if (["DELETE","DELETEALL","LOCK","UNLOCK","CLEAR"].includes(head)) send("STATUS");
```

and add `lockdown` to the `EVT` refresh list on the line above it.

---

## Firmware

The state machines are sound. 110 assertions across the command protocol,
settings clamping and persistence, unlock, relock, lockout, tamper, the
separation leash and non-blocking enrolment all pass, including the awkward
cases: commands split across BLE packets, a 400-character line against a
96-byte buffer, a corrupted settings blob, and a power cycle.

### F1 — `UNLOCK` over the radio opens the wallet with no authentication
**Severity: high (security).** Already flagged in the source, repeated here so it
does not get lost: `startBLE()` requires no pairing or bonding, and `UNLOCK` is
accepted from any connected client. Anything that can reach the radio can open
the wallet without a fingerprint. Fine on the bench, not fine in a pocket.
Delete the branch before carrying it, or gate it behind a bonded connection.

### F2 — A wake line that never clears keeps the wallet awake indefinitely
**Severity: medium (battery).** With the R502 touch line held high — a resting
finger, a damp or dirty sensor — the wallet cycles `ST_SCAN → ST_SLEEP → ST_SCAN`
forever and never reaches the `delay(60)` idle path. Measured: it re-enters
`ST_SCAN` 4 times per 30 seconds, continuously, at full scan current. This is the
same code the POWER NOTE at the bottom of the sketch already promises to replace
with `__WFE()` and GPIO sense; worth doing sooner if the sensor ever sits against
fabric.

### F3 — A freshly enrolled finger is re-scanned immediately
**Severity: low (UX).** `serviceEnroll()` finishes and the main loop scans on the
very next pass, with the finger still on the platen. On real hardware the new
template will usually match and the wallet will pop open the instant you finish
enrolling — surprising, though arguably convenient. If the fresh template does
*not* match cleanly it silently burns attempts toward the 30-second lockout.
Consider requiring a lift before scanning resumes after `ENROLL ok`.

### F5 — The wallet goes deaf for over a second during its own alarms
**Severity: low, but know about it.** `beep()` is `tone()` followed by `delay()`,
so alarm and ring sequences block the main loop: about **1.25 s** for a tamper
(latch move plus six beeps) and **1.6 s** for `RING`. Commands sent during that
window are not dropped — they queue and answer late — but the app looks
unresponsive, and continuous shaking re-triggers the sequence so the wallet can
stay blocked for as long as the shaking lasts.

This is also the reason naive UI timing tests flake: measured 3.5 s after a
shake, the screen can still legitimately be showing the pre-tamper state. It is
worth non-blocking beeps eventually, on the same pass as the `__WFE()` sleep work.

### F4 — Find My is wired but inert
**Severity: none, expected.** `FINDMY_PUBLIC_KEY` is still 28 zero bytes and
`MONO_ENABLE_FINDMY` is 0. The advertisement layout itself is correct — the byte
order, the `0xC0` address masking and the 29-byte payload all verify against the
OpenHaystack reference. Paste a real key and it should work; with the placeholder
it would advertise happily and never produce a report you can decrypt.

---

## CAD

### C1 — `N_CARDS_B` does nothing
**Severity: medium (it is a trap).** `cav_d` is computed at
[mono_wallet_cad.py:213](mono_wallet_cad.py:213) and then never used. The card
pocket depth is actually fixed by `B_BACK_Z` and `B_WALL_BACK`. Proven by
regeneration: raising `N_CARDS_B` from 6 to 12 leaves the pocket at 5.05 mm.

The CONFIG block says "Edit the CONFIG block and re-run", so this will bite
someone. Either wire `cav_d` into the cut, or derive `B_BACK_Z` from it:

```python
B_BACK_Z = -(B_WALL_BACK + N_CARDS_B * CARD_T + 0.4)
```

### C2 — The card mouth is narrower than a card, in both models
**Severity: medium — depends on what you intended.** An ISO/IEC 7810 ID-1 card is
53.98 mm wide. The openings are **45.4 mm** on Model A and **47.5 mm** on Model B.
The pockets themselves are the right size (54.68 mm), so cards fit once they are
*in* — but they can only be loaded by opening the case, and Model A does not open.

If the mouth is a deliberate retention lip and you load cards before screwing
Model B shut, this is correct as drawn and only Model A is odd. If you meant to
slide cards in and out, the mouth needs to be at least 54.4 mm.

### C3 — The servo pocket may leave the servo standing up in a 7 mm bay
**Severity: medium — measure your part.** `B3` builds the servo pocket from
`SERVO_H × SERVO_W` (6.5 × 15.5 mm), which leaves `SERVO_T` = 16.0 mm as the
vertical dimension. `B_BAY_D` is 7.0 mm. Either `SERVO_T` is mislabelled for
your servo, or the pocket wants to be 16.0 × 6.5 mm. The comment at the top of
the MODEL B block assumes 6.5 mm is the depth driver, which suggests the
dimensions are simply named inconsistently — but check before printing B3.

### C4 — Two stale comments
**Severity: cosmetic.**
[mono_wallet_cad.py:130](mono_wallet_cad.py:130) annotates `cav_t` as 4.86 mm; with
`N_CARDS_A = 4` it is 3.34 mm (4.86 is the six-card figure).
[mono_wallet_cad.py:213](mono_wallet_cad.py:213) annotates `cav_d` as 5.72 mm; with
`N_CARDS_B = 6` it is 4.96 mm.

### C5 — Model A walls are 0.76 mm
**Severity: none, already documented.** Under two 0.4 mm perimeters. The
docstring calls it fragile and it is; force 2 perimeters and treat it as a
looks-only shell, which is what it is for.

---

## What passed that is worth knowing passed

- All six STLs are watertight closed volumes with sane topology and no
  degenerate faces. They will slice.
- Both models hit their stated envelopes exactly: Model A closes at 7.20 mm
  (the GLB figure), Model B at 16.50 mm.
- The component tray drops into the bay with 0.20 mm per side, and the LiPo,
  XIAO and servo pockets all sit inside the bay footprint without colliding
  with each other or with the R502 barrel. The tightest clearance is the XIAO
  at 1.60 mm from the sensor barrel.
- Screw heads stop at y = 6.10 mm, 0.10 mm clear of the card pocket. Tight, but
  it is on the right side of the line.
- Settings survive a power cycle, and a corrupted settings blob falls back to
  defaults rather than loading garbage.
- The separation leash fires its quiet warning and its full alert at the
  configured times and switches the radio to the fast beacon only on the alert.
- Enrolment is genuinely non-blocking: BLE stays responsive throughout, which
  was the point of the v2 rewrite.

## Housekeeping

An empty 0-byte file named `git` is committed at the repo root — almost
certainly a mistyped redirect. Safe to remove:

```bash
git rm --cached git && rm git
```

## What none of this can tell you

Real BLE range and timing, R502 match quality against actual fingers, servo
torque against a real latch, current draw, and print fit after shrinkage.
