#include "busy_flags.h"

volatile bool i2cBusy = false;
volatile bool dataBusy = false;

bool takeBusyFlag(volatile bool* flag, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (*flag) {
    if (millis() - start > timeoutMs) {
      return false;
    }
    delay(10);
  }
  *flag = true;
  return true;
}

void releaseBusyFlag(volatile bool* flag) {
  *flag = false;
}
