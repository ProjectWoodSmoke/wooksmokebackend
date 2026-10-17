#pragma once

#include <Arduino.h>

#include "sensor_data.h"

// Every function here requires the caller to hold dataBusy.

void pushSnapshot(const SensorData& snapshot);

/// Copies oldest-first into out, which needs room for MAX_READINGS_BUFFER,
/// leaving the buffer intact. Returns the number copied.
uint8_t peekSnapshots(SensorData* out);

/// Drops the oldest count entries, keeping anything pushed since the peek, so
/// a send is only acknowledged once it has actually succeeded.
void discardSnapshots(uint8_t count);

uint8_t bufferedSnapshotCount();

bool hasBufferedData();
