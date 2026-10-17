#include "lightning_sensor.h"

#include <SPI.h>

#include "SparkFun_AS3935.h"
#include "busy_flags.h"
#include "config.h"
#include "readings_buffer.h"
#include "sensor_data.h"
#include "time_sync.h"

// Not defined by the SparkFun library; its examples declare them locally.
constexpr uint8_t LIGHTNING_INT = 0x08;
constexpr uint8_t DISTURBER_INT = 0x04;
constexpr uint8_t NOISE_INT     = 0x01;

static SparkFun_AS3935 lightning;

bool lightningSensorReady = false;

bool lightningSensorBegin() {
  pinMode(LIGHTNING_IRQ_PIN, INPUT);
  pinMode(LIGHTNING_CS_PIN, OUTPUT);
  digitalWrite(LIGHTNING_CS_PIN, HIGH);  // CS idles high, active low
  SPI.begin();

  if (!lightning.beginSPI(LIGHTNING_CS_PIN, LIGHTNING_SPI_SPEED)) {
    Serial.println("[SETUP] AS3935 NOT FOUND - lightning detection disabled, continuing without it");
    lightningSensorReady = false;
    return false;
  }

  lightning.setIndoorOutdoor(OUTDOOR);  // INDOOR if the enclosure is inside
  lightning.maskDisturber(false);

  // Tunes the LC antenna to the chip's 500 kHz reference; without it the IRQ
  // pin may never reliably trigger.
  lightning.calibrateOsc();

  lightningSensorReady = true;
  Serial.println("[SETUP] AS3935 OK");
  return true;
}

/// Distinguishes a quiet sensor from a dead one.
static void logHeartbeat() {
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 10000) {
    Serial.printf("[LIGHTNING] Alive, watching IRQ pin (currently %s)\n",
                 digitalRead(LIGHTNING_IRQ_PIN) == HIGH ? "HIGH" : "LOW");
    lastHeartbeat = millis();
  }
}

void lightningSensorService() {
  if (!lightningSensorReady) {
    return;
  }

  logHeartbeat();

  if (digitalRead(LIGHTNING_IRQ_PIN) != HIGH) {
    return;
  }

  delay(2);  // datasheet: the interrupt register needs ~2 ms to settle

  // On SPI, so no bus lock is needed; only the epoch fetch below touches I2C.
  const uint8_t intVal = lightning.readInterruptReg();

  bool strikeDetected = false;
  bool disturberDetected = false;
  float distanceKm = 0;
  uint32_t energy = 0;

  if (intVal == NOISE_INT) {
    Serial.println("[LIGHTNING] Noise level too high");
  } else if (intVal == DISTURBER_INT) {
    Serial.println("[LIGHTNING] Disturber detected");
    disturberDetected = true;
  } else if (intVal == LIGHTNING_INT) {
    const byte distance = lightning.distanceToStorm();
    const long energyVal = lightning.lightningEnergy();
    strikeDetected = true;
    distanceKm = (float)distance;
    energy = (uint32_t)energyVal;
    Serial.printf("[LIGHTNING] Strike detected! ~%d km away, energy=%ld\n", distance, energyVal);
  }

  if (!strikeDetected && !disturberDetected) {
    return;
  }

  uint32_t epochTime;
  if (!getEpochNow(&epochTime)) {
    Serial.println("[LIGHTNING] No time base, dropping event");
    return;
  }

  if (!takeBusyFlag(&dataBusy, 1000)) {
    Serial.println("[LIGHTNING] Data busy, dropping event");
    return;
  }

  SensorData snapshot = {};
  snapshot.envValid = false;
  snapshot.bmv080Valid = false;
  snapshot.lightningValid = true;
  snapshot.lightningStrike = strikeDetected;
  snapshot.lightningDisturber = disturberDetected;
  snapshot.lightningDistanceKm = distanceKm;
  snapshot.lightningEnergy = energy;
  snapshot.epochUnix = epochTime;

  pushSnapshot(snapshot);

  Serial.printf("[LIGHTNING] Event buffered @ %lu [Buffer: %d/%d]\n",
               (unsigned long)epochTime, bufferedSnapshotCount(), MAX_READINGS_BUFFER);

  releaseBusyFlag(&dataBusy);
}
