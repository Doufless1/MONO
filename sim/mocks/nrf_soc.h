/* nrf_soc.h - host mock of the two SoftDevice RNG calls the firmware uses.
   Deterministic (xorshift64) so a failing test fails the same way twice; the
   WebAssembly build reseeds it from the browser's crypto.getRandomValues. */
#ifndef MOCK_NRF_SOC_H
#define MOCK_NRF_SOC_H
#include <cstdint>

#define NRF_SUCCESS                          0
#define NRF_ERROR_SOC_RAND_NOT_ENOUGH_VALUES 0x2005

extern uint64_t g_rngState;
extern bool     g_rngEmpty;      // test hook: the pool never fills

inline uint32_t sd_rand_application_bytes_available_get(uint8_t *avail) {
  *avail = g_rngEmpty ? 0 : 64;
  return NRF_SUCCESS;
}

inline uint32_t sd_rand_application_vector_get(uint8_t *out, uint8_t len) {
  if (g_rngEmpty) return NRF_ERROR_SOC_RAND_NOT_ENOUGH_VALUES;
  for (uint8_t i = 0; i < len; i++) {
    g_rngState ^= g_rngState << 13;
    g_rngState ^= g_rngState >> 7;
    g_rngState ^= g_rngState << 17;
    out[i] = (uint8_t)(g_rngState >> 32);
  }
  return NRF_SUCCESS;
}
#endif
