/* Adafruit_Fingerprint.h - host mock of the GROW R502 over UART.
   Models a scriptable sensor: what is currently on the platen, and a
   template database that survives across the test. */
#ifndef MOCK_FP_H
#define MOCK_FP_H
#include "Arduino.h"
#include <set>

#define FINGERPRINT_OK              0x00
#define FINGERPRINT_PACKETRECIEVEERR 0x01
#define FINGERPRINT_NOFINGER        0x02
#define FINGERPRINT_IMAGEFAIL       0x03
#define FINGERPRINT_IMAGEMESS       0x06
#define FINGERPRINT_FEATUREFAIL     0x07
#define FINGERPRINT_NOMATCH         0x08
#define FINGERPRINT_NOTFOUND        0x09
#define FINGERPRINT_ENROLLMISMATCH  0x0A
#define FINGERPRINT_BADLOCATION     0x0B
#define FINGERPRINT_FLASHERR        0x18

// what is physically on the sensor right now
enum Platen { PLATEN_EMPTY, PLATEN_ENROLLED, PLATEN_UNKNOWN, PLATEN_SMUDGE };

struct MockFP {
  Platen  platen   = PLATEN_EMPTY;
  uint16_t whoId   = 1;      // which enrolled finger, when platen==ENROLLED
  int      fingerName = 0;   // identity used to decide if two captures match
  std::set<uint16_t> db;     // stored template ids
  bool     password  = true; // verifyPassword() result
  bool     failStore = false;
  int      buf[3]    = {0,0,0};  // captured "finger identity" per slot
};
extern MockFP g_fp;

class Adafruit_Fingerprint {
public:
  uint16_t fingerID = 0, confidence = 0, templateCount = 0;
  Adafruit_Fingerprint(MockSerial *) {}
  void begin(uint32_t) {}
  bool verifyPassword() { return g_fp.password; }

  uint8_t getTemplateCount() { templateCount = (uint16_t)g_fp.db.size(); return FINGERPRINT_OK; }

  uint8_t getImage() {
    return g_fp.platen == PLATEN_EMPTY ? FINGERPRINT_NOFINGER : FINGERPRINT_OK;
  }

  uint8_t image2Tz(uint8_t slot = 1) {
    if (g_fp.platen == PLATEN_EMPTY)  return FINGERPRINT_IMAGEFAIL;
    if (g_fp.platen == PLATEN_SMUDGE) return FINGERPRINT_FEATUREFAIL;
    if (slot < 3) g_fp.buf[slot] = g_fp.fingerName ? g_fp.fingerName : (int)g_fp.whoId;
    return FINGERPRINT_OK;
  }

  uint8_t fingerFastSearch() {
    if (g_fp.platen != PLATEN_ENROLLED) return FINGERPRINT_NOTFOUND;
    if (!g_fp.db.count(g_fp.whoId))     return FINGERPRINT_NOTFOUND;
    fingerID = g_fp.whoId; confidence = 120;
    return FINGERPRINT_OK;
  }

  // the two captures must be the same finger, exactly like the real module
  uint8_t createModel() {
    return (g_fp.buf[1] && g_fp.buf[1] == g_fp.buf[2]) ? FINGERPRINT_OK
                                                       : FINGERPRINT_ENROLLMISMATCH;
  }
  uint8_t storeModel(uint16_t id) {
    if (g_fp.failStore) return FINGERPRINT_FLASHERR;
    g_fp.db.insert(id); return FINGERPRINT_OK;
  }
  uint8_t deleteModel(uint16_t id) {
    return g_fp.db.erase(id) ? FINGERPRINT_OK : FINGERPRINT_BADLOCATION;
  }
  uint8_t emptyDatabase() { g_fp.db.clear(); return FINGERPRINT_OK; }
};
#endif
