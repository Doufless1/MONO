/* ============================================================================
   MONO WALLET  -  prototype firmware  v3
   ----------------------------------------------------------------------------
   Target : Seeed XIAO nRF52840 Sense
   Sensor : GROW R502-A capacitive fingerprint module, UART 57600
   Latch  : micro servo (1.7 g class) driving a blocking pin

   NEW IN v3  (docs/REVIEW.md has the evidence and the test behind each line)
     - Only a paired phone can open the wallet over the air. A phone pairs by
       asking, then the owner touches the sensor to say yes. v2 opened for
       anything that could reach the radio.
     - Nothing blocks. Beeps, LED patterns and servo moves are scheduled, so a
       command is answered within one loop pass even in the middle of an alarm.
     - The wallet pushes STATUS whenever its state changes, so the app shows
       what the wallet is doing instead of guessing which replies mean a refresh.
     - A new finger goes into the first free slot. v2 used count+1, which
       overwrote someone else's finger after any delete.
     - The fast alarm beacon is derived from the alarm, so it always ends when
       the alarm does. v2 could leave it on until the battery went flat.
     - Walking away and hanging up are told apart: only a lost link arms the
       separation alarm, and only the paired phone coming back disarms it.
     - Knocks are sampled at a fixed 50 Hz, so the force it takes to raise the
       alarm no longer depends on what the main loop happens to be doing.
     - A finger resting on the sensor is scanned once, not forever.
     - USB and BLE each get their own line buffer, and an overlong line is
       refused instead of being truncated and run.

   BOARD SETUP  (Arduino IDE)
     1. Preferences > Additional Board Manager URLs:
        https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
     2. Boards Manager: install "Seeed nRF52 Boards"
     3. Board: "Seeed XIAO nRF52840 Sense"  (the NON-mbed entry)
     4. Libraries: "Adafruit Fingerprint Sensor Library", "Seeed Arduino LSM6DS3"

   Wiring, pins and every tunable: config.h.   Line protocol: docs/PROTOCOL.md.

   SECURITY REALITY
     The R502 keeps its templates in ordinary flash on the module, and the
     phone keys live in the nRF52's internal flash. Neither is a secure
     element. This is a faithful functional mock of a biometric wallet, not a
     secure one; docs/SECURITY.md says exactly what it stops and what it does not.
   ========================================================================= */

#include <bluefruit.h>
#include <Adafruit_Fingerprint.h>
#include <Servo.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrf_soc.h>
#include "LSM6DS3.h"
#include "Wire.h"
#include "config.h"
#include "sha256.h"
#include "findmy.h"

using namespace Adafruit_LittleFS_Namespace;

#define SETTINGS_FILE  "/mono.cfg"
#define SETTINGS_MAGIC 0x4D4F4E33UL     // "MON3"

// ================================================================= types ==
// Every type lives above the first function: the Arduino IDE generates
// prototypes there, and they cannot name a type declared further down.

enum Port       : uint8_t { PORT_USB, PORT_BLE, PORT_COUNT };
enum SensorMode : uint8_t { SEN_READY, SEN_SCAN, SEN_REJECT, SEN_LOCKOUT };
enum Enroll     : uint8_t { EN_IDLE, EN_PLACE, EN_LIFT, EN_AGAIN };
enum Prio       : uint8_t { PRIO_CHIRP, PRIO_RING, PRIO_ALARM };
enum RadioMode  : uint8_t { RADIO_APP, RADIO_FINDMY };
enum Need       : uint8_t { NEED_NOTHING, NEED_TRUST };

// Persisted. Layout changes need a new SETTINGS_MAGIC.
struct Settings {
  uint32_t magic;
  uint16_t size;            // sizeof(Settings) when written: catches layout drift
  uint16_t releaseHoldMs;   // how long the latch stays open
  uint16_t tamperMilliG;    // knock threshold, milli-g away from 1 g
  uint16_t sepWarnS;        // link lost this long -> quiet warning; 0 = leash off
  uint16_t sepAlertS;       // -> full alert + fast beacon
  uint8_t  maxFails;
  uint8_t  servoLocked;     // degrees
  uint8_t  servoReleased;   // degrees
  uint8_t  findmyEnabled;
  uint8_t  phones;          // bit i set = phoneKey[i] holds a paired phone
  uint8_t  phoneKey[MAX_PHONES][PHONE_KEY_LEN];
  uint16_t crc;             // CRC-16/CCITT of everything above
};

struct Note        { uint16_t hz, onMs, offMs; };
struct Player      { const Note *seq; uint8_t len, idx, prio; bool on; uint32_t nextAt; };
struct LineBuf     { char buf[LINE_MAX]; uint8_t len; bool overflow; };
struct Session     { bool authed, nonceLive, bye; uint8_t nonce[NONCE_LEN]; };
struct LatchState  { bool open, powered; uint32_t relockAt, powerOffAt; };
struct TamperState { bool above; uint8_t knocks; uint32_t firstKnockAt, nextSampleAt; };
struct AlarmState  { bool on; uint32_t lastShakeAt, nextBurstAt; };
struct LeashState  { bool armed, warned, alerted; uint32_t lostAt; };
struct PairRequest { bool pending; uint32_t until; };
struct RadioState  { bool started; RadioMode mode; uint16_t interval; uint32_t sliceAt; };
struct Flash       { uint32_t start, until; uint16_t period; };
struct Snapshot    { uint8_t latch, sensor, alarm, enroll, fingers, link, auth, pair, setup, leash, batt; };

// Everything the wallet knows at runtime, in one place. A power cycle is
// `mono = MonoState()`; that is exactly what the test harness does.
struct MonoState {
  SensorMode  sensor = SEN_READY;
  uint32_t    sensorSince = 0;
  bool        wakeArmed = true;     // false until the finger has left the sensor
  bool        sensorOk = false;
  uint8_t     failCount = 0;
  uint32_t    slots = 0;            // bit (id-1) set = template id is in use

  Enroll      enroll = EN_IDLE;
  uint32_t    enrollSince = 0;
  uint8_t     enrollId = 0;

  LatchState  latch = {};
  TamperState tamper = {};
  AlarmState  alarm = {};
  LeashState  leash = {};
  PairRequest pair = {};
  RadioState  radio = {};
  Player      sound = {};
  Flash       flash = {};
  bool        ledLit = false;

  LineBuf     rx[PORT_COUNT] = {};
  Port        port = PORT_USB;      // where the command being handled came from
  Session     ble = {};
  uint32_t    ownerUntil = 0;       // USB console trusted until then

  uint16_t    vbatMv = 0;
  uint32_t    vbatAt = 0;
  Snapshot    pushed = {};
  bool        pushedOnce = false;
};

// Written from the BLE stack's callbacks, applied by loop(). Keeping the
// callbacks to a flag each means no state is shared across tasks.
struct LinkEvents { volatile bool connected, disconnected; volatile uint8_t reason; };

typedef void (*CommandFn)(char *arg);
struct Command { const char *name; Need need; CommandFn fn; };

// ============================================================== objects ==
Settings   cfg;
MonoState  mono;
LinkEvents linkEv = {};

Adafruit_Fingerprint finger(&Serial1);
Servo          servo;
LSM6DS3        imu(I2C_MODE, 0x6A);
BLEUart        bleuart;
ble_gap_addr_t identityAddr;      // ours; Find My borrows a different one

// Defined further down, used before.
static void onMatch(int16_t id);
static void pairComplete();
static void alarmEnd();

// ============================================================== helpers ==
static inline bool reached(uint32_t deadline) { return (int32_t)(millis() - deadline) >= 0; }

static uint32_t msUntil(uint32_t deadline) {
  int32_t d = (int32_t)(deadline - millis());
  return d > 0 ? (uint32_t)d : 0;
}

static long clampL(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint8_t popcount32(uint32_t v) {
  uint8_t n = 0;
  while (v) { v &= v - 1; n++; }
  return n;
}

static void upcase(char *s)   { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
static void downcase(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

// Whole-string decimal only: "12" yes, "12x", "" and "banana" no.
static bool parseLong(const char *s, long *out) {
  if (!s || !*s) return false;
  char *end = nullptr;
  long v = strtol(s, &end, 10);
  if (*end) return false;
  *out = v;
  return true;
}

static void toHex(const uint8_t *b, uint8_t n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (uint8_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
  out[2 * n] = 0;
}

static int8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool fromHex(const char *s, uint8_t *out, uint8_t n) {
  if (!s || strlen(s) != (size_t)n * 2) return false;
  for (uint8_t i = 0; i < n; i++) {
    int8_t hi = hexNibble(s[2 * i]), lo = hexNibble(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

// The SoftDevice keeps a pool of bytes from the RNG peripheral (thermal noise).
static bool randomBytes(uint8_t *out, uint8_t n) {
  uint32_t t0 = millis();
  while (n) {
    uint8_t avail = 0;
    sd_rand_application_bytes_available_get(&avail);
    uint8_t take = avail < n ? avail : n;
    if (take && sd_rand_application_vector_get(out, take) == NRF_SUCCESS) {
      out += take; n -= take;
      continue;
    }
    if (millis() - t0 > 250) return false;
    delay(1);                       // the pool refills in microseconds
  }
  return true;
}

static uint8_t fingerCount() { return popcount32(mono.slots); }
static uint8_t phoneCount()  { return popcount32(cfg.phones); }

// Factory-fresh: nothing enrolled and nobody paired, so there is nothing to
// protect yet and the first phone to PAIR becomes the owner. A wallet whose
// sensor is missing is never factory-fresh, or unplugging it would reset it.
static bool setupMode() { return mono.sensorOk && fingerCount() == 0 && cfg.phones == 0; }

// ============================================================== output ==
static void writeLine(Port p, const char *s) {
  if (p == PORT_USB) { Serial.println(s); return; }
  if (Bluefruit.connected()) { bleuart.print(s); bleuart.print('\n'); }
}

// Answers the command being handled, on the port it arrived on.
static void reply(const char *fmt, ...) {
  char b[OUT_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  writeLine(mono.port, b);
#if MONO_BENCH
  if (mono.port == PORT_BLE) { Serial.print("ble< "); Serial.println(b); }
#endif
}

// Tells everyone who may know: the USB console, and the phone once it has
// proved it is the paired one.
static void notify(const char *fmt, ...) {
  char b[OUT_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  writeLine(PORT_USB, b);
  if (mono.ble.authed) writeLine(PORT_BLE, b);
}

// ============================================================ settings ==
static uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*p++) << 8;
    for (uint8_t i = 0; i < 8; i++)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static uint16_t settingsCrc(const Settings &s) {
  return crc16((const uint8_t *)&s, offsetof(Settings, crc));
}

static void settingsDefaults() {
  memset(&cfg, 0, sizeof cfg);      // padding too, so the CRC is reproducible
  cfg.magic         = SETTINGS_MAGIC;
  cfg.size          = sizeof(Settings);
  cfg.releaseHoldMs = 6000;
  cfg.tamperMilliG  = 2500;
  cfg.sepWarnS      = 20;
  cfg.sepAlertS     = 60;
  cfg.maxFails      = 5;
  cfg.servoLocked   = 20;
  cfg.servoReleased = 95;
  cfg.findmyEnabled = 0;
  cfg.phones        = 0;
}

// Defence in depth: a blob that passed its CRC is still clamped before use.
static void settingsSanitize() {
  cfg.releaseHoldMs = (uint16_t)clampL(cfg.releaseHoldMs, 1000, 60000);
  cfg.tamperMilliG  = (uint16_t)clampL(cfg.tamperMilliG, 500, 8000);
  if (cfg.sepWarnS) cfg.sepWarnS = (uint16_t)clampL(cfg.sepWarnS, 5, 3600);
  cfg.sepAlertS     = (uint16_t)clampL(cfg.sepAlertS, 10, 7200);
  cfg.maxFails      = (uint8_t)clampL(cfg.maxFails, 1, 20);
  cfg.servoLocked   = (uint8_t)clampL(cfg.servoLocked, 0, 180);
  cfg.servoReleased = (uint8_t)clampL(cfg.servoReleased, 0, 180);
  cfg.findmyEnabled = cfg.findmyEnabled ? 1 : 0;
  cfg.phones       &= (uint8_t)((1u << MAX_PHONES) - 1);
}

static void settingsSave() {
  cfg.magic = SETTINGS_MAGIC;
  cfg.size  = sizeof(Settings);
  cfg.crc   = settingsCrc(cfg);
  InternalFS.remove(SETTINGS_FILE);
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_WRITE)) {
    f.write((const uint8_t *)&cfg, sizeof cfg);
    f.close();
  }
}

// Anything short, foreign, torn or corrupt falls back to the defaults.
static bool settingsLoad() {
  settingsDefaults();
  File f(InternalFS);
  if (!f.open(SETTINGS_FILE, FILE_O_READ)) return false;
  Settings tmp;
  int got = f.read((uint8_t *)&tmp, sizeof tmp);
  f.close();
  if (got != (int)sizeof tmp || tmp.magic != SETTINGS_MAGIC ||
      tmp.size != sizeof(Settings) || tmp.crc != settingsCrc(tmp)) return false;
  cfg = tmp;
  settingsSanitize();
  return true;
}

// =============================================================== sound ==
// The buzzer plays one pattern at a time and never with delay(). An alarm
// outranks a ring, which outranks a chirp, so nothing important is cut short.
static const Note P_ACCEPT[]    = { {2600, 80, 0} };
static const Note P_REJECT[]    = { {400, 220, 0} };
static const Note P_STEP[]      = { {1800, 60, 0} };
static const Note P_SAVED[]     = { {2400, 120, 0} };
static const Note P_FAILED[]    = { {400, 250, 0} };
static const Note P_PAIRING[]   = { {1800, 70, 70}, {2400, 70, 0} };
static const Note P_PAIRED[]    = { {2400, 90, 60}, {3000, 140, 0} };
static const Note P_SEP_WARN[]  = { {2000, 300, 0} };
static const Note P_SEP_ALERT[] = { {2600, 200, 120}, {2600, 200, 120}, {2600, 200, 0} };
static const Note P_TAMPER[]    = { {3200, 90, 60}, {3200, 90, 60}, {3200, 90, 60},
                                    {3200, 90, 60}, {3200, 90, 60}, {3200, 90, 0} };
static const Note P_RING[]      = { {3000, 120, 80}, {3000, 120, 80}, {3000, 120, 80},
                                    {3000, 120, 80}, {3000, 120, 80}, {3000, 120, 80},
                                    {3000, 120, 80}, {3000, 120, 0} };
#define PLAY(p, prio) play(p, (uint8_t)(sizeof(p) / sizeof((p)[0])), prio)

static void soundStop() {
  if (mono.sound.on) noTone(PIN_BUZZER);
  mono.sound = Player();
}

static void play(const Note *seq, uint8_t len, uint8_t prio) {
  if (mono.sound.seq && mono.sound.prio > prio) return;
  soundStop();
  mono.sound.seq = seq; mono.sound.len = len; mono.sound.prio = prio;
  mono.sound.nextAt = millis();
}

static void serviceSound() {
  Player &s = mono.sound;
  if (!s.seq || !reached(s.nextAt)) return;
  if (!s.on) {
    if (s.idx >= s.len) { s = Player(); return; }
    tone(PIN_BUZZER, s.seq[s.idx].hz);
    s.on = true;
    s.nextAt = millis() + s.seq[s.idx].onMs;
  } else {
    noTone(PIN_BUZZER);
    s.on = false;
    s.nextAt = millis() + s.seq[s.idx].offMs;
    s.idx++;
  }
}

// ================================================================= LED ==
static void ledFlash(uint8_t n, uint16_t ms) {
  mono.flash.start = millis();
  mono.flash.period = ms;
  mono.flash.until = millis() + 2UL * n * ms;
}

// One function decides the LED from the state, so it can never disagree
// with it. Most urgent first.
static bool ledWanted() {
  uint32_t now = millis();
  if (mono.flash.until && !reached(mono.flash.until))
    return ((now - mono.flash.start) / mono.flash.period) % 2 == 0;
  if (mono.alarm.on)                return (now / 250) % 2 == 0;
  if (mono.pair.pending)            return (now / 120) % 4 == 0;
  if (mono.enroll == EN_LIFT)       return false;
  if (mono.enroll != EN_IDLE)       return (now / 600) % 2 == 0;
  uint32_t in = now - mono.sensorSince;
  if (mono.sensor == SEN_SCAN)      return true;
  if (mono.sensor == SEN_REJECT)    return (in / 150) % 2 == 0;
  if (mono.sensor == SEN_LOCKOUT)   return (in / 500) % 2 == 0;
  return mono.latch.open;
}

static void serviceLed() {
  bool on = ledWanted();
  if (on != mono.ledLit) { digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW); mono.ledLit = on; }
}

// =========================================================== mechanism ==
// The servo is powered for one settle time per move and then detached, so it
// draws nothing while idle, and the move never stalls the loop.
static void servoTo(uint8_t angle) {
  if (!mono.latch.powered) { servo.attach(PIN_SERVO); mono.latch.powered = true; }
  servo.write(angle);
  mono.latch.powerOffAt = millis() + SERVO_SETTLE_MS;
}

static void latchOpen() {
  mono.latch.open = true;
  mono.latch.relockAt = millis() + cfg.releaseHoldMs;
  servoTo(cfg.servoReleased);
}

static void latchClose() {
  mono.latch.open = false;
  servoTo(cfg.servoLocked);
}

// The relock timer runs whatever else is going on - v2 froze it during an
// enrolment and left the wallet open.
static void serviceLatch() {
  if (mono.latch.open && reached(mono.latch.relockAt)) { latchClose(); notify("EVT relocked"); }
  if (mono.latch.powered && reached(mono.latch.powerOffAt)) {
    servo.detach();
    mono.latch.powered = false;
  }
}

// ========================================================= fingerprint ==
#define MATCH_NONE   (-1)   // nothing readable on the sensor
#define MATCH_RETRY  (-2)   // a finger, but no fair verdict (poor image, UART hiccup)
#define MATCH_REJECT (-3)   // a readable finger that is not enrolled

static int16_t tryMatch() {
  if (finger.getImage() != FINGERPRINT_OK) return MATCH_NONE;
  if (finger.image2Tz(1) != FINGERPRINT_OK) return MATCH_RETRY;
  uint8_t r = finger.fingerFastSearch();
  if (r == FINGERPRINT_OK) return (int16_t)finger.fingerID;
  return r == FINGERPRINT_NOTFOUND ? MATCH_REJECT : MATCH_RETRY;
}

static uint32_t slotBit(long id) { return 1UL << (id - 1); }

// The R502 can say how many templates it holds but not which ids, and count+1
// is not a free id once anything has been deleted. Probe ids 1..MAX_FINGERS
// once at boot; after that every store and delete keeps the map current.
static void slotsRefresh() {
  mono.slots = 0;
  for (uint8_t id = 1; id <= MAX_FINGERS; id++)
    if (finger.loadModel(id) == FINGERPRINT_OK) mono.slots |= slotBit(id);
}

static int16_t firstFreeSlot() {
  for (uint8_t id = 1; id <= MAX_FINGERS; id++)
    if (!(mono.slots & slotBit(id))) return id;
  return -1;
}

// Non-blocking enrolment: every step reports as it happens.
static bool enrolling() { return mono.enroll != EN_IDLE; }

static void enrollStep(Enroll e) { mono.enroll = e; mono.enrollSince = millis(); }

// The finger that just enrolled is still on the sensor; make it lift before
// the next scan, or the wallet pops open the moment you finish (v2 did).
static void enrollEnd() { enrollStep(EN_IDLE); mono.wakeArmed = false; }

static void enrollFail(const char *why) {
  notify("ENROLL fail %s", why);
  enrollEnd();
  PLAY(P_FAILED, PRIO_CHIRP);
}

static void enrollBegin() {
  if (enrolling())    { reply("ENROLL busy"); return; }
  if (!mono.sensorOk) { reply("ENROLL fail nosensor"); return; }
  int16_t id = firstFreeSlot();
  if (id < 0)         { reply("ENROLL fail full"); return; }
  mono.enrollId = (uint8_t)id;
  notify("ENROLL start id=%u", mono.enrollId);
  notify("ENROLL place");
  enrollStep(EN_PLACE);
}

static void serviceEnroll() {
  if (!enrolling()) return;
  if (millis() - mono.enrollSince > ENROLL_STEP_MS) { enrollFail("timeout"); return; }

  switch (mono.enroll) {
    case EN_PLACE:
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) { enrollFail("bad_image"); return; }
      PLAY(P_STEP, PRIO_CHIRP);
      ledFlash(1, 100);
      notify("ENROLL lift");
      enrollStep(EN_LIFT);
      break;

    case EN_LIFT:
      if (finger.getImage() != FINGERPRINT_NOFINGER) return;
      notify("ENROLL again");
      enrollStep(EN_AGAIN);
      break;

    case EN_AGAIN:
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(2) != FINGERPRINT_OK)             { enrollFail("bad_image"); return; }
      if (finger.createModel() != FINGERPRINT_OK)           { enrollFail("no_match"); return; }
      if (finger.storeModel(mono.enrollId) != FINGERPRINT_OK) { enrollFail("store"); return; }
      mono.slots |= slotBit(mono.enrollId);
      notify("ENROLL ok id=%u", mono.enrollId);
      PLAY(P_SAVED, PRIO_CHIRP);
      enrollEnd();
      break;

    default: break;
  }
}

// ============================================================== sensor ==
static void sensorEnter(SensorMode s) { mono.sensor = s; mono.sensorSince = millis(); }

static void serviceSensor() {
  bool touched = digitalRead(PIN_FP_WAKE) == HIGH;
  if (!touched) mono.wakeArmed = true;          // a lifted finger re-arms the sensor
  if (enrolling() || !mono.sensorOk) return;    // enrolment owns the sensor

  uint32_t in = millis() - mono.sensorSince;
  switch (mono.sensor) {

    case SEN_READY:
      if (touched && mono.wakeArmed && !mono.latch.open) sensorEnter(SEN_SCAN);
      break;

    case SEN_SCAN: {
      int16_t r = tryMatch();
      if (r >= 0) {
        mono.wakeArmed = false;
        sensorEnter(SEN_READY);
        onMatch(r);
      } else if (r == MATCH_REJECT) {
        mono.wakeArmed = false;
        sensorEnter(SEN_REJECT);
        notify("EVT rejected");
        PLAY(P_REJECT, PRIO_CHIRP);
      } else if (!touched || in > SCAN_WINDOW_MS) {
        // A wake line stuck high (a damp sensor, a resting thumb) is scanned
        // for one window and then ignored until it clears. v2 rescanned it
        // forever at full current.
        if (touched) mono.wakeArmed = false;
        sensorEnter(SEN_READY);
      }
      break;
    }

    case SEN_REJECT:
      // Wait for the finger to leave, so one resting thumb costs one try.
      if (!touched || in > REJECT_HOLD_MS) {
        if (++mono.failCount >= cfg.maxFails) { sensorEnter(SEN_LOCKOUT); notify("EVT lockout"); }
        else                                  sensorEnter(SEN_READY);
      }
      break;

    case SEN_LOCKOUT:
      if (in > LOCKOUT_MS) { mono.failCount = 0; sensorEnter(SEN_READY); notify("EVT lockout end"); }
      break;
  }
}

// ======================================================== alarm/tamper ==
static void alarmTrigger() {
  if (!mono.alarm.on) {
    mono.alarm.on = true;
    latchClose();                     // whatever was going on, the pin goes in
    notify("EVT tamper");
  }
  mono.alarm.lastShakeAt = millis();  // more shaking keeps it loud
  mono.alarm.nextBurstAt = millis();
}

static void alarmEnd() {
  mono.alarm.on = false;
  if (mono.sound.prio == PRIO_ALARM) soundStop();
}

// Beeps in bursts until a minute after the last shake, then keeps quiet but
// stays locked down and fast-beaconing until the owner clears it.
static void serviceAlarm() {
  if (!mono.alarm.on) return;
  if (millis() - mono.alarm.lastShakeAt > ALARM_SOUND_MS) return;
  if (reached(mono.alarm.nextBurstAt)) {
    PLAY(P_TAMPER, PRIO_ALARM);
    mono.alarm.nextBurstAt = millis() + ALARM_BURST_EVERY_MS;
  }
}

static bool tamperSpike() {
  float x = imu.readFloatAccelX(), y = imu.readFloatAccelY(), z = imu.readFloatAccelZ();
  float mag = sqrtf(x * x + y * y + z * z);
  return fabsf(mag - 1.0f) > (cfg.tamperMilliG / 1000.0f);
}

// Sampled on a fixed 20 ms grid whatever the loop is doing, and counting
// rising edges: three separate knocks, not three samples of one long one.
// v2 sampled once per loop pass, so the same knock was ignored while asleep
// and raised the alarm while the sensor was busy.
static void serviceTamper() {
  TamperState &t = mono.tamper;
  if (!reached(t.nextSampleAt)) return;
  do t.nextSampleAt += TAMPER_SAMPLE_MS; while (reached(t.nextSampleAt));

  if (mono.latch.open) { t.above = false; t.knocks = 0; return; }   // owner is using it

  bool spike = tamperSpike();
  bool knock = spike && !t.above;
  t.above = spike;
  if (!knock) return;

  uint32_t now = millis();
  if (t.knocks == 0 || now - t.firstKnockAt > TAMPER_WINDOW_MS) { t.knocks = 0; t.firstKnockAt = now; }
  if (++t.knocks < TAMPER_KNOCKS) return;
  t.knocks = 0;
  alarmTrigger();
}

static void onMatch(int16_t id) {
  mono.failCount = 0;
  mono.ownerUntil = millis() + OWNER_WINDOW_MS;
  mono.leash = LeashState();          // the owner has it in hand: it is not lost
  if (mono.alarm.on) { alarmEnd(); notify("EVT lockdown cleared"); }
  if (mono.pair.pending) { pairComplete(); return; }   // the touch said yes to a phone, not "open"
  latchOpen();
  notify("EVT unlocked id=%d", id);
  PLAY(P_ACCEPT, PRIO_CHIRP);
}

// ================================================================ leash ==
static bool ownerLinked() { return Bluefruit.connected() && mono.ble.authed; }

static void serviceLeash() {
  LeashState &l = mono.leash;
  if (ownerLinked()) { l = LeashState(); return; }   // the paired phone is back
  if (!l.armed || cfg.sepWarnS == 0) return;

  uint32_t goneS  = (millis() - l.lostAt) / 1000;
  uint32_t alertS = cfg.sepAlertS > cfg.sepWarnS ? cfg.sepAlertS : cfg.sepWarnS + 1u;
  if (!l.alerted && goneS >= alertS) {
    l.alerted = l.warned = true;
    notify("EVT separation alert");
    PLAY(P_SEP_ALERT, PRIO_ALARM);
  } else if (!l.warned && goneS >= cfg.sepWarnS) {
    l.warned = true;
    notify("EVT separation warn");
    PLAY(P_SEP_WARN, PRIO_CHIRP);
    ledFlash(2, 120);
  }
}

// =============================================================== links ==
static void onConnect(uint16_t conn) { (void)conn; linkEv.connected = true; }
static void onDisconnect(uint16_t conn, uint8_t reason) {
  (void)conn;
  linkEv.reason = reason;
  linkEv.disconnected = true;
}

// The radio lost the phone: supervision timeout (0x08), LL response timeout
// (0x22), or the link never came up (0x3E). Anything else - the phone hung up
// (0x13), we did (0x16), it powered off (0x15) - is a goodbye, not a
// walk-away. v2 sounded the alarm after the user tapped Disconnect.
static bool isLinkLoss(uint8_t reason) { return reason == 0x08 || reason == 0x22 || reason == 0x3E; }

static void resetSession() {
  mono.ble = Session();
  mono.rx[PORT_BLE] = LineBuf();
  mono.pair.pending = false;
}

static void serviceLinkEvents() {
  if (linkEv.disconnected) {
    linkEv.disconnected = false;
    // Only the paired phone's departure can arm the leash. An unpaired
    // stranger connecting and leaving means nothing.
    if (mono.ble.authed) {
      mono.leash = LeashState();
      mono.leash.armed  = isLinkLoss(linkEv.reason) && !mono.ble.bye && cfg.sepWarnS != 0;
      mono.leash.lostAt = millis();
    }
    resetSession();
  }
  if (linkEv.connected) { linkEv.connected = false; resetSession(); }
}

// ============================================================ pairing ==
static const char AUTH_TAG[] = "MONO-AUTH-1";

// The phone proves it holds its key by answering HMAC(key, tag || nonce).
static void authMac(const uint8_t *key, const uint8_t *nonce, uint8_t out[32]) {
  uint8_t msg[sizeof(AUTH_TAG) - 1 + NONCE_LEN];
  memcpy(msg, AUTH_TAG, sizeof(AUTH_TAG) - 1);
  memcpy(msg + sizeof(AUTH_TAG) - 1, nonce, NONCE_LEN);
  hmacSha256(key, PHONE_KEY_LEN, msg, sizeof msg, out);
}

static int8_t freePhoneSlot() {
  for (uint8_t i = 0; i < MAX_PHONES; i++) if (!(cfg.phones & (1u << i))) return (int8_t)i;
  return -1;
}

// Called straight from PAIR on a factory-fresh wallet, and otherwise from the
// owner's confirming touch. The key goes to the phone that asked and nowhere
// else - never the USB console, not even in a bench build.
static void pairComplete() {
  mono.pair.pending = false;
  int8_t slot = freePhoneSlot();
  if (slot < 0) { writeLine(PORT_BLE, "PAIR fail full"); return; }
  uint8_t key[PHONE_KEY_LEN];
  if (!randomBytes(key, sizeof key)) { writeLine(PORT_BLE, "PAIR fail rng"); return; }

  memcpy(cfg.phoneKey[slot], key, sizeof key);
  cfg.phones |= (uint8_t)(1u << slot);
  settingsSave();

  char hex[2 * PHONE_KEY_LEN + 1], line[OUT_MAX];
  toHex(key, sizeof key, hex);
  memset(key, 0, sizeof key);
  snprintf(line, sizeof line, "PAIR ok slot=%d key=%s", slot, hex);
  writeLine(PORT_BLE, line);
  memset(hex, 0, sizeof hex);
  memset(line, 0, sizeof line);

  mono.ble.authed = true;
  notify("EVT paired slot=%d", slot);
  PLAY(P_PAIRED, PRIO_CHIRP);
}

static void servicePairing() {
  if (mono.pair.pending && reached(mono.pair.until)) {
    mono.pair.pending = false;
    writeLine(PORT_BLE, "PAIR fail timeout");
  }
}

// USB is trusted only when nobody owns the wallet yet, or for a minute after
// the owner's finger opened it. MONO_BENCH trusts it always (see config.h).
static bool trusted(Port p) {
  if (p == PORT_BLE) return mono.ble.authed;
#if MONO_BENCH
  return true;
#else
  return setupMode() || !reached(mono.ownerUntil);
#endif
}

// ============================================================== battery ==
// 3.0 V reference, 12 bits, through the board's 1 M / 510 k divider. VBAT_ENABLE
// stays LOW permanently (setup()): Seeed warn that driving P0.14 HIGH - which
// v2 did after every read to save the divider's ~3 uA - can put P0.31 at its
// 3.6 V absolute limit.
static uint16_t batteryMillivolts() {
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(1);
  float raw = (float)analogRead(PIN_VBAT);
  return (uint16_t)(raw * (3000.0f * 1510.0f / (4096.0f * 510.0f)));
}

static void serviceBattery() {
  if (!reached(mono.vbatAt)) return;
  mono.vbatMv = batteryMillivolts();
  mono.vbatAt = millis() + BATT_PERIOD_MS;
}

// =============================================================== status ==
static const char *sensorName() {
  static const char *const N[] = { "ready", "scan", "reject", "lockout" };
  return N[mono.sensor];
}
static const char *enrollName() {
  static const char *const N[] = { "idle", "place", "lift", "again" };
  return N[mono.enroll];
}
static uint8_t leashCode() {
  if (cfg.sepWarnS == 0)  return 0;   // off
  if (mono.leash.alerted) return 4;
  if (mono.leash.warned)  return 3;
  if (mono.leash.armed)   return 2;   // lost, counting
  return 1;                           // ok
}
static const char *leashName() {
  static const char *const N[] = { "off", "ok", "lost", "warn", "alert" };
  return N[leashCode()];
}

static void formatStatus(char *b, size_t n) {
  snprintf(b, n,
           "STATUS latch=%s sensor=%s alarm=%u enroll=%s fingers=%u vbat=%u.%02u "
           "link=%u auth=%u pair=%u setup=%u leash=%s",
           mono.latch.open ? "open" : "locked", sensorName(), mono.alarm.on ? 1 : 0,
           enrollName(), fingerCount(), mono.vbatMv / 1000, (mono.vbatMv % 1000) / 10,
           Bluefruit.connected() ? 1 : 0, mono.ble.authed ? 1 : 0,
           mono.pair.pending ? 1 : 0, setupMode() ? 1 : 0, leashName());
}

static Snapshot snapshot() {
  Snapshot s;
  s.latch   = mono.latch.open;
  s.sensor  = mono.sensor;
  s.alarm   = mono.alarm.on;
  s.enroll  = mono.enroll;
  s.fingers = fingerCount();
  s.link    = Bluefruit.connected() ? 1 : 0;
  s.auth    = mono.ble.authed;
  s.pair    = mono.pair.pending;
  s.setup   = setupMode();
  s.leash   = leashCode();
  s.batt    = (uint8_t)(mono.vbatMv / 50);   // pushes on a 50 mV move, not on noise
  return s;
}

// Whenever anything the app shows changes, the paired phone hears about it.
// This replaces the app's guesswork about which replies imply a refresh.
static void pushStatus() {
  Snapshot s = snapshot();
  if (mono.pushedOnce && memcmp(&s, &mono.pushed, sizeof s) == 0) return;
  mono.pushed = s;
  mono.pushedOnce = true;
  char b[OUT_MAX];
  formatStatus(b, sizeof b);
  if (mono.ble.authed) writeLine(PORT_BLE, b);
#if MONO_BENCH
  writeLine(PORT_USB, b);
#endif
}

// ================================================================ radio ==
static void startAppAdvertising(uint16_t interval, bool fromBeacon) {
  Bluefruit.Advertising.stop();
  if (fromBeacon) Bluefruit.setAddr(&identityAddr);   // take our own address back
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(interval, interval);
  Bluefruit.Advertising.start(0);
}

static void radioApply(RadioMode mode, uint16_t interval) {
  RadioState &r = mono.radio;
  if (r.started && r.mode == mode && r.interval == interval) return;
  if (mode == RADIO_APP) startAppAdvertising(interval, r.started && r.mode == RADIO_FINDMY);
#if MONO_ENABLE_FINDMY
  else findmyStart();
#endif
  r.started = true;
  r.mode = mode;
  r.interval = interval;
}

/* The advertising interval is derived from the state on every pass instead of
   being switched by whoever raised an alarm, so it cannot be left fast after
   the alarm ends - v2 could, and a reconnect never put it back.

   Find My and the app link cannot both own the radio: the beacon needs a
   different address and must be non-connectable. So alternate. While the app
   is actually connected we never slice away, or we would drop it mid-task. */
static void serviceRadio() {
  uint16_t interval = (mono.alarm.on || mono.leash.alerted) ? ADV_FAST : ADV_NORMAL;
  RadioMode mode = RADIO_APP;
#if MONO_ENABLE_FINDMY
  if (cfg.findmyEnabled && !Bluefruit.connected()) {
    mode = mono.radio.mode;
    uint32_t slice = mode == RADIO_FINDMY ? FINDMY_SLICE_MS : APPLINK_SLICE_MS;
    if (millis() - mono.radio.sliceAt >= slice) {
      mode = mode == RADIO_APP ? RADIO_FINDMY : RADIO_APP;
      mono.radio.sliceAt = millis();
    }
  } else {
    mono.radio.sliceAt = millis();
  }
#endif
  radioApply(mode, interval);
}

static void startBLE() {
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("MONO");
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
#if MONO_LINK_ENCRYPTION
  bleuart.setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
#endif
  bleuart.begin();
  identityAddr = Bluefruit.getAddr();
  radioApply(RADIO_APP, ADV_NORMAL);
}

// ============================================================= commands ==
// Text lines, so you can drive the whole thing from a serial monitor. Every
// reply starts with the command name; docs/PROTOCOL.md has the full list.

static void cmdHelp(char *arg);

static void cmdHello(char *) {
  char hex[2 * NONCE_LEN + 1] = "-";
  if (mono.port == PORT_BLE && randomBytes(mono.ble.nonce, NONCE_LEN)) {
    mono.ble.nonceLive = true;
    toHex(mono.ble.nonce, NONCE_LEN, hex);
  }
  reply("HELLO v=%u fw=%s phones=%u setup=%u nonce=%s", MONO_PROTOCOL, MONO_FW_VERSION,
        phoneCount(), setupMode() ? 1 : 0, hex);
}

static void cmdAuth(char *arg) {
  if (mono.port != PORT_BLE) { reply("AUTH fail port"); return; }
  if (!mono.ble.nonceLive)   { reply("AUTH fail nonce"); return; }
  mono.ble.nonceLive = false;               // one answer per challenge, right or wrong
  uint8_t mac[32];
  if (!fromHex(arg, mac, sizeof mac)) { reply("AUTH fail format"); return; }
  for (uint8_t i = 0; i < MAX_PHONES; i++) {
    if (!(cfg.phones & (1u << i))) continue;
    uint8_t want[32];
    authMac(cfg.phoneKey[i], mono.ble.nonce, want);
    if (equalConstantTime(want, mac, sizeof mac)) {
      mono.ble.authed = true;
      reply("AUTH ok slot=%u", i);
      notify("EVT linked");
      return;
    }
  }
  reply("AUTH fail");
}

static void cmdPair(char *) {
  if (mono.port != PORT_BLE) { reply("PAIR fail port"); return; }
  if (setupMode())           { pairComplete(); return; }
  if (!mono.sensorOk)        { reply("PAIR fail nosensor"); return; }
  if (freePhoneSlot() < 0)   { reply("PAIR fail full"); return; }
  mono.pair.pending = true;
  mono.pair.until = millis() + PAIR_CONFIRM_MS;
  reply("PAIR wait");
  PLAY(P_PAIRING, PRIO_CHIRP);
}

static void cmdUnpair(char *) {
  memset(cfg.phoneKey, 0, sizeof cfg.phoneKey);
  cfg.phones = 0;
  settingsSave();
  reply("UNPAIR ok");
  mono.ble.authed = false;                  // whoever was trusted, is not now
}

static void cmdBye(char *) {
  mono.ble.bye = true;                      // the next disconnect is a goodbye
  reply("BYE ok");
}

static void cmdStatus(char *) {
  mono.vbatMv = batteryMillivolts();
  char b[OUT_MAX];
  formatStatus(b, sizeof b);
  reply("%s", b);
}

static void cmdGet(char *) {
  reply("GET relock=%u tamper=%u sepwarn=%u sepalert=%u maxfails=%u "
        "servolock=%u servorel=%u findmy=%u phones=%u",
        cfg.releaseHoldMs, cfg.tamperMilliG, cfg.sepWarnS, cfg.sepAlertS,
        cfg.maxFails, cfg.servoLocked, cfg.servoReleased, cfg.findmyEnabled, phoneCount());
}

// Replies with the value actually stored, which after clamping may not be
// the value asked for - v2 echoed the request.
static void cmdSet(char *arg) {
  if (!arg) { reply("SET fail noarg"); return; }
  char *val = strchr(arg, ' ');
  if (!val) { reply("SET fail noval"); return; }
  *val++ = 0;
  while (*val == ' ') val++;
  downcase(arg);
  long v, eff;
  if (!parseLong(val, &v)) { reply("SET fail badval %s", arg); return; }

  if      (!strcmp(arg, "relock"))    eff = cfg.releaseHoldMs = (uint16_t)clampL(v, 1000, 60000);
  else if (!strcmp(arg, "tamper"))    eff = cfg.tamperMilliG  = (uint16_t)clampL(v, 500, 8000);
  else if (!strcmp(arg, "sepwarn"))   eff = cfg.sepWarnS      = (uint16_t)(v <= 0 ? 0 : clampL(v, 5, 3600));
  else if (!strcmp(arg, "sepalert"))  eff = cfg.sepAlertS     = (uint16_t)clampL(v, 10, 7200);
  else if (!strcmp(arg, "maxfails"))  eff = cfg.maxFails      = (uint8_t)clampL(v, 1, 20);
  else if (!strcmp(arg, "servolock")) eff = cfg.servoLocked   = (uint8_t)clampL(v, 0, 180);
  else if (!strcmp(arg, "servorel"))  eff = cfg.servoReleased = (uint8_t)clampL(v, 0, 180);
  else if (!strcmp(arg, "findmy"))    eff = cfg.findmyEnabled = v ? 1 : 0;
  else { reply("SET fail unknown %s", arg); return; }

  settingsSave();
  if (!strncmp(arg, "servo", 5))                      // tune the latch live at the bench
    servoTo(mono.latch.open ? cfg.servoReleased : cfg.servoLocked);
  if (cfg.sepWarnS == 0) mono.leash = LeashState();   // leash switched off: stand down
  reply("SET ok %s=%ld", arg, eff);
}

static void cmdList(char *) {
  char ids[3 * MAX_FINGERS + 1] = "";
  size_t n = 0;
  for (uint8_t id = 1; id <= MAX_FINGERS; id++)
    if (mono.slots & slotBit(id)) n += snprintf(ids + n, sizeof ids - n, n ? ",%u" : "%u", id);
  reply("LIST count=%u ids=%s", fingerCount(), ids);
}

static void cmdEnroll(char *) { enrollBegin(); }

static void cmdCancel(char *) {
  if (enrolling()) enrollEnd();
  reply("CANCEL ok");
}

static void cmdDelete(char *arg) {
  long id;
  if (!arg) { reply("DELETE fail noarg"); return; }
  if (!parseLong(arg, &id) || id < 1 || id > MAX_FINGERS) { reply("DELETE fail badid"); return; }
  if ((mono.slots & slotBit(id)) && finger.deleteModel((uint16_t)id) == FINGERPRINT_OK) {
    mono.slots &= ~slotBit(id);
    reply("DELETE ok id=%ld", id);
  } else {
    reply("DELETE fail id=%ld", id);
  }
}

static void cmdDeleteAll(char *) {
  if (finger.emptyDatabase() == FINGERPRINT_OK) { mono.slots = 0; reply("DELETEALL ok"); }
  else                                          reply("DELETEALL fail");
}

static void cmdRing(char *) {
  reply("RING ok");
  PLAY(P_RING, PRIO_RING);
}

// Locks the latch and nothing else. v2 also dropped the wallet out of a
// tamper lockdown, silently, and left the alarm beacon running.
static void cmdLock(char *) {
  if (mono.latch.open) latchClose();
  reply("LOCK ok");
}

// The paired phone is the owner, so opening also ends an alarm - the same
// as the owner's finger does.
static void cmdUnlock(char *) {
  if (mono.alarm.on) { alarmEnd(); notify("EVT lockdown cleared"); }
  latchOpen();
  reply("UNLOCK ok");
}

// "All is well": alarm off, lockout over, leash stood down.
static void cmdClear(char *) {
  alarmEnd();
  mono.leash = LeashState();
  mono.failCount = 0;
  if (mono.sensor == SEN_LOCKOUT || mono.sensor == SEN_REJECT) sensorEnter(SEN_READY);
  reply("CLEAR ok");
}

// HELP lists them in this order. Only HELLO, AUTH, PAIR and HELP are open to
// a phone that has not proved who it is.
static const Command COMMANDS[] = {
  { "STATUS",    NEED_TRUST,   cmdStatus    },
  { "GET",       NEED_TRUST,   cmdGet       },
  { "SET",       NEED_TRUST,   cmdSet       },
  { "LIST",      NEED_TRUST,   cmdList      },
  { "ENROLL",    NEED_TRUST,   cmdEnroll    },
  { "CANCEL",    NEED_TRUST,   cmdCancel    },
  { "DELETE",    NEED_TRUST,   cmdDelete    },
  { "DELETEALL", NEED_TRUST,   cmdDeleteAll },
  { "RING",      NEED_TRUST,   cmdRing      },
  { "LOCK",      NEED_TRUST,   cmdLock      },
  { "UNLOCK",    NEED_TRUST,   cmdUnlock    },
  { "CLEAR",     NEED_TRUST,   cmdClear     },
  { "HELLO",     NEED_NOTHING, cmdHello     },
  { "AUTH",      NEED_NOTHING, cmdAuth      },
  { "PAIR",      NEED_NOTHING, cmdPair      },
  { "UNPAIR",    NEED_TRUST,   cmdUnpair    },
  { "BYE",       NEED_TRUST,   cmdBye       },
  { "HELP",      NEED_NOTHING, cmdHelp      },
};
static const uint8_t N_COMMANDS = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

static void cmdHelp(char *) {
  char b[OUT_MAX] = "HELP";
  size_t n = 4;
  for (uint8_t i = 0; i < N_COMMANDS && n < sizeof b; i++)
    n += snprintf(b + n, sizeof b - n, " %s", COMMANDS[i].name);
  reply("%s", b);
}

static void dispatch(Port p, char *line) {
  mono.port = p;
  while (*line == ' ') line++;
  if (!*line) return;

  char *arg = strchr(line, ' ');
  if (arg) {
    *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!*arg) arg = nullptr;
  }
  upcase(line);

  for (uint8_t i = 0; i < N_COMMANDS; i++) {
    const Command &c = COMMANDS[i];
    if (strcmp(line, c.name)) continue;
    if (c.need == NEED_TRUST && !trusted(p)) { reply("ERR auth %s", c.name); return; }
    c.fn(arg);
    return;
  }
  reply("ERR unknown %s", line);
}

static void feedByte(Port p, char c) {
  LineBuf &l = mono.rx[p];
  if (c == '\r') return;
  if (c == '\n') {
    if (l.overflow)  { mono.port = p; reply("ERR toolong"); }
    else if (l.len)  { l.buf[l.len] = 0; dispatch(p, l.buf); }
    l.len = 0;
    l.overflow = false;
    return;
  }
  if (l.overflow) return;
  if (l.len < LINE_MAX - 1) l.buf[l.len++] = c;
  else                      l.overflow = true;   // refuse the whole line, never run a prefix
}

static void pumpPorts() {
  while (Serial.available())  feedByte(PORT_USB, (char)Serial.read());
  while (bleuart.available()) feedByte(PORT_BLE, (char)bleuart.read());
}

// ================================================================= idle ==
// Sleep until the next thing is due, never longer than one tamper sample.
// On this core delay() is FreeRTOS vTaskDelay(): the CPU really does sleep.
static void idle() {
  uint32_t wait = IDLE_MAX_MS, w;
  if (mono.sound.seq    && (w = msUntil(mono.sound.nextAt))      < wait) wait = w;
  if (mono.latch.powered && (w = msUntil(mono.latch.powerOffAt)) < wait) wait = w;
  if ((w = msUntil(mono.tamper.nextSampleAt)) < wait) wait = w;
  if ((mono.sensor == SEN_SCAN || enrolling()) && wait > 5) wait = 5;   // the R502 paces these
  if (wait) delay(wait);
}

// ================================================================ setup ==
void setup() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FP_WAKE, INPUT);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);           // and it stays LOW - see batteryMillivolts()

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { }

  Serial.println("");
  Serial.println("MONO wallet - firmware " MONO_FW_VERSION);

  InternalFS.begin();
  Serial.println(settingsLoad() ? "settings: restored" : "settings: defaults");

  finger.begin(57600);
  mono.sensorOk = finger.verifyPassword();
  if (mono.sensorOk) {
    slotsRefresh();
    finger.getTemplateCount();
    Serial.print("R502 online, fingers: ");
    Serial.println((int)fingerCount());
    if (finger.templateCount > fingerCount())
      Serial.println("note: the R502 holds templates outside ids 1-10; DELETEALL clears them");
  } else {
    Serial.println("R502 NOT FOUND - check 3V3 and whether TX/RX are crossed");
  }

  if (imu.begin() != 0) Serial.println("IMU init failed");

  latchClose();
  startBLE();

  mono.vbatMv = batteryMillivolts();
  mono.vbatAt = millis() + BATT_PERIOD_MS;
  char b[48];
  snprintf(b, sizeof b, "battery: %u.%02u V", mono.vbatMv / 1000, (mono.vbatMv % 1000) / 10);
  Serial.println(b);
#if MONO_BENCH
  Serial.println("BENCH BUILD - the USB console can open this wallet. Do not carry it.");
#endif
  if (setupMode()) Serial.println("setup mode: nothing enrolled, nobody paired - the first phone to PAIR owns it");
  Serial.println("type HELP for commands");
  ledFlash(2, 80);
}

// ================================================================= loop ==
void loop() {
  serviceLinkEvents();
  pumpPorts();
  serviceBattery();
  serviceLatch();
  serviceEnroll();
  serviceSensor();
  serviceTamper();
  serviceAlarm();
  serviceLeash();
  servicePairing();
  serviceSound();
  serviceLed();
  serviceRadio();
  pushStatus();
  idle();
}

/* ----------------------------------------------------------------------------
   POWER NOTE  -  where the battery actually goes
   ----------------------------------------------------------------------------
   v2's note blamed delay() for a busy-wait. On this core it is not one:
   delay() is FreeRTOS vTaskDelay(), so the CPU sleeps between passes. With
   v3's idle() it also sleeps through alarms, lockouts and the open window,
   which v2 spent spinning flat out. What is left, roughly in order:

     R502 main supply (red, 3V3)   always on as wired; switch it with a load
                                   switch and keep only Vt (white, ~2 uA)
                                   powered for wake-on-touch
     IMU polling at 50 Hz          wakes the CPU and the I2C bus every 20 ms;
                                   the LSM6DS3TR-C's own wake-up interrupt on
                                   INT1 (P0.11) would replace it entirely
     BLE advertising at ~152 ms    tens of uA average; Find My adds 30-50 uA
     battery divider               ~3 uA, the price of keeping P0.14 LOW

   Measure with a PPK2 or a meter in series with the battery lead before
   believing any figure here, including these.
--------------------------------------------------------------------------- */
