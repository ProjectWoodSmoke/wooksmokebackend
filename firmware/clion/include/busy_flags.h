#pragma once

#include <Arduino.h>

// Every caller runs from loop(), so a flag is always free when tested and a
// "busy" log line cannot currently happen. These exist in case anything moves
// to an interrupt or a second task.

extern volatile bool i2cBusy;
extern volatile bool dataBusy;

/// On timeout returns false, and the caller must neither proceed nor release.
bool takeBusyFlag(volatile bool* flag, uint32_t timeoutMs);

void releaseBusyFlag(volatile bool* flag);
