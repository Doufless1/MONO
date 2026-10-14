/* ============================================================================
   MONO firmware - host test harness
   ----------------------------------------------------------------------------
   Compiles mono_firmware.ino UNMODIFIED against mock hardware and drives it
   with a virtual clock. No board, no sensor, no servo, no battery.

   Built with the target's own language standard, so anything the real
   toolchain would reject is rejected here first:

     g++ -std=gnu++11 -Wall -Wextra -x c++ -I tests/mocks \
         -o tests/build/test_firmware tests/test_firmware.cpp

   Add -DMONO_ENABLE_FINDMY=1 or -DMONO_BENCH=1 for the other two builds.
   tests/run_all.sh runs all three.

   Tags like [REVIEW F7] point at docs/REVIEW.md, where each bug this suite
   pins down is written up with the reproduction that found it.

   What this can prove : protocol, parsing, the security policy, settings,
                         persistence and every state transition.
   What it cannot prove : real BLE timing, R502 UART behaviour, servo torque,
                          current draw. Those need the hardware.
   ========================================================================= */

// Every mocked global lives in one place, shared with mono_sim.cpp and the
// WebAssembly build so the three can never diverge.
#include "../sim/mocks/mock_impl.h"

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

static bool startsWith(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

static bool sawIn(const std::vector<std::string> &v, const char *p) {
  for (size_t i = 0; i < v.size(); i++) if (startsWith(v[i], p)) return true;
  return false;
}
static int countIn(const std::vector<std::string> &v, const char *p) {
  int n = 0;
  for (size_t i = 0; i < v.size(); i++) if (startsWith(v[i], p)) n++;
  return n;
}
static std::string lastIn(const std::vector<std::string> &v, const char *p) {
  for (size_t i = v.size(); i-- > 0;) if (startsWith(v[i], p)) return v[i];
  return "";
}
static bool subIn(const std::vector<std::string> &v, const char *needle) {
  for (size_t i = 0; i < v.size(); i++) if (v[i].find(needle) != std::string::npos) return true;
  return false;
}
static bool sawUsb(const char *p) { return sawIn(g_out, p); }
static bool sawBle(const char *p) { return sawIn(g_bleOut, p); }

// " key=value" out of a reply line
static std::string field(const std::string &line, const char *key) {
  std::string k = std::string(" ") + key + "=";
  size_t i = line.find(k);
  if (i == std::string::npos) return "";
  i += k.size();
  size_t j = line.find(' ', i);
  return line.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

static void clearOut() { g_out.clear(); g_bleOut.clear(); bleuart.tx.clear(); }

// ------------------------------------------------------------ the clock --
static void pumpMs(uint32_t ms) {
  uint32_t end = g_millis + ms;
  while ((int32_t)(g_millis - end) < 0) {
    uint32_t t0 = g_millis;
    loop();
    if (g_millis == t0) g_millis += 1;   // a real MCU loop is not free
  }
}

template <class F> static bool pumpUntil(F done, uint32_t maxMs) {
  uint32_t end = g_millis + maxMs;
  while ((int32_t)(g_millis - end) < 0) {
    uint32_t t0 = g_millis;
    loop();
    if (g_millis == t0) g_millis += 1;
    if (done()) return true;
  }
  return false;
}

static void alignTo(uint32_t grid) { while (g_millis % grid) g_millis++; }

static uint32_t g_longestDelay = 0;
static void recordDelay(uint32_t ms) { if (ms > g_longestDelay) g_longestDelay = ms; }

// --------------------------------------------------------- the hardware --
static void hardReset(bool wipeFS) {
  g_millis = 0; g_beeps.clear(); g_buzzerOn = false; g_servoWrites.clear(); clearOut();
  memset(g_pinState, 0, sizeof(g_pinState));
  g_vbatRaw = 1868.0f;
  g_fp = MockFP(); g_imu = MockIMU(); g_adv = AdvState(); g_addr = g_identityAddr;
  g_connected = false; g_servoAngle = -1; g_servoAttached = false;
  g_wakeOverride = false; g_rngEmpty = false; g_rngState = 0x9E3779B97F4A7C15ULL;
  if (wipeFS) InternalFS.files.clear();
  bleuart.in.clear(); bleuart.rd = 0; bleuart.partial.clear();
  Serial.in.clear(); Serial.rd = 0; Serial.pending.clear();
  mono = MonoState();                       // the power cycle the comment promises
  linkEv.connected = linkEv.disconnected = false; linkEv.reason = 0;
  settingsDefaults();
}

static void boot(bool wipeFS = true) { hardReset(wipeFS); setup(); clearOut(); }

static const int OWNER = 1, FRIEND = 7, STRANGER = 0;   // finger identities
static void touch(int who) { g_fp.platen = PLATEN_UNKNOWN; g_fp.fingerName = who; }
static void lift()         { g_fp.platen = PLATEN_EMPTY;   g_fp.fingerName = 0; }
static void tapFinger(int who) { touch(who); pumpMs(300); lift(); pumpMs(300); }

// ------------------------------------------------------------ the phone --
static const uint8_t WALKED_AWAY = 0x08, HUNG_UP = 0x13;
static uint8_t g_key[PHONE_KEY_LEN];

static void bleSend(const std::string &s) { bleuart.feed(s + "\n"); loop(); }
static void usbSend(const std::string &s) { Serial.feed(s + "\n"); loop(); }
static void phoneArrives() { g_connected = true; Bluefruit.Periph.onConn(0); loop(); }
static void phoneLeaves(uint8_t why) { g_connected = false; Bluefruit.Periph.onDisc(0, why); loop(); }

static bool takeKey() {
  std::string k = field(lastIn(g_bleOut, "PAIR ok"), "key");
  return k.size() == 2 * PHONE_KEY_LEN && fromHex(k.c_str(), g_key, PHONE_KEY_LEN);
}
static std::string hello() { bleSend("HELLO"); return field(lastIn(g_bleOut, "HELLO"), "nonce"); }
static std::string answer(const uint8_t *key, const std::string &nonceHex) {
  uint8_t nonce[NONCE_LEN], mac[32];
  char hex[65];
  if (!fromHex(nonceHex.c_str(), nonce, NONCE_LEN)) return "nonce?";
  authMac(key, nonce, mac);
  toHex(mac, 32, hex);
  return hex;
}
static bool phoneAuth() {
  std::string n = hello();
  bleSend("AUTH " + answer(g_key, n));
  return startsWith(lastIn(g_bleOut, "AUTH"), "AUTH ok");
}

// A factory-fresh wallet, claimed by a phone that is still connected and
// trusted. Most sections start here.
static bool bootPaired() {
  boot();
  phoneArrives();
  bleSend("PAIR");
  bool ok = takeKey();
  pumpMs(50);
  clearOut();
  return ok;
}

static bool enrollFinger(int who) {
  size_t before = g_bleOut.size();
  bleSend("ENROLL");
  touch(who); pumpMs(300); lift(); pumpMs(300);
  touch(who); pumpMs(300); lift(); pumpMs(100);
  for (size_t i = before; i < g_bleOut.size(); i++)
    if (startsWith(g_bleOut[i], "ENROLL ok")) return true;
  return false;
}

static bool bootWithOwner() { bool ok = bootPaired() && enrollFinger(OWNER); pumpMs(50); clearOut(); return ok; }

// =============================================================== tests ====
int main() {
  printf("\n================ MONO firmware - hardware-free test run ================\n");
#if MONO_BENCH
  printf("  build: MONO_BENCH=1\n");
#endif
#if MONO_ENABLE_FINDMY
  printf("  build: MONO_ENABLE_FINDMY=1\n");
#endif

  // ------------------------------------------------------------ boot ----
  section("Boot");
  hardReset(true);
  setup();
  CHECK(mono.sensor == SEN_READY,           "boots with the sensor ready");
  CHECK(!mono.latch.open,                   "boots locked");
  CHECK(g_servoAngle == cfg.servoLocked,    "latch driven to LOCKED on boot");
  pumpMs(SERVO_SETTLE_MS + 50);
  CHECK(g_servoAttached == false,           "servo detached once it has settled (idle draw = 0)");
  CHECK(g_adv.running,                      "advertising started");
  CHECK(g_adv.interval == ADV_NORMAL,       "advertising at the normal interval");
  CHECK(g_adv.connectable,                  "advertising is connectable so the app can pair");
  CHECK(sawUsb("R502 online"),              "fingerprint sensor detected at boot");
  CHECK(cfg.magic == SETTINGS_MAGIC,        "settings initialised from defaults");
  CHECK(setupMode() && subIn(g_out, "setup mode"), "a factory-fresh wallet says it is in setup mode");
  CHECK(g_pinState[VBAT_ENABLE] == LOW,     "battery divider enable is driven LOW [REVIEW F10]");
  usbSend("STATUS"); pumpMs(BATT_PERIOD_MS + 1000);
  CHECK(g_pinState[VBAT_ENABLE] == LOW,     "... and stays LOW through every battery read [REVIEW F10]");

  hardReset(true); g_fp.password = false; setup();
  CHECK(subIn(g_out, "R502 NOT FOUND"),     "missing sensor is reported, not silently ignored");
  CHECK(!setupMode(),                       "a wallet without its sensor is never factory-fresh");

  // ------------------------------------------------------- framing ----
  section("Line protocol");
  boot();                                   // setup mode: the USB console is trusted
  usbSend("HELP");
  CHECK(sawUsb("HELP STATUS"),              "HELP lists the command set, and starts with HELP");

  clearOut(); usbSend("STATUS");
  std::string st = lastIn(g_out, "STATUS ");
  CHECK(!st.empty(),                        "STATUS replies");
  CHECK(field(st, "latch") == "locked",     "STATUS names the latch state");
  CHECK(field(st, "fingers") == "0",        "STATUS carries the finger count");
  CHECK(startsWith(field(st, "vbat"), "4.0"), "STATUS reports a plausible battery voltage");
  CHECK(field(st, "link") == "0",           "STATUS reports the link down when disconnected");

  clearOut(); usbSend("status");
  CHECK(sawUsb("STATUS "),                  "lower-case commands are accepted");
  clearOut(); usbSend("  STATUS");
  CHECK(sawUsb("STATUS "),                  "leading whitespace tolerated");
  clearOut(); usbSend("WOBBLE");
  CHECK(sawUsb("ERR unknown WOBBLE"),       "unknown command rejected cleanly");
  clearOut(); usbSend("");
  CHECK(g_out.empty(),                      "empty line is ignored, not answered");
  clearOut(); Serial.feed("STATUS\r\n"); loop();
  CHECK(sawUsb("STATUS "),                  "CRLF line endings handled");

  bootPaired();
  bleuart.feed("STA"); loop();
  CHECK(!sawBle("STATUS "),                 "a partial command is buffered, not acted on");
  bleuart.feed("TUS\n"); loop();
  CHECK(sawBle("STATUS "),                  "a command split across BLE packets reassembles");

  clearOut();
  g_fp.db[1] = OWNER; g_fp.db[2] = FRIEND;
  bleSend("DELETEALL " + std::string(300, 'x'));
  CHECK(sawBle("ERR toolong"),              "a 310-character line is refused whole [REVIEW F9]");
  CHECK(g_fp.db.size() == 2,                "... and its DELETEALL prefix never runs [REVIEW F9]");
  clearOut(); bleSend("STATUS");
  CHECK(sawBle("STATUS "),                  "the next line after an overlong one works normally");

  clearOut();
  bleuart.feed("STA"); loop();
  Serial.feed("HELP\n"); loop();
  bleuart.feed("TUS\n"); loop();
  CHECK(sawBle("STATUS ") && sawUsb("HELP "), "USB and BLE lines arriving interleaved stay intact [REVIEW F6]");
  CHECK(!subIn(g_out, "ERR unknown") && !subIn(g_bleOut, "ERR unknown"),
                                            "... neither is corrupted into the other");
#if !MONO_BENCH
  CHECK(!sawUsb("STATUS "),                 "a reply goes to the port that asked, not both");
#endif

  // ------------------------------------------------------- security ----
  section("Who may do what");
  boot(); g_fp.db[1] = OWNER; mono.slots = 1;   // owned by somebody, nobody paired here
  cfg.phones = 1; settingsSave();               // ... and some other phone holds the key
  phoneArrives(); clearOut();
  bleSend("UNLOCK");
  CHECK(sawBle("ERR auth UNLOCK"),          "an unpaired phone cannot UNLOCK [REVIEW F1]");
  CHECK(!mono.latch.open && g_servoAngle == cfg.servoLocked, "... and the latch never moves [REVIEW F1]");
  bleSend("STATUS");
  CHECK(sawBle("ERR auth STATUS"),          "an unpaired phone learns nothing from STATUS");
  bleSend("SET servolock 95");
  CHECK(sawBle("ERR auth SET") && cfg.servoLocked == 20,
                                            "an unpaired phone cannot retune the latch to stay open");
  bleSend("ENROLL");
  CHECK(sawBle("ERR auth ENROLL") && !enrolling(),
                                            "an unpaired phone cannot enrol a finger of its own");
  bleSend("UNPAIR");
  CHECK(sawBle("ERR auth UNPAIR") && cfg.phones == 1,
                                            "... nor unpair the owner's phone");
  bleSend("HELP");
  CHECK(sawBle("HELP "),                    "HELP is open to anyone");

  boot(); phoneArrives(); clearOut();
  bleSend("PAIR");
  std::string pr = lastIn(g_bleOut, "PAIR ok");
  CHECK(!pr.empty() && field(pr, "key").size() == 32, "a factory-fresh wallet pairs at once and hands over a 128-bit key");
  CHECK(mono.ble.authed,                    "... and trusts that phone straight away");
  CHECK(!setupMode(),                       "... after which it is no longer factory-fresh");
  CHECK(!subIn(g_out, field(pr, "key").c_str()), "the key never appears on the USB console");
  CHECK(pumpUntil([] { return sawBle("STATUS "); }, 50), "the new owner is sent a STATUS without asking");

  // a second phone
  takeKey();
  uint8_t ownerKey[PHONE_KEY_LEN]; memcpy(ownerKey, g_key, sizeof ownerKey);
  bleSend("ENROLL"); touch(OWNER); pumpMs(300); lift(); pumpMs(300); touch(OWNER); pumpMs(300); lift(); pumpMs(300);
  phoneLeaves(HUNG_UP); phoneArrives(); clearOut();
  bleSend("PAIR");
  CHECK(sawBle("PAIR wait"),                "once owned, a new phone has to wait for the owner's finger");
  pumpMs(PAIR_CONFIRM_MS + 500);
  CHECK(sawBle("PAIR fail timeout"),        "nobody touched the sensor: the request expires");
  CHECK(!mono.ble.authed && cfg.phones == 1, "... and no key was issued");

  clearOut(); bleSend("PAIR"); tapFinger(STRANGER);
  CHECK(!sawBle("PAIR ok") && mono.failCount == 1,
                                            "a stranger's finger does not confirm it, and counts as a wrong try");
  tapFinger(OWNER);
  CHECK(sawBle("PAIR ok slot=1"),           "the owner's finger confirms the new phone");
  CHECK(!mono.latch.open,                   "... and the confirming touch does not open the wallet");
  CHECK(cfg.phones == 3,                    "two phones are now paired");

  // challenge and response
  phoneLeaves(HUNG_UP); phoneArrives(); clearOut();
  memcpy(g_key, ownerKey, sizeof g_key);
  std::string n1 = hello(), n2 = hello();
  CHECK(n1.size() == 32 && n2.size() == 32 && n1 != n2, "HELLO hands out a fresh 128-bit nonce every time");
  bleSend("AUTH " + answer(g_key, n1));
  CHECK(sawBle("AUTH fail"),                "only the latest nonce counts");
  std::string n3 = hello();
  std::string good = answer(g_key, n3);
  bleSend("AUTH " + good);
  CHECK(sawBle("AUTH ok slot=0"),           "the right HMAC authenticates the phone");
  bleSend("STATUS");
  CHECK(sawBle("STATUS "),                  "... which may now read STATUS");

  phoneLeaves(HUNG_UP); phoneArrives(); clearOut();
  hello();
  bleSend("AUTH " + good);
  CHECK(sawBle("AUTH fail") && !mono.ble.authed, "a recorded AUTH replayed against a new nonce fails");
  bleSend("AUTH " + good);
  CHECK(sawBle("AUTH fail nonce"),          "a nonce is good for one answer only");
  std::string n4 = hello();
  uint8_t wrong[PHONE_KEY_LEN]; memcpy(wrong, ownerKey, sizeof wrong); wrong[0] ^= 1;
  bleSend("AUTH " + answer(wrong, n4));
  CHECK(sawBle("AUTH fail") && !mono.ble.authed, "a key one bit wrong is refused");
  hello(); clearOut(); bleSend("AUTH xyz");
  CHECK(sawBle("AUTH fail format"),         "a malformed answer is refused");
  clearOut(); usbSend("AUTH 00"); usbSend("PAIR");
  CHECK(sawUsb("AUTH fail port") && sawUsb("PAIR fail port"), "pairing and AUTH only make sense over BLE");

  pumpMs(OWNER_WINDOW_MS + 100);            // let the confirming touch's window lapse
  clearOut(); usbSend("UNLOCK");
#if MONO_BENCH
  CHECK(sawUsb("UNLOCK ok"),                "bench build: the USB console may UNLOCK");
  pumpMs(cfg.releaseHoldMs + 500);
#else
  CHECK(sawUsb("ERR auth UNLOCK") && !mono.latch.open,
                                            "the USB port on the outside of the wallet cannot UNLOCK it");
  tapFinger(OWNER); pumpMs(cfg.releaseHoldMs + 500); clearOut();
  usbSend("STATUS");
  CHECK(sawUsb("STATUS "),                  "for a minute after the owner's finger, the console is trusted");
  pumpMs(OWNER_WINDOW_MS); clearOut(); usbSend("STATUS");
  CHECK(sawUsb("ERR auth STATUS"),          "... and then it is not");
#endif

  phoneAuth(); clearOut();
  bleSend("UNPAIR");
  CHECK(sawBle("UNPAIR ok") && cfg.phones == 0, "UNPAIR forgets every phone");
  CHECK(!mono.ble.authed,                   "... including the one that asked");
  phoneLeaves(HUNG_UP); phoneArrives();
  CHECK(!phoneAuth(),                       "a forgotten phone's key no longer opens a session");

  bootPaired(); cfg.phones = 0x0F; settingsSave(); enrollFinger(OWNER);
  phoneLeaves(HUNG_UP); phoneArrives(); clearOut();
  bleSend("PAIR");
  CHECK(sawBle("PAIR fail full"),           "with four phones paired, a fifth is told so");

  boot(); g_rngEmpty = true; phoneArrives(); clearOut();
  bleSend("PAIR");
  CHECK(sawBle("PAIR fail rng") && cfg.phones == 0, "no randomness, no key: pairing fails safe");
  bleSend("HELLO");
  CHECK(field(lastIn(g_bleOut, "HELLO"), "nonce") == "-", "... and HELLO offers no nonce to answer");

  // ------------------------------------------------------ SHA-256 ----
  section("SHA-256 and HMAC (NIST and RFC 4231 vectors)");
  {
    struct V { std::string msg; const char *want; } vs[] = {
      { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
      { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
      { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
      { std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
      { std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
      { std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
      { std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" },
    };
    const char *names[] = { "empty message", "\"abc\"", "448-bit message",
                            "55 bytes (padding fits)", "56 bytes (padding spills a block)",
                            "exactly one block", "one million 'a'" };
    for (int i = 0; i < 7; i++) {
      Sha256 s; uint8_t d[32]; char hex[65];
      sha256Init(&s); sha256Update(&s, (const uint8_t *)vs[i].msg.data(), vs[i].msg.size());
      sha256Final(&s, d); toHex(d, 32, hex);
      CHECK(!strcmp(hex, vs[i].want), (std::string("SHA-256 of ") + names[i]).c_str());
    }
    uint8_t mac[32]; char hex[65];
    uint8_t k1[20]; memset(k1, 0x0b, sizeof k1);
    hmacSha256(k1, 20, (const uint8_t *)"Hi There", 8, mac); toHex(mac, 32, hex);
    CHECK(!strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC-SHA256 RFC 4231 case 1");
    const char *m2 = "what do ya want for nothing?";
    hmacSha256((const uint8_t *)"Jefe", 4, (const uint8_t *)m2, strlen(m2), mac); toHex(mac, 32, hex);
    CHECK(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "HMAC-SHA256 RFC 4231 case 2");
    uint8_t k6[131]; memset(k6, 0xaa, sizeof k6);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmacSha256(k6, sizeof k6, (const uint8_t *)m6, strlen(m6), mac); toHex(mac, 32, hex);
    CHECK(!strcmp(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "HMAC-SHA256 RFC 4231 case 6 (key longer than a block)");
    uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
    CHECK(equalConstantTime(a, a, 4) && !equalConstantTime(a, b, 4), "constant-time compare tells equal from unequal");
  }

  // -------------------------------------------------------- settings ----
  section("Settings");
  bootPaired(); clearOut();
  bleSend("GET");
  std::string gl = lastIn(g_bleOut, "GET ");
  CHECK(field(gl, "relock") == "6000",      "GET reports default relock");
  CHECK(field(gl, "tamper") == "2500",      "GET reports a default tamper threshold the app has a preset for");
  CHECK(field(gl, "maxfails") == "5",       "GET reports default max fails");
  CHECK(field(gl, "findmy") == "0",         "Find My off by default");
  CHECK(field(gl, "phones") == "1",         "GET counts the paired phones");

  clearOut(); bleSend("SET relock 3000");
  CHECK(sawBle("SET ok relock=3000") && cfg.releaseHoldMs == 3000, "SET relock takes effect");
  clearOut(); bleSend("SET relock 999999");
  CHECK(sawBle("SET ok relock=60000"),      "over-range relock clamps, and the reply says what was stored [REVIEW F8]");
  clearOut(); bleSend("SET relock 5");
  CHECK(sawBle("SET ok relock=1000"),       "under-range relock clamps to 1000 ms");
  clearOut(); bleSend("SET tamper 99999");
  CHECK(sawBle("SET ok tamper=8000"),       "over-range tamper clamps to 8000 mg");
  clearOut(); bleSend("SET servolock 900");
  CHECK(sawBle("SET ok servolock=180"),     "servo angle clamps to 180 deg");
  CHECK(g_servoAngle == 180,                "... and the latch moves to it at once, for tuning at the bench");
  bleSend("SET servolock 20");
  clearOut(); bleSend("SET relock banana");
  CHECK(sawBle("SET fail badval relock") && cfg.releaseHoldMs == 1000,
                                            "a non-number is refused, not stored as 0 [REVIEW F8]");
  clearOut(); bleSend("SET relock 12x");
  CHECK(sawBle("SET fail badval relock"),   "trailing junk is refused too");
  clearOut(); bleSend("SET nonsense 5");
  CHECK(sawBle("SET fail unknown nonsense"), "unknown setting key rejected");
  clearOut(); bleSend("SET relock");
  CHECK(sawBle("SET fail noval"),           "SET with no value rejected");
  clearOut(); bleSend("SET");
  CHECK(sawBle("SET fail noarg"),           "bare SET rejected");
  clearOut(); bleSend("SET sepwarn 0");
  CHECK(sawBle("SET ok sepwarn=0"),         "the leash can really be switched off");
  bleSend("STATUS");
  CHECK(field(lastIn(g_bleOut, "STATUS "), "leash") == "off", "... and STATUS says so");

  bootPaired();
  bleSend("SET sepalert 300");
  bleSend("SET maxfails 3");
  hardReset(false); setup();               // battery pull: flash survives
  phoneArrives();
  CHECK(phoneAuth(),                        "a paired phone is still paired after a power cycle");
  clearOut(); bleSend("GET");
  CHECK(field(lastIn(g_bleOut, "GET "), "sepalert") == "300", "settings survive a power cycle");
  CHECK(field(lastIn(g_bleOut, "GET "), "maxfails") == "3",   "second setting survives too");

  InternalFS.files["/mono.cfg"][10] ^= 0xFF;   // one flipped byte
  hardReset(false); setup();
  CHECK(cfg.maxFails == 5 && cfg.phones == 0, "a corrupted blob fails its CRC and falls back to defaults");

  InternalFS.files["/mono.cfg"] = std::vector<uint8_t>(16, 0);
  memcpy(&InternalFS.files["/mono.cfg"][0], "2NOM", 4);   // "MON2", little endian: a v2 blob
  hardReset(false); setup();
  CHECK(cfg.magic == SETTINGS_MAGIC && cfg.releaseHoldMs == 6000, "a v2 settings blob is not misread as v3");

  settingsDefaults(); cfg.maxFails = 200; cfg.servoReleased = 250;
  settingsSave();                           // a valid CRC over invalid values
  hardReset(false); setup();
  CHECK(cfg.maxFails == 20 && cfg.servoReleased == 180, "even a blob that passes its CRC is clamped before use");

  // ----------------------------------------------------- unlock flow ----
  section("Unlock and relock");
  bootWithOwner();
  touch(OWNER); pumpMs(300);
  CHECK(sawBle("EVT unlocked id=1"),        "the enrolled finger unlocks");
  CHECK(mono.latch.open,                    "the latch is open after a match");
  CHECK(g_servoAngle == cfg.servoReleased,  "latch driven OPEN on unlock");
  CHECK(field(lastIn(g_bleOut, "STATUS "), "latch") == "open", "the phone is told at once, without asking [REVIEW A1]");

  clearOut(); lift();
  pumpMs(cfg.releaseHoldMs + 500);
  CHECK(sawBle("EVT relocked"),             "auto-relocks after the hold window");
  CHECK(g_servoAngle == cfg.servoLocked,    "latch driven back to LOCKED");
  CHECK(field(lastIn(g_bleOut, "STATUS "), "latch") == "locked", "... and the phone is told that too");

  bootWithOwner();
  tapFinger(OWNER); bleSend("ENROLL");
  pumpMs(cfg.releaseHoldMs + 500);
  CHECK(!mono.latch.open && g_servoAngle == cfg.servoLocked,
                                            "the wallet relocks on time even while enrolling [REVIEW F11]");
  bleSend("CANCEL");

  bootWithOwner();
  touch(STRANGER); pumpMs(300);
  CHECK(sawBle("EVT rejected"),             "an unenrolled finger is rejected");
  CHECK(!mono.latch.open && g_servoAngle == cfg.servoLocked, "latch stays LOCKED on a reject");
  lift(); pumpMs(300);

  bootWithOwner();
  g_fp.platen = PLATEN_SMUDGE; pumpMs(2000);
  CHECK(mono.failCount == 0 && !sawBle("EVT rejected"),
                                            "a smudged image is not held against you as a wrong finger");
  lift(); pumpMs(300);

  bootWithOwner();
  g_wakeOverride = true; g_wakeValue = HIGH;    // touch line stuck high, no readable image
  CHECK(pumpUntil([] { return mono.sensor == SEN_SCAN; }, 500), "an asserted wake line starts a scan");
  CHECK(pumpUntil([] { return mono.sensor == SEN_READY; }, SCAN_WINDOW_MS + 500),
                                            "the scan window expires when no image arrives");
  {
    int scans = 0; SensorMode prev = mono.sensor;
    uint32_t end = g_millis + 30000;
    while (g_millis < end) {
      uint32_t t0 = g_millis; loop(); if (g_millis == t0) g_millis += 1;
      if (prev != SEN_SCAN && mono.sensor == SEN_SCAN) scans++;
      prev = mono.sensor;
    }
    CHECK(scans == 0, "a wake line that never clears is not rescanned again and again [REVIEW F2]");
  }
  g_wakeValue = LOW; pumpMs(100); g_wakeValue = HIGH;
  CHECK(pumpUntil([] { return mono.sensor == SEN_SCAN; }, 500), "once the line clears, the next touch scans again");
  g_wakeOverride = false; pumpMs(SCAN_WINDOW_MS + 500);
  CHECK(!sawBle("EVT unlocked"),            "a stuck wake line never opens the wallet");

  bootPaired(); clearOut();
  bleSend("ENROLL");
  touch(OWNER); pumpMs(300); lift(); pumpMs(300); touch(OWNER); pumpMs(300);
  CHECK(sawBle("ENROLL ok"),                "enrolment completes with the finger still down");
  pumpMs(2000);
  CHECK(!sawBle("EVT unlocked") && !sawBle("EVT rejected") && mono.sensor == SEN_READY,
                                            "the freshly enrolled finger is not rescanned until it lifts [REVIEW F3]");
  lift(); pumpMs(200); touch(OWNER); pumpMs(300);
  CHECK(sawBle("EVT unlocked id=1"),        "lift and touch again: it opens");

  // ---------------------------------------------------------- lockout ----
  section("Lockout after repeated failures");
  bootWithOwner();
  for (int i = 0; i < 5; i++) tapFinger(STRANGER);
  CHECK(sawBle("EVT lockout"),              "5 wrong fingers trigger lockout");
  CHECK(mono.sensor == SEN_LOCKOUT,         "the sensor is locked out");
  clearOut(); tapFinger(OWNER);
  CHECK(!sawBle("EVT unlocked"),            "during lockout even the right finger is ignored");
  bleSend("LOCK");
  CHECK(mono.sensor == SEN_LOCKOUT,         "LOCK does not end a lockout early [REVIEW F4]");
  pumpMs(LOCKOUT_MS + 1000);
  CHECK(mono.sensor == SEN_READY && sawBle("EVT lockout end"), "lockout expires after 30 s, and says so");
  CHECK(mono.failCount == 0,                "fail counter resets after lockout");

  bootWithOwner(); bleSend("SET maxfails 2"); clearOut();
  tapFinger(STRANGER); tapFinger(STRANGER);
  CHECK(sawBle("EVT lockout"),              "lockout honours a custom maxfails");
  bleSend("CLEAR");
  CHECK(mono.sensor == SEN_READY && mono.failCount == 0, "CLEAR from the owner's phone ends a lockout");

  bootWithOwner();
  touch(STRANGER); pumpMs(10000); lift(); pumpMs(300);
  CHECK(mono.failCount == 1,                "a wrong thumb left resting on the sensor costs one try, not several");

  // ----------------------------------------------------------- tamper ----
  section("Tamper detection and lockdown");
  bootWithOwner();
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false;
  CHECK(sawBle("EVT tamper"),               "shaking it raises the alarm");
  CHECK(mono.alarm.on,                      "the wallet is in lockdown");
  CHECK(g_adv.interval == ADV_FAST,         "radio switches to the fast beacon");
  CHECK(g_servoAngle == cfg.servoLocked,    "latch forced LOCKED");
  CHECK(field(lastIn(g_bleOut, "STATUS "), "alarm") == "1", "the phone is told without asking [REVIEW A1]");
  CHECK(countIn(g_bleOut, "EVT tamper") == 1, "continued shaking raises one alarm, not a flood of them");

  {
    size_t b0 = g_beeps.size();
    pumpMs(6000);
    CHECK(g_beeps.size() - b0 >= 12,        "it keeps beeping in bursts, as the app tells the owner [REVIEW F12]");
    pumpMs(ALARM_SOUND_MS);
    size_t b1 = g_beeps.size();
    pumpMs(10000);
    CHECK(g_beeps.size() == b1,             "it goes quiet a minute after the last shake, to save the battery");
    CHECK(mono.alarm.on && g_adv.interval == ADV_FAST, "... but stays locked down and beaconing");
    g_imu.shaking = true; pumpMs(600); g_imu.shaking = false;
    CHECK(g_beeps.size() > b1,              "shaking it again starts the beeping again");
  }

  clearOut();
  for (int i = 0; i < 5; i++) tapFinger(STRANGER);
  CHECK(sawBle("EVT lockout") && mono.alarm.on, "wrong fingers during lockdown count toward a lockout [REVIEW F5]");
  bleSend("LOCK");
  CHECK(mono.alarm.on && g_adv.interval == ADV_FAST, "LOCK does not quietly cancel the alarm [REVIEW F4]");
  clearOut(); bleSend("CLEAR");
  CHECK(sawBle("CLEAR ok") && !mono.alarm.on, "CLEAR ends the lockdown");
  pumpMs(50);
  CHECK(g_adv.interval == ADV_NORMAL,       "... and the radio returns to the normal interval");

  bootWithOwner();
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false; clearOut();
  touch(OWNER); pumpMs(300); lift();
  CHECK(sawBle("EVT lockdown cleared") && sawBle("EVT unlocked id=1"), "the owner's finger clears lockdown and opens");
  pumpMs(50);
  CHECK(g_adv.interval == ADV_NORMAL,       "beacon slows down again after clearing");

  bootWithOwner();
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false; clearOut();
  bleSend("UNLOCK");
  CHECK(sawBle("UNLOCK ok") && !mono.alarm.on && mono.latch.open, "UNLOCK from the paired phone ends the alarm too");

  bootWithOwner();
  g_imu.jolt(g_millis + 10, 30); pumpMs(400);
  CHECK(!mono.alarm.on,                     "a single knock does not trip lockdown");
  g_imu.z = 5.0f; pumpMs(1600); g_imu.z = 1.0f; pumpMs(100);
  CHECK(!mono.alarm.on,                     "a sustained push is one knock, not three");
  uint32_t t0 = g_millis;
  g_imu.jolt(t0 + 100, 30); g_imu.jolt(t0 + 900, 30); g_imu.jolt(t0 + 1800, 30);
  pumpMs(2200);
  CHECK(!mono.alarm.on,                     "three knocks spread over more than 1.5 s are not an attack");

  {
    // The same physical knocks, at the same instants, in three different
    // states. v2 sampled once per loop pass, so the verdict depended on the
    // state; v3 samples a fixed 50 Hz grid, so it must not.
    struct Case { const char *name; int setup; } cases[] = {
      { "while idle", 0 }, { "while a finger is being rejected", 1 }, { "while locked out", 2 } };
    bool longKnocks[3], shortKnocks[3];
    for (int c = 0; c < 3; c++) {
      for (int pass = 0; pass < 2; pass++) {
        bootWithOwner();
        if (cases[c].setup == 1) touch(STRANGER);
        if (cases[c].setup == 2) { bleSend("SET maxfails 1"); tapFinger(STRANGER); }
        pumpMs(200); alignTo(1000);
        uint32_t s = g_millis, len = pass == 0 ? 30 : 3;
        uint32_t off = pass == 0 ? 0 : 5;   // the 3 ms knocks land between two 20 ms samples
        g_imu.jolt(s + off, len); g_imu.jolt(s + 300 + off, len); g_imu.jolt(s + 600 + off, len);
        pumpMs(900);
        (pass == 0 ? longKnocks : shortKnocks)[c] = mono.alarm.on;
        lift();
      }
    }
    CHECK(longKnocks[0] && longKnocks[1] && longKnocks[2],
          "three 30 ms knocks raise the alarm in every state [REVIEW F7]");
    CHECK(shortKnocks[0] == shortKnocks[1] && shortKnocks[1] == shortKnocks[2],
          "the same 3 ms knocks get the same verdict in every state [REVIEW F7]");
  }

  bootWithOwner();
  tapFinger(OWNER);                         // open
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false;
  CHECK(!mono.alarm.on,                     "tamper is ignored while the owner has it open");

  // ------------------------------------------------------------ leash ----
  section("Separation alarm");
  bootPaired();
  phoneLeaves(WALKED_AWAY); clearOut();
  pumpMs(21000);
  CHECK(sawUsb("EVT separation warn"),      "quiet warning 20 s after the link is lost");
  CHECK(g_adv.interval == ADV_NORMAL,       "the warning does not yet speed up the beacon");
  clearOut(); pumpMs(45000);
  CHECK(sawUsb("EVT separation alert"),     "full alert after 60 s");
  CHECK(g_adv.interval == ADV_FAST,         "the alert switches to the fast beacon");

  phoneArrives(); pumpMs(1000);
  CHECK(mono.leash.alerted && g_adv.interval == ADV_FAST,
                                            "a stranger connecting does not silence the alarm [REVIEW F13]");
  CHECK(phoneAuth(),                        "the paired phone comes back and proves it");
  pumpMs(50);
  CHECK(!mono.leash.alerted && !mono.leash.armed, "... which stands the leash down");
  CHECK(g_adv.interval == ADV_NORMAL,       "... and puts the beacon back to normal [REVIEW F14]");

  bootPaired();
  phoneLeaves(HUNG_UP); clearOut(); pumpMs(90000);
  CHECK(!sawUsb("EVT separation"),          "tapping Disconnect in the app is not walking away [REVIEW F15]");

  bootPaired();
  bleSend("BYE"); phoneLeaves(WALKED_AWAY); clearOut(); pumpMs(90000);
  CHECK(!sawUsb("EVT separation"),          "after BYE, even a dropped link is not an alarm");

  bootPaired();
  bleSend("SET sepwarn 0"); phoneLeaves(WALKED_AWAY); clearOut(); pumpMs(90000);
  CHECK(!sawUsb("EVT separation"),          "with the leash off, nothing happens");

  bootWithOwner();
  phoneLeaves(WALKED_AWAY); pumpMs(65000);
  CHECK(mono.leash.alerted,                 "(the wallet was left behind and is alerting)");
  tapFinger(OWNER); pumpMs(50);
  CHECK(!mono.leash.alerted && g_adv.interval == ADV_NORMAL,
                                            "the owner's finger on it means it is not lost: the alert stands down");

  boot(); clearOut(); pumpMs(90000);
  CHECK(!sawUsb("EVT separation"),          "no alarm if a phone never connected");

  boot(); phoneArrives(); phoneLeaves(WALKED_AWAY); clearOut(); pumpMs(90000);
  CHECK(!sawUsb("EVT separation"),          "no alarm when a phone that never paired wanders off");

  // ----------------------------------------------------------- enrol ----
  section("Fingerprint enrolment");
  bootPaired(); clearOut();
  CHECK(enrollFinger(OWNER),                "two matching presses enrol successfully");
  CHECK(sawBle("ENROLL start id=1"),        "the first finger lands at id 1");
  CHECK(sawBle("ENROLL place") && sawBle("ENROLL lift") && sawBle("ENROLL again"),
                                            "every step is reported as it happens");
  CHECK(g_fp.db.count(1) == 1,              "the template is really stored on the module");
  clearOut(); bleSend("STATUS");
  CHECK(field(lastIn(g_bleOut, "STATUS "), "fingers") == "1", "STATUS reflects the new finger");
  clearOut(); enrollFinger(FRIEND);
  CHECK(sawBle("ENROLL ok id=2"),           "the second finger gets id 2");
  clearOut(); bleSend("LIST");
  CHECK(sawBle("LIST count=2 ids=1,2"),     "LIST names the ids, not just a count");

  bootPaired();
  enrollFinger(1); enrollFinger(2); enrollFinger(3);
  bleSend("DELETE 1"); clearOut();
  enrollFinger(9);
  CHECK(sawBle("ENROLL ok id=1"),           "after a delete, a new finger takes the free slot [REVIEW F16]");
  CHECK(g_fp.db[3] == 3,                    "... and finger 3 is untouched (v2 overwrote it) [REVIEW F16]");
  clearOut(); tapFinger(3);
  CHECK(sawBle("EVT unlocked id=3"),        "finger 3 still opens the wallet");

  bootPaired(); clearOut();
  bleSend("ENROLL"); touch(OWNER); pumpMs(300); lift(); pumpMs(300); touch(FRIEND); pumpMs(300); lift();
  CHECK(sawBle("ENROLL fail no_match"),     "two different fingers are refused");
  CHECK(g_fp.db.empty(),                    "nothing is stored after a failed enrolment");

  bootPaired(); clearOut();
  bleSend("ENROLL");
  pumpMs(ENROLL_STEP_MS + 1000);
  CHECK(sawBle("ENROLL fail timeout"),      "enrolment times out after 20 s");
  CHECK(!enrolling(),                       "the state machine returns to idle on timeout");

  bootPaired(); clearOut();
  bleSend("ENROLL"); bleSend("ENROLL");
  CHECK(sawBle("ENROLL busy"),              "a second ENROLL while busy is refused");
  bleSend("CANCEL");
  CHECK(sawBle("CANCEL ok") && !enrolling(), "CANCEL aborts the enrolment");

  bootPaired();
  for (int i = 1; i <= MAX_FINGERS; i++) enrollFinger(100 + i);
  clearOut(); bleSend("ENROLL");
  CHECK(sawBle("ENROLL fail full"),         "ten fingers is the limit the app promises");

  bootPaired(); g_fp.db[1] = OWNER; g_fp.db[2] = FRIEND; mono.slots = 3; clearOut();
  bleSend("DELETE 1");
  CHECK(sawBle("DELETE ok id=1") && g_fp.db.count(1) == 0, "DELETE removes a template");
  clearOut(); bleSend("DELETE 1");
  CHECK(sawBle("DELETE fail id=1"),         "deleting an empty slot reports failure");
  clearOut(); bleSend("DELETE 0"); bleSend("DELETE 11"); bleSend("DELETE abc");
  CHECK(countIn(g_bleOut, "DELETE fail badid") == 3, "ids outside 1-10, and non-numbers, are refused");
  clearOut(); bleSend("DELETE");
  CHECK(sawBle("DELETE fail noarg"),        "bare DELETE rejected");
  clearOut(); bleSend("DELETEALL");
  CHECK(sawBle("DELETEALL ok") && g_fp.db.empty(), "DELETEALL wipes the library");
  CHECK(!setupMode(),                       "with a phone still paired, an empty library is not setup mode");

  // ---------------------------------------------------- non-blocking ----
  section("Nothing blocks [REVIEW F12]");
  bootWithOwner();
  g_longestDelay = 0; g_sleepHook = recordDelay;
  bleSend("RING");
  CHECK(sawBle("RING ok"),                  "RING acknowledges");
  pumpMs(2500);
  CHECK(g_beeps.size() >= 8,                "RING really drives the buzzer 8 times");
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false; pumpMs(3000);
  tapFinger(OWNER); pumpMs(cfg.releaseHoldMs + 500);
  enrollFinger(FRIEND);
  g_sleepHook = nullptr;
  {
    char m[160];
    snprintf(m, sizeof m, "through a ring, an alarm, an unlock and an enrolment, no delay() exceeded %lu ms (longest %lu)",
             (unsigned long)IDLE_MAX_MS, (unsigned long)g_longestDelay);
    CHECK(g_longestDelay <= IDLE_MAX_MS, m);
  }
  bleSend("CLEAR");
  clearOut();
  bleuart.feed("RING\nSTATUS\n");
  uint32_t asked = g_millis;
  pumpUntil([] { return sawBle("STATUS "); }, 3000);
  CHECK(g_millis - asked <= IDLE_MAX_MS,    "a STATUS sent behind a RING is answered at once (v2: 1.6 s later)");

  // ----------------------------------------------------- status push ----
  section("The wallet tells the phone [REVIEW A1]");
  bootWithOwner();
  pumpMs(5000);
  int quiet = countIn(g_bleOut, "STATUS ");
  CHECK(quiet == 0,                         "nothing changes, nothing is pushed");
  tapFinger(OWNER); pumpMs(cfg.releaseHoldMs + 500);
  CHECK(countIn(g_bleOut, "STATUS ") >= 2,  "an unlock and a relock each push a STATUS");
  boot(); g_fp.db[1] = OWNER; mono.slots = 1; cfg.phones = 1;
  phoneArrives(); clearOut();
  tapFinger(OWNER);
  CHECK(!sawBle("STATUS ") && !sawBle("EVT "), "an unauthenticated phone is told nothing");

  // ---------------------------------------------------------- findmy ----
  section("Find My beacon (header checked directly)");
  boot();
  findmyBuildPayload();
  CHECK(findmy_adv[0] == 0x1e,              "AD length byte is 0x1e (30)");
  CHECK(findmy_adv[1] == 0xff,              "AD type is manufacturer specific");
  CHECK(findmy_adv[2] == 0x4c && findmy_adv[3] == 0x00, "Apple company id 0x004C, little endian");
  CHECK(findmy_adv[4] == 0x12 && findmy_adv[5] == 0x19, "offline-finding type and payload length");
  { bool keyOk = true;
    for (int i = 0; i < 22; i++) if (findmy_adv[7 + i] != FINDMY_PUBLIC_KEY[6 + i]) keyOk = false;
    CHECK(keyOk,                            "key bytes 6..27 copied into the payload"); }
  CHECK(findmy_adv[29] == (uint8_t)(FINDMY_PUBLIC_KEY[0] >> 6), "top two bits of key[0] land in byte 29");
  findmySetAddress();
  CHECK(g_addr.addr_type == BLE_GAP_ADDR_TYPE_RANDOM_STATIC, "BLE address type is random-static");
  CHECK((g_addr.addr[5] & 0xC0) == 0xC0,    "top two address bits forced to 11");
  CHECK(g_addr.addr[5] == (uint8_t)(FINDMY_PUBLIC_KEY[0] | 0xC0) && g_addr.addr[0] == FINDMY_PUBLIC_KEY[5],
                                            "address bytes reversed for the SoftDevice");
  findmyStart();
  CHECK(g_adv.data.size() == 29,            "29 payload bytes handed to the radio");
  CHECK(!g_adv.connectable,                 "beacon advertising is non-connectable");
  CHECK(g_adv.interval == 1600,             "beacon interval is 1 s");
  { bool placeholder = true;
    for (int i = 0; i < 28; i++) if (FINDMY_PUBLIC_KEY[i]) placeholder = false;
    if (placeholder)
      printf("    NOTE  FINDMY_PUBLIC_KEY is still all zeros - the beacon would\n"
             "          advertise but no report could ever be decrypted.\n"); }

#if MONO_ENABLE_FINDMY
  section("Find My time-slicing (MONO_ENABLE_FINDMY=1)");
  bootPaired(); bleSend("SET findmy 1"); phoneLeaves(HUNG_UP);
  pumpMs(APPLINK_SLICE_MS + 500);
  CHECK(!g_adv.connectable && g_adv.interval == 1600, "with the phone gone, the radio slices to the Find My beacon");
  CHECK(memcmp(g_addr.addr, g_identityAddr.addr, 6) != 0, "... under the key-derived address");
  pumpMs(FINDMY_SLICE_MS + 500);
  CHECK(g_adv.connectable && g_adv.interval == ADV_NORMAL, "then back to app advertising");
  CHECK(memcmp(g_addr.addr, g_identityAddr.addr, 6) == 0,
                                            "... with the wallet's own address restored, so the phone can find it [REVIEW F17]");
  phoneArrives(); pumpMs(30000);
  CHECK(g_adv.connectable,                  "while the phone is connected, it never slices away");
  phoneLeaves(HUNG_UP);
  g_imu.shaking = true; pumpMs(600); g_imu.shaking = false;
  CHECK(pumpUntil([] { return g_adv.connectable && g_adv.interval == ADV_FAST; }, 20000),
                                            "during an alarm, the app slices use the fast interval");
#endif

#if MONO_BENCH
  section("Bench build (MONO_BENCH=1)");
  bootWithOwner(); clearOut();
  bleSend("GET");
  CHECK(sawUsb("ble< GET "),                "BLE replies are mirrored to the USB console");
  boot(); phoneArrives(); clearOut(); bleSend("PAIR");
  CHECK(sawBle("PAIR ok") && !subIn(g_out, field(lastIn(g_bleOut, "PAIR ok"), "key").c_str()),
                                            "the pairing key is never mirrored, even here");
#endif

  // ----------------------------------------------------------- summary --
  printf("\n----------------------------------------------------------------------\n");
  printf("  %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf("\n  Failures:\n");
    for (size_t i = 0; i < g_failures.size(); i++) printf("    - %s\n", g_failures[i].c_str());
  }
  printf("\n");
  return g_fail ? 1 : 0;
}
