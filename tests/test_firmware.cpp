/* ============================================================================
   MONO firmware - host test harness
   ----------------------------------------------------------------------------
   Compiles mono_firmware.ino UNMODIFIED against mock hardware and drives it
   with a virtual clock. No board, no sensor, no servo, no battery.

   What this can prove : protocol, parsing, settings, persistence, and every
                         state transition in the lock/tamper/leash/enrol logic.
   What it cannot prove : real BLE timing, R502 UART behaviour, servo torque,
                          current draw. Those need the hardware.
   ========================================================================= */

// Every mocked global lives in one place, shared with mono_sim.cpp so the
// simulator and the test suite can never diverge. g_sleepHook stays null here,
// which is what makes a 30-second lockout test finish instantly.
#include "mocks/mock_impl.h"

// ================= the firmware under test, included verbatim =============
#include "../mono_firmware/mono_firmware.ino"
// ==========================================================================

// --------------------------------------------------------- tiny test kit --
static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failures;

#define CHECK(cond, msg) do {                                                  \
    if (cond) { g_pass++; printf("    PASS  %s\n", msg); }                     \
    else { g_fail++; printf("    FAIL  %s   (line %d)\n", msg, __LINE__);      \
           g_failures.push_back(std::string(msg)); }                           \
  } while (0)

static void section(const char *s) { printf("\n  == %s ==\n", s); }

static bool sawLine(const char *prefix) {
  size_t n = strlen(prefix);
  for (auto &l : g_out) if (l.size() >= n && l.compare(0, n, prefix) == 0) return true;
  return false;
}
static std::string findLine(const char *prefix) {
  size_t n = strlen(prefix);
  for (auto &l : g_out) if (l.size() >= n && l.compare(0, n, prefix) == 0) return l;
  return "";
}
static bool sawSub(const char *needle) {
  for (auto &l : g_out) if (l.find(needle) != std::string::npos) return true;
  return false;
}
static void clearOut() { g_out.clear(); }

static void hardReset(bool wipeFS) {
  g_millis = 0; g_beeps.clear(); g_servoWrites.clear(); g_out.clear();
  memset(g_pinState, 0, sizeof(g_pinState));
  g_vbatRaw = 1868.0f;
  g_fp = MockFP(); g_imu = MockIMU(); g_adv = AdvState();
  g_connected = false; g_servoAngle = -1; g_servoAttached = false;
  g_wakeOverride = false;
  if (wipeFS) InternalFS.files.clear();
  bleuart.in.clear(); bleuart.rd = 0; bleuart.tx.clear();
  Serial.in.clear(); Serial.rd = 0; Serial.pending.clear();
  state = ST_SLEEP; enrollState = EN_IDLE;
  stateEnteredAt = 0; enrollEnteredAt = 0; enrollId = 0; failCount = 0;
  lastLinkSeen = 0; linked = false; sepWarned = false; sepAlerted = false;
  tamperFirstHit = 0; tamperHits = 0; beaconMode = false; sliceStartedAt = 0;
  lineLen = 0;
}

static void pumpMs(uint32_t ms) {
  uint32_t end = g_millis + ms;
  while (g_millis < end) {
    uint32_t t0 = g_millis;
    loop();
    if (g_millis == t0) g_millis += 1;   // a real MCU loop is not free
  }
}
// Returns as soon as the firmware reaches `want`. ST_SCAN -> ST_SLEEP -> ST_SCAN
// can cycle faster than a fixed-interval sample, so catch the edge itself.
static bool pumpUntilState(State want, uint32_t maxMs) {
  uint32_t end = g_millis + maxMs;
  while (g_millis < end) {
    uint32_t t0 = g_millis;
    loop();
    if (g_millis == t0) g_millis += 1;
    if (state == want) return true;
  }
  return false;
}

static void cmd(const char *s) { bleuart.feed(std::string(s) + "\n"); loop(); }

static void boot(bool wipeFS = true) { hardReset(wipeFS); setup(); clearOut(); }

// enrol one finger; name/secondName decide whether the two presses match
static bool doEnroll(int name, int secondName) {
  cmd("ENROLL");
  g_fp.platen = PLATEN_UNKNOWN; g_fp.fingerName = name;       pumpMs(400);
  g_fp.platen = PLATEN_EMPTY;                                 pumpMs(400);
  g_fp.platen = PLATEN_UNKNOWN; g_fp.fingerName = secondName; pumpMs(400);
  g_fp.platen = PLATEN_EMPTY;   g_fp.fingerName = 0;
  return sawLine("ENROLL ok");
}

static void rejectOnce() {
  g_fp.platen = PLATEN_UNKNOWN; pumpMs(400);   // SLEEP -> SCAN -> REJECT
  g_fp.platen = PLATEN_EMPTY;   pumpMs(400);   // finger lifts -> SLEEP/LOCKOUT
}

// =============================================================== tests ====
int main() {
  printf("\n================ MONO firmware - hardware-free test run ================\n");

  // ------------------------------------------------------------ boot ----
  section("Boot");
  hardReset(true);
  setup();
  CHECK(state == ST_SLEEP,               "boots into ST_SLEEP");
  CHECK(g_servoAngle == cfg.servoLocked, "latch driven to LOCKED on boot");
  CHECK(g_servoAttached == false,        "servo detached after the move (idle draw = 0)");
  CHECK(g_adv.running,                   "advertising started");
  CHECK(g_adv.interval == ADV_NORMAL,    "advertising at the normal interval");
  CHECK(g_adv.connectable,               "advertising is connectable so the app can pair");
  CHECK(sawSub("R502 online"),           "fingerprint sensor detected at boot");
  CHECK(cfg.magic == SETTINGS_MAGIC,     "settings initialised from defaults");

  hardReset(true); g_fp.password = false; setup();
  CHECK(sawSub("R502 NOT FOUND"),        "missing sensor is reported, not silently ignored");

  // -------------------------------------------------------- protocol ----
  section("Command protocol");
  boot();
  cmd("HELP");
  CHECK(sawSub("CMDS STATUS ENROLL"),    "HELP lists the command set");

  clearOut(); cmd("STATUS");
  std::string st = findLine("STATUS ");
  CHECK(!st.empty(),                                "STATUS replies");
  CHECK(st.find("state=") != std::string::npos,     "STATUS carries state");
  CHECK(st.find("templates=") != std::string::npos, "STATUS carries template count");
  CHECK(st.find("vbat=4.0") != std::string::npos,   "STATUS reports a plausible battery voltage");
  CHECK(st.find("link=0") != std::string::npos,     "STATUS reports link down when disconnected");

  clearOut(); cmd("status");
  CHECK(sawLine("STATUS "),              "lower-case commands are accepted");

  clearOut(); cmd("  STATUS");
  CHECK(sawLine("STATUS "),              "leading whitespace tolerated");

  clearOut(); cmd("WOBBLE");
  CHECK(sawLine("ERR unknown"),          "unknown command rejected cleanly");

  clearOut(); cmd("");
  CHECK(g_out.empty(),                   "empty line is ignored, not answered");

  // split across two BLE packets, as a real 20-byte MTU would deliver it
  clearOut();
  bleuart.feed("STA"); loop();
  CHECK(!sawLine("STATUS "),             "partial command is buffered, not acted on");
  bleuart.feed("TUS\n"); loop();
  CHECK(sawLine("STATUS "),              "command split across packets reassembles");

  clearOut(); bleuart.feed("STATUS\r\n"); loop();
  CHECK(sawLine("STATUS "),              "CRLF line endings handled");

  clearOut();
  { std::string huge(400, 'X'); bleuart.feed(huge + "\n"); loop(); }
  CHECK(sawLine("ERR unknown"),          "400-char line does not overflow the 96-byte buffer");

  clearOut(); g_connected = true; cmd("HELP");
  CHECK(bleuart.tx.find("CMDS") != std::string::npos,
                                         "replies also go out over BLE when connected");
  g_connected = false;

  // -------------------------------------------------------- settings ----
  section("Settings");
  boot();
  clearOut(); cmd("GET");
  std::string gl = findLine("GET ");
  CHECK(gl.find("relock=6000") != std::string::npos, "GET reports default relock");
  CHECK(gl.find("tamper=2600") != std::string::npos, "GET reports default tamper threshold");
  CHECK(gl.find("maxfails=5")  != std::string::npos, "GET reports default max fails");
  CHECK(gl.find("findmy=0")    != std::string::npos, "Find My off by default");

  clearOut(); cmd("SET relock 3000"); cmd("GET");
  CHECK(sawSub("relock=3000"),           "SET relock takes effect");

  clearOut(); cmd("SET relock 999999"); cmd("GET");
  CHECK(sawSub("relock=60000"),          "over-range relock clamps to 60000 ms");
  clearOut(); cmd("SET relock 5"); cmd("GET");
  CHECK(sawSub("relock=1000"),           "under-range relock clamps to 1000 ms");
  clearOut(); cmd("SET tamper 99999"); cmd("GET");
  CHECK(sawSub("tamper=8000"),           "over-range tamper clamps to 8000 mg");
  clearOut(); cmd("SET servolock 900"); cmd("GET");
  CHECK(sawSub("servolock=180"),         "servo angle clamps to 180 deg");

  clearOut(); cmd("SET nonsense 5");
  CHECK(sawLine("SET fail unknown"),     "unknown setting key rejected");
  clearOut(); cmd("SET relock");
  CHECK(sawLine("SET fail noval"),       "SET with no value rejected");
  clearOut(); cmd("SET");
  CHECK(sawLine("SET fail noarg"),       "bare SET rejected");
  clearOut(); cmd("DELETE");
  CHECK(sawLine("DELETE fail noarg"),    "bare DELETE rejected");

  // persistence across a battery pull
  boot();
  cmd("SET sepalert 300");
  cmd("SET maxfails 3");
  hardReset(false);                      // power cycle, flash survives
  setup(); clearOut();
  cmd("GET");
  CHECK(sawSub("sepalert=300"),          "settings survive a power cycle");
  CHECK(sawSub("maxfails=3"),            "second setting survives too");
  CHECK(cfg.magic == SETTINGS_MAGIC,     "magic validates the restored blob");

  // corrupt the stored blob - must fall back to defaults, not garbage
  InternalFS.files["/mono.cfg"] = std::vector<uint8_t>(sizeof(Settings), 0xAB);
  hardReset(false); setup(); clearOut();
  cmd("GET");
  CHECK(sawSub("relock=6000"),           "corrupt settings blob falls back to defaults");

  // ------------------------------------------------------ unlock flow ----
  section("Unlock / relock");
  boot();
  g_fp.db.insert(1); g_fp.whoId = 1;
  clearOut();
  g_fp.platen = PLATEN_ENROLLED;
  pumpMs(500);
  CHECK(sawLine("EVT unlocked id=1"),      "enrolled finger unlocks");
  CHECK(state == ST_RELEASED,              "state is RELEASED after a match");
  CHECK(g_servoAngle == cfg.servoReleased, "latch driven OPEN on unlock");

  clearOut(); g_fp.platen = PLATEN_EMPTY;
  pumpMs(cfg.releaseHoldMs + 800);
  CHECK(sawLine("EVT relocked"),           "auto-relocks after the hold window");
  CHECK(g_servoAngle == cfg.servoLocked,   "latch driven back to LOCKED");
  CHECK(state == ST_SLEEP,                 "returns to sleep after relock");

  boot();
  clearOut(); g_fp.platen = PLATEN_UNKNOWN;
  pumpMs(500);
  CHECK(sawLine("EVT rejected"),           "unenrolled finger is rejected");
  CHECK(g_servoAngle == cfg.servoLocked,   "latch stays LOCKED on a reject");

  boot();
  g_wakeOverride = true; g_wakeValue = HIGH;    // touch line asserted, no readable image
  clearOut();
  CHECK(pumpUntilState(ST_SCAN, 500),      "asserted wake line enters the scan state");
  CHECK(pumpUntilState(ST_SLEEP, SCAN_WINDOW_MS + 2000),
                                           "scan window expires back to sleep when no image arrives");
  CHECK(!sawLine("EVT unlocked"),          "a stuck wake line never opens the wallet");
  { // measure the cost of a wake line that never clears (dirty or wet sensor)
    int cycles = 0;
    uint32_t end = g_millis + 30000;
    State prev = state;
    while (g_millis < end) {
      uint32_t t0 = g_millis; loop(); if (g_millis == t0) g_millis += 1;
      if (prev != ST_SCAN && state == ST_SCAN) cycles++;
      prev = state;
    }
    printf("    NOTE  a permanently asserted wake line re-enters ST_SCAN %d times\n"
           "          per 30 s and never reaches the idle delay - see POWER NOTE.\n",
           cycles);
  }
  g_wakeOverride = false;

  // ---------------------------------------------------------- lockout ----
  section("Lockout after repeated failures");
  boot();
  clearOut();
  for (int i = 0; i < 5; i++) rejectOnce();
  CHECK(sawLine("EVT lockout"),            "5 failed tries trigger lockout");
  CHECK(state == ST_LOCKOUT,               "state is LOCKOUT");
  clearOut();
  g_fp.platen = PLATEN_EMPTY;
  pumpMs(LOCKOUT_MS + 2000);
  CHECK(state == ST_SLEEP,                 "lockout expires after 30 s");
  CHECK(failCount == 0,                    "fail counter resets after lockout");

  boot(); cmd("SET maxfails 2"); clearOut();
  rejectOnce(); rejectOnce();
  CHECK(sawLine("EVT lockout"),            "lockout honours a custom maxfails");

  // ----------------------------------------------------------- tamper ----
  section("Tamper detection");
  boot(); clearOut();
  g_imu.z = 5.0f;                          // 4 g spike, threshold is 2.6 g
  pumpMs(600);
  CHECK(sawLine("EVT tamper"),             "3 spikes inside the window trigger lockdown");
  CHECK(state == ST_LOCKDOWN,              "state is LOCKDOWN");
  CHECK(g_adv.interval == ADV_FAST,        "radio switches to the fast beacon");
  CHECK(g_servoAngle == cfg.servoLocked,   "latch forced LOCKED on tamper");

  clearOut(); g_imu.z = 1.0f; cmd("CLEAR");
  CHECK(sawLine("CLEAR ok"),               "CLEAR acknowledges");
  CHECK(state == ST_SLEEP,                 "CLEAR leaves lockdown");
  CHECK(g_adv.interval == ADV_NORMAL,      "radio returns to the normal interval");

  boot(); clearOut();
  g_imu.z = 5.0f; pumpMs(2);               // one hit only
  g_imu.z = 1.0f; pumpMs(400);
  CHECK(state != ST_LOCKDOWN,              "a single knock does not trip lockdown");

  boot();
  g_fp.db.insert(1); g_fp.whoId = 1; g_fp.platen = PLATEN_ENROLLED;
  pumpMs(500);                             // unlock first
  clearOut(); g_imu.z = 5.0f; pumpMs(600);
  CHECK(state == ST_RELEASED,              "tamper ignored while the owner has it open");

  boot(); clearOut();
  g_imu.z = 5.0f; pumpMs(600);             // into lockdown
  g_imu.z = 1.0f;
  g_fp.db.insert(1); g_fp.whoId = 1; g_fp.platen = PLATEN_ENROLLED;
  clearOut(); pumpMs(600);
  CHECK(sawLine("EVT lockdown cleared"),   "an enrolled finger clears lockdown");
  CHECK(g_adv.interval == ADV_NORMAL,      "beacon slows down again after clearing");

  // ------------------------------------------------------------ leash ----
  section("Separation alarm");
  boot();
  g_connected = true;  pumpMs(200);        // link up
  g_connected = false; clearOut();
  pumpMs(21000);
  CHECK(sawLine("EVT separation warn"),    "quiet warning after sepWarnS (20 s)");
  CHECK(g_adv.interval == ADV_NORMAL,      "warning does not yet speed up the beacon");
  clearOut();
  pumpMs(45000);
  CHECK(sawLine("EVT separation alert"),   "full alert after sepAlertS (60 s)");
  CHECK(g_adv.interval == ADV_FAST,        "alert switches to the fast beacon");

  clearOut(); g_connected = true; pumpMs(200);
  CHECK(sepWarned == false && sepAlerted == false, "reconnecting rearms the alarm");

  boot(); clearOut();                      // never linked since boot
  pumpMs(90000);
  CHECK(!sawLine("EVT separation"),        "no alarm if the phone never connected");

  // ----------------------------------------------------------- enrol ----
  section("Fingerprint enrolment");
  boot(); clearOut();
  CHECK(doEnroll(7, 7),                    "two matching presses enrol successfully");
  CHECK(sawSub("ENROLL ok id=1"),          "first template lands at id 1");
  CHECK(g_fp.db.count(1) == 1,             "template actually stored on the module");
  clearOut(); cmd("STATUS");
  CHECK(sawSub("templates=1"),             "STATUS reflects the new template");

  clearOut();
  doEnroll(7, 7);
  CHECK(sawSub("ENROLL ok id=2"),          "second enrolment gets id 2");

  boot(); clearOut();
  doEnroll(7, 9);                          // two different fingers
  CHECK(sawLine("ENROLL fail no_match"),   "mismatched presses are refused");
  CHECK(g_fp.db.empty(),                   "nothing stored after a failed enrolment");

  boot(); clearOut();
  cmd("ENROLL");
  CHECK(sawLine("ENROLL place"),           "enrolment prompts for the first press");
  pumpMs(21000);                           // never place a finger
  CHECK(sawLine("ENROLL fail timeout"),    "enrolment times out after 20 s");
  CHECK(enrollState == EN_IDLE,            "state machine returns to idle on timeout");

  boot(); clearOut();
  cmd("ENROLL"); cmd("ENROLL");
  CHECK(sawLine("ENROLL busy"),            "a second ENROLL while busy is refused");

  boot(); clearOut();
  cmd("ENROLL"); cmd("CANCEL");
  CHECK(sawLine("CANCEL ok"),              "CANCEL acknowledges");
  CHECK(enrollState == EN_IDLE,            "CANCEL aborts the enrolment");

  boot();
  g_fp.db.insert(1); g_fp.db.insert(2); clearOut();
  cmd("DELETE 1");
  CHECK(sawLine("DELETE ok id=1"),         "DELETE removes a template");
  CHECK(g_fp.db.count(1) == 0,             "template really gone");
  clearOut(); cmd("DELETE 99");
  CHECK(sawLine("DELETE fail id=99"),      "deleting a missing id reports failure");
  clearOut(); cmd("DELETEALL");
  CHECK(sawLine("DELETEALL ok"),           "DELETEALL wipes the database");
  CHECK(g_fp.db.empty(),                   "database is empty afterwards");

  // ---------------------------------------------------------- manual ----
  section("Manual controls");
  boot(); clearOut();
  cmd("RING");
  CHECK(sawLine("RING ok"),                "RING acknowledges");
  CHECK(g_beeps.size() >= 8,               "RING actually drives the buzzer 8 times");

  clearOut(); cmd("UNLOCK");
  CHECK(sawLine("UNLOCK ok"),              "UNLOCK opens the latch");
  CHECK(g_servoAngle == cfg.servoReleased, "servo moved to the released angle");
  clearOut(); cmd("LOCK");
  CHECK(g_servoAngle == cfg.servoLocked,   "LOCK closes it again");
  CHECK(state == ST_SLEEP,                 "LOCK returns to sleep");

  // ---------------------------------------------------------- findmy ----
  section("Find My beacon (compiled out, header verified directly)");
  boot();
  findmyBuildPayload();
  CHECK(findmy_adv[0] == 0x1e,             "AD length byte is 0x1e (30)");
  CHECK(findmy_adv[1] == 0xff,             "AD type is manufacturer specific");
  CHECK(findmy_adv[2] == 0x4c && findmy_adv[3] == 0x00,
                                           "Apple company id 0x004C, little endian");
  CHECK(findmy_adv[4] == 0x12 && findmy_adv[5] == 0x19,
                                           "offline-finding type and payload length");
  { bool keyOk = true;
    for (int i = 0; i < 22; i++) if (findmy_adv[7+i] != FINDMY_PUBLIC_KEY[6+i]) keyOk = false;
    CHECK(keyOk,                           "key bytes 6..27 copied into the payload"); }
  CHECK(findmy_adv[29] == (uint8_t)(FINDMY_PUBLIC_KEY[0] >> 6),
                                           "top two bits of key[0] land in byte 29");

  findmySetAddress();
  CHECK(g_addr.addr_type == BLE_GAP_ADDR_TYPE_RANDOM_STATIC,
                                           "BLE address type is random-static");
  CHECK((g_addr.addr[5] & 0xC0) == 0xC0,   "top two address bits forced to 11");
  CHECK(g_addr.addr[5] == (uint8_t)(FINDMY_PUBLIC_KEY[0] | 0xC0) &&
        g_addr.addr[0] == FINDMY_PUBLIC_KEY[5],
                                           "address bytes reversed for the SoftDevice");

  findmyStart();
  CHECK(g_adv.data.size() == 29,           "29 payload bytes handed to the radio");
  CHECK(!g_adv.connectable,                "beacon advertising is non-connectable");
  CHECK(g_adv.interval == 1600,            "beacon interval is 1 s");

  { bool placeholder = true;
    for (int i = 0; i < 28; i++) if (FINDMY_PUBLIC_KEY[i]) placeholder = false;
    if (placeholder)
      printf("    NOTE  FINDMY_PUBLIC_KEY is still all zeros - the beacon would\n"
             "          advertise but no report could ever be decrypted.\n"); }

  // ----------------------------------------------------------- summary --
  printf("\n----------------------------------------------------------------------\n");
  printf("  %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf("\n  Failures:\n");
    for (auto &f : g_failures) printf("    - %s\n", f.c_str());
  }
  printf("\n");
  return g_fail ? 1 : 0;
}
