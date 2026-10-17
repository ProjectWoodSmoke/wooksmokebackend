#include <Arduino.h>
#include <bsec2.h>
#include "SparkFun_BMV080_Arduino_Library.h"
#include <SPI.h>
#include "SparkFun_AS3935.h"
#include <Wire.h>
#include <Notecard.h>
#include <Preferences.h>

#ifndef ARRAY_LEN
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#endif

// NOTE: AUTH_TOKEN below is a live secret key (sk_...) committed in plaintext.
// If this file is or will be in version control (even a private repo),
// consider moving it to a build-time secret / .env-style include that's
// gitignored, since anyone with read access to the repo can use it as-is.
#define PRODUCT_UID "com.gmail.joshkroker:hsd_dev"
static constexpr char AUTH_TOKEN[] = "sk_6dfcbad24491451052a0ef57ba1180b165c3abb24f8076d317e224800343cb10";
static constexpr char DEVICE_ID[] = "sensor-001";
static constexpr char DEVICE_NAME[] = "Air Quality Monitor 1";
static constexpr char DEVICE_TYPE[] = "air-quality-monitor";
static constexpr char DEVICES_URL[] = "https://api.ubitechino.com/devices";

#define I2C_SDA A4
#define I2C_SCL A5

// AS3935 lightning detector (SparkFun MA5532-AE breakout), wired via SPI.
// Pin choices below match the Arduino Nano ESP32's default hardware SPI:
//   SCK = D13, MISO/CIPO = D12, MOSI/COPI = D11 (fixed by the board's SPI bus)
//   CS  = D10 (the board's default SS pin, but any free GPIO would work)
//   IRQ = D9  (any free GPIO - there's no dedicated IRQ pin on this bus)
#define LIGHTNING_CS_PIN   D10
#define LIGHTNING_IRQ_PIN  D9
#define LIGHTNING_SPI_SPEED 2000000  // 2 MHz - safely under the AS3935's ~10MHz max

// The SparkFun_AS3935 library does NOT define these event-code constants
// itself - every official example sketch defines them locally, so we do
// the same here. (INDOOR/OUTDOOR are already provided by the library.)
#define LIGHTNING_INT  0x08
#define DISTURBER_INT  0x04
#define NOISE_INT      0x01

const uint32_t SERIAL_SPEED = 115200;
const uint8_t BMV080_ADDR = 0x57;
const uint32_t SENSOR_READ_INTERVAL = 1000;
const uint32_t DATA_SEND_INTERVAL = 30000;
// Must hold at least DATA_SEND_INTERVAL / SENSOR_READ_INTERVAL entries (30),
// or the ring buffer overwrites readings before they are ever sent. Sized
// with headroom so a send that gets deferred doesn't immediately drop data.
const uint8_t MAX_READINGS_BUFFER = 40;

// Self-heating temperature offset for BSEC's compensated temp/humidity
// outputs. This depends on your enclosure/airflow - 2.0C is a reasonable
// starting point, but re-tune it once you can compare against a reference
// thermometer sitting next to (not touching) your enclosure.
#define BSEC_TEMP_OFFSET 2.0f

// Whether the periodic data.qo note forces an immediate Notecard sync.
// false keeps the I2C request short; see the note at the JAddBoolToObject
// call in sendBatchedData(). Device registration still syncs immediately -
// that one is a single shot at boot, not a repeating 30s cost.
#define NOTECARD_FORCE_SYNC_ON_SEND false

// ---------------------------------------------------------------------------
// BSEC calibration-state persistence.
//
// This is what makes the AI gas sensing actually usable in the field. BSEC
// learns a baseline for the specific BME688 it's running on, and that learning
// is what moves iaq_accuracy from 0 (meaningless output) up to 3 (fully
// calibrated). It lives in RAM inside the BSEC algorithm - so without saving
// it, EVERY reboot throws away the calibration and restarts the multi-hour
// warm-up from scratch.
//
// We snapshot the state blob into NVS and restore it on boot. Saves are rate
// limited (NVS has finite write endurance) and only happen once the algorithm
// is confident, since persisting an uncalibrated baseline is worse than
// persisting nothing.
// ---------------------------------------------------------------------------
#define BSEC_NVS_NAMESPACE "bsec"
#define BSEC_NVS_STATE_KEY "state"
#define BSEC_NVS_ACC_KEY   "acc"

// Bosch's own examples use a 6-hour save period. Long enough to be gentle on
// flash, short enough that an unexpected power cut loses little learning.
const uint32_t BSEC_STATE_SAVE_INTERVAL = 6UL * 60UL * 60UL * 1000UL;

// Only persist state once BSEC reports this accuracy or better.
// 3 = fully calibrated. Saving at 1-2 would bake in a half-learned baseline.
const uint8_t BSEC_MIN_ACCURACY_TO_SAVE = 3;

// ---------------------------------------------------------------------------
// Compact reading-type codes.
// MUST stay in sync with config/readingTypes.js on the server.
// Append-only: never reorder or remove an entry, only add new codes at the
// end. 0-3 (temperature/humidity/smoke/co) are the server's pre-existing,
// documented codes — this device does not use smoke or co, but those slots
// are reserved and must not be reused for anything else.
// ---------------------------------------------------------------------------
#define READING_TEMPERATURE        0
#define READING_HUMIDITY           1
// 2 = smoke, 3 = co (reserved on the server, unused by this device)
#define READING_PRESSURE           4  // server default unit is hPa — no override needed
#define READING_GAS_RESISTANCE     5
#define READING_ALTITUDE           6
#define READING_PM1                7
#define READING_PM25               8
#define READING_PM10               9
#define READING_OBSTRUCTED         10
#define READING_LIGHTNING_DISTANCE 11  // km, only meaningful when READING_LIGHTNING_STRIKE is 1
#define READING_LIGHTNING_ENERGY   12  // unitless relative "energy" value from the AS3935
#define READING_LIGHTNING_STRIKE   13  // 0/1 - an actual lightning strike was detected
#define READING_LIGHTNING_DISTURBER 14 // 0/1 - a non-lightning disturber event was detected
// BSEC2/AI-derived codes. The server gates alerting on 15/17/18/19 until
// READING_IAQ_ACCURACY is >= 1, so it is important that every snapshot
// carrying those values also carries the accuracy code.
#define READING_IAQ                 15 // Bosch Index for Air Quality (0-500, lower is better)
#define READING_IAQ_ACCURACY        16 // 0=uncalibrated .. 3=fully calibrated
#define READING_CO2_EQUIVALENT      17 // ppm, estimated from VOCs (not a direct CO2 measurement)
#define READING_BREATH_VOC_EQUIVALENT 18 // ppm, estimated breath-relevant VOC equivalent
#define READING_GAS_PERCENTAGE      19 // 0-100%, relative air quality/contamination percentage
#define READING_BSEC_STABILIZATION  20 // 0/1 - heater/algorithm stabilization complete
#define READING_BSEC_RUN_IN         21 // 0/1 - initial run-in period complete

SparkFunBMV080 bmv080;
SparkFun_AS3935 lightning;
Notecard notecard;
Bsec2 envSensor;
Preferences bsecPrefs;

volatile bool i2cBusy = false;
volatile bool dataBusy = false;

bool lightningSensorReady = false;
bool envSensorReady = false;

uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE] = { 0 };
uint32_t lastBsecStateSave = 0;
bool bsecStateRestored = false;
uint8_t bsecSavedAccuracy = 0;

struct SensorData {
  // BME688/BSEC2 group
  bool envValid;
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
  bool stabilized;
  bool runInComplete;

  // BMV080 group
  bool bmv080Valid;
  bool bmv080Obstructed;
  float pm1;
  float pm25;
  float pm10;

  // AS3935 group
  bool lightningValid;
  bool lightningStrike;
  bool lightningDisturber;
  float lightningDistanceKm;
  uint32_t lightningEnergy;

  uint32_t epochUnix;  // seconds since epoch, from Notecard card.time
};

// Latest values produced by the BSEC2 callback. Populated any time
// envSensor.run() decides a new BSEC cycle has completed - which, at
// BSEC_SAMPLE_RATE_LP, is roughly every 3s. readSensors() just snapshots
// whatever is here on its own 1s cadence, so a given reading may reuse the
// same BSEC output more than once between BSEC cycles.
struct LatestEnvData {
  bool valid = false;
  float temperature = 0;
  float humidity = 0;
  float pressure = 0;
  float gasResistance = 0;
  float iaq = 0;
  uint8_t iaqAccuracy = 0;
  float co2Equivalent = 0;
  float breathVocEquivalent = 0;
  float gasPercentage = 0;
  bool stabilized = false;
  bool runInComplete = false;
};

LatestEnvData latestEnv;

// Latest particulate values from the BMV080.
//
// bmv080.readSensor() returns _dataAvailable - true ONLY on the call where
// the driver's data-ready callback happened to fire, false otherwise. It is
// NOT an error indicator; SparkFun's own Example_01 polls it at 10Hz inside
// an `if` and silently ignores every false. Polling it once per second (the
// old behaviour here) samples at almost exactly the sensor's own output rate,
// so the ready window gets aliased over and reads appear to "fail" forever.
// serviceBmv080() polls it every loop() iteration instead and caches the last
// good sample for readSensors() to pick up.
struct LatestPmData {
  bool valid = false;
  float pm1 = 0;
  float pm25 = 0;
  float pm10 = 0;
  bool obstructed = false;
  uint32_t lastUpdate = 0;
};

LatestPmData latestPm;

// ---------------------------------------------------------------------------
// BMV080 measurement mode.
//
// 0            = continuous. The sensor measures without pause and emits a
//                result about once a second (our own logs: 13 samples in 13s).
//                This is the maximum sustained-power mode - the optical front
//                end never gets to rest.
// >0 (seconds) = duty cycling. The sensor measures, reports, then sleeps, on
//                this period. SparkFun's Example_02 uses 20.
//
// Note this is the SENSOR's measurement mode, not how the host retrieves data.
// Retrieval is a separate axis and both of SparkFun's examples poll with
// readSensor() exactly as serviceBmv080() does - continuous mode does not push
// data at us, and it is not a flood: ~1 sample/second either way.
//
// The reason to reach for duty cycling here is power, not data rate: with the
// failure signature pointing at supply stability, this cuts both the average
// draw and the fraction of time the sensor is powered up and vulnerable.
// It reduces exposure; it does not repair a marginal rail.
#define BMV080_DUTY_CYCLE_SECONDS 0

#if BMV080_DUTY_CYCLE_SECONDS > 0
const uint32_t PM_NOMINAL_PERIOD_MS = BMV080_DUTY_CYCLE_SECONDS * 1000UL;
#else
const uint32_t PM_NOMINAL_PERIOD_MS = 1000UL;  // continuous emits ~1Hz
#endif

// Everything below scales off the nominal output period, so switching modes
// does not silently leave the watchdog firing mid-sleep or mark good data
// stale. In continuous mode these evaluate to 10s / 30s / 70s.

// How long a cached particulate sample stays usable before a snapshot stops
// carrying PM values.
const uint32_t PM_SAMPLE_MAX_AGE =
    (PM_NOMINAL_PERIOD_MS * 3UL > 10000UL) ? PM_NOMINAL_PERIOD_MS * 3UL : 10000UL;

// How long the BMV080 may go without producing a sample before we tear the
// driver down and re-open it. Observed failure mode: the sensor streams
// normally at 1Hz for a while and then stops permanently while the BME688 and
// Notecard keep transacting fine on the same bus - the I2C bus is healthy and
// only this device is wedged. Nothing in the SparkFun/Bosch API reports that.
//
// Doubles as the retry interval when the sensor is missing at boot: a failed
// start also stamps bmv080LastActivity, so recovery re-attempts on this cadence.
const uint32_t PM_STALL_TIMEOUT = PM_SAMPLE_MAX_AGE + 20000UL;

// Separate, longer limit until the FIRST sample of a given start. The BMV080
// does not emit immediately after setMode() - in practice the first sample
// lands tens of seconds after boot - so reusing PM_STALL_TIMEOUT here would
// tear the driver down mid-warm-up and loop forever without ever getting data.
const uint32_t PM_FIRST_SAMPLE_TIMEOUT = PM_STALL_TIMEOUT + 40000UL;

// GPIO gating power to the BMV080, for the case the logs actually show: the
// part still ACKs its I2C address but its sensor core is dead, and close() ->
// init() -> setMode() cannot revive it. Only removing power does.
//
// -1 disables this and the watchdog does the soft reinit alone. To enable,
// set it to a free pin driving a high-side load switch or P-MOSFET with
// active-HIGH enable - an ESP32 pin cannot source the sensor's peak current
// directly, so do not wire the sensor's supply straight to a GPIO.
#define BMV080_POWER_PIN -1
const uint32_t BMV080_POWER_OFF_MS = 300;  // hold unpowered long enough for the rail to fully collapse

bool latestPmIsFresh() {
  return latestPm.valid && (millis() - latestPm.lastUpdate <= PM_SAMPLE_MAX_AGE);
}

SensorData readingsBuffer[MAX_READINGS_BUFFER];
uint8_t bufferIndex = 0;
uint8_t bufferCount = 0;
volatile bool hasDataToSend = false;

float altitude(const float press, const float seaLevel = 1013.25) {
  return 44330.0 * (1.0 - pow(press / seaLevel, 0.1903));
}

// Reentrancy guards for the shared I2C bus and the readings buffer.
//
// NOTE: every caller of these runs from loop() - there are no ISRs and no
// extra FreeRTOS tasks in this sketch - so in practice the flag is always
// free when tested and takeBusyFlag() never waits or times out. They are kept
// as cheap insurance in case anything here ever moves to an interrupt or a
// second task; do not read a "bus was busy" log line as an explanation for a
// sensor problem, because it cannot currently happen.
bool takeBusyFlag(volatile bool* flag, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (*flag) {
    if (millis() - start > timeoutMs) {
      return false;
    }
    delay(10);
  }
  *flag = true;
  return true;
}

void releaseBusyFlag(volatile bool* flag) {
  *flag = false;
}

// Prints BSEC/BME68x status and warning codes. Unlike Bosch's stock example,
// this does NOT halt on error - it mirrors how the rest of this sketch
// degrades gracefully (see lightningSensorReady) so one flaky sensor doesn't
// take down the whole device.
void checkBsecStatus(Bsec2 bsec) {
  if (bsec.status < BSEC_OK) {
    Serial.printf("[BSEC] ERROR code: %d\n", bsec.status);
  } else if (bsec.status > BSEC_OK) {
    // Name the common warnings. A bare number sent us looking things up by
    // hand; code 100 in particular is not a sensor fault at all.
    const char *name;
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

// ---------------------------------------------------------------------------
// BSEC state save/restore
// ---------------------------------------------------------------------------

// Restores a previously-saved BSEC calibration blob from NVS. Call after
// begin() but BEFORE updateSubscription(), matching Bosch's reference order.
bool restoreBsecState() {
  if (!bsecPrefs.begin(BSEC_NVS_NAMESPACE, /*readOnly=*/true)) {
    Serial.println("[BSEC] No saved state namespace yet (first boot)");
    return false;
  }

  size_t len = bsecPrefs.getBytesLength(BSEC_NVS_STATE_KEY);
  if (len != BSEC_MAX_STATE_BLOB_SIZE) {
    // Either nothing saved yet, or the blob was written by a different BSEC
    // version whose state layout doesn't match. Either way, discard it and
    // relearn rather than feeding BSEC a blob it can't parse.
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
    return false;
  }

  Serial.printf("[BSEC] Restored calibration state (was accuracy %d/3)\n", bsecSavedAccuracy);
  return true;
}

// Persists the current BSEC calibration blob to NVS.
void saveBsecState(uint8_t accuracy) {
  if (!envSensor.getState(bsecState)) {
    Serial.println("[BSEC] getState() FAILED - not saving");
    checkBsecStatus(envSensor);
    return;
  }

  if (!bsecPrefs.begin(BSEC_NVS_NAMESPACE, /*readOnly=*/false)) {
    Serial.println("[BSEC] Could not open NVS for writing");
    return;
  }

  size_t written = bsecPrefs.putBytes(BSEC_NVS_STATE_KEY, bsecState, BSEC_MAX_STATE_BLOB_SIZE);
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

// Decides whether it's time to persist calibration. Two triggers:
//   1. The first time we reach full accuracy after a cold start - capture that
//      hard-won calibration immediately rather than waiting up to 6 hours.
//   2. Periodically thereafter, so the saved baseline keeps tracking drift.
void maybeSaveBsecState() {
  if (!envSensorReady || !latestEnv.valid) {
    return;
  }

  uint8_t accuracy = latestEnv.iaqAccuracy;
  if (accuracy < BSEC_MIN_ACCURACY_TO_SAVE) {
    return;
  }

  bool firstFullCalibration = (bsecSavedAccuracy < BSEC_MIN_ACCURACY_TO_SAVE);
  bool periodicDue = (lastBsecStateSave != 0) &&
                     (millis() - lastBsecStateSave >= BSEC_STATE_SAVE_INTERVAL);

  if (firstFullCalibration || periodicDue) {
    saveBsecState(accuracy);
  }
}

// Called by the BSEC2 library whenever it finishes processing a new set of
// outputs. Just stashes them in latestEnv - readSensors() picks them up on
// its own timer.
void newDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
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
        // Already in hPa. The BSEC2 Arduino library converts Pa -> hPa itself
        // before feeding the value to the algorithm (bsec2.cpp: "Convert
        // sensor raw pressure unit from pascal to hecto pascal",
        // data.pressure *= 0.01f), so dividing by 100 again here produced the
        // physically impossible ~9.6 hPa readings - and, via altitude(),
        // a bogus ~26,000 m altitude on every snapshot sent to the server.
        latestEnv.pressure = output.signal;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        latestEnv.gasResistance = output.signal / 1000.0f;  // Ohms -> kOhms
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

  // Persisting here (rather than in loop()) means we only ever snapshot state
  // right after BSEC has produced a coherent set of outputs.
  maybeSaveBsecState();
}

// Drives the BSEC2 state machine. Must be called very frequently (every
// loop() iteration) - NOT gated to SENSOR_READ_INTERVAL - because BSEC
// manages the BME688's internal heater/sample timing itself and needs
// regular servicing to stay on schedule.
//
// It takes i2cBusy for consistency with the other bus users, but see the note
// on takeBusyFlag(): nothing here runs off loop(), so the flag is always free
// and this never actually waits. A slow Notecard call delays BSEC by blocking
// loop() itself, not by holding the flag.
void serviceBsec() {
  if (!envSensorReady) {
    return;
  }

  if (!takeBusyFlag(&i2cBusy, 50)) {
    return;  // bus busy this iteration, try again next loop()
  }

  if (!envSensor.run()) {
    checkBsecStatus(envSensor);
  }

  releaseBusyFlag(&i2cBusy);
}

uint32_t bmv080PollCount = 0;
uint32_t bmv080SampleCount = 0;
uint32_t bmv080Recoveries = 0;         // reinit attempts that SUCCEEDED
uint32_t bmv080RecoveryAttempts = 0;   // reinit attempts, successful or not

// Most events the driver is allowed to hand us in one serviceBmv080() call.
// The BMV080 buffers events and Bosch's own status codes call out falling
// behind as a distinct failure: E_BMV080_WARNING_FIFO_EVENTS_OVERFLOW (4) and
// E_BMV080_WARNING_FIFO_HW_BUFFER_SIZE (209), both hinted with "Call
// bmv080_serve_interrupt more frequently, or switch from polling to interrupt
// based use". SparkFun's Example_03 drains in a do/while for the same reason,
// so one readSensor() per loop() iteration is not enough - drain instead.
// Bounded so a misbehaving sensor can't spin here forever.
const uint8_t BMV080_MAX_DRAIN = 16;

// millis() of the last thing that counts as the sensor being alive: either a
// delivered sample or a (re)start attempt. Drives the stall watchdog.
uint32_t bmv080LastActivity = 0;

bool bmv080Opened = false;     // a successful init() owns a driver handle
bool bmv080Measuring = false;  // setMode(CONTINUOUS) succeeded

// Whether the current start has yielded a sample yet. Reset by every
// (re)start, because the warm-up applies after a recovery too - not just at
// boot - and the watchdog must give that warm-up room before firing again.
bool bmv080GotSampleSinceStart = false;

uint32_t bmv080StallLimit() {
  return bmv080GotSampleSinceStart ? PM_STALL_TIMEOUT : PM_FIRST_SAMPLE_TIMEOUT;
}

// Brings the BMV080 from "nothing" to "streaming in continuous mode".
// Used both at boot and by the stall watchdog, so there is exactly one
// start sequence to keep correct.
//
// close() is only called when we actually own a handle - calling it against a
// NULL handle is not something the Bosch blob promises to tolerate. Note that
// an init() which fails *after* its internal open() succeeded will leak that
// handle; there is no public API to detect that case, and in practice it
// means the part is gone anyway.
bool startBmv080() {
  if (bmv080Opened) {
    bmv080.close();
    bmv080Opened = false;
    delay(50);
  }

  bmv080Measuring = false;
  bmv080GotSampleSinceStart = false;
  bmv080LastActivity = millis();  // stamped up front so every failure path backs off

#if BMV080_POWER_PIN >= 0
  // Hard power cycle before trying to talk to it again.
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
  bmv080Opened = true;

  // If the mode fails to start, readSensor() can never report data no matter
  // how often it is polled - so this separates "not measuring" from a
  // retrieval-rate problem.
#if BMV080_DUTY_CYCLE_SECONDS > 0
  // Order matters: the period is a sensor parameter, and the Bosch API rejects
  // parameter writes once a measurement is running with
  // E_BMV080_ERROR_PARAM_LOCKED (179). Set it before setMode(), as Example_02
  // does, not after.
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

  bmv080Measuring = true;
  bmv080LastActivity = millis();
  return true;
}

// Called by the watchdog below once the sensor has gone quiet for too long.
// Probes with a real I2C round trip first, because that is the one piece of
// information readSensor() cannot give us (see the heartbeat note), and it
// tells you which physical fault to chase.
void recoverBmv080() {
  bmv080RecoveryAttempts++;

  // Two separate questions, which the previous single "answers on I2C" probe
  // wrongly collapsed into one:
  //   acked  - does the part still ACK its address? Bare I2C ping via
  //            isConnected(); touches no registers and needs no driver handle.
  //   regsOk - does a real register read through the Bosch driver work?
  //
  // "ACK yes / regs no" means the part is powered and present on the bus but
  // its sensor core is wedged. That is NOT the same as "gone", and the old
  // wording ("does NOT answer on I2C") implied the wrong fault entirely.
  //
  // regsOk needs a live driver handle, so it only means anything while
  // bmv080Opened is true. After a failed reinit there is no handle and ID()
  // returns false whatever the hardware is doing - reported as "n/a".
  bool acked = bmv080.isConnected();
  bool regsOk = false;
  if (bmv080Opened) {
    char id[13] = { 0 };
    regsOk = bmv080.ID(id);
  }

  Serial.printf("[BMV080] STALLED - no sample for %lus. Address ACK: %s, register read: %s\n",
               (unsigned long)((millis() - bmv080LastActivity) / 1000),
               acked ? "YES" : "NO",
               bmv080Opened ? (regsOk ? "YES" : "NO") : "n/a (no handle)");

  if (!acked) {
    Serial.println("[BMV080]   -> not on the bus at all: unpowered, or a wiring/pull-up fault.");
  } else if (bmv080Opened && !regsOk) {
    Serial.println("[BMV080]   -> powered and ACKing, but the sensor core is not responding.");
    Serial.println("[BMV080]      Bosch files this pattern under \"check that power provided to");
    Serial.println("[BMV080]      BMV080 is sufficient & stable\". A soft reinit rarely clears it;");
    Serial.println("[BMV080]      see BMV080_POWER_PIN for the hard power-cycle path.");
  }

  if (startBmv080()) {
    bmv080Recoveries++;
    Serial.printf("[BMV080] Reinitialized OK (recovery #%lu)\n",
                 (unsigned long)bmv080Recoveries);
  } else {
    Serial.printf("[BMV080] Reinit failed - retrying in %lus\n",
                 (unsigned long)(bmv080StallLimit() / 1000));
  }
}

// Pumps the BMV080 driver's service routine. Like serviceBsec(), this must
// run every loop() iteration rather than on the 1s sensor cadence - see the
// aliasing note on LatestPmData - and it also runs the stall watchdog.
void serviceBmv080() {
  if (!takeBusyFlag(&i2cBusy, 50)) {
    return;  // see takeBusyFlag(): cannot currently happen
  }

  bmv080PollCount++;

  // Drain, don't take one sample per loop() - see BMV080_MAX_DRAIN.
  uint8_t drained = 0;
  while (drained < BMV080_MAX_DRAIN && bmv080.readSensor()) {
    latestPm.pm1 = bmv080.PM1();
    latestPm.pm25 = bmv080.PM25();
    latestPm.pm10 = bmv080.PM10();
    latestPm.obstructed = bmv080.isObstructed();
    latestPm.valid = true;
    latestPm.lastUpdate = millis();
    bmv080LastActivity = latestPm.lastUpdate;
    bmv080GotSampleSinceStart = true;
    bmv080SampleCount++;
    drained++;
  }

  if (drained == 0 && millis() - bmv080LastActivity > bmv080StallLimit()) {
    recoverBmv080();
  }

  releaseBusyFlag(&i2cBusy);

  // readSensor() can't tell us WHY it returned false: it returns false both
  // when the driver's service routine errored AND when it ran fine but no
  // data-ready callback fired. The library's sensorServiceRoutine() is
  // private, so those two are indistinguishable from out here - which is why
  // recoverBmv080() probes with the public ID() call instead.
  //
  // Poll/sample counts separate the cases that matter: polls climbing while
  // samples stay frozen means the sensor has stopped measuring. Polls are
  // simply the loop() rate; they are never limited by lock contention.
  static unsigned long lastPmHeartbeat = 0;
  if (millis() - lastPmHeartbeat > 10000) {
    char age[40];
    if (latestPm.valid) {
      snprintf(age, sizeof(age), "last sample %lus ago",
               (unsigned long)((millis() - latestPm.lastUpdate) / 1000));
    } else {
      snprintf(age, sizeof(age), "no sample since boot");
    }
    // recoveries is ok/attempted. Reporting only successes made a reinit that
    // kept failing look identical to one that never ran, which is exactly the
    // state "recoveries:0 measuring:NO" was hiding.
    Serial.printf("[BMV080] polls:%lu samples:%lu recoveries:%lu/%lu measuring:%s (%s)\n",
                 (unsigned long)bmv080PollCount, (unsigned long)bmv080SampleCount,
                 (unsigned long)bmv080Recoveries, (unsigned long)bmv080RecoveryAttempts,
                 bmv080Measuring ? "YES" : "NO", age);
    lastPmHeartbeat = millis();
  }
}

// ---------------------------------------------------------------------------
// Timekeeping.
//
// The Notecard's card.time request is an I2C round trip that can take
// hundreds of milliseconds. Previously this ran on every 1s sensor read,
// which meant the I2C bus was tied up a large fraction of the time and
// serviceBsec() (50ms timeout) was frequently starved - bad, because BSEC has
// to hit the heater timing it schedules for itself.
//
// Instead we sync the Notecard clock occasionally and interpolate with
// millis() in between. millis() drift over a few minutes is far below the 1s
// resolution the server stores anyway.
// ---------------------------------------------------------------------------
const uint32_t TIME_RESYNC_INTERVAL = 5UL * 60UL * 1000UL;  // 5 minutes

uint32_t epochAtSync = 0;      // Notecard epoch captured at the last sync
uint32_t millisAtSync = 0;     // millis() at that same instant
bool haveTimeBase = false;

// Get Unix epoch seconds from Notecard - assumes i2cBusy is already held by caller.
bool getEpochFromNotecard_Internal(uint32_t* epochOut) {
  J *req = notecard.newRequest("card.time");
  if (!req) {
    Serial.println("[TIME] ERROR: Failed to create request");
    return false;
  }

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println("[TIME] ERROR: No response from Notecard");
    return false;
  }

  double unixTime = JGetNumber(rsp, "time");
  notecard.deleteResponse(rsp);

  if (unixTime == 0) {
    Serial.println("[TIME] ERROR: Unix time is 0 (Notecard has no time fix yet)");
    return false;
  }

  *epochOut = (uint32_t)unixTime;
  return true;
}

// Refreshes the millis()->epoch mapping. Assumes i2cBusy is held by caller.
bool syncTimeBase_Internal() {
  uint32_t epoch;
  if (!getEpochFromNotecard_Internal(&epoch)) {
    return false;
  }
  epochAtSync = epoch;
  millisAtSync = millis();
  haveTimeBase = true;
  Serial.printf("[TIME] Synced: epoch %lu\n", (unsigned long)epoch);
  return true;
}

// True when the cached time base is stale (or absent) and should be refreshed.
bool timeBaseNeedsSync() {
  return !haveTimeBase || (millis() - millisAtSync >= TIME_RESYNC_INTERVAL);
}

// Current epoch seconds interpolated from the cached time base.
// Returns false until the first successful Notecard sync.
bool currentEpoch(uint32_t* epochOut) {
  if (!haveTimeBase) {
    return false;
  }
  *epochOut = epochAtSync + ((millis() - millisAtSync) / 1000UL);
  return true;
}

// Takes i2cBusy itself, syncs if stale, and returns the current epoch.
bool getEpochNow(uint32_t* epochOut) {
  if (timeBaseNeedsSync()) {
    if (takeBusyFlag(&i2cBusy, 5000)) {
      syncTimeBase_Internal();
      releaseBusyFlag(&i2cBusy);
    } else {
      Serial.println("[TIME] I2C busy, using cached time base");
    }
  }
  return currentEpoch(epochOut);
}

class NotecardManager {
public:
  void begin() {
    Serial.println("[NC] Initializing Notecard...");

    if (!takeBusyFlag(&i2cBusy, 5000)) {
      Serial.println("[NC] ERROR: I2C busy timeout");
      return;
    }

    notecard.begin();
    notecard.setDebugOutputStream(Serial);

    J *ver = notecard.newRequest("card.version");
    if (ver) {
      J *rsp = notecard.requestAndResponse(ver);
      if (rsp) {
        Serial.println("[NC] Notecard connected");
        notecard.deleteResponse(rsp);
      }
    }

    J *req = notecard.newRequest("hub.set");
    if (req) {
      JAddStringToObject(req, "product", PRODUCT_UID);
      JAddStringToObject(req, "mode", "continuous");
      notecard.sendRequest(req);
    }

    // Establish the initial time base while we already hold the bus.
    syncTimeBase_Internal();

    releaseBusyFlag(&i2cBusy);
    Serial.println("[NC] Ready");
  }

  bool registerDevice() {
    Serial.println("[NC] Registering device...");

    if (!takeBusyFlag(&i2cBusy, 10000)) {
      Serial.println("[NC] ERROR: I2C busy");
      return false;
    }

    J *req = notecard.newRequest("note.add");
    if (!req) {
      releaseBusyFlag(&i2cBusy);
      Serial.println("[NC] ERROR: Request creation failed");
      return false;
    }

    JAddStringToObject(req, "file", "device.qo");
    JAddBoolToObject(req, "sync", true);

    J *body = JCreateObject();
    if (!body) {
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

    bool ok = notecard.sendRequest(req);
    releaseBusyFlag(&i2cBusy);

    Serial.printf("[NC] Registration %s\n", ok ? "OK" : "FAILED");
    return ok;
  }

  // Builds one [typeCode, value] or [typeCode, value, unitOverrideCode] tuple
  // and appends it to readingsArr.
  void appendReading(J* readingsArr, int typeCode, float value, int unitOverrideCode = -1) {
    J *tuple = JCreateArray();
    JAddItemToArray(tuple, JCreateNumber(typeCode));
    JAddItemToArray(tuple, JCreateNumber(value));
    if (unitOverrideCode >= 0) {
      JAddItemToArray(tuple, JCreateNumber(unitOverrideCode));
    }
    JAddItemToArray(readingsArr, tuple);
  }

  bool sendBatchedData(SensorData* readings, uint8_t count) {
    Serial.printf("[NC] Sending %d batched readings (compact format)...\n", count);

    if (!takeBusyFlag(&i2cBusy, 10000)) {
      Serial.println("[NC] ERROR: I2C busy timeout during send");
      return false;
    }

    J *req = notecard.newRequest("note.add");
    if (!req) {
      Serial.println("[NC] ERROR: Failed to create note.add request");
      releaseBusyFlag(&i2cBusy);
      return false;
    }

    JAddStringToObject(req, "file", "data.qo");
    // sync:false. Forcing an immediate sync every 30s makes this I2C request
    // block for far longer, and loop() is single-threaded - every millisecond
    // spent here is a millisecond the BMV080 is not serviced, which is the
    // condition Bosch's FIFO-overflow warnings (4 / 209) describe. hub.set
    // already puts the Notecard in "continuous" mode, so queued notes go out
    // promptly on their own. Set back to true if you need the old latency.
    JAddBoolToObject(req, "sync", NOTECARD_FORCE_SYNC_ON_SEND);

    J *body = JCreateObject();
    J *dArray = JCreateArray();

    // One device entry: [ deviceId, [ snapshot, snapshot, ... ] ]
    J *deviceEntry = JCreateArray();
    JAddItemToArray(deviceEntry, JCreateString(DEVICE_ID));

    J *snapshots = JCreateArray();

    for (uint8_t i = 0; i < count; i++) {
      SensorData &data = readings[i];

      // One snapshot: [ epochSeconds, [ [type,val], [type,val,unitOverride], ... ] ]
      J *snapshot = JCreateArray();
      JAddItemToArray(snapshot, JCreateNumber(data.epochUnix));

      J *readingsArr = JCreateArray();

      // BME688/BSEC2 readings - only present when this snapshot came from a
      // periodic environmental read (not a lightning-only event).
      if (data.envValid) {
        appendReading(readingsArr, READING_TEMPERATURE, data.temperature);
        appendReading(readingsArr, READING_HUMIDITY, data.humidity);
        appendReading(readingsArr, READING_PRESSURE, data.pressure);
        appendReading(readingsArr, READING_GAS_RESISTANCE, data.gasResistance);
        appendReading(readingsArr, READING_ALTITUDE, data.altitude);
        // IAQ_ACCURACY must accompany the algorithm outputs below - the server
        // uses it to decide whether they're trustworthy enough to alert on.
        appendReading(readingsArr, READING_IAQ_ACCURACY, data.iaqAccuracy);
        appendReading(readingsArr, READING_IAQ, data.iaq);
        appendReading(readingsArr, READING_CO2_EQUIVALENT, data.co2Equivalent);
        appendReading(readingsArr, READING_BREATH_VOC_EQUIVALENT, data.breathVocEquivalent);
        appendReading(readingsArr, READING_GAS_PERCENTAGE, data.gasPercentage);
        appendReading(readingsArr, READING_BSEC_STABILIZATION, data.stabilized ? 1 : 0);
        appendReading(readingsArr, READING_BSEC_RUN_IN, data.runInComplete ? 1 : 0);
      }

      // BMV080 particulate readings - only present when the BMV080 read
      // succeeded for this snapshot.
      if (data.bmv080Valid) {
        appendReading(readingsArr, READING_PM1, data.pm1);
        appendReading(readingsArr, READING_PM25, data.pm25);
        appendReading(readingsArr, READING_PM10, data.pm10);
        appendReading(readingsArr, READING_OBSTRUCTED, data.bmv080Obstructed ? 1 : 0);
      }

      // AS3935 lightning readings - only present when the IRQ line fired
      // and produced an event snapshot.
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
    bool ok = notecard.sendRequest(req);

    if (ok) {
      Serial.printf("[NC] ✓ Successfully sent %d readings\n", count);
    } else {
      Serial.printf("[NC] ✗ Failed to send %d readings\n", count);
    }

    releaseBusyFlag(&i2cBusy);
    return ok;
  }
};

NotecardManager notecardManager;

unsigned long lastSensorRead = 0;
unsigned long lastDataSend = 0;
unsigned long lastRegister = 0;
bool deviceRegistered = false;

// Pushes a fully-populated SensorData snapshot into the ring buffer.
// Assumes dataBusy is already held by the caller.
void pushSnapshot(const SensorData& snapshot) {
  readingsBuffer[bufferIndex] = snapshot;

  bufferIndex = (bufferIndex + 1) % MAX_READINGS_BUFFER;
  if (bufferCount < MAX_READINGS_BUFFER) {
    bufferCount++;
  }

  hasDataToSend = true;
}

void readSensors() {
  if (millis() - lastSensorRead < SENSOR_READ_INTERVAL) {
    return;
  }

  if (!takeBusyFlag(&i2cBusy, 5000)) {
    Serial.println("[SENS] I2C busy, skipping");
    lastSensorRead = millis();
    return;
  }

  // Refresh the epoch time base only when it's gone stale, rather than on
  // every read - see the timekeeping note above.
  if (timeBaseNeedsSync()) {
    syncTimeBase_Internal();
  }

  releaseBusyFlag(&i2cBusy);

  // Particulate values come from the cache serviceBmv080() maintains, on the
  // same "snapshot whatever the sensor last produced" model used for BSEC.
  bool bmv080Valid = latestPmIsFresh();
  float pm1 = bmv080Valid ? latestPm.pm1 : 0;
  float pm25 = bmv080Valid ? latestPm.pm25 : 0;
  float pm10 = bmv080Valid ? latestPm.pm10 : 0;
  bool obstructed = bmv080Valid ? latestPm.obstructed : false;

  // No dedicated "no fresh sample" line here any more - at a 1s cadence it
  // buried every other log message. The [SENS] line below prints PM2.5 as
  // "--" when the snapshot carries no particulate data, and the [BMV080]
  // heartbeat reports the stall and any recovery attempts.

  uint32_t epochTime;
  if (!currentEpoch(&epochTime)) {
    Serial.println("[SENS] No time base yet, skipping reading");
    lastSensorRead = millis();
    return;
  }

  if (!takeBusyFlag(&dataBusy, 1000)) {
    Serial.println("[SENS] Data busy, skipping");
    lastSensorRead = millis();
    return;
  }

  // BSEC2 produces its own output on its own cadence (serviceBsec(), called
  // every loop() iteration). We just snapshot whatever it last computed -
  // envSensorReady && latestEnv.valid means at least one BSEC cycle has
  // completed since boot.
  bool envValid = envSensorReady && latestEnv.valid;

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
  snapshot.bmv080Valid = bmv080Valid;
  snapshot.pm1 = pm1;
  snapshot.pm25 = pm25;
  snapshot.pm10 = pm10;
  snapshot.bmv080Obstructed = obstructed;
  snapshot.lightningValid = false;  // periodic env read, no lightning event here
  snapshot.epochUnix = epochTime;

  pushSnapshot(snapshot);

  // "--" rather than 0.0 when there is no fresh particulate sample: the old
  // line printed the zero-fill, which reads exactly like a real 0.0 ug/m3.
  char pmStr[12];
  if (bmv080Valid) {
    snprintf(pmStr, sizeof(pmStr), "%.1f", pm25);
  } else {
    snprintf(pmStr, sizeof(pmStr), "--");
  }

  Serial.printf("[SENS] T:%.1f H:%.1f P:%.1f Gas:%.1f IAQ:%.1f(acc%d) PM2.5:%s @ %lu [Buffer: %d/%d]\n",
               snapshot.temperature, snapshot.humidity, snapshot.pressure,
               snapshot.gasResistance, snapshot.iaq, snapshot.iaqAccuracy, pmStr,
               (unsigned long)epochTime, bufferCount, MAX_READINGS_BUFFER);

  releaseBusyFlag(&dataBusy);
  lastSensorRead = millis();
}

// Lightning events are asynchronous (an actual strike, disturber, or noise
// event can happen at any moment), so this is polled every loop() iteration
// rather than on the SENSOR_READ_INTERVAL cadence used for BME688/BMV080.
void pollLightningDetector() {
  if (!lightningSensorReady) {
    return;
  }

  // Heartbeat so "no output" during a quiet period isn't confused with the
  // sensor being dead - prints the current IRQ pin state every 10s.
  static unsigned long lastLightningHeartbeat = 0;
  if (millis() - lastLightningHeartbeat > 10000) {
    Serial.printf("[LIGHTNING] Alive, watching IRQ pin (currently %s)\n",
                 digitalRead(LIGHTNING_IRQ_PIN) == HIGH ? "HIGH" : "LOW");
    lastLightningHeartbeat = millis();
  }

  if (digitalRead(LIGHTNING_IRQ_PIN) != HIGH) {
    return;
  }

  // Per the AS3935 datasheet, allow ~2ms after IRQ goes high before reading
  // the interrupt register so it has settled.
  delay(2);

  // AS3935 is on its own SPI bus here, separate from the Notecard's I2C
  // link, so these calls don't need to take i2cBusy - only the epoch
  // timestamp fetch below (which may go over I2C) needs that lock.
  uint8_t intVal = lightning.readInterruptReg();

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
    byte distance = lightning.distanceToStorm();
    long energyVal = lightning.lightningEnergy();
    strikeDetected = true;
    distanceKm = (float)distance;
    energy = (uint32_t)energyVal;
    Serial.printf("[LIGHTNING] Strike detected! ~%d km away, energy=%ld\n", distance, energyVal);
  }

  if (!strikeDetected && !disturberDetected) {
    // Pure noise event - nothing worth buffering/sending.
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
               (unsigned long)epochTime, bufferCount, MAX_READINGS_BUFFER);

  releaseBusyFlag(&dataBusy);
}

void sendData() {
  if (!hasDataToSend || bufferCount == 0 || millis() - lastDataSend < DATA_SEND_INTERVAL) {
    return;
  }

  Serial.printf("[SEND] Preparing to send %d readings...\n", bufferCount);

  if (!takeBusyFlag(&dataBusy, 1000)) {
    Serial.println("[SEND] Data busy");
    return;
  }

  SensorData sendBuffer[MAX_READINGS_BUFFER];
  uint8_t sendCount = bufferCount;

  uint8_t startIdx = (bufferIndex - bufferCount + MAX_READINGS_BUFFER) % MAX_READINGS_BUFFER;
  for (uint8_t i = 0; i < sendCount; i++) {
    sendBuffer[i] = readingsBuffer[(startIdx + i) % MAX_READINGS_BUFFER];
  }

  bufferCount = 0;
  bufferIndex = 0;
  hasDataToSend = false;

  releaseBusyFlag(&dataBusy);

  notecardManager.sendBatchedData(sendBuffer, sendCount);
  lastDataSend = millis();
}

void registerDevice() {
  if (deviceRegistered || millis() - lastRegister < 5000) {
    return;
  }

  Serial.println("[REG] Attempting registration...");
  deviceRegistered = notecardManager.registerDevice();
  lastRegister = millis();

  if (!deviceRegistered) {
    Serial.println("[REG] Will retry in 5s");
  }
}

// ---------------------------------------------------------------------------
// I2C bus scan.
//
// Walks addresses 0x08-0x77 (0x00-0x07 and 0x78-0x7F are reserved by the I2C
// spec and must not be probed) and prints a 16x16 map of what ACKed, plus a
// named list for the parts this board is expected to have. Run once at boot
// BEFORE any driver init, so the map shows the raw bus rather than whatever
// state a half-initialized driver left a device in.
//
// Note: the AS3935 is on SPI, not I2C, so it will never appear here.
// ---------------------------------------------------------------------------

// Reads the idle levels of SDA/SCL *before* the I2C peripheral claims the
// pins. On a healthy idle bus both sit HIGH, held there by the pull-ups on
// the breakout boards. Anything else is a wiring/power fault, and no amount
// of address probing will tell you that - every address just NACKs.
void reportI2CPinState() {
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);
  delayMicroseconds(200);

  bool sda = (digitalRead(I2C_SDA) == HIGH);
  bool scl = (digitalRead(I2C_SCL) == HIGH);

  Serial.printf("[I2C] Idle levels (no internal pullup): SDA=%s SCL=%s\n",
               sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW");

  if (sda && scl) {
    Serial.println("[I2C] Bus looks idle and pulled up - pull-ups present and powered.");
    return;
  }

  // Distinguish "no external pull-ups at all" from "something is actively
  // holding the line low". The ESP32's internal pull-ups are weak (~45k),
  // but they're strong enough to lift a floating line and NOT strong enough
  // to overpower a device clamping it.
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(200);

  bool sdaPu = (digitalRead(I2C_SDA) == HIGH);
  bool sclPu = (digitalRead(I2C_SCL) == HIGH);

  if (!scl && !sclPu) {
    Serial.println("[I2C] SCL is held LOW even with an internal pullup - short to GND or a dead device.");
  }
  if (!sda && !sdaPu) {
    Serial.println("[I2C] SDA is held LOW even with an internal pullup - a slave is clamping the bus.");
  }
  if ((!sda && sdaPu) || (!scl && sclPu)) {
    Serial.println("[I2C] Line(s) floated up only with the internal pullup:");
    Serial.println("[I2C]   no external pull-ups on the bus, or the breakouts aren't powered.");
  }
}

// Frees a bus where a slave was left mid-transaction (classic after a reset
// or reflash that interrupted a read). Clocking SCL up to 9 times lets the
// device finish shifting out its byte and release SDA; a STOP condition then
// resets its state machine. Harmless when the bus is already healthy.
//
// Uses open-drain so we never drive against a device that's pulling low -
// which does mean this can only work if external pull-ups exist.
bool recoverI2CBus() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  delayMicroseconds(100);

  if (digitalRead(I2C_SDA) == HIGH) {
    return true;  // SDA already free, nothing to recover
  }

  Serial.println("[I2C] SDA stuck LOW - clocking the bus to release it...");

  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);

  for (uint8_t i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(10);
  }

  // STOP condition: SDA rises while SCL is high.
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);
  digitalWrite(I2C_SDA, HIGH);
  delayMicroseconds(10);

  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(100);

  bool freed = (digitalRead(I2C_SDA) == HIGH);
  Serial.printf("[I2C] Bus recovery %s\n", freed ? "SUCCEEDED" : "FAILED - SDA still low");
  return freed;
}

// Best-effort name for a responding address. Several of these are the
// datasheet's default/alternate addresses, not necessarily what's populated.
const char* i2cDeviceName(uint8_t addr) {
  switch (addr) {
    case 0x17: return "Notecard";
    case 0x57: return "BMV080 (particulate)";
    case 0x76: return "BME688 (BSEC2), addr LOW";
    case 0x77: return "BME688 (BSEC2), addr HIGH";
    default:   return nullptr;
  }
}

void scanI2CBus() {
  Serial.println("\n[I2C] Scanning bus (0x08-0x77)...");
  Serial.println("[I2C]      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");

  uint8_t found[120];
  uint8_t foundCount = 0;

  for (uint8_t row = 0x00; row <= 0x70; row += 0x10) {
    Serial.printf("[I2C] %02x:", row);

    for (uint8_t col = 0x00; col <= 0x0f; col++) {
      uint8_t addr = row + col;

      // Reserved ranges - never probe these.
      if (addr < 0x08 || addr > 0x77) {
        Serial.print("   ");
        continue;
      }

      Wire.beginTransmission(addr);
      uint8_t error = Wire.endTransmission();

      if (error == 0) {
        Serial.printf(" %02x", addr);
        if (foundCount < ARRAY_LEN(found)) {
          found[foundCount++] = addr;
        }
      } else if (error == 4) {
        // Bus-level error rather than a plain NACK - worth distinguishing,
        // since a run of these usually means bad wiring or missing pull-ups.
        Serial.print(" ER");
      } else {
        Serial.print(" --");
      }
    }
    Serial.println();
  }

  if (foundCount == 0) {
    Serial.println("[I2C] No devices found. Check SDA/SCL wiring, pull-ups, and power.");
    return;
  }

  Serial.printf("[I2C] %d device(s) found:\n", foundCount);
  for (uint8_t i = 0; i < foundCount; i++) {
    const char* name = i2cDeviceName(found[i]);
    Serial.printf("[I2C]   0x%02x  %s\n", found[i], name ? name : "unknown device");
  }
  Serial.println();
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

  // Diagnose and, if needed, free the bus BEFORE Wire takes over the pins.
  // Once the I2C peripheral owns them these raw reads aren't possible.
  reportI2CPinState();
  recoverI2CBus();

  Wire.begin(I2C_SDA, I2C_SCL);
  // 400kHz. The BMV080 moves a comparatively large payload per sample and
  // SparkFun's own examples call out 400kHz as the upgrade; at 100kHz the bus
  // spends far longer occupied for the same data. All three devices here
  // (BME688, BMV080, Notecard) are rated for fast mode. If the bus scan below
  // starts reporting "ER" entries after this change, drop back to 100000 -
  // that would point at wiring/pull-up marginality, not at the sensor.
  Wire.setClock(400000);
  delay(100);

  scanI2CBus();

  Serial.println("[SETUP] Init BME688 (BSEC2)...");

  // BSEC_SAMPLE_RATE_LP runs a full BSEC cycle roughly every 3s, which is
  // the closest stock rate to this sketch's 1s read cadence without going
  // to ULP (300s - too slow for near-real-time IAQ). Outputs chosen here
  // are the standard "AI-derived air quality" set BSEC2 exposes in this
  // mode - NOT gas classification (see note below setup()).
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

    // Restore learned calibration BEFORE subscribing, matching the order in
    // Bosch's basic_config_state example.
    bsecStateRestored = restoreBsecState();

    bool subscribed = envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP);

    // updateSubscription() returns false on ANY non-zero status - including
    // positive warning codes - because bsec2.cpp does `if (status != BSEC_OK)
    // return false`. BSEC_W_SU_SAMPLERATEMISMATCH (14) is one of those: the
    // subscription is live and working, BSEC is only noting that the config
    // blob compiled into the library wasn't built for the LP sample rate.
    // Treating that warning as fatal disables the BME688 for no reason, so
    // only a NEGATIVE status counts as a real error here.
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

  // A missing or wedged BMV080 no longer halts the device. Halting here threw
  // away the BME688, lightning detector and Notecard uplink over one failed
  // sensor, which contradicts how every other sensor in this sketch degrades
  // (see lightningSensorReady). serviceBmv080()'s watchdog retries on its own.
  Serial.println("[SETUP] Init BMV080...");
  if (startBmv080()) {
    Serial.println("[SETUP] BMV080 OK - continuous mode started");
  } else {
    Serial.println("[SETUP] BMV080 unavailable - continuing without particulate data");
    Serial.printf("[SETUP]   (auto-retry every %lus)\n",
                 (unsigned long)(bmv080StallLimit() / 1000));
  }

  Serial.println("[SETUP] Init AS3935 Lightning Detector (SPI)...");
  pinMode(LIGHTNING_IRQ_PIN, INPUT);
  pinMode(LIGHTNING_CS_PIN, OUTPUT);
  digitalWrite(LIGHTNING_CS_PIN, HIGH);  // CS idles high, active low
  SPI.begin();
  if (!lightning.beginSPI(LIGHTNING_CS_PIN, LIGHTNING_SPI_SPEED)) {
    Serial.println("[SETUP] AS3935 NOT FOUND - lightning detection disabled, continuing without it");
    lightningSensorReady = false;
  } else {
    // OUTDOOR gives higher sensitivity; use INDOOR if the enclosure is inside.
    lightning.setIndoorOutdoor(OUTDOOR);
    // false = report disturbers as their own event instead of silently
    // masking them; flip to true once the site's noise floor is dialed in.
    lightning.maskDisturber(false);
    // Required on first boot (and after any antenna/board rework) - tunes
    // the onboard LC antenna to the chip's internal 500kHz reference.
    // Without this the sensor may never reliably trigger the IRQ pin.
    lightning.calibrateOsc();
    lightningSensorReady = true;
    Serial.println("[SETUP] AS3935 OK");
  }

  Serial.println("[SETUP] Init Notecard...");
  notecardManager.begin();

  Serial.println("\n========================================");
  Serial.println("  Setup Complete");
  Serial.println("========================================\n");
}

void loop() {
  static unsigned long lastStatus = 0;

  registerDevice();
  serviceBsec();          // must run every iteration, not gated to 1s
  serviceBmv080();        // likewise - polling at 1s aliases the sensor's own rate
  pollLightningDetector();
  readSensors();
  sendData();

  if (millis() - lastStatus > 30000) {
    Serial.printf("\n[STATUS] Uptime: %lu s, Heap: %d, Reg: %s, Buffer: %d/%d\n",
                 millis() / 1000, ESP.getFreeHeap(),
                 deviceRegistered ? "YES" : "NO",
                 bufferCount, MAX_READINGS_BUFFER);
    Serial.printf("[STATUS] IAQ acc: %d/3 (saved: %d/3, restored at boot: %s), stab: %d, runIn: %d\n",
                 latestEnv.iaqAccuracy, bsecSavedAccuracy,
                 bsecStateRestored ? "YES" : "NO",
                 latestEnv.stabilized ? 1 : 0, latestEnv.runInComplete ? 1 : 0);
    Serial.printf("[STATUS] BMV080: measuring %s, samples %lu, recoveries %lu/%lu, PM data %s\n",
                 bmv080Measuring ? "YES" : "NO",
                 (unsigned long)bmv080SampleCount,
                 (unsigned long)bmv080Recoveries, (unsigned long)bmv080RecoveryAttempts,
                 latestPmIsFresh() ? "FRESH" : "STALE");
    // Guard the subtraction - once a send is overdue this underflows and
    // prints ~4294967 sec, which is what the "Next send in" line was showing.
    uint32_t sinceSend = millis() - lastDataSend;
    uint32_t nextSendSec = (sinceSend >= DATA_SEND_INTERVAL)
                             ? 0
                             : (DATA_SEND_INTERVAL - sinceSend) / 1000;
    Serial.printf("[STATUS] Next send in: %lu sec\n\n", (unsigned long)nextSendSec);
    lastStatus = millis();
  }

  // Was delay(100), which capped the whole loop - and therefore BMV080
  // servicing - at roughly 7Hz. Bosch's FIFO-overflow warnings (4 / 209) say
  // explicitly to call bmv080_serve_interrupt more frequently, so give the
  // sensor as much service as the bus allows. delay(1) rather than no delay
  // at all: loop() is a FreeRTOS task here, and yielding keeps lower-priority
  // system tasks (and the task watchdog) happy.
  delay(1);
}

// ---------------------------------------------------------------------------
// About actual gas RECOGNITION (classification):
//
// Everything above gets you BSEC2's standard AI-derived air-quality metrics
// (IAQ, CO2-equivalent, breath-VOC-equivalent, gas %). Those are single
// composite scores - useful, but they don't tell you *which* gas is present.
//
// To classify/recognize distinct gases (e.g. tell cooking fumes apart from
// something else) you need BSEC2's separate scanning/classification mode:
//   1. Use Bosch's BME AI-Studio to record labeled "scans" of the gases you
//      care about and train a classifier - this produces a binary config
//      blob (a big byte array), not something derivable from datasheet code.
//   2. Load that config with envSensor.setConfig(yourConfigArray) BEFORE
//      calling setState()/updateSubscription().
//   3. Subscribe to BSEC_OUTPUT_GAS_ESTIMATE_1 .. BSEC_OUTPUT_GAS_ESTIMATE_4
//      (each is a 0-1 confidence for one of your trained classes) instead
//      of / in addition to the IAQ outputs, using BSEC_SAMPLE_RATE_SCAN.
//   4. This mode's sample rate and duty cycle differ from LP/ULP - check
//      the AI-Studio-exported config's docs for the timing it expects.
//   5. Server side: reserve codes 22-25 for the four gas estimates in
//      config/readingTypes.js before the firmware starts sending them.
//
// This is a genuinely separate workflow (train in AI-Studio -> export
// config -> flash config -> subscribe to GAS_ESTIMATE outputs) rather than
// a code change alone, so it's left out of the default build above.
// ---------------------------------------------------------------------------
