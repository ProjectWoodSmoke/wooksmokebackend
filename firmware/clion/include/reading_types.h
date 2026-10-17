#pragma once

// Wire-format type codes. MUST stay in sync with config/readingTypes.js.
//
// APPEND-ONLY: firmware in the field and every Reading row already stored
// depend on these positions. Codes 2 (smoke) and 3 (co) belong to other device
// types and must not be reused.

constexpr int READING_TEMPERATURE    = 0;
constexpr int READING_HUMIDITY       = 1;
constexpr int READING_PRESSURE       = 4;
constexpr int READING_GAS_RESISTANCE = 5;
constexpr int READING_ALTITUDE       = 6;
constexpr int READING_PM1            = 7;
constexpr int READING_PM25           = 8;
constexpr int READING_PM10           = 9;
constexpr int READING_OBSTRUCTED     = 10;

constexpr int READING_LIGHTNING_DISTANCE  = 11;
constexpr int READING_LIGHTNING_ENERGY    = 12;
constexpr int READING_LIGHTNING_STRIKE    = 13;
constexpr int READING_LIGHTNING_DISTURBER = 14;

// The server suppresses alerting on 15, 17, 18 and 19 until READING_IAQ_ACCURACY
// is at least 1, so every snapshot carrying those must also carry the accuracy.
constexpr int READING_IAQ                   = 15;
constexpr int READING_IAQ_ACCURACY          = 16;  ///< 0 = uncalibrated, 3 = calibrated.
constexpr int READING_CO2_EQUIVALENT        = 17;  ///< Estimated from VOCs, not real CO2.
constexpr int READING_BREATH_VOC_EQUIVALENT = 18;
constexpr int READING_GAS_PERCENTAGE        = 19;
constexpr int READING_BSEC_STABILIZATION    = 20;
constexpr int READING_BSEC_RUN_IN           = 21;
