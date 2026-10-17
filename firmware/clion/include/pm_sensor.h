#pragma once

#include <Arduino.h>

/// readSensor() returns true only on the call where its data-ready callback
/// fired, so polling at the 1 s snapshot cadence aliases the sensor's own ~1 Hz
/// rate and reads appear to fail forever. pmSensorService() polls faster.
struct LatestPmData {
  bool  valid = false;
  float pm1 = 0;
  float pm25 = 0;
  float pm10 = 0;
  bool  obstructed = false;
  uint32_t lastUpdate = 0;
};

extern LatestPmData latestPm;

extern bool pmSensorMeasuring;
extern uint32_t pmSensorSampleCount;
extern uint32_t pmSensorRecoveries;        ///< Reinit attempts that succeeded.
extern uint32_t pmSensorRecoveryAttempts;  ///< Reinit attempts, successful or not.

bool latestPmIsFresh();

/// Also the restart path used by the stall watchdog.
bool pmSensorBegin();

uint32_t pmSensorStallLimit();

/// Must run every loop() iteration; see LatestPmData.
void pmSensorService();
