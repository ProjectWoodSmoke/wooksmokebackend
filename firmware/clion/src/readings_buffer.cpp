#include "readings_buffer.h"

#include "config.h"

static SensorData readingsBuffer[MAX_READINGS_BUFFER];
static uint8_t bufferIndex = 0;
static uint8_t bufferCount = 0;
static volatile bool hasDataToSend = false;

void pushSnapshot(const SensorData& snapshot) {
  readingsBuffer[bufferIndex] = snapshot;

  bufferIndex = (bufferIndex + 1) % MAX_READINGS_BUFFER;
  if (bufferCount < MAX_READINGS_BUFFER) {
    bufferCount++;
  }

  hasDataToSend = true;
}

uint8_t peekSnapshots(SensorData* out) {
  const uint8_t sendCount = bufferCount;
  const uint8_t startIdx = (bufferIndex - bufferCount + MAX_READINGS_BUFFER) % MAX_READINGS_BUFFER;

  for (uint8_t i = 0; i < sendCount; i++) {
    out[i] = readingsBuffer[(startIdx + i) % MAX_READINGS_BUFFER];
  }

  return sendCount;
}

void discardSnapshots(uint8_t count) {
  // The newest entries always end at bufferIndex, so shrinking the count is
  // what moves the oldest-entry position forward.
  bufferCount = (count >= bufferCount) ? 0 : bufferCount - count;
  hasDataToSend = (bufferCount > 0);
}

uint8_t bufferedSnapshotCount() {
  return bufferCount;
}

bool hasBufferedData() {
  return hasDataToSend;
}
