/* ============================================================================
   MONO WALLET  -  prototype firmware  v2
   ----------------------------------------------------------------------------
   Target : Seeed XIAO nRF52840 Sense
   Sensor : GROW R502 capacitive fingerprint module, UART 57600
   Latch  : micro servo (1.7 g class) driving a blocking pin

   NEW IN v2
     - Full BLE command channel. The phone app can enroll and delete
       fingerprints, read status, ring the wallet and change settings.
       v1 could only enroll over the USB cable.
     - Enrolment is now a non-blocking state machine, so BLE stays responsive
       and the app can show live progress.
     - Settings persist across battery pulls (LittleFS on internal flash).
     - Optional Apple Find My beacon (findmy.h) time-sliced with the app link.

   BOARD SETUP  (Arduino IDE)
     1. Preferences > Additional Board Manager URLs:
        https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
     2. Boards Manager: install "Seeed nRF52 Boards"
     3. Board: "Seeed XIAO nRF52840 Sense"  (the NON-mbed entry)
     4. Libraries: "Adafruit Fingerprint Sensor Library", "Seeed Arduino LSM6DS3"

   WIRING
     R502 red    3V3  -> 3V3          R502 yellow TX -> D7 (XIAO RX)
     R502 black  GND  -> GND          R502 green  RX -> D6 (XIAO TX)
     R502 blue   WAKE -> D2           R502 white  Vt -> 3V3  (touch power)
     servo signal     -> D3           buzzer + -> D4
     status LED +     -> D5 via 330R  LiPo -> BAT pads (check polarity!)

   SECURITY REALITY
     The R502 stores its templates in ordinary flash on the module, not in a
     secure element. This is a faithful functional mock of a biometric wallet,
     not a secure one. Do not trust it with anything you would mind losing.
   ========================================================================= */

#include <bluefruit.h>
#include <Adafruit_Fingerprint.h>
#include <Servo.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "LSM6DS3.h"
#include "Wire.h"
#include "findmy.h"

using namespace Adafruit_LittleFS_Namespace;

// ---------------------------------------------------------------- pins ----
#define PIN_FP_WAKE    2
#define PIN_SERVO      3
#define PIN_BUZZER     4
#define PIN_LED        5
#ifndef VBAT_ENABLE
  #define VBAT_ENABLE 14
#endif
#ifndef PIN_VBAT
  #define PIN_VBAT    32
#endif

#define SETTINGS_FILE  "/mono.cfg"
#define SETTINGS_MAGIC 0x4D4F4E32UL     // "MON2"

// ----------------------------------------------------------- settings ----
struct Settings {
  uint32_t magic;
  uint16_t releaseHoldMs;   // how long the latch stays open
  uint16_t tamperMilliG;    // spike threshold, milli-g above rest
  uint16_t sepWarnS;        // link down this long -> quiet warning
  uint16_t sepAlertS;       // -> full alert + fast beacon
  uint8_t  maxFails;
  uint8_t  servoLocked;     // degrees
  uint8_t  servoReleased;   // degrees
  uint8_t  findmyEnabled;
};

Settings cfg;

void settingsDefaults() {
  cfg.magic         = SETTINGS_MAGIC;
  cfg.releaseHoldMs = 6000;
  cfg.tamperMilliG  = 2600;
  cfg.sepWarnS      = 20;
  cfg.sepAlertS     = 60;
  cfg.maxFails      = 5;
  cfg.servoLocked   = 20;
  cfg.servoReleased = 95;
  cfg.findmyEnabled = 0;
}

void settingsSave() {
  InternalFS.remove(SETTINGS_FILE);
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_WRITE)) {
    f.write((uint8_t *)&cfg, sizeof(cfg));
    f.close();
  }
}

void settingsLoad() {
  settingsDefaults();
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_READ)) {
    Settings tmp;
    if (f.read((uint8_t *)&tmp, sizeof(tmp)) == (int)sizeof(tmp) &&
        tmp.magic == SETTINGS_MAGIC) {
      cfg = tmp;
    }
    f.close();
  }
}

// -------------------------------------------------------------- tuning ----
const uint32_t SCAN_WINDOW_MS   = 8000;
const uint32_t LOCKOUT_MS       = 30000;
const uint16_t TAMPER_HITS      = 3;
const uint32_t TAMPER_WINDOW_MS = 1500;
const uint16_t ADV_NORMAL       = 244;    // x0.625 ms ~152 ms
const uint16_t ADV_FAST         = 32;     //           ~20 ms
const uint32_t FINDMY_SLICE_MS  = 8000;   // beacon slice when time-slicing
const uint32_t APPLINK_SLICE_MS = 8000;   // connectable slice

// -------------------------------------------------------------- state ----
enum State : uint8_t {
  ST_SLEEP, ST_SCAN, ST_RELEASED, ST_REJECT, ST_LOCKOUT, ST_LOCKDOWN
};
enum Enroll : uint8_t {
  EN_IDLE, EN_P1_WAIT, EN_LIFT, EN_P2_WAIT
};

State  state       = ST_SLEEP;
Enroll enrollState = EN_IDLE;

uint32_t stateEnteredAt = 0, enrollEnteredAt = 0;
uint8_t  enrollId = 0, failCount = 0;
uint32_t lastLinkSeen = 0;
bool     linked = false, sepWarned = false, sepAlerted = false;
uint32_t tamperFirstHit = 0;
uint16_t tamperHits = 0;
bool     beaconMode = false;
uint32_t sliceStartedAt = 0;

// ------------------------------------------------------------ objects ----
Adafruit_Fingerprint finger(&Serial1);
Servo   latch;
LSM6DS3 imu(I2C_MODE, 0x6A);
BLEUart bleuart;

char lineBuf[96];
uint8_t lineLen = 0;

// ======================================================== small helpers ==
void led(bool on) { digitalWrite(PIN_LED, on ? HIGH : LOW); }

void beep(uint16_t freq, uint16_t ms) {
  tone(PIN_BUZZER, freq, ms);
  delay(ms);
  noTone(PIN_BUZZER);
}

void blink(uint8_t n, uint16_t ms) {
  for (uint8_t i = 0; i < n; i++) { led(true); delay(ms); led(false); delay(ms); }
}

float batteryVolts() {
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  digitalWrite(VBAT_ENABLE, LOW);
  delay(5);
  float raw = analogRead(PIN_VBAT);
  digitalWrite(VBAT_ENABLE, HIGH);
  return raw * 3.0f / 4096.0f * (1510.0f / 510.0f);
}

// Every reply goes to both transports so the USB console and the app agree.
void say(const char *s) {
  Serial.println(s);
  if (Bluefruit.connected()) { bleuart.print(s); bleuart.print('\n'); }
}

void sayf(const char *fmt, ...) {
  char b[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  say(b);
}

void enter(State s)        { state = s; stateEnteredAt = millis(); }
void enterEnroll(Enroll e) { enrollState = e; enrollEnteredAt = millis(); }

// =========================================================== mechanism ==
// The servo is detached after every move, so it draws nothing while idle.
void lockLatch() {
  latch.attach(PIN_SERVO); latch.write(cfg.servoLocked);   delay(350); latch.detach();
}
void releaseLatch() {
  latch.attach(PIN_SERVO); latch.write(cfg.servoReleased); delay(350); latch.detach();
}

// ========================================================= fingerprint ==
#define MATCH_NOFINGER (-1)
#define MATCH_REJECT   (-2)

int16_t tryMatch() {
  uint8_t r = finger.getImage();
  if (r == FINGERPRINT_NOFINGER) return MATCH_NOFINGER;
  if (r != FINGERPRINT_OK)       return MATCH_NOFINGER;
  if (finger.image2Tz() != FINGERPRINT_OK)         return MATCH_REJECT;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) return MATCH_REJECT;
  return finger.fingerID;
}

// Non-blocking enrolment. v1 blocked here for up to 30 s, which stalled BLE
// and made the app look frozen. Now every step reports as it happens.
void enrollBegin() {
  if (enrollState != EN_IDLE) { say("ENROLL busy"); return; }
  finger.getTemplateCount();
  enrollId = finger.templateCount + 1;
  if (enrollId < 1 || enrollId > 200) { say("ENROLL fail full"); return; }
  sayf("ENROLL start id=%u", enrollId);
  say("ENROLL place");
  enterEnroll(EN_P1_WAIT);
}

void enrollAbort(const char *why) {
  sayf("ENROLL fail %s", why);
  enterEnroll(EN_IDLE);
  beep(400, 250);
}

void serviceEnroll() {
  if (enrollState == EN_IDLE) return;
  if (millis() - enrollEnteredAt > 20000) { enrollAbort("timeout"); return; }

  switch (enrollState) {
    case EN_P1_WAIT:
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) { enrollAbort("bad_image"); return; }
      beep(1800, 60); blink(1, 100);
      say("ENROLL lift");
      enterEnroll(EN_LIFT);
      break;

    case EN_LIFT:
      if (finger.getImage() != FINGERPRINT_NOFINGER) return;
      say("ENROLL again");
      enterEnroll(EN_P2_WAIT);
      break;

    case EN_P2_WAIT: {
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(2) != FINGERPRINT_OK) { enrollAbort("bad_image"); return; }
      if (finger.createModel() != FINGERPRINT_OK) { enrollAbort("no_match"); return; }
      if (finger.storeModel(enrollId) != FINGERPRINT_OK) { enrollAbort("store"); return; }
      sayf("ENROLL ok id=%u", enrollId);
      beep(2400, 120);
      enterEnroll(EN_IDLE);
      break;
    }
    default: break;
  }
}

// =============================================================== tamper ==
bool tamperSpike() {
  float x = imu.readFloatAccelX(), y = imu.readFloatAccelY(), z = imu.readFloatAccelZ();
  float mag = sqrtf(x * x + y * y + z * z);
  return fabsf(mag - 1.0f) > (cfg.tamperMilliG / 1000.0f);
}

void goFastBeacon() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.setInterval(ADV_FAST, ADV_FAST);
  Bluefruit.Advertising.start(0);
}

void goNormalBeacon() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.setInterval(ADV_NORMAL, ADV_NORMAL);
  Bluefruit.Advertising.start(0);
}

void serviceTamper() {
  if (state == ST_RELEASED) return;          // owner is using it
  if (!tamperSpike()) return;

  uint32_t now = millis();
  if (now - tamperFirstHit > TAMPER_WINDOW_MS) { tamperFirstHit = now; tamperHits = 0; }
  if (++tamperHits < TAMPER_HITS) return;

  tamperHits = 0;
  enter(ST_LOCKDOWN);
  lockLatch();
  say("EVT tamper");
  goFastBeacon();
  for (uint8_t i = 0; i < 6; i++) { beep(3200, 90); delay(60); }
}

// ================================================================ leash ==
void serviceLeash() {
  if (Bluefruit.connected()) {
    lastLinkSeen = millis(); linked = true;
    sepWarned = sepAlerted = false;
    return;
  }
  if (!linked) return;

  uint32_t goneS = (millis() - lastLinkSeen) / 1000;

  if (goneS > cfg.sepAlertS && !sepAlerted) {
    sepAlerted = true;
    say("EVT separation alert");
    goFastBeacon();
    for (uint8_t i = 0; i < 3; i++) { beep(2600, 200); delay(120); }
  } else if (goneS > cfg.sepWarnS && !sepWarned) {
    sepWarned = true;
    say("EVT separation warn");
    beep(2000, 300);
    blink(2, 120);
  }
}

// ============================================================= commands ==
// Text lines, so you can drive the whole thing from nRF Connect before the
// app exists. Every reply starts with the command name.
void handleCommand(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  char *sp = strchr(line, ' ');
  char *arg = nullptr;
  if (sp) { *sp = 0; arg = sp + 1; }

  for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

  if (!strcmp(line, "STATUS")) {
    finger.getTemplateCount();
    sayf("STATUS state=%u templates=%u vbat=%.2f link=%u lockdown=%u enroll=%u",
         state, finger.templateCount, batteryVolts(),
         Bluefruit.connected() ? 1 : 0,
         state == ST_LOCKDOWN ? 1 : 0, enrollState);

  } else if (!strcmp(line, "ENROLL")) {
    enrollBegin();

  } else if (!strcmp(line, "CANCEL")) {
    enterEnroll(EN_IDLE);
    say("CANCEL ok");

  } else if (!strcmp(line, "LIST")) {
    finger.getTemplateCount();
    sayf("LIST count=%u", finger.templateCount);

  } else if (!strcmp(line, "DELETE")) {
    if (!arg) { say("DELETE fail noarg"); return; }
    uint16_t id = atoi(arg);
    sayf(finger.deleteModel(id) == FINGERPRINT_OK ? "DELETE ok id=%u"
                                                  : "DELETE fail id=%u", id);

  } else if (!strcmp(line, "DELETEALL")) {
    sayf(finger.emptyDatabase() == FINGERPRINT_OK ? "DELETEALL ok"
                                                  : "DELETEALL fail");

  } else if (!strcmp(line, "RING")) {
    say("RING ok");
    for (uint8_t i = 0; i < 8; i++) { beep(3000, 120); delay(80); }

  } else if (!strcmp(line, "LOCK")) {
    lockLatch(); enter(ST_SLEEP); say("LOCK ok");

  } else if (!strcmp(line, "UNLOCK")) {
    // Convenience for debugging. It is also a hole: anything that can reach
    // the radio can open the wallet. Remove this before you carry it.
    releaseLatch(); enter(ST_RELEASED); say("UNLOCK ok");

  } else if (!strcmp(line, "CLEAR")) {
    if (state == ST_LOCKDOWN) { enter(ST_SLEEP); goNormalBeacon(); }
    sepWarned = sepAlerted = false;
    failCount = 0;
    say("CLEAR ok");

  } else if (!strcmp(line, "GET")) {
    sayf("GET relock=%u tamper=%u sepwarn=%u sepalert=%u maxfails=%u "
         "servolock=%u servorel=%u findmy=%u",
         cfg.releaseHoldMs, cfg.tamperMilliG, cfg.sepWarnS, cfg.sepAlertS,
         cfg.maxFails, cfg.servoLocked, cfg.servoReleased, cfg.findmyEnabled);

  } else if (!strcmp(line, "SET")) {
    if (!arg) { say("SET fail noarg"); return; }
    char *vs = strchr(arg, ' ');
    if (!vs) { say("SET fail noval"); return; }
    *vs = 0;
    long v = atol(vs + 1);
    for (char *p = arg; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    bool ok = true;
    if      (!strcmp(arg, "relock"))    cfg.releaseHoldMs = constrain(v, 1000, 60000);
    else if (!strcmp(arg, "tamper"))    cfg.tamperMilliG  = constrain(v, 500, 8000);
    else if (!strcmp(arg, "sepwarn"))   cfg.sepWarnS      = constrain(v, 5, 3600);
    else if (!strcmp(arg, "sepalert"))  cfg.sepAlertS     = constrain(v, 10, 7200);
    else if (!strcmp(arg, "maxfails"))  cfg.maxFails      = constrain(v, 1, 20);
    else if (!strcmp(arg, "servolock")) cfg.servoLocked   = constrain(v, 0, 180);
    else if (!strcmp(arg, "servorel"))  cfg.servoReleased = constrain(v, 0, 180);
    else if (!strcmp(arg, "findmy"))    cfg.findmyEnabled = v ? 1 : 0;
    else ok = false;

    if (ok) { settingsSave(); sayf("SET ok %s=%ld", arg, v); }
    else    { sayf("SET fail unknown %s", arg); }

  } else if (!strcmp(line, "HELP")) {
    say("CMDS STATUS ENROLL CANCEL LIST DELETE<id> DELETEALL RING LOCK "
        "UNLOCK CLEAR GET SET<k><v> HELP");

  } else {
    sayf("ERR unknown %s", line);
  }
}

void feedLine(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    lineBuf[lineLen] = 0;
    if (lineLen) handleCommand(lineBuf);
    lineLen = 0;
    return;
  }
  if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
}

void pumpTransports() {
  while (Serial.available())  feedLine((char)Serial.read());
  while (bleuart.available()) feedLine((char)bleuart.read());
}

// ================================================================== BLE ==
void onConnect(uint16_t h) {
  (void)h;
  linked = true; lastLinkSeen = millis();
  sepWarned = sepAlerted = false;
  say("EVT linked");
}
void onDisconnect(uint16_t h, uint8_t reason) {
  (void)h; (void)reason;
  lastLinkSeen = millis();
}

void startAppAdvertising() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(ADV_NORMAL, ADV_NORMAL);
  Bluefruit.Advertising.start(0);
}

void startBLE() {
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("MONO");
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
  bleuart.begin();
  startAppAdvertising();
}

/* Find My and the app link cannot both own the radio: the beacon needs a
   different MAC and must be non-connectable. So alternate. While the app is
   actually connected we never slice away, or we would drop the user mid-task. */
void serviceRadioSlice() {
#if MONO_ENABLE_FINDMY
  if (!cfg.findmyEnabled) {
    if (beaconMode) { beaconMode = false; startAppAdvertising(); }
    return;
  }
  if (Bluefruit.connected()) { sliceStartedAt = millis(); return; }

  uint32_t slice = beaconMode ? FINDMY_SLICE_MS : APPLINK_SLICE_MS;
  if (millis() - sliceStartedAt < slice) return;

  sliceStartedAt = millis();
  beaconMode = !beaconMode;
  if (beaconMode) findmyStart();
  else            startAppAdvertising();
#endif
}

// ================================================================ setup ==
void setup() {
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FP_WAKE, INPUT);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, HIGH);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { }

  Serial.println("\nMONO wallet - firmware v2");

  InternalFS.begin();
  settingsLoad();

  finger.begin(57600);
  if (finger.verifyPassword()) {
    finger.getTemplateCount();
    Serial.print("R502 online, templates: ");
    Serial.println(finger.templateCount);
  } else {
    Serial.println("R502 NOT FOUND - check 3V3 and whether TX/RX are crossed");
  }

  if (imu.begin() != 0) Serial.println("IMU init failed");

  lockLatch();
  startBLE();
  sliceStartedAt = millis();

  Serial.print("battery: "); Serial.print(batteryVolts(), 2); Serial.println(" V");
  Serial.println("type HELP for commands (works over USB or BLE)");
  blink(2, 80);
  enter(ST_SLEEP);
}

// ================================================================= loop ==
void loop() {
  pumpTransports();
  serviceEnroll();
  serviceTamper();
  serviceLeash();
  serviceRadioSlice();

  if (enrollState != EN_IDLE) { delay(20); return; }   // enrolling owns the sensor

  uint32_t inState = millis() - stateEnteredAt;

  switch (state) {

    case ST_SLEEP:
      if (digitalRead(PIN_FP_WAKE) == HIGH) { enter(ST_SCAN); led(true); }
      else delay(60);          // placeholder - see the power note at the end
      break;

    case ST_SCAN: {
      int16_t id = tryMatch();
      if (id >= 0) {
        failCount = 0;
        releaseLatch();
        sayf("EVT unlocked id=%d", id);
        beep(2600, 80);
        enter(ST_RELEASED);
      } else if (id == MATCH_REJECT) {
        say("EVT rejected");
        beep(400, 220);
        enter(ST_REJECT);
      } else if (inState > SCAN_WINDOW_MS) {
        led(false);
        enter(ST_SLEEP);
      }
      break;
    }

    case ST_RELEASED:
      if (inState > cfg.releaseHoldMs) {
        lockLatch(); led(false);
        say("EVT relocked");
        enter(ST_SLEEP);
      }
      break;

    case ST_REJECT:
      // Wait for the finger to leave, or one resting thumb burns through
      // maxfails in a second.
      led((inState / 150) % 2 == 0);
      if (digitalRead(PIN_FP_WAKE) == LOW || inState > 4000) {
        led(false);
        if (++failCount >= cfg.maxFails) { say("EVT lockout"); enter(ST_LOCKOUT); }
        else enter(ST_SLEEP);
      }
      break;

    case ST_LOCKOUT:
      if (inState > LOCKOUT_MS) { failCount = 0; enter(ST_SLEEP); }
      else led((inState / 500) % 2 == 0);
      break;

    case ST_LOCKDOWN:
      if (digitalRead(PIN_FP_WAKE) == HIGH && tryMatch() >= 0) {
        say("EVT lockdown cleared");
        goNormalBeacon();
        releaseLatch();
        enter(ST_RELEASED);
      } else led((inState / 250) % 2 == 0);
      break;
  }
}

/* ----------------------------------------------------------------------------
   POWER NOTE  -  read this before you complain about battery life
   ----------------------------------------------------------------------------
   The delay(60) in ST_SLEEP is a placeholder busy-wait. It gives you days,
   not months. To reach the "weeks" figure in the brief, configure PIN_FP_WAKE
   as a GPIO sense pin and park the CPU:

     #include "nrf_gpio.h"
     __WFE(); __SEV(); __WFE();       // sleep until an event

   Measure with a multimeter in series on the battery lead. Expect roughly:

     busy-wait loop        ~5-8   mA    -> days
     delay() idling        ~1-2   mA    -> a couple of weeks
     __WFE + GPIO sense    ~20-60 uA    -> months (R502 standby is ~2 uA)

   The R502's touch-power pin (white) must stay energised for wake-on-touch;
   that is the 2 uA. Everything else should be asleep.

   Find My beaconing costs you real current on top of this -- a 1 s advertising
   interval runs perhaps 30-50 uA average by itself. That is the price of being
   findable, and it is why AirTags ship a coin cell rather than a 250 mAh pouch.
--------------------------------------------------------------------------- */
