/* ============================================================================
   findmy.h  -  Apple "Find My" beacon for the MONO wallet   [EXPERIMENTAL]
   ----------------------------------------------------------------------------
   Broadcasts an OpenHaystack-compatible advertisement so that ANY passing
   iPhone reports the wallet's location to Apple's Find My network. No SIM,
   no subscription, worldwide coverage, and the wallet itself never needs GPS.

   HOW IT WORKS
     Passing iPhones see the advertisement, encrypt THEIR OWN GPS fix with the
     P-224 public key you broadcast, and upload it to Apple. Only someone with
     the matching private key can decrypt it -- that is you, via your
     macless-haystack server. Apple cannot read it and neither can anyone else.

   BEFORE THIS WORKS YOU MUST
     1. Clone https://github.com/dchristl/macless-haystack
     2. Run their generate_keys.py -> gives you a keypair + a .keys file
     3. Paste the 28-byte PUBLIC key below
     4. Stand up their two Docker containers (anisette + endpoint) and load
        the PRIVATE key into the app. That is what turns reports into a map pin.

   HONEST STATUS
     The upstream nRF52 firmware build is marked UNTESTED by its own authors,
     and this is an independent reimplementation on top of Bluefruit. The
     advertisement bytes below match the OpenHaystack reference exactly, but
     treat the whole file as experimental. The proven path is a second, tiny
     nRF51822 module running upstream's TESTED nRF51 build -- see the build
     sheet. Set MONO_ENABLE_FINDMY to 0 to compile this out entirely.
   ========================================================================= */

#ifndef MONO_FINDMY_H
#define MONO_FINDMY_H

#include <bluefruit.h>

#ifndef MONO_ENABLE_FINDMY
#define MONO_ENABLE_FINDMY 0     // <-- set to 1 once you have pasted a real key
#endif

/* ---------------------------------------------------------------------------
   YOUR PUBLIC KEY  -  28 bytes, from macless-haystack's generate_keys.py
   The placeholder below is NOT a valid key. Replace every byte.
   With a placeholder key the wallet still advertises, but no report you can
   ever decrypt will come back, so nothing will appear on your map.
--------------------------------------------------------------------------- */
static const uint8_t FINDMY_PUBLIC_KEY[28] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ---------------------------------------------------------------------------
   Advertisement layout (OpenHaystack reference, 31 bytes total).

     [ 0] 0x1e        length of everything after this byte (30)
     [ 1] 0xff        AD type: manufacturer specific data
     [ 2] 0x4c 0x00   Apple company ID (little endian 0x004C)
     [ 4] 0x12 0x19   offline-finding type, payload length
     [ 6] 0x00        status byte
     [ 7] key[6..27]  22 bytes of the public key
     [29] key[0] >> 6 the top two bits of key byte 0
     [30] 0x00        hint

   Key bytes 0..5 do not travel in the payload -- they become the BLE address.
   Note there is no Flags AD structure: 31 bytes is the entire legacy
   advertising PDU, so it is all spent here. That is legal only because this
   is non-connectable, non-scannable advertising.
--------------------------------------------------------------------------- */
static uint8_t findmy_adv[31] = {
  0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0x00,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0x00, 0x00
};

inline void findmyBuildPayload() {
  memcpy(&findmy_adv[7], &FINDMY_PUBLIC_KEY[6], 22);
  findmy_adv[29] = FINDMY_PUBLIC_KEY[0] >> 6;
  findmy_adv[30] = 0x00;
}

/* The MAC is derived from key bytes 0..5, with the top two bits forced to 11
   so it is a valid BLE random-static address.

   CAREFUL: ble_gap_addr_t.addr is stored LEAST significant byte first, while
   the OpenHaystack reference writes the address in human MAC order. So the
   key bytes go in reversed. Getting this backwards is the classic reason a
   DIY tag advertises happily and is never seen by the network. */
inline void findmySetAddress() {
  ble_gap_addr_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
  addr.addr[5] = FINDMY_PUBLIC_KEY[0] | 0xC0;
  addr.addr[4] = FINDMY_PUBLIC_KEY[1];
  addr.addr[3] = FINDMY_PUBLIC_KEY[2];
  addr.addr[2] = FINDMY_PUBLIC_KEY[3];
  addr.addr[1] = FINDMY_PUBLIC_KEY[4];
  addr.addr[0] = FINDMY_PUBLIC_KEY[5];
  Bluefruit.setAddr(&addr);
}

/* Swap the radio into beacon mode. The wallet stops being connectable while
   this is running -- your app cannot reach it. That is the trade, and it is
   why the main sketch time-slices between the two rather than picking one. */
inline void findmyStart() {
  findmyBuildPayload();
  Bluefruit.Advertising.stop();
  findmySetAddress();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
                                &findmy_adv[2], 29);
  Bluefruit.Advertising.setInterval(1600, 1600);   // x0.625 ms = 1 s
  Bluefruit.Advertising.start(0);
}

#endif
