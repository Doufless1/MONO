# Security

MONO is a prototype, and this document says plainly what it defends against
and what it does not. The short version: **v3 closes the remote attacks a
thief with a phone or a laptop could mount** — v2 opened for anyone in radio
range — and **leaves the physical ones**, which need hardware this prototype
does not have.

## What is worth protecting

| Asset | Where it lives |
|---|---|
| The cards inside | the wallet, behind a servo-driven latch |
| The power to open it | enrolled fingerprints (on the R502) and paired phones' keys |
| The fingerprints themselves | the R502's own flash, as templates |
| Where you lost it | the phone, only if you turn it on |

## Who might try

1. **A thief holding the wallet**, with a phone or laptop running nRF Connect.
2. **A thief with a USB-C cable** — the port is on the outside of the case.
3. **Someone nearby while you use it**, listening to the radio.
4. **Someone who wants the alarm to stay quiet** while they walk off with it.

## What v3 stops

| Attack | v2 | v3 | Test |
|---|---|---|---|
| Send `UNLOCK` from any phone in range | opened | `ERR auth UNLOCK` — only a paired phone that has answered a fresh challenge | *an unpaired phone cannot UNLOCK* |
| Retune the latch so it never closes (`SET servolock 95`) | worked | needs trust | *an unpaired phone cannot retune the latch* |
| Enrol your own finger over the air, or unpair the owner | worked | needs trust | *an unpaired phone cannot enrol a finger of its own* |
| Pair your own phone to someone's wallet | — | needs **the owner's finger on the sensor** | *a stranger's finger does not confirm it* |
| Record an `AUTH` and replay it later | — | each answer binds a fresh 128-bit nonce, good once | *a recorded AUTH replayed against a new nonce fails* |
| Guess a key | — | 128-bit keys, 256-bit MACs, constant-time compare, one guess per nonce | *a key one bit wrong is refused* |
| Brute-force fingers during a tamper alarm | unlimited | the same lockout everywhere | *wrong fingers during lockdown count toward a lockout* |
| Silence the walk-away alarm by connecting | any connection did it | only the paired phone, authenticated | *a stranger connecting does not silence the alarm* |
| Type `UNLOCK` on the USB console | opened | trusted only while factory-fresh, or for a minute after the owner's finger | *the USB port on the outside of the wallet cannot UNLOCK it* |
| Read the pairing key off the USB console | — | never printed there, not even in a bench build | *the key never appears on the USB console* |
| Read a key over someone's shoulder | — | the app's console shows `key=••••` | — |

### The cryptography

- **Keys**: 128 bits per phone, from the nRF52's RNG peripheral (thermal
  noise, through the SoftDevice's pool), up to four phones.
- **Challenge**: 128 fresh random bits per `HELLO`; consumed by the first
  `AUTH`, right or wrong.
- **Answer**: `HMAC-SHA256(key, "MONO-AUTH-1" ‖ nonce)` — the tag separates
  this use of the key from any future one. The wallet's SHA-256 is checked
  against the NIST and RFC 4231 vectors; the app uses WebCrypto; the
  integration tests prove the two agree byte for byte.
- **Failure is safe**: no randomness, no key and no nonce (`PAIR fail rng`).

## What it does not stop

Said plainly, because a security claim without its limits is not one.

- **Sniffing the pairing.** By default the BLE link is not encrypted, so the
  key in `PAIR ok` crosses the air in the clear **once**, during the 20 s the
  owner is confirming. Build with `-DMONO_LINK_ENCRYPTION=1` and the UART
  service refuses unencrypted links: the phone's OS pairs with LE Secure
  Connections first, which defeats passive sniffing. It is "Just Works", so an
  attacker actively in the middle at that moment could still interpose.
- **Keys at rest.** The phone keeps its key in `localStorage`, not a secure
  enclave; anything that can run script on the app's origin can read it. The
  wallet keeps phone keys in the nRF52's internal flash. Neither is a secure
  element.
- **Templates.** The R502 stores fingerprint templates in ordinary flash on
  the module.
- **Opening the case.** Four screws. Inside, the UART between the R502 and the
  XIAO is unauthenticated, so a replaced sensor could answer "match"; the servo
  could be driven directly. A real product needs a sealed, tamper-evident
  enclosure and a secure element.
- **Reading the firmware.** APPROTECT is not enabled; a debugger on SWD can
  read the flash, keys included.
- **Spoofed fingers.** Gelatin and latex fakes, against which the R502's
  anti-spoofing is limited.
- **Relaying.** An attacker relaying radio between your phone and your wallet
  over a distance.
- **A bench build.** `MONO_BENCH=1` trusts the USB console unconditionally:
  never carry one. The boot banner says so.

## Hardening, in order of value

1. Build with `MONO_LINK_ENCRYPTION=1` once you have checked your phone shows
   its pairing dialog.
2. Enable APPROTECT on the nRF52 for any build you carry.
3. Power the R502's main supply from a load switch, so the UART is dead while idle.
4. Replace the phone-side key store with a platform keystore (a native app).
5. Add a secure element (an ATECC608 is the usual choice) for keys and an
   authenticated sensor.

Found something? Open an issue — this is a prototype, and a report is the point.
