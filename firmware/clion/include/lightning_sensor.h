#pragma once

#include <Arduino.h>

extern bool lightningSensorReady;

bool lightningSensorBegin();

/// Must run every loop() iteration; strikes are asynchronous. Writes to the
/// readings buffer directly, so there is no cache for main.cpp to sample.
void lightningSensorService();
