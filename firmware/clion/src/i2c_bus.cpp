#include "i2c_bus.h"

#include <Wire.h>

#include "config.h"

void reportI2CPinState() {
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);
  delayMicroseconds(200);

  const bool sda = (digitalRead(I2C_SDA) == HIGH);
  const bool scl = (digitalRead(I2C_SCL) == HIGH);

  Serial.printf("[I2C] Idle levels (no internal pullup): SDA=%s SCL=%s\n",
               sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW");

  if (sda && scl) {
    Serial.println("[I2C] Bus looks idle and pulled up - pull-ups present and powered.");
    return;
  }

  // The internal pull-ups (~45k) can lift a floating line but not overpower a
  // device clamping it, separating "no pull-ups" from "a slave holds the line".
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(200);

  const bool sdaPu = (digitalRead(I2C_SDA) == HIGH);
  const bool sclPu = (digitalRead(I2C_SCL) == HIGH);

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

bool recoverI2CBus() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  delayMicroseconds(100);

  if (digitalRead(I2C_SDA) == HIGH) {
    return true;  // already free
  }

  Serial.println("[I2C] SDA stuck LOW - clocking the bus to release it...");

  // Nine clocks let a device stuck mid-read finish its byte and release SDA.
  // Open-drain, so this only works when external pull-ups are present.
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);

  for (uint8_t i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(10);
  }

  // STOP (SDA rising while SCL is high) resets the slave's state machine.
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

  const bool freed = (digitalRead(I2C_SDA) == HIGH);
  Serial.printf("[I2C] Bus recovery %s\n", freed ? "SUCCEEDED" : "FAILED - SDA still low");
  return freed;
}

/// Datasheet defaults, not confirmation of what is actually populated.
static const char* i2cDeviceName(uint8_t addr) {
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
      const uint8_t addr = row + col;

      // 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec.
      if (addr < 0x08 || addr > 0x77) {
        Serial.print("   ");
        continue;
      }

      Wire.beginTransmission(addr);
      const uint8_t error = Wire.endTransmission();

      if (error == 0) {
        Serial.printf(" %02x", addr);
        if (foundCount < ARRAY_LEN(found)) {
          found[foundCount++] = addr;
        }
      } else if (error == 4) {
        // Bus-level error, not a plain NACK: bad wiring or missing pull-ups.
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
