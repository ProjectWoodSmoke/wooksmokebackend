#pragma once

#include <Arduino.h>

/// Each group carries its own validity flag because a snapshot may come from
/// the periodic read or from an asynchronous lightning event; only flagged
/// groups are serialized.
struct SensorData {
  // BME688 / BSEC2
  bool  envValid;
  float temperature;
  float humidity;
  float pressure;
  float gasResistance;
  float altitude;
  float iaq;
  uint8_t iaqAccuracy;
  float co2Equivalent;
  float breathVocEquivalent;
  float gasPercentage;
  bool  stabilized;
  bool  runInComplete;

  // BMV080
  bool  bmv080Valid;
  bool  bmv080Obstructed;
  float pm1;
  float pm25;
  float pm10;

  // AS3935
  bool     lightningValid;
  bool     lightningStrike;
  bool     lightningDisturber;
  float    lightningDistanceKm;
  uint32_t lightningEnergy;

  uint32_t epochUnix;
};

/// Barometric altitude in metres from station pressure in hPa.
inline float altitude(float press, float seaLevel = 1013.25f) {
  return 44330.0 * (1.0 - pow(press / seaLevel, 0.1903));
}
