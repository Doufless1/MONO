#ifndef MOCK_IMU_H
#define MOCK_IMU_H
#include "Arduino.h"
#define I2C_MODE 0
struct MockIMU { float x = 0.f, y = 0.f, z = 1.0f; int beginResult = 0; };
extern MockIMU g_imu;
class LSM6DS3 {
public:
  LSM6DS3(int, uint8_t) {}
  int   begin() { return g_imu.beginResult; }
  float readFloatAccelX() { return g_imu.x; }
  float readFloatAccelY() { return g_imu.y; }
  float readFloatAccelZ() { return g_imu.z; }
};
#endif
