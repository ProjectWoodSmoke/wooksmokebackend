#pragma once

#include <Arduino.h>

/// BSEC completes a cycle roughly every 3 s while snapshots are taken every
/// 1 s, so consecutive snapshots may reuse the same values.
struct LatestEnvData {
  bool  valid = false;
  float temperature = 0;
  float humidity = 0;
  float pressure = 0;
  float gasResistance = 0;
  float iaq = 0;
  uint8_t iaqAccuracy = 0;
  float co2Equivalent = 0;
  float breathVocEquivalent = 0;
  float gasPercentage = 0;
  bool  stabilized = false;
  bool  runInComplete = false;
};

extern LatestEnvData latestEnv;

extern bool envSensorReady;
extern bool bsecStateRestored;
extern uint8_t bsecSavedAccuracy;

bool envSensorBegin();

/// Must run every loop() iteration: BSEC schedules the heater timing itself.
void envSensorService();
