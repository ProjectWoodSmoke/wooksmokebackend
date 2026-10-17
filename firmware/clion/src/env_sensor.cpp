#include "env_sensor.h"

#include <Preferences.h>
#include <Wire.h>
#include <bsec2.h>

#include "busy_flags.h"
#include "config.h"

static Bsec2 envSensor;
static Preferences bsecPrefs;

LatestEnvData latestEnv;

bool envSensorReady = false;
bool bsecStateRestored = false;
uint8_t bsecSavedAccuracy = 0;

static uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE] = { 0 };
static uint32_t lastBsecStateSave = 0;

/// Never halts: one flaky sensor must not take down the whole device.
static void checkBsecStatus(const Bsec2& bsec) {
  if (bsec.status < BSEC_OK) {
    Serial.printf("[BSEC] ERROR code: %d\n", bsec.status);
  } else if (bsec.status > BSEC_OK) {
    const char* name;
    switch (bsec.status) {
      case BSEC_W_SU_SAMPLERATEMISMATCH:
        name = "subscribed sample rate != the compiled-in config's rate (harmless here)";
        break;
      case BSEC_W_SC_CALL_TIMING_VIOLATION:
        name = "sensor control called outside its timing window - loop() blocked too long";
        break;
      case BSEC_W_SC_MODEXCEEDULPTIMELIMIT:
        name = "ULP plus not allowed, an ULP measurement is in progress";
        break;
      case BSEC_W_SC_MODINSUFFICIENTWAITTIME:
        name = "ULP plus not allowed, insufficient time since the last one";
        break;
      default:
        name = "see bsec_datatypes.h";
        break;
    }
    Serial.printf("[BSEC] Warning code: %d (%s)\n", bsec.status, name);
  }

  if (bsec.sensor.status < BME68X_OK) {
    Serial.printf("[BSEC] BME68X ERROR code: %d\n", bsec.sensor.status);
  } else if (bsec.sensor.status > BME68X_OK) {
    Serial.printf("[BSEC] BME68X warning code: %d\n", bsec.sensor.status);
  }
}

/// Must run after begin() but before updateSubscription(), matching Bosch's
/// reference order.
static bool restoreBsecState() {
  if (!bsecPrefs.begin(BSEC_NVS_NAMESPACE, /*readOnly=*/true)) {
    Serial.println("[BSEC] No saved state namespace yet (first boot)");
    return false;
  }

  // A mismatched length means a blob from a BSEC version with a different
  // state layout; relearn rather than feed BSEC something it cannot parse.
  const size_t len = bsecPrefs.getBytesLength(BSEC_NVS_STATE_KEY);
  if (len != BSEC_MAX_STATE_BLOB_SIZE) {
    if (len != 0) {
      Serial.printf("[BSEC] Saved state size %u != expected %u, ignoring\n",
                   (unsigned)len, (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
    } else {
      Serial.println("[BSEC] No saved calibration state (first boot)");
    }
    bsecPrefs.end();
    return false;
  }

  bsecPrefs.getBytes(BSEC_NVS_STATE_KEY, bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  bsecSavedAccuracy = bsecPrefs.getUChar(BSEC_NVS_ACC_KEY, 0);
  bsecPrefs.end();

  if (!envSensor.setState(bsecState)) {
    Serial.println("[BSEC] setState() FAILED - continuing with fresh calibration");
    checkBsecStatus(envSensor);
    // The blob was not applied, so nothing is calibrated. Leaving the accuracy
    // read from NVS would suppress the first-calibration save below.
    bsecSavedAccuracy = 0;
    return false;
  }

  // Starts the periodic clock at boot. Left at 0, maybeSaveBsecState() can
  // never fire again this boot: the restored accuracy already rules out the
  // first-calibration trigger.
  lastBsecStateSave = millis();

  Serial.printf("[BSEC] Restored calibration state (was accuracy %d/3)\n", bsecSavedAccuracy);
  return true;
}

static void saveBsecState(uint8_t accuracy) {
  if (!envSensor.getState(bsecState)) {
    Serial.println("[BSEC] getState() FAILED - not saving");
    checkBsecStatus(envSensor);
    return;
  }

  if (!bsecPrefs.begin(BSEC_NVS_NAMESPACE, /*readOnly=*/false)) {
    Serial.println("[BSEC] Could not open NVS for writing");
    return;
  }

  const size_t written = bsecPrefs.putBytes(BSEC_NVS_STATE_KEY, bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  bsecPrefs.putUChar(BSEC_NVS_ACC_KEY, accuracy);
  bsecPrefs.end();

  if (written == BSEC_MAX_STATE_BLOB_SIZE) {
    bsecSavedAccuracy = accuracy;
    lastBsecStateSave = millis();
    Serial.printf("[BSEC] Calibration state saved (accuracy %d/3)\n", accuracy);
  } else {
    Serial.printf("[BSEC] Save incomplete: wrote %u of %u bytes\n",
                 (unsigned)written, (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
  }
}

/// Saves on first reaching full accuracy, then periodically to track drift.
static void maybeSaveBsecState() {
  if (!envSensorReady || !latestEnv.valid) {
    return;
  }

  const uint8_t accuracy = latestEnv.iaqAccuracy;
  if (accuracy < BSEC_MIN_ACCURACY_TO_SAVE) {
    return;
  }

  const bool firstFullCalibration = (bsecSavedAccuracy < BSEC_MIN_ACCURACY_TO_SAVE);
  const bool periodicDue = (lastBsecStateSave != 0) &&
                           (millis() - lastBsecStateSave >= BSEC_STATE_SAVE_INTERVAL);

  if (firstFullCalibration || periodicDue) {
    saveBsecState(accuracy);
  }
}

/// Signature is fixed by the library's callback typedef.
static void newDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
  if (!outputs.nOutputs) {
    return;
  }

  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData output = outputs.output[i];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        latestEnv.temperature = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        latestEnv.humidity = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        // Already hPa; the library converts before feeding the algorithm.
        latestEnv.pressure = output.signal;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        latestEnv.gasResistance = output.signal / 1000.0f;  // Ohms to kOhms
        break;
      case BSEC_OUTPUT_IAQ:
        latestEnv.iaq = output.signal;
        latestEnv.iaqAccuracy = (uint8_t)output.accuracy;
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        latestEnv.co2Equivalent = output.signal;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        latestEnv.breathVocEquivalent = output.signal;
        break;
      case BSEC_OUTPUT_GAS_PERCENTAGE:
        latestEnv.gasPercentage = output.signal;
        break;
      case BSEC_OUTPUT_STABILIZATION_STATUS:
        latestEnv.stabilized = (output.signal != 0.0f);
        break;
      case BSEC_OUTPUT_RUN_IN_STATUS:
        latestEnv.runInComplete = (output.signal != 0.0f);
        break;
      default:
        break;
    }
  }

  latestEnv.valid = true;

  Serial.printf("[BSEC] IAQ:%.1f (acc %d) CO2eq:%.0fppm bVOC:%.2fppm Gas%%:%.1f T:%.1f H:%.1f Gas:%.1fkΩ stab:%d runIn:%d\n",
               latestEnv.iaq, latestEnv.iaqAccuracy, latestEnv.co2Equivalent,
               latestEnv.breathVocEquivalent, latestEnv.gasPercentage,
               latestEnv.temperature, latestEnv.humidity, latestEnv.gasResistance,
               latestEnv.stabilized ? 1 : 0, latestEnv.runInComplete ? 1 : 0);

  // Here rather than loop(), so state is captured right after a coherent set
  // of outputs.
  maybeSaveBsecState();
}

bool envSensorBegin() {
  bsecSensor sensorList[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_GAS_PERCENTAGE,
    BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS,
  };

  bool bmeReady = envSensor.begin(BME68X_I2C_ADDR_HIGH, Wire);
  if (!bmeReady) {
    bmeReady = envSensor.begin(BME68X_I2C_ADDR_LOW, Wire);
    if (bmeReady) {
      Serial.println("[SETUP] BME688 found at 0x76");
    }
  } else {
    Serial.println("[SETUP] BME688 found at 0x77");
  }

  if (bmeReady) {
    envSensor.setTemperatureOffset(BSEC_TEMP_OFFSET);

    // Must precede updateSubscription(), matching Bosch's reference order.
    bsecStateRestored = restoreBsecState();

    const bool subscribed =
        envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP);

    // Returns false on ANY non-zero status, warnings included; only a
    // negative status is a real error. Treating warning 14 (sample-rate
    // mismatch) as fatal would disable the BME688 for nothing.
    if (!subscribed && envSensor.status < BSEC_OK) {
      Serial.println("[SETUP] BSEC subscription FAILED");
      checkBsecStatus(envSensor);
      bmeReady = false;
    } else {
      if (!subscribed) {
        Serial.println("[SETUP] BSEC subscription returned a warning - continuing anyway");
        checkBsecStatus(envSensor);
      }
      envSensor.attachCallback(newDataCallback);
      Serial.printf("[SETUP] BSEC library version %d.%d.%d.%d\n",
                   envSensor.version.major, envSensor.version.minor,
                   envSensor.version.major_bugfix, envSensor.version.minor_bugfix);
      Serial.println("[SETUP] BME688/BSEC2 configured successfully");
      if (bsecStateRestored) {
        Serial.println("[SETUP] Calibration restored - IAQ accuracy should recover quickly.");
      } else {
        Serial.println("[SETUP] NOTE: no saved calibration. The sensor needs conditioning");
        Serial.println("[SETUP]       time (hours; ideally run 24h once) before IAQ accuracy");
        Serial.println("[SETUP]       reaches 3/3. Calibration is saved automatically once it");
        Serial.println("[SETUP]       does, so later reboots will not repeat this wait.");
      }
    }
  } else {
    Serial.println("[SETUP] BME688 FAILED - check wiring and I2C address");
  }

  envSensorReady = bmeReady;
  return envSensorReady;
}

void envSensorService() {
  if (!envSensorReady) {
    return;
  }

  if (!takeBusyFlag(&i2cBusy, 50)) {
    return;
  }

  if (!envSensor.run()) {
    checkBsecStatus(envSensor);
  }

  releaseBusyFlag(&i2cBusy);
}
