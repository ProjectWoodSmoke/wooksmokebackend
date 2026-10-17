#include "pm_sensor.h"

#include <Wire.h>

#include "SparkFun_BMV080_Arduino_Library.h"
#include "busy_flags.h"
#include "config.h"

static SparkFunBMV080 bmv080;

LatestPmData latestPm;

bool pmSensorMeasuring = false;
uint32_t pmSensorSampleCount = 0;
uint32_t pmSensorRecoveries = 0;
uint32_t pmSensorRecoveryAttempts = 0;

static uint32_t pollCount = 0;

/// A delivered sample or a start attempt.
static uint32_t lastActivity = 0;

static bool driverOpen = false;

/// Reset by every restart: the warm-up applies after a recovery too, and the
/// watchdog must allow for it before firing again.
static bool gotSampleSinceStart = false;

bool latestPmIsFresh() {
  return latestPm.valid && (millis() - latestPm.lastUpdate <= PM_SAMPLE_MAX_AGE);
}

uint32_t pmSensorStallLimit() {
  return gotSampleSinceStart ? PM_STALL_TIMEOUT : PM_FIRST_SAMPLE_TIMEOUT;
}

bool pmSensorBegin() {
  // Only close a handle we own: the Bosch blob makes no promise about NULL.
  if (driverOpen) {
    bmv080.close();
    driverOpen = false;
    delay(50);
  }

  pmSensorMeasuring = false;
  gotSampleSinceStart = false;
  lastActivity = millis();  // stamped up front so every failure path backs off

#if BMV080_POWER_PIN >= 0
  Serial.println("[BMV080] Power cycling...");
  pinMode(BMV080_POWER_PIN, OUTPUT);
  digitalWrite(BMV080_POWER_PIN, LOW);
  delay(BMV080_POWER_OFF_MS);
  digitalWrite(BMV080_POWER_PIN, HIGH);
  delay(100);  // let the rail settle before the first transaction
#endif

  if (!bmv080.begin(BMV080_ADDR, Wire)) {
    Serial.println("[BMV080] Not responding at 0x57");
    return false;
  }

  if (!bmv080.init()) {
    Serial.println("[BMV080] init() FAILED - no particulate data will be produced");
    return false;
  }
  driverOpen = true;

#if BMV080_DUTY_CYCLE_SECONDS > 0
  // Order matters: the API rejects parameter writes once a measurement is
  // running, with E_BMV080_ERROR_PARAM_LOCKED (179).
  if (!bmv080.setDutyCyclingPeriod(BMV080_DUTY_CYCLE_SECONDS)) {
    Serial.println("[BMV080] setDutyCyclingPeriod() FAILED");
    return false;
  }
  if (!bmv080.setMode(SF_BMV080_MODE_DUTY_CYCLE)) {
    Serial.println("[BMV080] setMode(DUTY_CYCLE) FAILED - sensor is not measuring");
    return false;
  }
  Serial.printf("[BMV080] Duty cycling every %ds\n", BMV080_DUTY_CYCLE_SECONDS);
#else
  if (!bmv080.setMode(SF_BMV080_MODE_CONTINUOUS)) {
    Serial.println("[BMV080] setMode(CONTINUOUS) FAILED - sensor is not measuring");
    return false;
  }
#endif

  pmSensorMeasuring = true;
  lastActivity = millis();
  return true;
}

/// Probes the part, reports which physical fault to chase, and restarts.
static void recoverPmSensor() {
  pmSensorRecoveryAttempts++;

  // "ACK yes, regs no" means powered and present but the sensor core is
  // wedged, which is not the same as gone.
  const bool acked = bmv080.isConnected();
  bool regsOk = false;
  if (driverOpen) {
    char id[13] = { 0 };
    regsOk = bmv080.ID(id);
  }

  Serial.printf("[BMV080] STALLED - no sample for %lus. Address ACK: %s, register read: %s\n",
               (unsigned long)((millis() - lastActivity) / 1000),
               acked ? "YES" : "NO",
               driverOpen ? (regsOk ? "YES" : "NO") : "n/a (no handle)");

  if (!acked) {
    Serial.println("[BMV080]   -> not on the bus at all: unpowered, or a wiring/pull-up fault.");
  } else if (driverOpen && !regsOk) {
    Serial.println("[BMV080]   -> powered and ACKing, but the sensor core is not responding.");
    Serial.println("[BMV080]      Bosch files this pattern under \"check that power provided to");
    Serial.println("[BMV080]      BMV080 is sufficient & stable\". A soft reinit rarely clears it;");
    Serial.println("[BMV080]      see BMV080_POWER_PIN for the hard power-cycle path.");
  }

  if (pmSensorBegin()) {
    pmSensorRecoveries++;
    Serial.printf("[BMV080] Reinitialized OK (recovery #%lu)\n",
                 (unsigned long)pmSensorRecoveries);
  } else {
    Serial.printf("[BMV080] Reinit failed - retrying in %lus\n",
                 (unsigned long)(pmSensorStallLimit() / 1000));
  }
}

/// Distinguishes a quiet sensor from a dead one.
static void logHeartbeat() {
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat <= 10000) {
    return;
  }

  char age[40];
  if (latestPm.valid) {
    snprintf(age, sizeof(age), "last sample %lus ago",
             (unsigned long)((millis() - latestPm.lastUpdate) / 1000));
  } else {
    snprintf(age, sizeof(age), "no sample since boot");
  }

  // Polls climbing while samples stay frozen means the sensor stopped
  // measuring.
  Serial.printf("[BMV080] polls:%lu samples:%lu recoveries:%lu/%lu measuring:%s (%s)\n",
               (unsigned long)pollCount, (unsigned long)pmSensorSampleCount,
               (unsigned long)pmSensorRecoveries, (unsigned long)pmSensorRecoveryAttempts,
               pmSensorMeasuring ? "YES" : "NO", age);
  lastHeartbeat = millis();
}

void pmSensorService() {
  if (!takeBusyFlag(&i2cBusy, 50)) {
    return;
  }

  pollCount++;

  uint8_t drained = 0;
  while (drained < BMV080_MAX_DRAIN && bmv080.readSensor()) {
    latestPm.pm1 = bmv080.PM1();
    latestPm.pm25 = bmv080.PM25();
    latestPm.pm10 = bmv080.PM10();
    latestPm.obstructed = bmv080.isObstructed();
    latestPm.valid = true;
    latestPm.lastUpdate = millis();
    lastActivity = latestPm.lastUpdate;
    gotSampleSinceStart = true;
    pmSensorSampleCount++;
    drained++;
  }

  if (drained == 0 && millis() - lastActivity > pmSensorStallLimit()) {
    recoverPmSensor();
  }

  releaseBusyFlag(&i2cBusy);

  // readSensor() cannot say WHY it returned false, hence these counters and
  // the ID() probe in recoverPmSensor().
  logHeartbeat();
}
