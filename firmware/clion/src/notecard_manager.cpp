#include "notecard_manager.h"

#include "busy_flags.h"
#include "config.h"
#include "notecard_device.h"
#include "reading_types.h"
#include "secrets.h"
#include "time_sync.h"

NotecardManager notecardManager;

uint32_t notecardRecoveries = 0;
uint32_t notecardRecoveryAttempts = 0;

void NotecardManager::begin() {
  Serial.println("[NC] Initializing Notecard...");

  if (!takeBusyFlag(&i2cBusy, 5000)) {
    Serial.println("[NC] ERROR: I2C busy timeout");
    return;
  }

  notecard.begin();
  notecard.setDebugOutputStream(Serial);

  J* ver = notecard.newRequest("card.version");
  if (ver) {
    J* rsp = notecard.requestAndResponse(ver);
    if (rsp) {
      Serial.println("[NC] Notecard connected");
      notecard.deleteResponse(rsp);
    }
  }

  J* req = notecard.newRequest("hub.set");
  if (req) {
    JAddStringToObject(req, "product", PRODUCT_UID);
    JAddStringToObject(req, "mode", "continuous");
    notecard.sendRequest(req);
  }

  syncTimeBaseLocked();

  releaseBusyFlag(&i2cBusy);
  Serial.println("[NC] Ready");
}

bool NotecardManager::registerDevice() {
  Serial.println("[NC] Registering device...");

  if (!takeBusyFlag(&i2cBusy, 10000)) {
    Serial.println("[NC] ERROR: I2C busy");
    return false;
  }

  J* req = notecard.newRequest("note.add");
  if (!req) {
    releaseBusyFlag(&i2cBusy);
    Serial.println("[NC] ERROR: Request creation failed");
    return false;
  }

  JAddStringToObject(req, "file", "device.qo");
  // Syncs immediately: a single shot at boot, unlike the data note.
  JAddBoolToObject(req, "sync", true);

  J* body = JCreateObject();
  if (!body) {
    JDelete(req);
    releaseBusyFlag(&i2cBusy);
    Serial.println("[NC] ERROR: Body creation failed");
    return false;
  }

  JAddStringToObject(body, "request_type", "create_device");
  JAddStringToObject(body, "url", DEVICES_URL);
  JAddStringToObject(body, "auth_token", AUTH_TOKEN);
  JAddStringToObject(body, "deviceId", DEVICE_ID);
  JAddStringToObject(body, "name", DEVICE_NAME);
  JAddStringToObject(body, "type", DEVICE_TYPE);
  JAddStringToObject(body, "status", "active");
  JAddNumberToObject(body, "timestamp", millis());
  JAddItemToObject(req, "body", body);

  const bool ok = notecard.sendRequest(req);
  releaseBusyFlag(&i2cBusy);

  Serial.printf("[NC] Registration %s\n", ok ? "OK" : "FAILED");
  return ok;
}

void NotecardManager::appendReading(J* readingsArr, int typeCode, float value, int unitOverrideCode) {
  J* tuple = JCreateArray();
  if (!tuple) {
    return;
  }
  JAddItemToArray(tuple, JCreateNumber(typeCode));
  JAddItemToArray(tuple, JCreateNumber(value));
  if (unitOverrideCode >= 0) {
    JAddItemToArray(tuple, JCreateNumber(unitOverrideCode));
  }
  JAddItemToArray(readingsArr, tuple);
}

bool NotecardManager::sendBatchedData(const SensorData* readings, uint8_t count) {
  Serial.printf("[NC] Sending %d batched readings (compact format)...\n", count);

  if (!takeBusyFlag(&i2cBusy, 10000)) {
    Serial.println("[NC] ERROR: I2C busy timeout during send");
    return false;
  }

  J* req = notecard.newRequest("note.add");
  if (!req) {
    Serial.println("[NC] ERROR: Failed to create note.add request");
    releaseBusyFlag(&i2cBusy);
    return false;
  }

  JAddStringToObject(req, "file", "data.qo");
  JAddBoolToObject(req, "sync", NOTECARD_FORCE_SYNC_ON_SEND);

  J* body = JCreateObject();
  J* dArray = JCreateArray();
  J* deviceEntry = JCreateArray();
  J* snapshots = JCreateArray();

  if (!body || !dArray || !deviceEntry || !snapshots) {
    JDelete(body);
    JDelete(dArray);
    JDelete(deviceEntry);
    JDelete(snapshots);
    JDelete(req);
    releaseBusyFlag(&i2cBusy);
    Serial.println("[NC] ERROR: Out of memory building the batch");
    return false;
  }

  JAddItemToArray(deviceEntry, JCreateString(DEVICE_ID));

  for (uint8_t i = 0; i < count; i++) {
    const SensorData& data = readings[i];

    J* snapshot = JCreateArray();
    J* readingsArr = JCreateArray();
    if (!snapshot || !readingsArr) {
      // Dropping one snapshot beats losing the whole batch; the rest still go.
      JDelete(snapshot);
      JDelete(readingsArr);
      continue;
    }

    JAddItemToArray(snapshot, JCreateNumber(data.epochUnix));

    if (data.envValid) {
      appendReading(readingsArr, READING_TEMPERATURE, data.temperature);
      appendReading(readingsArr, READING_HUMIDITY, data.humidity);
      appendReading(readingsArr, READING_PRESSURE, data.pressure);
      appendReading(readingsArr, READING_GAS_RESISTANCE, data.gasResistance);
      appendReading(readingsArr, READING_ALTITUDE, data.altitude);
      // Must accompany the algorithm outputs below; see reading_types.h.
      appendReading(readingsArr, READING_IAQ_ACCURACY, data.iaqAccuracy);
      appendReading(readingsArr, READING_IAQ, data.iaq);
      appendReading(readingsArr, READING_CO2_EQUIVALENT, data.co2Equivalent);
      appendReading(readingsArr, READING_BREATH_VOC_EQUIVALENT, data.breathVocEquivalent);
      appendReading(readingsArr, READING_GAS_PERCENTAGE, data.gasPercentage);
      appendReading(readingsArr, READING_BSEC_STABILIZATION, data.stabilized ? 1 : 0);
      appendReading(readingsArr, READING_BSEC_RUN_IN, data.runInComplete ? 1 : 0);
    }

    if (data.bmv080Valid) {
      appendReading(readingsArr, READING_PM1, data.pm1);
      appendReading(readingsArr, READING_PM25, data.pm25);
      appendReading(readingsArr, READING_PM10, data.pm10);
      appendReading(readingsArr, READING_OBSTRUCTED, data.bmv080Obstructed ? 1 : 0);
    }

    if (data.lightningValid) {
      appendReading(readingsArr, READING_LIGHTNING_STRIKE, data.lightningStrike ? 1 : 0);
      appendReading(readingsArr, READING_LIGHTNING_DISTURBER, data.lightningDisturber ? 1 : 0);
      if (data.lightningStrike) {
        appendReading(readingsArr, READING_LIGHTNING_DISTANCE, data.lightningDistanceKm);
        appendReading(readingsArr, READING_LIGHTNING_ENERGY, (float)data.lightningEnergy);
      }
    }

    JAddItemToArray(snapshot, readingsArr);
    JAddItemToArray(snapshots, snapshot);
  }

  JAddItemToArray(deviceEntry, snapshots);
  JAddItemToArray(dArray, deviceEntry);
  JAddItemToObject(body, "d", dArray);
  JAddItemToObject(req, "body", body);

  Serial.println("[NC] Sending request to Notecard...");
  const bool ok = notecard.sendRequest(req);

  if (ok) {
    consecutiveFailures = 0;
    Serial.printf("[NC] ✓ Successfully sent %d readings\n", count);
  } else {
    consecutiveFailures++;
    Serial.printf("[NC] ✗ Failed to send %d readings (%u in a row)\n",
                 count, consecutiveFailures);
  }

  releaseBusyFlag(&i2cBusy);

  // After the release: recover() re-enters begin(), which takes the flag.
  if (!ok && consecutiveFailures >= NOTECARD_FAILURES_BEFORE_REINIT) {
    recover();
  }

  return ok;
}

/// Never reboots, so the buffered readings survive. Safe on the shared bus: the
/// library's I2C transport is a singleton, so begin() reuses it instead of
/// re-running Wire.begin() underneath the other sensors.
void NotecardManager::recover() {
  notecardRecoveryAttempts++;
  Serial.printf("[NC] %u consecutive send failures - reinitializing...\n", consecutiveFailures);

  begin();

  // begin() reports nothing, so prove the link with a card.time round trip.
  uint32_t epoch;
  if (getEpochNow(&epoch)) {
    notecardRecoveries++;
    Serial.printf("[NC] Reinitialized OK (recovery #%lu)\n", (unsigned long)notecardRecoveries);
  } else {
    Serial.println("[NC] Reinit did not restore the link");
  }

  // Cleared either way, so the next attempt is another
  // NOTECARD_FAILURES_BEFORE_REINIT sends away rather than one per send.
  consecutiveFailures = 0;
}
