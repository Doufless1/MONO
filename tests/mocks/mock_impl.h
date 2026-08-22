/* mock_impl.h - the single definition of every mocked global.
   Include this exactly once per binary, before the firmware itself. Both the
   test harness and the interactive simulator pull it in so the two can never
   drift apart. */
#ifndef MOCK_IMPL_H
#define MOCK_IMPL_H

#include "Arduino.h"
#include "Adafruit_Fingerprint.h"
#include "bluefruit.h"
#include "Servo.h"
#include "Adafruit_LittleFS.h"
#include "LSM6DS3.h"

uint32_t g_millis = 0;
void (*g_sleepHook)(uint32_t) = nullptr;
std::vector<Beep> g_beeps;
std::vector<int>  g_servoWrites;
std::vector<std::string> g_out;
int   g_pinState[64] = {0};
float g_vbatRaw = 1868.0f;              // ~4.05 V through the 1510/510 divider
MockSerial Serial, Serial1;
MockFP     g_fp;
MockIMU    g_imu;
AdvState   g_adv;
ble_gap_addr_t g_addr;
bool g_connected = false;
MockBluefruit Bluefruit;
int  g_servoAngle = -1;
bool g_servoAttached = false;
Adafruit_LittleFS_Namespace::MockFS InternalFS;

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
