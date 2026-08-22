/* ============================================================================
   mono_sim - the MONO wallet, running on your PC
   ----------------------------------------------------------------------------
   Same mono_firmware.ino as the real board, same mocks as the test suite, but
   on a wall-clock timer and speaking its line protocol over stdin/stdout.

   Anything the app would send over BLE, send here as a plain line:
       STATUS / GET / ENROLL / RING / LOCK / UNLOCK / SET relock 3000 ...

   Lines beginning with ! are the bench, not the wallet - they stand in for the
   physical world the firmware would otherwise be reading:
       !finger <id>    an enrolled finger is on the sensor
       !finger new     an unrecognised finger is on the sensor
       !press <n>      a finger with identity <n> (for enrolment)
       !lift           finger removed
       !tamper on|off  shake it
       !link 0|1       phone connected / walked away
       !batt <volts>   set the battery voltage
       !peek           dump the internal state as one line
   ========================================================================= */

#include "mocks/mock_impl.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <deque>
#include <iostream>
#include <sstream>

static void realSleep(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ================= the firmware under test, included verbatim =============
#include "../mono_firmware/mono_firmware.ino"
// ==========================================================================

static std::mutex             g_inMx;
static std::deque<std::string> g_inQ;
static bool                   g_done = false;

static void readerThread() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::lock_guard<std::mutex> lk(g_inMx);
    g_inQ.push_back(line);
  }
  std::lock_guard<std::mutex> lk(g_inMx);
  g_done = true;
}

static std::chrono::steady_clock::time_point g_t0;
static uint32_t wallMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - g_t0).count();
}

static void emit(const std::string &s) {
  std::cout << s << "\n";
  std::cout.flush();
}

static void flushFirmwareOutput() {
  for (auto &l : g_out) emit(l);
  g_out.clear();
}

// ------------------------------------------------------- bench directives --
static void bench(const std::string &line) {
  std::istringstream is(line.substr(1));
  std::string verb, arg;
  is >> verb >> arg;

  if (verb == "finger") {
    if (arg == "new") { g_fp.platen = PLATEN_UNKNOWN; g_fp.fingerName = 0; }
    else if (arg == "smudge") { g_fp.platen = PLATEN_SMUDGE; }
    else {
      uint16_t id = (uint16_t)atoi(arg.c_str());
      if (!id) id = 1;
      g_fp.whoId = id; g_fp.platen = PLATEN_ENROLLED; g_fp.fingerName = 0;
    }
    emit("BENCH finger " + arg);

  } else if (verb == "press") {
    g_fp.platen = PLATEN_UNKNOWN;
    g_fp.fingerName = arg.empty() ? 1 : atoi(arg.c_str());
    emit("BENCH press " + arg);

  } else if (verb == "lift") {
    g_fp.platen = PLATEN_EMPTY; g_fp.fingerName = 0;
    emit("BENCH lift");

  } else if (verb == "tamper") {
    g_imu.z = (arg == "off") ? 1.0f : 5.0f;
    emit("BENCH tamper " + std::string(arg == "off" ? "off" : "on"));

  } else if (verb == "link") {
    bool up = (arg == "1");
    g_connected = up;
    if (up && Bluefruit.Periph.onConn)  Bluefruit.Periph.onConn(0);
    if (!up && Bluefruit.Periph.onDisc) Bluefruit.Periph.onDisc(0, 0);
    emit("BENCH link " + std::string(up ? "1" : "0"));

  } else if (verb == "batt") {
    float v = atof(arg.c_str());
    g_vbatRaw = v / (3.0f / 4096.0f * (1510.0f / 510.0f));
    emit("BENCH batt " + arg);

  } else if (verb == "peek") {
    char b[256];
    snprintf(b, sizeof b,
             "PEEK state=%u enroll=%u fails=%u servo=%d adv=%u linked=%u "
             "templates=%u ms=%lu",
             state, enrollState, failCount, g_servoAngle, g_adv.interval,
             linked ? 1 : 0, (unsigned)g_fp.db.size(), (unsigned long)g_millis);
    emit(b);

  } else {
    emit("BENCH ERR unknown " + verb);
  }
}

// ===========================================================================
int main() {
  std::ios::sync_with_stdio(false);
  g_t0 = std::chrono::steady_clock::now();
  g_sleepHook = realSleep;              // blocking calls really block now

  setup();
  flushFirmwareOutput();
  emit("SIM ready - the wallet is running, no hardware attached");

  std::thread rt(readerThread);
  rt.detach();

  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_inMx);
      while (!g_inQ.empty()) {
        std::string line = g_inQ.front(); g_inQ.pop_front();
        if (!line.empty() && line[0] == '!') bench(line);
        else { bleuart.feed(line + "\n"); }
      }
      if (g_done && g_inQ.empty()) break;
    }

    uint32_t w = wallMs();
    if (w > g_millis) g_millis = w;      // never rewind, delay() also advances

    loop();
    flushFirmwareOutput();

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return 0;
}
