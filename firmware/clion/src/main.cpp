#include <Arduino.h>
#include <Wire.h>

#include "busy_flags.h"
#include "config.h"
#include "env_sensor.h"
#include "i2c_bus.h"
#include "lightning_sensor.h"
#include "notecard_manager.h"
#include "pm_sensor.h"
#include "readings_buffer.h"
#include "sensor_data.h"
#include "time_sync.h"

static unsigned long lastSnapshot = 0;
static unsigned long lastDataSend = 0;
static unsigned long lastRegisterAttempt = 0;
static bool deviceRegistered = false;

/// Reads no hardware: samples whatever env_sensor and pm_sensor last published.
static void captureSnapshot() {
  if (millis() - lastSnapshot < SENSOR_READ_INTERVAL) {
    return;
  }

  if (!takeBusyFlag(&i2cBusy, 5000)) {
    Serial.println("[SENS] I2C busy, skipping");
    lastSnapshot = millis();
    return;
  }

  if (timeBaseNeedsSync()) {
    syncTimeBaseLocked();
  }

  releaseBusyFlag(&i2cBusy);

  const bool pmValid = latestPmIsFresh();
  const float pm1 = pmValid ? latestPm.pm1 : 0;
  const float pm25 = pmValid ? latestPm.pm25 : 0;
  const float pm10 = pmValid ? latestPm.pm10 : 0;
  const bool obstructed = pmValid ? latestPm.obstructed : false;

  uint32_t epochTime;
  if (!currentEpoch(&epochTime)) {
    Serial.println("[SENS] No time base yet, skipping reading");
    lastSnapshot = millis();
    return;
  }

  if (!takeBusyFlag(&dataBusy, 1000)) {
    Serial.println("[SENS] Data busy, skipping");
    lastSnapshot = millis();
    return;
  }

  const bool envValid = envSensorReady && latestEnv.valid;

  SensorData snapshot = {};
  snapshot.envValid = envValid;
  if (envValid) {
    snapshot.temperature = latestEnv.temperature;
    snapshot.humidity = latestEnv.humidity;
    snapshot.pressure = latestEnv.pressure;
    snapshot.gasResistance = latestEnv.gasResistance;
    snapshot.altitude = altitude(latestEnv.pressure);
    snapshot.iaq = latestEnv.iaq;
    snapshot.iaqAccuracy = latestEnv.iaqAccuracy;
    snapshot.co2Equivalent = latestEnv.co2Equivalent;
    snapshot.breathVocEquivalent = latestEnv.breathVocEquivalent;
    snapshot.gasPercentage = latestEnv.gasPercentage;
    snapshot.stabilized = latestEnv.stabilized;
    snapshot.runInComplete = latestEnv.runInComplete;
  }
  snapshot.bmv080Valid = pmValid;
  snapshot.pm1 = pm1;
  snapshot.pm25 = pm25;
  snapshot.pm10 = pm10;
  snapshot.bmv080Obstructed = obstructed;
  snapshot.lightningValid = false;  // periodic read, not an event
  snapshot.epochUnix = epochTime;

  pushSnapshot(snapshot);

  char pmStr[12];
  if (pmValid) {
    snprintf(pmStr, sizeof(pmStr), "%.1f", pm25);
  } else {
    snprintf(pmStr, sizeof(pmStr), "--");
  }

  Serial.printf("[SENS] T:%.1f H:%.1f P:%.1f Gas:%.1f IAQ:%.1f(acc%d) PM2.5:%s @ %lu [Buffer: %d/%d]\n",
               snapshot.temperature, snapshot.humidity, snapshot.pressure,
               snapshot.gasResistance, snapshot.iaq, snapshot.iaqAccuracy, pmStr,
               (unsigned long)epochTime, bufferedSnapshotCount(), MAX_READINGS_BUFFER);

  releaseBusyFlag(&dataBusy);
  lastSnapshot = millis();
}

/// Copies out of the buffer rather than holding dataBusy across the send, so an
/// in-flight Notecard call never blocks new snapshots. The copy is only
/// discarded once the send succeeds.
static void sendPendingReadings() {
  if (!hasBufferedData() || bufferedSnapshotCount() == 0 ||
      millis() - lastDataSend < DATA_SEND_INTERVAL) {
    return;
  }

  Serial.printf("[SEND] Preparing to send %d readings...\n", bufferedSnapshotCount());

  if (!takeBusyFlag(&dataBusy, 1000)) {
    Serial.println("[SEND] Data busy");
    return;
  }

  // Static, not automatic: 40 x sizeof(SensorData) is ~3 KB, a large share of
  // the 8 KB loop task stack. Safe because loop() is the only caller.
  static SensorData sendBuffer[MAX_READINGS_BUFFER];
  const uint8_t sendCount = peekSnapshots(sendBuffer);

  releaseBusyFlag(&dataBusy);

  if (notecardManager.sendBatchedData(sendBuffer, sendCount)) {
    if (takeBusyFlag(&dataBusy, 1000)) {
      discardSnapshots(sendCount);
      releaseBusyFlag(&dataBusy);
    }
  } else {
    Serial.printf("[SEND] Failed - %d readings kept for the next attempt\n", sendCount);
  }

  lastDataSend = millis();
}

static void ensureDeviceRegistered() {
  if (deviceRegistered || millis() - lastRegisterAttempt < 5000) {
    return;
  }

  Serial.println("[REG] Attempting registration...");
  deviceRegistered = notecardManager.registerDevice();
  lastRegisterAttempt = millis();

  if (!deviceRegistered) {
    Serial.println("[REG] Will retry in 5s");
  }
}

static void logStatus() {
  Serial.printf("\n[STATUS] Uptime: %lu s, Heap: %d, Reg: %s, Buffer: %d/%d\n",
               millis() / 1000, ESP.getFreeHeap(),
               deviceRegistered ? "YES" : "NO",
               bufferedSnapshotCount(), MAX_READINGS_BUFFER);
  Serial.printf("[STATUS] IAQ acc: %d/3 (saved: %d/3, restored at boot: %s), stab: %d, runIn: %d\n",
               latestEnv.iaqAccuracy, bsecSavedAccuracy,
               bsecStateRestored ? "YES" : "NO",
               latestEnv.stabilized ? 1 : 0, latestEnv.runInComplete ? 1 : 0);
  Serial.printf("[STATUS] BMV080: measuring %s, samples %lu, recoveries %lu/%lu, PM data %s\n",
               pmSensorMeasuring ? "YES" : "NO",
               (unsigned long)pmSensorSampleCount,
               (unsigned long)pmSensorRecoveries, (unsigned long)pmSensorRecoveryAttempts,
               latestPmIsFresh() ? "FRESH" : "STALE");
  Serial.printf("[STATUS] Notecard: recoveries %lu/%lu\n",
               (unsigned long)notecardRecoveries,
               (unsigned long)notecardRecoveryAttempts);

  // Clamped: once a send is overdue the subtraction underflows.
  const uint32_t sinceSend = millis() - lastDataSend;
  const uint32_t nextSendSec =
      (sinceSend >= DATA_SEND_INTERVAL) ? 0 : (DATA_SEND_INTERVAL - sinceSend) / 1000;
  Serial.printf("[STATUS] Next send in: %lu sec\n\n", (unsigned long)nextSendSec);
}

void setup() {
  Serial.begin(SERIAL_SPEED);
  delay(2000);

  Serial.println("\n========================================");
  Serial.println("  Air Quality Monitor - Batched Mode (compact)");
  Serial.println("  BME688 + BSEC2 (AI air quality) Version");
  Serial.println("========================================");
  Serial.printf("Read interval: %lu sec\n", (unsigned long)(SENSOR_READ_INTERVAL / 1000));
  Serial.printf("Send interval: %lu sec\n", (unsigned long)(DATA_SEND_INTERVAL / 1000));
  Serial.printf("Max readings per batch: %d\n", MAX_READINGS_BUFFER);
  Serial.printf("Monthly requests: ~%lu / 5000\n",
               (unsigned long)((30UL * 24UL * 60UL * 60UL * 1000UL) / DATA_SEND_INTERVAL));
  Serial.printf("Free heap: %d bytes\n\n", ESP.getFreeHeap());

  Serial.println("[SETUP] Init I2C...");

  reportI2CPinState();
  recoverI2CBus();

  Wire.begin(I2C_SDA, I2C_SCL);
  // "ER" entries in the scan below mean wiring or pull-up marginality, not a
  // sensor fault; drop to 100000 if they appear.
  Wire.setClock(400000);
  delay(100);

  scanI2CBus();

  Serial.println("[SETUP] Init BME688 (BSEC2)...");
  envSensorBegin();

  Serial.println("[SETUP] Init BMV080...");
  if (pmSensorBegin()) {
    Serial.println("[SETUP] BMV080 OK - continuous mode started");
  } else {
    Serial.println("[SETUP] BMV080 unavailable - continuing without particulate data");
    Serial.printf("[SETUP]   (auto-retry every %lus)\n",
                 (unsigned long)(pmSensorStallLimit() / 1000));
  }

  Serial.println("[SETUP] Init AS3935 Lightning Detector (SPI)...");
  lightningSensorBegin();

  Serial.println("[SETUP] Init Notecard...");
  notecardManager.begin();

  Serial.println("\n========================================");
  Serial.println("  Setup Complete");
  Serial.println("========================================\n");
}

void loop() {
  static unsigned long lastStatus = 0;

  ensureDeviceRegistered();

  envSensorService();
  pmSensorService();
  lightningSensorService();

  captureSnapshot();
  sendPendingReadings();

  if (millis() - lastStatus > 30000) {
    logStatus();
    lastStatus = millis();
  }

  // delay(100) capped BMV080 servicing at ~7 Hz and tripped Bosch's FIFO
  // overflow warnings; 0 would starve the watchdog this task must feed.
  delay(1);
}
