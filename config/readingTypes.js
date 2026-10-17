/**
 * Single source of truth for the ultra-compact wire format's numeric codes.
 *
 * MUST stay in sync with the firmware's READING_* defines (firmware/main.cpp).
 *
 * APPEND-ONLY: never reorder or remove an entry, only add new codes at the
 * end. Old firmware in the field and every Reading row already written to the
 * database depend on these positions — and on the exact `type`/`unit` strings,
 * which are what alert thresholds match against.
 *
 * Codes 2 (smoke) and 3 (co) are reserved for other device types and are not
 * emitted by air-quality-monitor devices.
 */
const ULTRA_COMPACT_MAPPING = {
  0:  { type: 'temperature',           unit: '°C' },
  1:  { type: 'humidity',              unit: '%' },
  2:  { type: 'smoke',                 unit: 'ppm' },
  3:  { type: 'co',                    unit: 'ppm' },
  4:  { type: 'pressure',              unit: 'hPa' },
  5:  { type: 'gas_resistance',        unit: 'kOhm' },
  6:  { type: 'altitude',              unit: 'm' },
  7:  { type: 'pm1',                   unit: 'µg/m³' },
  8:  { type: 'pm25',                  unit: 'µg/m³' },
  9:  { type: 'pm10',                  unit: 'µg/m³' },
  10: { type: 'obstructed',            unit: '' },
  11: { type: 'lightning_distance_km', unit: 'km' },
  12: { type: 'lightning_energy',      unit: '' },
  13: { type: 'lightning_strike',      unit: '' },
  14: { type: 'lightning_disturber',   unit: '' },

  // BSEC2 AI gas-sensing outputs (BME688)
  15: { type: 'iaq',                   unit: '' },     // Bosch Index for Air Quality, 0–500 (lower is better)
  16: { type: 'iaq_accuracy',          unit: '' },     // 0 = uncalibrated … 3 = fully calibrated
  17: { type: 'co2_equivalent',        unit: 'ppm' },  // ESTIMATED from VOCs — not a real CO2 measurement
  18: { type: 'breath_voc_equivalent', unit: 'ppm' },
  19: { type: 'gas_percentage',        unit: '%' },    // relative air contamination, 0–100
  20: { type: 'bsec_stabilization',    unit: '' },     // 0 = still stabilizing, 1 = stabilized
  21: { type: 'bsec_run_in',           unit: '' },     // 0 = still running in, 1 = run-in complete
};

/**
 * Optional unit-override codes, applied only when a reading tuple carries a
 * third element. Same append-only rule as above, and the strings must match
 * ULTRA_COMPACT_MAPPING's canonical units.
 */
const UNIT_OVERRIDE_CODES = {
  0: '°C',
  1: '%',
  2: 'ppm',
  3: 'hPa',
  4: 'kOhm',
  5: 'm',
  6: 'µg/m³',
  7: 'bool',
  8: '',       // explicitly unitless
  9: 'km',
  10: 'Ohm',
};

const UNKNOWN_MAPPING = { type: 'unknown', unit: '' };

/**
 * Reading types produced by the BSEC2 fusion algorithm rather than measured
 * directly. Every one of these is derived from the same internal gas model, so
 * they are only meaningful once that model has calibrated itself — see
 * `isBsecOutputTrustworthy` below.
 *
 * `gas_resistance` is deliberately NOT in this set: it is the raw resistance
 * the BME688 heater actually measures, valid from the first sample.
 */
const BSEC_ALGORITHM_TYPES = new Set([
  'iaq',
  'co2_equivalent',
  'breath_voc_equivalent',
  'gas_percentage',
]);

/**
 * Accuracy value below which BSEC's algorithm outputs are stored but NOT
 * alerted on. At 0 the algorithm is still stabilising and its IAQ/CO2eq/VOC
 * numbers are placeholders, which would fire bogus "poor air quality" alerts
 * on every device restart.
 */
const MIN_TRUSTWORTHY_IAQ_ACCURACY = 1;

/** Whether a reading should be evaluated against alert thresholds. */
function isBsecOutputTrustworthy(readingType, iaqAccuracy) {
  if (!BSEC_ALGORITHM_TYPES.has(readingType)) return true;

  // No accuracy code tells us nothing about calibration, so trust the reading
  // rather than silently dropping alerts for it.
  if (iaqAccuracy === null || iaqAccuracy === undefined) return true;

  return Number(iaqAccuracy) >= MIN_TRUSTWORTHY_IAQ_ACCURACY;
}

/** Numeric code for iaq_accuracy, used to find it while expanding a snapshot. */
const IAQ_ACCURACY_CODE = 16;

module.exports = {
  ULTRA_COMPACT_MAPPING,
  UNIT_OVERRIDE_CODES,
  UNKNOWN_MAPPING,
  BSEC_ALGORITHM_TYPES,
  MIN_TRUSTWORTHY_IAQ_ACCURACY,
  IAQ_ACCURACY_CODE,
  isBsecOutputTrustworthy,
};
