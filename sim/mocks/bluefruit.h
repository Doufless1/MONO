/* bluefruit.h - host mock of the Adafruit nRF52 BLE stack.
   Records advertising state so tests can prove the wallet really switches to
   a fast beacon on an alarm - and really switches back afterwards - and
   captures every line sent to the phone. */
#ifndef MOCK_BLUEFRUIT_H
#define MOCK_BLUEFRUIT_H
#include "Arduino.h"

#define BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED       0x00
#define BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED 0x03
#define BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE             0x06
#define BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA              0xFF
#define BLE_GAP_ADDR_TYPE_RANDOM_STATIC                         0x01

typedef enum { SECMODE_NO_ACCESS = 0x00, SECMODE_OPEN = 0x11,
               SECMODE_ENC_NO_MITM = 0x21, SECMODE_ENC_WITH_MITM = 0x31 } SecureMode_t;

typedef struct { uint8_t addr_id_peer; uint8_t addr_type; uint8_t addr[6]; } ble_gap_addr_t;

struct AdvState {
  bool     running   = false;
  uint16_t interval  = 0;
  uint8_t  type      = 0;
  bool     connectable = true;
  std::vector<uint8_t> data;
  int      restarts  = 0;
};
extern AdvState       g_adv;
extern ble_gap_addr_t g_addr;           // the address the radio is using right now
extern ble_gap_addr_t g_identityAddr;   // the board's own, as getAddr() reports at boot
extern bool           g_connected;
extern std::vector<std::string> g_bleOut;   // every line the wallet sent to the phone

class BLEUart;

struct MockAdvertising {
  void stop()       { g_adv.running = false; }
  void clearData()  { g_adv.data.clear(); }
  void setType(uint8_t t) {
    g_adv.type = t;
    g_adv.connectable = (t == BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  }
  void addFlags(uint8_t) {}
  void addTxPower() {}
  template <typename T> void addService(T &) {}
  void addData(uint8_t, const void *p, uint8_t len) {
    const uint8_t *b = (const uint8_t *)p;
    g_adv.data.assign(b, b + len);
  }
  void restartOnDisconnect(bool) {}
  void setInterval(uint16_t a, uint16_t) { g_adv.interval = a; }
  void start(uint16_t) { g_adv.running = true; g_adv.restarts++; }
};
struct MockScanResponse { void clearData() {} void addName() {} };
struct MockPeriph {
  void (*onConn)(uint16_t) = nullptr;
  void (*onDisc)(uint16_t, uint8_t) = nullptr;
  void setConnectCallback(void (*f)(uint16_t)) { onConn = f; }
  void setDisconnectCallback(void (*f)(uint16_t, uint8_t)) { onDisc = f; }
};
struct MockBluefruit {
  MockAdvertising  Advertising;
  MockScanResponse ScanResponse;
  MockPeriph       Periph;
  bool begin() { return true; }
  void setTxPower(int) {}
  void setName(const char *) {}
  bool connected() { return g_connected; }
  ble_gap_addr_t getAddr() { return g_identityAddr; }
  bool setAddr(ble_gap_addr_t *a) { g_addr = *a; return true; }
};
extern MockBluefruit Bluefruit;

class BLEUart {
public:
  std::vector<char> in;  size_t rd = 0;
  std::string       tx;                  // everything sent to the phone, raw
  std::string       partial;             // the line being assembled into g_bleOut
  SecureMode_t      permRead = SECMODE_OPEN, permWrite = SECMODE_OPEN;
  void begin() {}
  void setPermission(SecureMode_t r, SecureMode_t w) { permRead = r; permWrite = w; }
  int  available() { return (int)(in.size() - rd); }
  int  read() { return rd < in.size() ? in[rd++] : -1; }
  void feed(const std::string &s) { for (char c : s) in.push_back(c); }
  void print(const char *s) { for (; *s; s++) print(*s); }
  void print(char c) {
    tx += c;
    if (c == '\n') { g_bleOut.push_back(partial); partial.clear(); }
    else           partial += c;
  }
};
#endif
