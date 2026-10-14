/* mock_impl.h - the single definition of every mocked global.
   Include this exactly once per binary, before the firmware itself. The test
   harness, the terminal simulator and the WebAssembly build all pull it in,
   so the three can never drift apart. */
#ifndef MOCK_IMPL_H
#define MOCK_IMPL_H

#include "Arduino.h"
#include "Adafruit_Fingerprint.h"
#include "bluefruit.h"
#include "Servo.h"
#include "Adafruit_LittleFS.h"
#include "LSM6DS3.h"
#include "nrf_soc.h"

uint32_t g_millis = 0;
void (*g_sleepHook)(uint32_t) = nullptr;
std::vector<Beep> g_beeps;
bool  g_buzzerOn = false;
std::vector<int>  g_servoWrites;
std::vector<std::string> g_out;
std::vector<std::string> g_bleOut;
int   g_pinState[64] = {0};
float g_vbatRaw = 1868.0f;              // ~4.05 V through the 1510/510 divider
MockSerial Serial, Serial1;
MockFP     g_fp;
MockIMU    g_imu;
AdvState   g_adv;
ble_gap_addr_t g_identityAddr = { 0, BLE_GAP_ADDR_TYPE_RANDOM_STATIC, {0x4F, 0x4E, 0x4F, 0x4D, 0x11, 0xC2} };
ble_gap_addr_t g_addr = g_identityAddr;
bool g_connected = false;
MockBluefruit Bluefruit;
int  g_servoAngle = -1;
bool g_servoAttached = false;
Adafruit_LittleFS_Namespace::MockFS InternalFS;
uint64_t g_rngState = 0x9E3779B97F4A7C15ULL;
bool     g_rngEmpty = false;

bool g_wakeOverride = false;
int  g_wakeValue = LOW;

int digitalRead(int pin) {
  if (pin == 2) {                       // PIN_FP_WAKE - the R502 touch line
    if (g_wakeOverride) return g_wakeValue;
    return g_fp.platen == PLATEN_EMPTY ? LOW : HIGH;
  }
  return pin < 64 ? g_pinState[pin] : LOW;
}

#endif
