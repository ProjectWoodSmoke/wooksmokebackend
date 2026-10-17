#pragma once

#include <Arduino.h>

#ifndef ARRAY_LEN
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#endif

// Pinout (Arduino Nano ESP32)

constexpr uint8_t I2C_SDA = A4;
constexpr uint8_t I2C_SCL = A5;

constexpr uint8_t  LIGHTNING_CS_PIN    = D10;
constexpr uint8_t  LIGHTNING_IRQ_PIN   = D9;
constexpr uint32_t LIGHTNING_SPI_SPEED = 2000000;

// Timing and buffering

constexpr uint32_t SERIAL_SPEED = 115200;
constexpr uint8_t  BMV080_ADDR  = 0x57;

constexpr uint32_t SENSOR_READ_INTERVAL = 1000;
constexpr uint32_t DATA_SEND_INTERVAL   = 30000;

// Below DATA_SEND_INTERVAL / SENSOR_READ_INTERVAL (30), readings are
// overwritten before they are ever sent.
constexpr uint8_t MAX_READINGS_BUFFER = 40;

// card.time costs an I2C round trip of hundreds of ms, which starved BSEC of
// the bus when read per sample.
constexpr uint32_t TIME_RESYNC_INTERVAL = 5UL * 60UL * 1000UL;

// BME688 / BSEC2

// Enclosure-dependent; re-tune against a reference thermometer.
constexpr float BSEC_TEMP_OFFSET = 2.0f;

// BSEC's learned baseline lives in RAM, so without persisting it every reboot
// restarts a multi-hour warm-up.
constexpr char BSEC_NVS_NAMESPACE[] = "bsec";
constexpr char BSEC_NVS_STATE_KEY[] = "state";
constexpr char BSEC_NVS_ACC_KEY[]   = "acc";

constexpr uint32_t BSEC_STATE_SAVE_INTERVAL = 6UL * 60UL * 60UL * 1000UL;

// Saving below 3 bakes in a half-learned baseline, worse than saving nothing.
constexpr uint8_t BSEC_MIN_ACCURACY_TO_SAVE = 3;

// Notecard

// Forcing a sync every 30 s blocks loop() and starves the BMV080. hub.set
// already runs in continuous mode, so queued notes go out promptly anyway.
constexpr bool NOTECARD_FORCE_SYNC_ON_SEND = false;

// Consecutive failed sends before the link is reinitialized. At one send per
// DATA_SEND_INTERVAL that is ~2 minutes, long enough to ride out a transient.
constexpr uint8_t NOTECARD_FAILURES_BEFORE_REINIT = 4;

// BMV080

// 0 = continuous, >0 = duty-cycle period in seconds. Must stay a macro: the
// code below branches on it with #if.
#define BMV080_DUTY_CYCLE_SECONDS 0

#if BMV080_DUTY_CYCLE_SECONDS > 0
constexpr uint32_t PM_NOMINAL_PERIOD_MS = BMV080_DUTY_CYCLE_SECONDS * 1000UL;
#else
constexpr uint32_t PM_NOMINAL_PERIOD_MS = 1000UL;
#endif

constexpr uint32_t PM_SAMPLE_MAX_AGE =
    (PM_NOMINAL_PERIOD_MS * 3UL > 10000UL) ? PM_NOMINAL_PERIOD_MS * 3UL : 10000UL;

// Silence tolerated before the driver is torn down and reopened: the sensor can
// stop streaming for good without any API call reporting it.
constexpr uint32_t PM_STALL_TIMEOUT = PM_SAMPLE_MAX_AGE + 20000UL;

// The first sample lands tens of seconds after setMode(), so reusing
// PM_STALL_TIMEOUT here would tear the driver down mid-warm-up, forever.
constexpr uint32_t PM_FIRST_SAMPLE_TIMEOUT = PM_STALL_TIMEOUT + 40000UL;

// Bosch flags falling behind as its own failure, so drain rather than take one
// sample per loop().
constexpr uint8_t BMV080_MAX_DRAIN = 16;

// GPIO gating power to the sensor, or -1 to disable; covers the part ACKing
// while its core is dead. Needs a high-side load switch with active-HIGH
// enable. Must stay a macro: the code branches on it with #if.
#define BMV080_POWER_PIN -1

constexpr uint32_t BMV080_POWER_OFF_MS = 300;
