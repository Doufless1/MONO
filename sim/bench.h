/* ============================================================================
   bench.h  -  the physical world around a simulated wallet
   ----------------------------------------------------------------------------
   The firmware reads a fingerprint sensor, an accelerometer, a battery and a
   radio link. On a desk those are real; in the simulator they are these
   directives. The terminal simulator and the WebAssembly build both include
   this file after the firmware, so a bench verb means the same thing in both.

       !touch <who>    a finger is on the sensor. <who> is the finger's
                       identity: 1 is the owner's thumb by convention, 0 is a
                       stranger. Enrol a finger, then touch with the same
                       identity and it matches - as on real hardware.
       !lift           finger off the sensor
       !smudge         something the sensor cannot get a clean image of
       !finger <id>    (v2) the finger that template <id> was made from
       !finger new     (v2) a stranger
       !press <who>    (v2) same as !touch
       !shake on|off   someone shaking it (5 g jolts, 40 ms in every 120 ms)
       !knock          one 30 ms, 5 g knock
       !link 1         the phone connects
       !link 0         the phone walks out of range (supervision timeout)
       !link bye       the phone hangs up deliberately
       !batt <volts>   battery voltage
       !peek           one line of internal state, as key=value pairs
   ========================================================================= */

#ifndef MONO_SIM_BENCH_H
#define MONO_SIM_BENCH_H

// Lines the bench itself prints (not the wallet), for the host to route.
static std::vector<std::string> g_benchOut;

static void benchSay(const std::string &s) { g_benchOut.push_back(s); }

static const char *platenName() {
  switch (g_fp.platen) {
    case PLATEN_EMPTY:    return "empty";
    case PLATEN_ENROLLED: return "enrolled";
    case PLATEN_UNKNOWN:  return "finger";
    case PLATEN_SMUDGE:   return "smudge";
  }
  return "?";
}

// Everything a digital twin needs to draw the wallet, plus the firmware's
// own view of itself. Flat key=value so it is trivial to parse anywhere.
static std::string benchPeek() {
  char b[512];
  snprintf(b, sizeof b,
           "PEEK ms=%lu servo=%d servo_on=%d led=%d buzz=%d hz=%u adv=%u adv_on=%d connectable=%d "
           "conn=%d auth=%d latch=%s sensor=%s alarm=%d enroll=%s fingers=%u phones=%u leash=%s "
           "fails=%u pair=%d setup=%d platen=%s who=%d shaking=%d vbat=%u.%02u",
           (unsigned long)g_millis, g_servoAngle, g_servoAttached ? 1 : 0,
           g_pinState[PIN_STATUS_LED] ? 1 : 0, g_buzzerOn ? 1 : 0,
           g_buzzerOn && !g_beeps.empty() ? g_beeps.back().freq : 0,
           g_adv.interval, g_adv.running ? 1 : 0, g_adv.connectable ? 1 : 0,
           g_connected ? 1 : 0, mono.ble.authed ? 1 : 0,
           mono.latch.open ? "open" : "locked", sensorName(), mono.alarm.on ? 1 : 0,
           enrollName(), fingerCount(), phoneCount(), leashName(), mono.failCount,
           mono.pair.pending ? 1 : 0, setupMode() ? 1 : 0, platenName(),
           g_fp.platen == PLATEN_UNKNOWN ? g_fp.fingerName : (int)g_fp.whoId,
           g_imu.shaking ? 1 : 0, mono.vbatMv / 1000, (mono.vbatMv % 1000) / 10);
  return b;
}

// "verb arg" -> two words. No iostreams: they would triple the wasm binary.
static void splitWords(const std::string &s, std::string &a, std::string &b) {
  size_t i = s.find_first_not_of(' ');
  size_t j = s.find(' ', i == std::string::npos ? s.size() : i);
  a = i == std::string::npos ? "" : s.substr(i, j - i);
  size_t k = s.find_first_not_of(' ', j == std::string::npos ? s.size() : j);
  size_t l = s.find(' ', k == std::string::npos ? s.size() : k);
  b = k == std::string::npos ? "" : s.substr(k, l - k);
}

static void bench(const std::string &line) {
  std::string verb, arg;
  splitWords(!line.empty() && line[0] == '!' ? line.substr(1) : line, verb, arg);

  if (verb == "touch" || verb == "press") {
    g_fp.platen = PLATEN_UNKNOWN;
    g_fp.fingerName = arg.empty() ? 1 : atoi(arg.c_str());
    benchSay("BENCH touch " + std::to_string(g_fp.fingerName));

  } else if (verb == "finger") {
    if (arg == "new") { g_fp.platen = PLATEN_UNKNOWN; g_fp.fingerName = 0; }
    else {
      int id = atoi(arg.c_str());
      g_fp.whoId = (uint16_t)(id > 0 ? id : 1);
      g_fp.platen = PLATEN_ENROLLED;
    }
    benchSay("BENCH finger " + arg);

  } else if (verb == "smudge") {
    g_fp.platen = PLATEN_SMUDGE;
    benchSay("BENCH smudge");

  } else if (verb == "lift") {
    g_fp.platen = PLATEN_EMPTY; g_fp.fingerName = 0;
    benchSay("BENCH lift");

  } else if (verb == "shake" || verb == "tamper") {
    g_imu.shaking = arg != "off";
    benchSay(std::string("BENCH shake ") + (g_imu.shaking ? "on" : "off"));

  } else if (verb == "knock") {
    g_imu.jolt(g_millis, 30);
    benchSay("BENCH knock");

  } else if (verb == "link") {
    if (arg == "1" || arg == "on") {
      if (!g_connected) { g_connected = true; Bluefruit.Periph.onConn(0); }
      benchSay("BENCH link 1");
    } else {
      // 0x08: the radio lost the phone. 0x13: the phone said goodbye.
      uint8_t why = (arg == "bye") ? 0x13 : 0x08;
      if (g_connected) { g_connected = false; Bluefruit.Periph.onDisc(0, why); }
      benchSay(std::string("BENCH link ") + (why == 0x13 ? "bye" : "0"));
    }

  } else if (verb == "batt") {
    float v = (float)atof(arg.c_str());
    g_vbatRaw = v / (3.0f / 4096.0f * (1510.0f / 510.0f));
    benchSay("BENCH batt " + arg);

  } else if (verb == "peek") {
    benchSay(benchPeek());

  } else {
    benchSay("BENCH ERR unknown " + verb);
  }
}

#endif
