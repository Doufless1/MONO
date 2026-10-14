#ifndef MOCK_SERVO_H
#define MOCK_SERVO_H
#include "Arduino.h"
extern int  g_servoAngle;
extern bool g_servoAttached;
class Servo {
public:
  void attach(int) { g_servoAttached = true; }
  void write(int deg) { g_servoAngle = deg; g_servoWrites.push_back(deg); }
  void detach() { g_servoAttached = false; }
};
#endif
