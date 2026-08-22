/* Arduino.h - host mock.  Virtual clock, recorded I/O.
   Everything the firmware does to a pin is recorded so a test can assert on
   physical behaviour (servo angle, beeps, LED) without a board attached. */
#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#define HIGH 1
#define LOW  0
#define INPUT  0
#define OUTPUT 1
#define AR_INTERNAL_3_0 3

// ------------------------------------------------------------ the clock --
extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }

/* Blocking == time passes. The test harness leaves g_sleepHook null so a
   6-second relock costs nothing; the interactive simulator points it at a real
   sleep so the wallet behaves at wall-clock speed for a human at the app. */
extern void (*g_sleepHook)(uint32_t);
inline void delay(uint32_t ms) {
  if (g_sleepHook) g_sleepHook(ms);
  g_millis += ms;
}

// ------------------------------------------------------------- recorders --
struct Beep { uint16_t freq, ms; uint32_t at; };
extern std::vector<Beep>        g_beeps;
extern std::vector<int>         g_servoWrites;
extern std::vector<std::string> g_out;      // every line the firmware emitted
extern int   g_pinState[64];
extern float g_vbatRaw;

// ---------------------------------------------------------------- gpio ----
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) { if (pin < 64) g_pinState[pin] = v; }
int  digitalRead(int pin);                 // defined by the harness
inline void analogReference(int) {}
inline void analogReadResolution(int) {}
inline int  analogRead(int) { return (int)g_vbatRaw; }

inline void tone(int, uint16_t f, uint16_t ms) { g_beeps.push_back({f, ms, g_millis}); }
inline void noTone(int) {}

template <typename T, typename L, typename H>
inline T constrain(T v, L lo, H hi) { return v < (T)lo ? (T)lo : (v > (T)hi ? (T)hi : v); }

// -------------------------------------------------------------- Serial ----
struct MockSerial {
  std::vector<char> in;
  size_t rd = 0;
  operator bool() const { return true; }
  void begin(uint32_t) {}
  int  available() { return (int)(in.size() - rd); }
  int  read() { return rd < in.size() ? in[rd++] : -1; }
  void feed(const std::string &s) { for (char c : s) in.push_back(c); }

  // A real serial port is a byte stream: print() accumulates, println()
  // terminates the line. Modelling that is what makes "R502 online, templates: 0"
  // arrive as one assertable line instead of two fragments.
  std::string pending;
  void flush_(const std::string &tail) { g_out.push_back(pending + tail); pending.clear(); }

  void println(const char *s)   { flush_(s); }
  void println()                { flush_(""); }
  void println(int v)           { flush_(std::to_string(v)); }
  void println(unsigned v)      { flush_(std::to_string(v)); }
  void println(long v)          { flush_(std::to_string(v)); }
  void println(unsigned long v) { flush_(std::to_string(v)); }
  void println(float v, int d)  { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); flush_(b); }
  void print(const char *s)     { pending += s; }
  void print(int v)             { pending += std::to_string(v); }
  void print(unsigned v)        { pending += std::to_string(v); }
  void print(char c)            { pending += c; }
  void print(float v, int d)    { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); pending += b; }
};
extern MockSerial Serial;
extern MockSerial Serial1;

#endif
