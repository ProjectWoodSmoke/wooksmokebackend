#pragma once

#include <Arduino.h>

/// Must precede Wire.begin(): once the peripheral owns the pins these raw reads
/// are impossible.
void reportI2CPinState();

/// Clocks a bus stuck mid-transaction free. Must also precede Wire.begin().
bool recoverI2CBus();

void scanI2CBus();
