#pragma once

#include <Arduino.h>

// Epoch time from the Notecard, interpolated with millis() between syncs.

/// Caller must already hold i2cBusy.
bool syncTimeBaseLocked();

bool timeBaseNeedsSync();

/// Cached value only, no bus traffic. False until the first successful sync.
bool currentEpoch(uint32_t* epochOut);

/// Takes i2cBusy itself, resyncing if stale. Caller must NOT hold it.
bool getEpochNow(uint32_t* epochOut);
