#pragma once

#include <Arduino.h>
#include <Notecard.h>

#include "sensor_data.h"

class NotecardManager {
public:
  void begin();

  bool registerDevice();

  /// Wire format: {"d": [[deviceId, [[epoch, [[type, value], ...]], ...]]]}
  bool sendBatchedData(const SensorData* readings, uint8_t count);

private:
  /// Appends [typeCode, value], or [typeCode, value, unitCode] when
  /// unitOverrideCode is >= 0.
  static void appendReading(J* readingsArr, int typeCode, float value, int unitOverrideCode = -1);

  /// Re-runs the init sequence in place. Caller must NOT hold i2cBusy.
  void recover();

  uint8_t consecutiveFailures = 0;
};

extern NotecardManager notecardManager;

extern uint32_t notecardRecoveries;        ///< Reinit attempts that restored the link.
extern uint32_t notecardRecoveryAttempts;  ///< Reinit attempts, successful or not.
