/* ============================================================================
   sha256.h  -  SHA-256 and HMAC-SHA256, FIPS 180-4 / RFC 2104
   ----------------------------------------------------------------------------
   Small, dependency-free and constant-time where it matters. This is what lets
   a paired phone prove it holds its key without ever sending the key again:
   the wallet sends a fresh nonce, the phone answers HMAC(key, nonce), and the
   wallet checks it. The host test suite runs the NIST and RFC 4231 vectors
   against this exact file.
   ========================================================================= */

#ifndef MONO_SHA256_H
#define MONO_SHA256_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct Sha256 {
  uint32_t h[8];
  uint64_t bytes;      /* message length so far */
  uint8_t  buf[64];
  uint8_t  fill;
};

static const uint32_t SHA256_K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256Ror(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256Block(Sha256 *s, const uint8_t *p) {
  uint32_t w[64];
  for (uint8_t i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
  for (uint8_t i = 16; i < 64; i++) {
    uint32_t s0 = sha256Ror(w[i - 15], 7) ^ sha256Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = sha256Ror(w[i - 2], 17) ^ sha256Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (uint8_t i = 0; i < 64; i++) {
    uint32_t t1 = h + (sha256Ror(e, 6) ^ sha256Ror(e, 11) ^ sha256Ror(e, 25)) +
                  ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i];
    uint32_t t2 = (sha256Ror(a, 2) ^ sha256Ror(a, 13) ^ sha256Ror(a, 22)) +
                  ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256Init(Sha256 *s) {
  static const uint32_t IV[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(s->h, IV, sizeof IV);
  s->bytes = 0;
  s->fill = 0;
}

static void sha256Update(Sha256 *s, const uint8_t *data, size_t n) {
  s->bytes += n;
  while (n--) {
    s->buf[s->fill++] = *data++;
    if (s->fill == 64) { sha256Block(s, s->buf); s->fill = 0; }
  }
}

static void sha256Final(Sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->bytes * 8;
  s->buf[s->fill++] = 0x80;
  if (s->fill > 56) {                       /* no room for the length: one more block */
    while (s->fill < 64) s->buf[s->fill++] = 0;
    sha256Block(s, s->buf);
    s->fill = 0;
  }
  while (s->fill < 56) s->buf[s->fill++] = 0;
  for (uint8_t i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256Block(s, s->buf);
  for (uint8_t i = 0; i < 8; i++) {
    out[4 * i]     = (uint8_t)(s->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)(s->h[i]);
  }
}

static void hmacSha256(const uint8_t *key, size_t keyLen,
                       const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  uint8_t k[64], pad[64], inner[32];
  Sha256 s;
  memset(k, 0, sizeof k);
  if (keyLen > 64) { sha256Init(&s); sha256Update(&s, key, keyLen); sha256Final(&s, k); }
  else             memcpy(k, key, keyLen);

  for (uint8_t i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha256Init(&s); sha256Update(&s, pad, 64); sha256Update(&s, msg, msgLen); sha256Final(&s, inner);
  for (uint8_t i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha256Init(&s); sha256Update(&s, pad, 64); sha256Update(&s, inner, 32); sha256Final(&s, out);
}

/* Compares every byte whatever it finds, so the time taken says nothing about
   how much of a forged MAC was right. */
static bool equalConstantTime(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

#endif
