/* Adafruit_Fingerprint.h - host mock of the GROW R502 over UART.
   Models a scriptable sensor: what is on the platen right now, and a template
   library that survives across the test.

   Fingers have identities, as real ones do. A template remembers the identity
   of the finger it was made from, and a later touch matches it only if it is
   the same finger. So "enrol my thumb, then open with my thumb" works in the
   simulator for the same reason it works on the bench. */
#ifndef MOCK_FP_H
#define MOCK_FP_H
#include "Arduino.h"
#include <map>

#define FINGERPRINT_OK               0x00
#define FINGERPRINT_PACKETRECIEVEERR 0x01
#define FINGERPRINT_NOFINGER         0x02
#define FINGERPRINT_IMAGEFAIL        0x03
#define FINGERPRINT_IMAGEMESS        0x06
#define FINGERPRINT_FEATUREFAIL      0x07
#define FINGERPRINT_NOMATCH          0x08
#define FINGERPRINT_NOTFOUND         0x09
#define FINGERPRINT_ENROLLMISMATCH   0x0A
#define FINGERPRINT_BADLOCATION      0x0B
#define FINGERPRINT_DBRANGEFAIL      0x0C   /* what loadModel() says about an empty slot */
#define FINGERPRINT_FLASHERR         0x18

/* What is physically on the sensor.
     PLATEN_ENROLLED  the finger template `whoId` was made from (bench shortcut)
     PLATEN_UNKNOWN   the finger whose identity is `fingerName`; 0 is a stranger
                      who matches nothing and cannot be enrolled
     PLATEN_SMUDGE    something the sensor cannot get a clean image of        */
enum Platen { PLATEN_EMPTY, PLATEN_ENROLLED, PLATEN_UNKNOWN, PLATEN_SMUDGE };

struct MockFP {
  Platen   platen     = PLATEN_EMPTY;
  uint16_t whoId      = 1;
  int      fingerName = 0;
  std::map<uint16_t, int> db;     // template id -> identity of the finger behind it
  bool     password   = true;     // verifyPassword() result: is the module there at all
  bool     failStore  = false;
  int      buf[3]     = {0, 0, 0};   // identity captured into char buffer 1 and 2

  int identityOnPlaten() const {
    if (platen == PLATEN_ENROLLED) {
      std::map<uint16_t, int>::const_iterator it = db.find(whoId);
      return it != db.end() ? it->second : -1000 - (int)whoId;   // a finger nobody enrolled
    }
    return platen == PLATEN_UNKNOWN ? fingerName : 0;
  }
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
    if (slot < 3) g_fp.buf[slot] = g_fp.identityOnPlaten();
    return FINGERPRINT_OK;
  }

  // Searches with char buffer 1, like the real module.
  uint8_t fingerFastSearch() {
    int who = g_fp.buf[1];
    if (who == 0) return FINGERPRINT_NOTFOUND;
    for (std::map<uint16_t, int>::const_iterator it = g_fp.db.begin(); it != g_fp.db.end(); ++it)
      if (it->second == who) { fingerID = it->first; confidence = 120; return FINGERPRINT_OK; }
    return FINGERPRINT_NOTFOUND;
  }

  // The two captures must be the same finger, exactly like the real module.
  uint8_t createModel() {
    return (g_fp.buf[1] > 0 && g_fp.buf[1] == g_fp.buf[2]) ? FINGERPRINT_OK
                                                           : FINGERPRINT_ENROLLMISMATCH;
  }
  uint8_t storeModel(uint16_t id) {
    if (g_fp.failStore) return FINGERPRINT_FLASHERR;
    g_fp.db[id] = g_fp.buf[1];
    return FINGERPRINT_OK;
  }
  uint8_t loadModel(uint16_t id) {
    return g_fp.db.count(id) ? FINGERPRINT_OK : FINGERPRINT_DBRANGEFAIL;
  }
  uint8_t deleteModel(uint16_t id) {
    return g_fp.db.erase(id) ? FINGERPRINT_OK : FINGERPRINT_BADLOCATION;
  }
  uint8_t emptyDatabase() { g_fp.db.clear(); return FINGERPRINT_OK; }
};
#endif
