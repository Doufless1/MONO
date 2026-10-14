/* ============================================================================
   The MONO wallet as a WebAssembly module
   ----------------------------------------------------------------------------
   mono_firmware.ino, unmodified, on the same mocks the test suite uses, behind
   a small C API that JavaScript drives. The browser demo and the Node
   integration tests both load this one binary, so the app is always tested
   against the firmware it will actually meet.

   Strings cross the boundary through two buffers in linear memory:
     JS -> wasm   write a NUL-terminated line into sim_inbox(), then call
                  sim_phone() / sim_usb() / sim_bench()
     wasm -> JS   sim_outbox() returns every line produced since the last
                  sim_outbox_clear(), one per line, each prefixed
                  "B " (to the phone), "U " (USB console) or "S " (bench)

   Time is virtual. sim_run(t) runs the main loop until the wallet's clock
   reaches t milliseconds; the host calls it from requestAnimationFrame (or a
   test loop) with however much time has really passed.

   Build: python sim/build_wasm.py   (zig c++ -target wasm32-wasi)
   ========================================================================= */

#include "../mocks/mock_impl.h"

// ================= the firmware under test, included verbatim =============
#include "../../mono_firmware/mono_firmware.ino"
// ==========================================================================
#include "../bench.h"

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

static char        g_inbox[512];
static std::string g_outbox;
static std::string g_scratch;

static void collect() {
  for (size_t i = 0; i < g_out.size(); i++)      { g_outbox += "U "; g_outbox += g_out[i];      g_outbox += '\n'; }
  for (size_t i = 0; i < g_bleOut.size(); i++)   { g_outbox += "B "; g_outbox += g_bleOut[i];   g_outbox += '\n'; }
  for (size_t i = 0; i < g_benchOut.size(); i++) { g_outbox += "S "; g_outbox += g_benchOut[i]; g_outbox += '\n'; }
  g_out.clear(); g_bleOut.clear(); g_benchOut.clear();
}

EXPORT(sim_inbox)      char *sim_inbox()      { return g_inbox; }
EXPORT(sim_inbox_size) int   sim_inbox_size() { return (int)sizeof g_inbox; }

// Seeds the RNG from the host (crypto.getRandomValues), so every demo session
// pairs with a different key, then powers the wallet on.
EXPORT(sim_boot) void sim_boot(uint32_t seedLo, uint32_t seedHi) {
  g_rngState = ((uint64_t)seedHi << 32 | seedLo) | 1;
  setup();
  collect();
}

EXPORT(sim_run) void sim_run(uint32_t untilMs) {
  // Bounded, so a tab that slept for an hour catches up in one call without
  // ever spinning forever.
  for (int guard = 0; (int32_t)(g_millis - untilMs) < 0 && guard < 200000; guard++) {
    uint32_t t0 = g_millis;
    loop();
    if (g_millis == t0) g_millis += 1;
  }
  collect();
}

EXPORT(sim_phone) void sim_phone() { bleuart.feed(std::string(g_inbox) + "\n"); }
EXPORT(sim_usb)   void sim_usb()   { Serial.feed(std::string(g_inbox) + "\n"); }
EXPORT(sim_bench) void sim_bench() { bench(g_inbox); collect(); }

EXPORT(sim_outbox)       const char *sim_outbox()       { return g_outbox.c_str(); }
EXPORT(sim_outbox_clear) void        sim_outbox_clear() { g_outbox.clear(); }

EXPORT(sim_peek) const char *sim_peek() { g_scratch = benchPeek(); return g_scratch.c_str(); }
EXPORT(sim_now)  uint32_t    sim_now()  { return g_millis; }
