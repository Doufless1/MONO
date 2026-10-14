/* LSM6DS3.h - host mock of the XIAO Sense IMU.
   Three ways to move the wallet:
     z = 5          a sustained 5 g push. One long knock, not a shake.
     shaking        what a hand shaking it produces: a 5 g jolt for 40 ms in
                    every 120 ms.
     jolt(from, ms) a single knock scheduled on the clock, so its physical
                    length is exact whatever the firmware's loop is doing.
                    That is what lets a test prove detection no longer depends
                    on the loop's pace. */
#ifndef MOCK_IMU_H
#define MOCK_IMU_H
#include "Arduino.h"
#include <utility>
#define I2C_MODE 0
struct MockIMU {
  float x = 0.f, y = 0.f, z = 1.0f;
  int   beginResult = 0;
  bool  shaking = false;
  std::vector<std::pair<uint32_t, uint32_t> > jolts;   // [from, until) at 5 g
  void jolt(uint32_t from, uint32_t ms) { jolts.push_back(std::make_pair(from, from + ms)); }
  float zNow() const {
    if (shaking && g_millis % 120 < 40) return 5.0f;
    for (size_t i = 0; i < jolts.size(); i++)
      if (g_millis >= jolts[i].first && g_millis < jolts[i].second) return 5.0f;
    return z;
  }
};
extern MockIMU g_imu;
class LSM6DS3 {
public:
  LSM6DS3(int, uint8_t) {}
  int   begin() { return g_imu.beginResult; }
  float readFloatAccelX() { return g_imu.x; }
  float readFloatAccelY() { return g_imu.y; }
  float readFloatAccelZ() { return g_imu.zNow(); }
};
#endif
