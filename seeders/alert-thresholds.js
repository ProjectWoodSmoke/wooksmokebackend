const { AlertThreshold } = require('../models');

const DEFAULT_THRESHOLDS = [
    {
        readingType:    'temperature',
        operator:       '>',
        thresholdValue: 80,
        severity:       'high',
        message:        'High temperature detected: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'temperature',
        operator:       '>',
        thresholdValue: 100,
        severity:       'critical',
        message:        'Critical temperature — immediate action required: {value}{unit}',
        enabled:        true
    },

    {
        readingType:    'humidity',
        operator:       '<',
        thresholdValue: 20,
        severity:       'medium',
        message:        'Low humidity warning: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'humidity',
        operator:       '<',
        thresholdValue: 10,
        severity:       'high',
        message:        'Critically low humidity: {value}{unit}',
        enabled:        true
    },

    {
        readingType:    'smoke',
        operator:       '>',
        thresholdValue: 50,
        severity:       'high',
        message:        'Elevated smoke level detected: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'smoke',
        operator:       '>',
        thresholdValue: 200,
        severity:       'critical',
        message:        'Critical smoke level — possible fire: {value}{unit}',
        enabled:        true
    },

    {
        readingType:    'co',
        operator:       '>',
        thresholdValue: 35,
        severity:       'medium',
        message:        'Elevated CO level: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'co',
        operator:       '>',
        thresholdValue: 200,
        severity:       'critical',
        message:        'Dangerous CO level detected: {value}{unit}',
        enabled:        true
    },

    // BME688 / BSEC2 outputs, only evaluated once the device reports
    // iaq_accuracy >= 1 — see isBsecOutputTrustworthy in config/readingTypes.js.
    // IAQ bands follow Bosch's published scale: 151–200 moderately polluted,
    // 251–350 severely polluted, 351+ extremely polluted.
    {
        readingType:    'iaq',
        operator:       '>',
        thresholdValue: 150,
        severity:       'medium',
        message:        'Air quality degraded (moderately polluted) — IAQ {value}',
        enabled:        true
    },
    {
        readingType:    'iaq',
        operator:       '>',
        thresholdValue: 250,
        severity:       'high',
        message:        'Poor air quality (severely polluted) — IAQ {value}',
        enabled:        true
    },
    {
        readingType:    'iaq',
        operator:       '>',
        thresholdValue: 350,
        severity:       'critical',
        message:        'Extremely polluted air — ventilate immediately. IAQ {value}',
        enabled:        true
    },

    // CO2-equivalent is estimated from VOCs, not measured, so these bands are
    // ventilation guidance rather than a CO2 alarm.
    {
        readingType:    'co2_equivalent',
        operator:       '>',
        thresholdValue: 1000,
        severity:       'low',
        message:        'Estimated CO2 rising — consider ventilating: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'co2_equivalent',
        operator:       '>',
        thresholdValue: 2000,
        severity:       'medium',
        message:        'High estimated CO2 — ventilation recommended: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'co2_equivalent',
        operator:       '>',
        thresholdValue: 5000,
        severity:       'high',
        message:        'Very high estimated CO2: {value}{unit}',
        enabled:        true
    },

    {
        readingType:    'breath_voc_equivalent',
        operator:       '>',
        thresholdValue: 2,
        severity:       'medium',
        message:        'Elevated VOC level detected: {value}{unit}',
        enabled:        true
    },
    {
        readingType:    'breath_voc_equivalent',
        operator:       '>',
        thresholdValue: 5,
        severity:       'high',
        message:        'High VOC level detected: {value}{unit}',
        enabled:        true
    }
];

async function seed(sequelize = null) {
    try {
        let inserted = 0;
        let skipped  = 0;

        for (const row of DEFAULT_THRESHOLDS) {
            const [, created] = await AlertThreshold.findOrCreate({
                where: {
                    readingType:    row.readingType,
                    operator:       row.operator,
                    thresholdValue: row.thresholdValue
                },
                defaults: row
            });
            created ? inserted++ : skipped++;
        }

        console.log(`[seed] alert_thresholds: ${inserted} inserted, ${skipped} already existed.`);
    } catch (err) {
        console.error('[seed] Failed:', err.message);
        if (!sequelize) {
            const RETRY_DELAY_MS = 10_000;
            console.log(`[seed] Retrying in ${RETRY_DELAY_MS / 1000} seconds...`);
            setTimeout(() => seed(), RETRY_DELAY_MS);
        }
    }
}

if (require.main === module) {
    require('dotenv').config();
    const sequelize = require('../config/database');
    sequelize.authenticate().then(() => seed(sequelize));
}

module.exports = { seed };