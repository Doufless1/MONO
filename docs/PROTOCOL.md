# The MONO line protocol, version 3

Plain text lines, so the whole wallet can be driven from a serial monitor, from
nRF Connect, or from the `mono_sim` terminal. The ONYX app speaks exactly this
and nothing else.

## Framing

- **Transport.** The Nordic UART Service: service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`,
  phone → wallet on RX `6e400002-…`, wallet → phone by notification on TX `6e400003-…`.
  The USB serial console (115200 baud) speaks the same lines.
- **Lines.** ASCII, terminated by `\n`; `\r` is ignored. At most 95 characters:
  a longer line is refused whole with `ERR toolong` — never truncated and run.
  USB and BLE each have their own line buffer, so a line split across BLE
  packets reassembles, and the two ports never corrupt each other.
- **Commands** are case-insensitive; arguments are separated by spaces.
- **Every reply starts with the command's name** (`STATUS …`, `SET ok …`).
  A reply goes to the port that asked. Events and pushed `STATUS` go to the USB
  console and, once it has authenticated, to the phone.
- **Errors:** `ERR unknown <CMD>`, `ERR auth <CMD>` (needs a trusted session),
  `ERR toolong`.

## Sessions and trust

A phone is **trusted** once it has proved it holds a key the wallet issued.
Until then it may only use `HELP`, `HELLO`, `AUTH` and `PAIR`.

```
→ HELLO
← HELLO v=3 fw=3.0.0 phones=1 setup=0 nonce=8204fc013a05f3e70a531376271aa501
→ AUTH 6f1c…(64 hex digits)
← AUTH ok slot=0
← EVT linked
← STATUS latch=locked sensor=ready alarm=0 enroll=idle fingers=2 vbat=4.05 link=1 auth=1 pair=0 setup=0 leash=ok
```

**The answer to a challenge** is `HMAC-SHA256(key, "MONO-AUTH-1" ‖ nonce)`,
with `nonce` the 16 bytes behind the hex in `HELLO`, written as 64 lowercase
hex digits. A nonce is good for **one** answer, right or wrong; `HELLO` again
for another. A recorded `AUTH` is useless against any later nonce.

**Pairing** a new phone:

| Situation | `PAIR` replies |
|---|---|
| Factory-fresh (nothing enrolled, nobody paired) | `PAIR ok slot=0 key=<32 hex>` at once — the first phone owns it |
| Owned | `PAIR wait`, then — once the owner's finger touches the sensor — `PAIR ok slot=N key=<32 hex>`, or after 20 s `PAIR fail timeout` |
| Four phones already paired | `PAIR fail full` |
| No sensor, no randomness, or asked over USB | `PAIR fail nosensor` · `PAIR fail rng` · `PAIR fail port` |

The confirming touch does not open the wallet. The key is sent to the phone
that asked, once, and never echoed to the USB console.

**The USB console** is trusted only while the wallet is factory-fresh, or for
a minute after the owner's finger opened it — the USB-C port is on the outside
of the wallet. A bench build (`-DMONO_BENCH=1`) trusts it always.

## Commands

| Command | Trust | Replies |
|---|:---:|---|
| `HELP` | open | `HELP STATUS GET SET …` — every command, in this order |
| `HELLO` | open | `HELLO v=3 fw=<version> phones=<n> setup=<0\|1> nonce=<32 hex \| ->` |
| `AUTH <64 hex>` | open | `AUTH ok slot=<n>` · `AUTH fail` · `AUTH fail nonce` · `AUTH fail format` · `AUTH fail port` |
| `PAIR` | open | see above |
| `STATUS` | ✓ | `STATUS …` — the fields below; re-reads the battery |
| `GET` | ✓ | `GET relock=… tamper=… sepwarn=… sepalert=… maxfails=… servolock=… servorel=… findmy=… phones=…` |
| `SET <key> <value>` | ✓ | `SET ok <key>=<value as stored>` · `SET fail noarg` · `SET fail noval` · `SET fail badval <key>` · `SET fail unknown <key>` |
| `LIST` | ✓ | `LIST count=<n> ids=<id,id,…>` |
| `ENROLL` | ✓ | `ENROLL start id=<n>`, then progress events (below) · `ENROLL busy` · `ENROLL fail full` · `ENROLL fail nosensor` |
| `CANCEL` | ✓ | `CANCEL ok` — abandons an enrolment |
| `DELETE <id>` | ✓ | `DELETE ok id=<n>` · `DELETE fail id=<n>` · `DELETE fail badid` · `DELETE fail noarg` |
| `DELETEALL` | ✓ | `DELETEALL ok` · `DELETEALL fail` |
| `RING` | ✓ | `RING ok`, then eight beeps |
| `LOCK` | ✓ | `LOCK ok` — closes the latch; leaves an alarm or a lockout alone |
| `UNLOCK` | ✓ | `UNLOCK ok` — opens for the relock time; also ends an alarm |
| `CLEAR` | ✓ | `CLEAR ok` — alarm off, lockout over, walk-away leash stood down |
| `UNPAIR` | ✓ | `UNPAIR ok` — forgets every phone, including the one that asked |
| `BYE` | ✓ | `BYE ok` — the next disconnect is a goodbye, not a walk-away |

## STATUS

Pushed to the trusted phone whenever any of it changes (the battery in 50 mV
steps), and sent on request.

| Field | Values | Meaning |
|---|---|---|
| `latch` | `locked` · `open` | The servo's position |
| `sensor` | `ready` · `scan` · `reject` · `lockout` | What the fingerprint path is doing |
| `alarm` | `0` · `1` | Tamper lockdown: locked, beeping in bursts, fast-beaconing |
| `enroll` | `idle` · `place` · `lift` · `again` | Enrolment step |
| `fingers` | `0`–`10` | Templates in MONO's slots 1–10 |
| `vbat` | volts, 2 dp | Battery |
| `link` | `0` · `1` | A phone is connected |
| `auth` | `0` · `1` | … and has proved who it is |
| `pair` | `0` · `1` | A pairing request is waiting for the owner's finger |
| `setup` | `0` · `1` | Factory-fresh: the next `PAIR` succeeds at once |
| `leash` | `off` · `ok` · `lost` · `warn` · `alert` | The walk-away alarm |

## Settings

| Key | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `relock` | 1000–60000 | ms | 6000 | How long the latch stays open |
| `tamper` | 500–8000 | mg | 2500 | Knock threshold, away from 1 g |
| `sepwarn` | 0, 5–3600 | s | 20 | Link lost this long → quiet warning. **0 switches the leash off** |
| `sepalert` | 10–7200 | s | 60 | → full alert and fast beacon (always after `sepwarn`) |
| `maxfails` | 1–20 | tries | 5 | Wrong fingers in a row before a 30 s lockout |
| `servolock` | 0–180 | ° | 20 | Latch closed; the servo moves at once, for tuning |
| `servorel` | 0–180 | ° | 95 | Latch open |
| `findmy` | 0 · 1 | | 0 | Time-slice the Find My beacon (needs `MONO_ENABLE_FINDMY`) |

Values outside the range are clamped, and the reply says what was stored.
Settings survive a battery pull: a CRC-checked blob on the nRF52's flash.

## Events

| Line | When |
|---|---|
| `EVT unlocked id=<n>` | An enrolled finger opened the wallet |
| `EVT rejected` | A readable finger that is not enrolled |
| `EVT relocked` | The latch closed on its own |
| `EVT lockout` · `EVT lockout end` | Too many wrong fingers · the 30 s are up |
| `EVT tamper` | Three separate knocks inside 1.5 s: lockdown |
| `EVT lockdown cleared` | The owner's finger, `UNLOCK` or `CLEAR` ended it |
| `EVT separation warn` · `EVT separation alert` | The paired phone's link was lost |
| `EVT linked` | The paired phone authenticated |
| `EVT paired slot=<n>` | A phone was paired |
| `ENROLL place` · `ENROLL lift` · `ENROLL again` | Enrolment progress |
| `ENROLL ok id=<n>` · `ENROLL fail <reason>` | `timeout` · `bad_image` · `no_match` · `store` |

## Changes from protocol 2

| v2 | v3 |
|---|---|
| Everything open to any client | Trusted sessions: `HELLO`, `AUTH`, `PAIR`, `UNPAIR`, `BYE` |
| `STATUS state=2 … lockdown=0 enroll=1` (numbers) | Named fields: `latch=open sensor=ready alarm=0 enroll=place …` |
| Clients polled `STATUS` after events | `STATUS` is pushed on every change |
| `LIST count=2` | `LIST count=2 ids=1,3` |
| `SET ok relock=999999` (the request) | `SET ok relock=60000` (what was stored) |
| `HELP` replied `CMDS …` | `HELP …` |
| Overlong lines truncated and run | `ERR toolong` |
| `sepwarn` could not be switched off | `sepwarn 0` |
