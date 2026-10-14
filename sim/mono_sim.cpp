/* ============================================================================
   mono_sim - the MONO wallet, running on your PC
   ----------------------------------------------------------------------------
   The same mono_firmware.ino as the real board and the same mocks as the test
   suite, but on a wall-clock timer and talking over stdin/stdout.

   An input line goes somewhere different depending on how it starts:
       STATUS        typed on the USB console (this is a bench build, so the
                     console is trusted - see MONO_BENCH in config.h)
       @PAIR         sent by the phone over BLE
       !touch 1      the physical world; bench.h has every verb

   An output line says where it came from:
       U ...         the wallet wrote it to the USB console
       B ...         the wallet sent it to the phone
       S ...         the simulator or the bench

   Build and run:
       g++ -std=gnu++11 -DMONO_BENCH=1 -x c++ -I sim/mocks -o build/mono_sim sim/mono_sim.cpp
       ./build/mono_sim
   ========================================================================= */

#include "mocks/mock_impl.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <deque>
#include <iostream>

static void realSleep(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ================= the firmware under test, included verbatim =============
#include "../mono_firmware/mono_firmware.ino"
// ==========================================================================
#include "bench.h"

static std::mutex              g_inMx;
static std::deque<std::string> g_inQ;
static bool                    g_done = false;

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

static void flush() {
  for (size_t i = 0; i < g_out.size(); i++)      std::cout << "U " << g_out[i] << "\n";
  for (size_t i = 0; i < g_bleOut.size(); i++)   std::cout << "B " << g_bleOut[i] << "\n";
  for (size_t i = 0; i < g_benchOut.size(); i++) std::cout << "S " << g_benchOut[i] << "\n";
  if (!g_out.empty() || !g_bleOut.empty() || !g_benchOut.empty()) std::cout.flush();
  g_out.clear(); g_bleOut.clear(); g_benchOut.clear();
}

int main() {
  std::ios::sync_with_stdio(false);
  g_t0 = std::chrono::steady_clock::now();
  g_sleepHook = realSleep;              // blocking calls really block now

  setup();
  flush();
  std::cout << "S ready - the wallet is running, no hardware attached. "
               "Lines: STATUS (USB)  @HELLO (phone)  !touch 1 (bench)\n";
  std::cout.flush();

  std::thread rt(readerThread);
  rt.detach();

  // One line per pass, so a piped script runs in order with the wallet
  // reacting in between - and when input ends, keep running for a moment so
  // the last lines are answered before we exit (v2 exited first).
  uint32_t quitAt = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_inMx);
      if (!g_inQ.empty()) {
        std::string line = g_inQ.front(); g_inQ.pop_front();
        if (!line.empty()) {
          if (line[0] == '!')      bench(line);
          else if (line[0] == '@') bleuart.feed(line.substr(1) + "\n");
          else                     Serial.feed(line + "\n");
        }
        flush();                         // the bench's word before the wallet's reaction
      } else if (g_done && !quitAt) {
        quitAt = wallMs() + 500;
      }
    }
    if (quitAt && (int32_t)(wallMs() - quitAt) >= 0) break;

    uint32_t w = wallMs();
    if (w > g_millis) g_millis = w;      // never rewind; delay() also advances it

    loop();
    flush();
  }
  flush();
  return 0;
}
