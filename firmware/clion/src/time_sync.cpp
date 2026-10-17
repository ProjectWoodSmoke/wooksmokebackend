#include "time_sync.h"

#include "busy_flags.h"
#include "config.h"
#include "notecard_device.h"

static uint32_t epochAtSync = 0;
static uint32_t millisAtSync = 0;
static bool haveTimeBase = false;

/// Caller must already hold i2cBusy.
static bool readNotecardEpoch(uint32_t* epochOut) {
  J* req = notecard.newRequest("card.time");
  if (!req) {
    Serial.println("[TIME] ERROR: Failed to create request");
    return false;
  }

  J* rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println("[TIME] ERROR: No response from Notecard");
    return false;
  }

  const double unixTime = JGetNumber(rsp, "time");
  notecard.deleteResponse(rsp);

  if (unixTime == 0) {
    Serial.println("[TIME] ERROR: Unix time is 0 (Notecard has no time fix yet)");
    return false;
  }

  *epochOut = (uint32_t)unixTime;
  return true;
}

bool syncTimeBaseLocked() {
  uint32_t epoch;
  if (!readNotecardEpoch(&epoch)) {
    return false;
  }
  epochAtSync = epoch;
  millisAtSync = millis();
  haveTimeBase = true;
  Serial.printf("[TIME] Synced: epoch %lu\n", (unsigned long)epoch);
  return true;
}

bool timeBaseNeedsSync() {
  return !haveTimeBase || (millis() - millisAtSync >= TIME_RESYNC_INTERVAL);
}

bool currentEpoch(uint32_t* epochOut) {
  if (!haveTimeBase) {
    return false;
  }
  *epochOut = epochAtSync + ((millis() - millisAtSync) / 1000UL);
  return true;
}

bool getEpochNow(uint32_t* epochOut) {
  if (timeBaseNeedsSync()) {
    if (takeBusyFlag(&i2cBusy, 5000)) {
      syncTimeBaseLocked();
      releaseBusyFlag(&i2cBusy);
    } else {
      Serial.println("[TIME] I2C busy, using cached time base");
    }
  }
  return currentEpoch(epochOut);
}
