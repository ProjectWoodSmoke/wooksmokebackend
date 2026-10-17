const express = require('express');
const { v4: uuid4 } = require('uuid');
const { Device, Reading, Alert, AlertThreshold } = require('../models');
const { authenticate } = require('../middleware/authenticate');
const { authorizeDevice } = require('../middleware/authorizeDevice');
const { validateSensorData, validateSensorDataCompact, validateSensorDataUltraCompact } = require('../middleware/validate');
const {
  ULTRA_COMPACT_MAPPING,
  UNIT_OVERRIDE_CODES,
  UNKNOWN_MAPPING,
  IAQ_ACCURACY_CODE,
  isBsecOutputTrustworthy,
} = require('../config/readingTypes');
const router = express.Router();

/**
 * @swagger
 * tags:
 *   name: Sensor Data
 *   description: >
 *     Submit, query, and delete sensor readings from BioBot field devices.
 *     Each submission is a batch of one or more device payloads. Every reading
 *     is automatically evaluated against the configured alert thresholds —
 *     matching readings create Alert records without any extra API call.
 */

/**
 * @swagger
 * components:
 *   schemas:
 *
 *     SensorReading:
 *       type: object
 *       description: A single sensor measurement from a field device.
 *       required:
 *         - value
 *         - unit
 *         - readingType
 *       properties:
 *         value:
 *           type: number
 *           format: float
 *           description: The numeric measurement value.
 *           example: 95.4
 *         unit:
 *           type: string
 *           description: The unit of measurement (e.g. "°C", "%", "ppm").
 *           example: "°C"
 *         readingType:
 *           type: string
 *           description: >
 *             Sensor category. Must match the readingType used in alert
 *             thresholds for automatic alert generation. Common values:
 *             temperature, humidity, smoke, co.
 *           example: "temperature"
 *         timestamp:
 *           type: string
 *           format: date-time
 *           description: >
 *             ISO 8601 timestamp of when the reading was taken on-device.
 *             Defaults to the server current time if omitted — useful when
 *             a device is submitting a backlog after reconnecting.
 *           example: "2026-04-18T14:22:00.000Z"
 *
 *     DevicePayload:
 *       type: object
 *       description: >
 *         A group of readings from one device. All readings in the array
 *         share the same requestId (assigned server-side) for traceability.
 *       required:
 *         - deviceId
 *         - readings
 *       properties:
 *         deviceId:
 *           type: string
 *           description: The unique identifier of the submitting device (must already exist in the DB).
 *           example: "BIOBOT-001"
 *         readings:
 *           type: array
 *           minItems: 1
 *           items:
 *             $ref: '#/components/schemas/SensorReading'
 *
 *     SubmitResult:
 *       type: object
 *       description: Per-device outcome within a batch submission response.
 *       properties:
 *         requestId:
 *           type: string
 *           format: uuid
 *           description: >
 *             Server-assigned UUID that uniquely identifies this device submission.
 *             Use it to look up all readings from this batch via GET /sensordata?requestId=<id>.
 *           example: "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
 *         success:
 *           type: boolean
 *           example: true
 *         message:
 *           type: string
 *           description: Present on success.
 *           example: "Readings submitted successfully"
 *         count:
 *           type: integer
 *           description: Number of readings saved (present on success).
 *           example: 3
 *         error:
 *           type: string
 *           description: Error detail (present on failure only).
 *           example: "Device not found"
 *         readings:
 *           type: array
 *           description: The persisted Reading records (present on success).
 *           items:
 *             type: object
 *
 *     DevicePayloadCompact:
 *       type: object
 *       description: >
 *         A group of readings from one device in a compact format to minimize payload size.
 *       required:
 *         - deviceId
 *         - v
 *         - u
 *         - t
 *       properties:
 *         deviceId:
 *           type: string
 *           description: The unique identifier of the submitting device.
 *           example: "BIOBOT-001"
 *         v:
 *           type: array
 *           description: Array of numeric values.
 *           items: { type: number }
 *           example: [95.4, 12.1]
 *         u:
 *           type: array
 *           description: Array of units matching the values.
 *           items: { type: string }
 *           example: ["°C", "%"]
 *         t:
 *           type: array
 *           description: Array of reading types matching the values.
 *           items: { type: string }
 *           example: ["temperature", "humidity"]
 *         ts:
 *           type: array
 *           description: Array of optional ISO 8601 timestamps. Use null for current server time.
 *           items: { type: string, format: date-time, nullable: true }
 *           example: ["2026-04-18T14:22:00.000Z", null]
 *
 *     UltraCompactReadingTuple:
 *       type: array
 *       description: >
 *         A single reading within a snapshot: [typeIndex, value, optionalUnitOverride].
 *         typeIndex mapping: 0=temperature(°C), 1=humidity(%), 2=smoke(ppm), 3=co(ppm),
 *         4=pressure(hPa), 5=gas_resistance(kOhm), 6=altitude(m), 7=pm1(µg/m³),
 *         8=pm25(µg/m³), 9=pm10(µg/m³), 10=obstructed (unitless boolean 0/1),
 *         11=lightning_distance_km (km, only sent alongside a strike event),
 *         12=lightning_energy (unitless relative value, only sent alongside a
 *         strike event), 13=lightning_strike (unitless boolean 0/1),
 *         14=lightning_disturber (unitless boolean 0/1),
 *         15=iaq (0-500 index), 16=iaq_accuracy (0-3), 17=co2_equivalent
 *         (ppm, estimated from VOCs — not a direct CO2 measurement),
 *         18=breath_voc_equivalent (ppm), 19=gas_percentage (%),
 *         20=bsec_stabilization (0/1), 21=bsec_run_in (0/1).
 *         Codes 2 and 3 (smoke, co) are reserved for other device types and are
 *         not emitted by air-quality-monitor devices.
 *       minItems: 2
 *       items:
 *         oneOf:
 *           - type: integer
 *             description: Reading type index (see mapping above).
 *           - type: number
 *             description: The measurement value.
 *           - type: integer
 *             description: Optional unit-override code (rarely used; omitted in most readings).
 *       example: [0, 25.51, null]
 *
 *     UltraCompactSnapshot:
 *       type: array
 *       description: >
 *         One sensor-read cycle: [epochSeconds, readings]. A single epoch
 *         timestamp (Unix seconds) is shared by every reading captured in
 *         that cycle. For the lightning detector, a snapshot may be emitted
 *         asynchronously (outside the normal periodic cadence) whenever the
 *         AS3935 IRQ line fires, and will contain only the lightning_* codes
 *         rather than the full environmental reading set.
 *       minItems: 2
 *       maxItems: 2
 *       items:
 *         oneOf:
 *           - type: integer
 *             description: Unix epoch seconds when this snapshot was taken.
 *             example: 1785208895
 *           - type: array
 *             description: Reading tuples captured at this epoch.
 *             items:
 *               $ref: '#/components/schemas/UltraCompactReadingTuple'
 *
 *     DevicePayloadUltraCompact:
 *       type: array
 *       description: >
 *         A single device's data in ultra-compact format: [deviceId, snapshots].
 *         snapshots is an array of UltraCompactSnapshot entries, allowing a
 *         device to batch multiple timestamped sensor-read cycles (up to
 *         MAX_READINGS_BUFFER on-device) into a single submission.
 *       minItems: 2
 *       maxItems: 2
 *       items:
 *         oneOf:
 *           - type: string
 *             description: Device ID
 *             example: "sensor-001"
 *           - type: array
 *             description: List of snapshots, one per sensor-read cycle.
 *             items:
 *               $ref: '#/components/schemas/UltraCompactSnapshot'
 *
 *     StoredReading:
 *       type: object
 *       description: A persisted reading record as returned by GET /sensordata.
 *       properties:
 *         id:
 *           type: integer
 *           example: 42
 *         value:
 *           type: number
 *           example: 95.4
 *         unit:
 *           type: string
 *           example: "°C"
 *         readingType:
 *           type: string
 *           example: "temperature"
 *         timestamp:
 *           type: string
 *           format: date-time
 *           example: "2026-04-18T14:22:00.000Z"
 *         requestId:
 *           type: string
 *           format: uuid
 *           example: "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
 *         device:
 *           type: object
 *           properties:
 *             deviceId:
 *               type: string
 *               example: "BIOBOT-001"
 *             name:
 *               type: string
 *               example: "Ridge Station Alpha"
 */

/**
 * @swagger
 * /sensordata:
 *   post:
 *     summary: Submit sensor readings from one or more devices
 *     description: >
 *       Accepts a batch of device payloads in a single request. Each payload
 *       contains a deviceId and an array of readings. The server assigns a
 *       unique requestId (UUID v4) to each device payload for end-to-end
 *       traceability.
 *
 *
 *       Alert evaluation — after readings are persisted, each one is
 *       automatically compared against every enabled row in the
 *       alert_thresholds table. A reading can trigger multiple alerts if it
 *       crosses several thresholds (e.g. a temperature of 105C crosses both
 *       the > 80 high and > 100 critical rules simultaneously).
 *
 *
 *       Partial success — the batch continues even if one device payload
 *       fails. The response status reflects the overall outcome:
 *       201 all succeeded, 207 mixed, 400 all failed.
 *     tags: [Sensor Data]
 *     security:
 *       - bearerAuth: []
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required:
 *               - requests
 *             properties:
 *               requests:
 *                 type: array
 *                 minItems: 1
 *                 description: One entry per device submitting readings in this batch.
 *                 items:
 *                   $ref: '#/components/schemas/DevicePayload'
 *           example:
 *             requests:
 *               - deviceId: "BIOBOT-001"
 *                 readings:
 *                   - value: 95.4
 *                     unit: "°C"
 *                     readingType: "temperature"
 *                     timestamp: "2026-04-18T14:22:00.000Z"
 *                   - value: 12.1
 *                     unit: "%"
 *                     readingType: "humidity"
 *               - deviceId: "BIOBOT-002"
 *                 readings:
 *                   - value: 73.0
 *                     unit: "ppm"
 *                     readingType: "smoke"
 *     responses:
 *       201:
 *         description: All device payloads in the batch were processed successfully.
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 totalRequests:
 *                   type: integer
 *                   example: 2
 *                 successful:
 *                   type: integer
 *                   example: 2
 *                 failed:
 *                   type: integer
 *                   example: 0
 *                 results:
 *                   type: array
 *                   items:
 *                     $ref: '#/components/schemas/SubmitResult'
 *       207:
 *         description: >
 *           Multi-Status — at least one payload succeeded and at least one
 *           failed. Inspect each entry in results individually.
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 totalRequests:
 *                   type: integer
 *                   example: 2
 *                 successful:
 *                   type: integer
 *                   example: 1
 *                 failed:
 *                   type: integer
 *                   example: 1
 *                 results:
 *                   type: array
 *                   items:
 *                     $ref: '#/components/schemas/SubmitResult'
 *       400:
 *         description: >
 *           Bad request — either the top-level requests array is missing or
 *           malformed, or every device payload in the batch failed.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *             examples:
 *               missingRequests:
 *                 summary: Top-level requests array absent or not an array
 *                 value:
 *                   error: "Invalid request format. Expected { requests: [...] }"
 *               allFailed:
 *                 summary: Every payload failed (e.g. all device IDs unknown)
 *                 value:
 *                   totalRequests: 1
 *                   successful: 0
 *                   failed: 1
 *                   results:
 *                     - requestId: "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
 *                       success: false
 *                       error: "Device not found"
 *
 * /sensordata/compact:
 *   post:
 *     summary: Submit sensor readings in a compact format
 *     description: >
 *       An optimized version of the submission endpoint designed for
 *       bandwidth-constrained devices (e.g. Blues Note cards).
 *       Reduces JSON overhead by using arrays for reading components
 *       instead of repeating object keys.
 *     tags: [Sensor Data]
 *     security:
 *       - bearerAuth: []
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required:
 *               - requests
 *             properties:
 *               requests:
 *                 type: array
 *                 minItems: 1
 *                 items:
 *                   $ref: '#/components/schemas/DevicePayloadCompact'
 *           example:
 *             requests:
 *               - deviceId: "BIOBOT-001"
 *                 v: [95.4, 12.1]
 *                 u: ["°C", "%"]
 *                 t: ["temperature", "humidity"]
 *                 ts: ["2026-04-18T14:22:00.000Z", null]
 *     responses:
 *       201:
 *         description: All device payloads in the batch were processed successfully.
 *       207:
 *         description: Multi-Status — partial success.
 *       400:
 *         description: Bad request.
 *
 *       401:
 *         description: Unauthorized — missing or invalid authentication token.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *
 * /sensordata/ultra-compact:
 *   post:
 *     summary: Submit sensor readings in an ultra-compact format
 *     description: >
 *       The most optimized submission format for extremely bandwidth-constrained
 *       scenarios (e.g. Notecard cellular/LoRa devices). Uses a nested array
 *       structure, numeric type indices, and a single shared Unix-epoch
 *       timestamp per batch of readings ("snapshot") rather than one
 *       timestamp per value.
 *
 *
 *       Each device entry is `[deviceId, snapshots]`. Each snapshot is
 *       `[epochSeconds, readings]`, where `readings` is an array of
 *       `[typeIndex, value]` (or `[typeIndex, value, unitOverrideCode]`)
 *       tuples captured at that epoch. This lets a device buffer several
 *       sensor-read cycles on-device and submit them all in one request
 *       when it reconnects or on its regular sync interval.
 *
 *
 *       typeIndex mapping: 0=temperature(°C), 1=humidity(%), 2=smoke(ppm),
 *       3=co(ppm), 4=pressure(hPa), 5=gas_resistance(kOhm), 6=altitude(m),
 *       7=pm1(µg/m³), 8=pm25(µg/m³), 9=pm10(µg/m³), 10=obstructed (0/1 flag),
 *       11=lightning_distance_km (km), 12=lightning_energy (unitless relative
 *       value), 13=lightning_strike (0/1 flag), 14=lightning_disturber (0/1
 *       flag), 15=iaq (0-500 index), 16=iaq_accuracy (0-3), 17=co2_equivalent
 *       (ppm, estimated), 18=breath_voc_equivalent (ppm), 19=gas_percentage
 *       (%), 20=bsec_stabilization (0/1 flag), 21=bsec_run_in (0/1 flag).
 *       Codes 2 and 3 are reserved for other device types.
 *
 *
 *       Codes 15-21 come from the BME688's BSEC2 AI gas-sensing algorithm.
 *       15, 17, 18 and 19 are all derived from the same internal gas model and
 *       are only meaningful once it has calibrated: while the device reports
 *       16=iaq_accuracy as 0, those four readings are still stored but are NOT
 *       evaluated against alert thresholds, and the response reports how many
 *       were held back in `results[].alertsSuppressed`. Code 5
 *       (gas_resistance) is the raw heater measurement and is never gated.
 *
 *
 *       Lightning readings (11-14) typically arrive in their own snapshot,
 *       separate from the periodic environmental snapshot, since the AS3935
 *       reports events asynchronously via its IRQ line rather than on a
 *       fixed polling interval. A snapshot containing only [13, 0] and
 *       [14, 1], for example, represents a disturber event with no actual
 *       strike.
 *     tags: [Sensor Data]
 *     security:
 *       - bearerAuth: []
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required:
 *               - d
 *             properties:
 *               d:
 *                 type: array
 *                 minItems: 1
 *                 description: Array of [deviceId, snapshots] pairs, one entry per device.
 *                 items:
 *                   $ref: '#/components/schemas/DevicePayloadUltraCompact'
 *           example:
 *             d:
 *               - - "sensor-001"
 *                 - - - 1785208895
 *                     - - [0, 25.51]
 *                       - [1, 32.81]
 *                       - [4, 952.28]
 *                       - [5, 49.69]
 *                       - [6, 520.45]
 *                       - [7, 0]
 *                       - [8, 0]
 *                       - [9, 0]
 *                   - - 1785208897
 *                     - - [0, 25.50]
 *                       - [1, 32.82]
 *                       - [4, 952.27]
 *                       - [5, 52.76]
 *                       - [6, 520.54]
 *                       - [7, 0]
 *                       - [8, 0]
 *                       - [9, 0]
 *                   - - 1785208912
 *                     - - [13, 1]
 *                       - [14, 0]
 *                       - [11, 6]
 *                       - [12, 4823]
 *     responses:
 *       201:
 *         description: All payloads processed successfully.
 *       207:
 *         description: Partial success.
 *       400:
 *         description: >
 *           Bad request — either the top-level `d` array is missing/malformed,
 *           a device entry does not match [deviceId, snapshots], a snapshot
 *           does not match [epoch, readings], or a reading tuple does not
 *           match [typeIndex, value].
 */

/**
 * Evaluate a single reading value against a threshold operator.
 * Returns true if the reading satisfies the threshold condition.
 */
function exceedsThreshold(readingValue, operator, thresholdValue) {
  const v = parseFloat(readingValue);
  const t = parseFloat(thresholdValue);
  switch (operator) {
    case '>':  return v > t;
    case '>=': return v >= t;
    case '<':  return v < t;
    case '<=': return v <= t;
    default:   return false;
  }
}

/**
 * Interpolate {value} and {unit} tokens in a threshold message template.
 * Example: "High temperature detected: {value}{unit}" → "High temperature detected: 95°C"
 */
function formatMessage(template, value, unit) {
  return template
      .replace('{value}', value)
      .replace('{unit}', unit ?? '');
}

async function processSensorData(requests, res) {
  // Loaded once per batch — the table is small and shared across all readings.
  const thresholds = await AlertThreshold.findAll({ where: { enabled: true } });

  const results = [];

  for (const request of requests) {
    const requestId = uuid4();
    const { deviceId, readings } = request;

    try {
      if (!deviceId || !readings || !Array.isArray(readings)) {
        results.push({
          requestId,
          success: false,
          error: 'Invalid request format: missing deviceId or readings'
        });
        continue;
      }

      const device = await Device.findOne({ where: { deviceId } });
      if (!device) {
        results.push({
          requestId,
          success: false,
          error: 'Device not found'
        });
        continue;
      }

      await device.update({ lastSeen: new Date() });

      let suppressedAlertReadings = 0;

      const createdReadings = await Promise.all(
          readings.map(reading =>
              Reading.create({
                deviceId: device.id,
                value: reading.value,
                unit: reading.unit,
                readingType: reading.readingType,
                timestamp: reading.timestamp || new Date(),
                requestId
              })
          )
      );

      // createdReadings is index-aligned with `readings`, so the input row
      // supplies the per-reading `skipAlerts` flag the Sequelize instance lacks.
      for (let i = 0; i < createdReadings.length; i++) {
        const reading = createdReadings[i];

        if (readings[i] && readings[i].skipAlerts) {
          suppressedAlertReadings++;
          continue;
        }

        const matchingThresholds = thresholds.filter(
            t => t.readingType.toLowerCase() === reading.readingType.toLowerCase()
        );

        for (const threshold of matchingThresholds) {
          if (exceedsThreshold(reading.value, threshold.operator, threshold.thresholdValue)) {
            await Alert.create({
              deviceId: device.id,
              severity: threshold.severity,
              message: formatMessage(threshold.message, reading.value, reading.unit),
              triggeredAt: new Date(),
              requestId
            });
          }
        }
      }

      results.push({
        requestId,
        success: true,
        message: 'Readings submitted successfully',
        count: createdReadings.length,
        // Readings stored but not threshold-checked because the sensor was
        // still calibrating.
        alertsSuppressed: suppressedAlertReadings,
        readings: createdReadings
      });
    } catch (error) {
      results.push({
        requestId,
        success: false,
        error: error.message
      });
    }
  }

  const successCount = results.filter(r => r.success).length;
  const statusCode = successCount === results.length ? 201 :
      successCount > 0 ? 207 : 400;

  return res.status(statusCode).json({
    totalRequests: results.length,
    successful: successCount,
    failed: results.length - successCount,
    results
  });
}

router.post('/sensordata', authenticate, validateSensorData, async (req, res) => {
  try {
    const { requests } = req.body;

    if (!requests || !Array.isArray(requests)) {
      return res.status(400).json({
        error: 'Invalid request format. Expected { requests: [...] }'
      });
    }

    return await processSensorData(requests, res);
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

router.post('/sensordata/compact', authenticate, validateSensorDataCompact, async (req, res) => {
  try {
    const { requests } = req.body;

    if (!requests || !Array.isArray(requests)) {
      return res.status(400).json({
        error: 'Invalid request format. Expected { requests: [...] }'
      });
    }

    const expandedRequests = requests.map(req => {
      const { deviceId, v, u, t, ts } = req;
      const readings = v.map((value, i) => ({
        value,
        unit: u[i],
        readingType: t[i],
        timestamp: ts ? ts[i] : null
      }));
      return { deviceId, readings };
    });

    return await processSensorData(expandedRequests, res);
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

router.post('/sensordata/ultra-compact', authenticate, validateSensorDataUltraCompact, async (req, res) => {
  try {
    const { d } = req.body;

    if (!d || !Array.isArray(d)) {
      return res.status(400).json({
        error: 'Invalid request format. Expected { d: [...] }'
      });
    }

    // d = [ [deviceId, [ [epochSeconds, [ [typeIndex, value, unitOverride?], ... ]], ... ]], ... ]
    const expandedRequests = d.map(entry => {
      const [deviceId, snapshots] = entry;

      const expandedReadings = [];

      for (const snapshot of snapshots) {
        const [epoch, readingPairs] = snapshot;
        const timestamp = new Date(epoch > 10000000000 ? epoch : epoch * 1000);

        // Read before the loop: it decides whether this snapshot's BSEC
        // outputs are calibrated enough to raise alerts.
        const accuracyPair = readingPairs.find(p => Array.isArray(p) && p[0] === IAQ_ACCURACY_CODE);
        const iaqAccuracy = accuracyPair ? accuracyPair[1] : null;

        for (const pair of readingPairs) {
          const [typeIdx, value, unitOverrideIdx] = pair;
          const mapping = ULTRA_COMPACT_MAPPING[typeIdx] || UNKNOWN_MAPPING;
          const unit = unitOverrideIdx !== undefined && unitOverrideIdx !== null
              ? (UNIT_OVERRIDE_CODES[unitOverrideIdx] ?? mapping.unit)
              : mapping.unit;

          expandedReadings.push({
            value,
            unit,
            readingType: mapping.type,
            timestamp,
            skipAlerts: !isBsecOutputTrustworthy(mapping.type, iaqAccuracy)
          });
        }
      }

      return { deviceId, readings: expandedReadings };
    });

    return await processSensorData(expandedRequests, res);
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

/**
 * @swagger
 * /sensordata:
 *   get:
 *     summary: Query stored sensor readings
 *     description: >
 *       Returns a list of readings filtered by any combination of the query
 *       parameters below. All filters are optional and AND-combined. Results
 *       include the parent device (deviceId and name) via a JOIN.
 *
 *
 *       Results are capped at the `limit` value (default 100). For large
 *       datasets use `startDate` / `endDate` to narrow the window, or
 *       `requestId` to retrieve exactly one submission batch.
 *     tags: [Sensor Data]
 *     security:
 *       - bearerAuth: []
 *     parameters:
 *       - in: query
 *         name: deviceId
 *         schema:
 *           type: string
 *         description: Filter by device identifier (e.g. "BIOBOT-001"). Returns 404 if the device does not exist.
 *         example: "BIOBOT-001"
 *       - in: query
 *         name: requestId
 *         schema:
 *           type: string
 *           format: uuid
 *         description: Filter by the server-assigned request UUID to retrieve all readings from a single submission batch.
 *         example: "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
 *       - in: query
 *         name: readingType
 *         schema:
 *           type: string
 *         description: Filter by sensor type (e.g. "temperature", "humidity", "smoke", "co", "lightning_strike").
 *         example: "temperature"
 *       - in: query
 *         name: startDate
 *         schema:
 *           type: string
 *           format: date-time
 *         description: Return only readings at or after this ISO 8601 timestamp (inclusive).
 *         example: "2026-04-18T00:00:00.000Z"
 *       - in: query
 *         name: endDate
 *         schema:
 *           type: string
 *           format: date-time
 *         description: Return only readings at or before this ISO 8601 timestamp (inclusive).
 *         example: "2026-04-18T23:59:59.999Z"
 *       - in: query
 *         name: sortBy
 *         schema:
 *           type: string
 *           enum: [timestamp, value, readingType, id]
 *           default: timestamp
 *         description: Field to sort results by. Invalid values silently fall back to timestamp.
 *       - in: query
 *         name: sortOrder
 *         schema:
 *           type: string
 *           enum: [ASC, DESC]
 *           default: DESC
 *         description: Sort direction. Case-insensitive; invalid values default to DESC.
 *       - in: query
 *         name: limit
 *         schema:
 *           type: integer
 *           default: 100
 *           minimum: 1
 *         description: Maximum number of readings to return.
 *         example: 50
 *     responses:
 *       200:
 *         description: Successfully retrieved readings matching the applied filters.
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 count:
 *                   type: integer
 *                   description: Number of readings returned.
 *                   example: 3
 *                 filters:
 *                   type: object
 *                   description: Echo of the filters that were applied, including resolved sort field and direction.
 *                   properties:
 *                     deviceId:
 *                       type: string
 *                       nullable: true
 *                       example: "BIOBOT-001"
 *                     requestId:
 *                       type: string
 *                       nullable: true
 *                       example: null
 *                     readingType:
 *                       type: string
 *                       nullable: true
 *                       example: "temperature"
 *                     startDate:
 *                       type: string
 *                       nullable: true
 *                       example: null
 *                     endDate:
 *                       type: string
 *                       nullable: true
 *                       example: null
 *                     sortBy:
 *                       type: string
 *                       example: "timestamp"
 *                     sortOrder:
 *                       type: string
 *                       example: "DESC"
 *                 readings:
 *                   type: array
 *                   items:
 *                     $ref: '#/components/schemas/StoredReading'
 *       401:
 *         description: Unauthorized — missing or invalid authentication token.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *       404:
 *         description: The specified deviceId does not exist.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *             example:
 *               error: "Device not found"
 *       500:
 *         description: Internal server error.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 */
router.get('/sensordata', authenticate, async (req, res) => {
  try {
    const {
      deviceId,
      limit = 100,
      requestId,
      readingType,
      startDate,
      endDate,
      sortBy = 'timestamp',
      sortOrder = 'DESC'
    } = req.query;

    const where = {};

    if (deviceId) {
      const device = await Device.findOne({ where: { deviceId } });
      if (!device) {
        return res.status(404).json({ error: 'Device not found' });
      }
      where.deviceId = device.id;
    }

    if (requestId) {
      where.requestId = requestId;
    }

    if (readingType) {
      where.readingType = readingType;
    }

    if (startDate || endDate) {
      where.timestamp = {};
      if (startDate) where.timestamp[require('sequelize').Op.gte] = new Date(startDate);
      if (endDate) where.timestamp[require('sequelize').Op.lte] = new Date(endDate);
    }

    const validSortFields = ['timestamp', 'value', 'readingType', 'id'];
    const orderField = validSortFields.includes(sortBy) ? sortBy : 'timestamp';
    const orderDirection = sortOrder.toUpperCase() === 'ASC' ? 'ASC' : 'DESC';

    const readings = await Reading.findAll({
      where,
      order: [[orderField, orderDirection]],
      limit: parseInt(limit, 10),
      include: [{ model: Device, as: 'device', attributes: ['deviceId', 'name'] }]
    });

    res.json({
      count: readings.length,
      filters: {
        deviceId,
        requestId,
        readingType,
        startDate,
        endDate,
        sortBy: orderField,
        sortOrder: orderDirection
      },
      readings
    });
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

/**
 * @swagger
 * /sensordata:
 *   delete:
 *     summary: Delete sensor readings
 *     description: >
 *       Deletes readings by either a single `readingId` or all readings for a
 *       given `deviceId`. Exactly one of the two parameters must be provided.
 *
 *
 *       Deleting by `deviceId` is a bulk operation — it removes every reading
 *       ever recorded for that device. This cannot be undone.
 *     tags: [Sensor Data]
 *     security:
 *       - bearerAuth: []
 *     parameters:
 *       - in: query
 *         name: readingId
 *         schema:
 *           type: integer
 *         description: >
 *           Delete a single reading by its primary key.
 *           Takes precedence over deviceId if both are supplied.
 *         example: 42
 *       - in: query
 *         name: deviceId
 *         schema:
 *           type: string
 *         description: >
 *           Delete ALL readings for this device. The device record itself is
 *           not deleted — only its associated readings.
 *         example: "BIOBOT-001"
 *     responses:
 *       200:
 *         description: Readings deleted successfully.
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 message:
 *                   type: string
 *             examples:
 *               byReadingId:
 *                 summary: Single reading deleted
 *                 value:
 *                   message: "Reading deleted successfully."
 *               byDeviceId:
 *                 summary: All readings for a device deleted
 *                 value:
 *                   message: "Deleted 47 readings for device BIOBOT-001."
 *       400:
 *         description: Neither deviceId nor readingId was provided.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *             example:
 *               error: "Provide deviceId or readingId to delete."
 *       401:
 *         description: Unauthorized — missing or invalid authentication token.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *       404:
 *         description: The specified readingId or deviceId was not found.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *             examples:
 *               readingNotFound:
 *                 summary: readingId does not exist
 *                 value:
 *                   error: "Reading not found."
 *               deviceNotFound:
 *                 summary: deviceId does not exist
 *                 value:
 *                   error: "Device not found."
 *       500:
 *         description: Internal server error.
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 */
router.delete('/sensordata', authenticate, async (req, res) => {
  try {
    const { deviceId, readingId } = req.query;

    if (!deviceId && !readingId) {
      return res.status(400).json({ error: 'Provide deviceId or readingId to delete.' });
    }

    if (readingId) {
      const reading = await Reading.findByPk(readingId);
      if (!reading) return res.status(404).json({ error: 'Reading not found.' });

      if (req.user.role !== 'admin') {
        const device = await Device.findByPk(reading.deviceId);
        if (!device) {
          return res.status(404).json({ error: 'Device not found.' });
        }
        if (device.createdBy === null) {
          return res.status(403).json({
            error: 'Forbidden: this device has no registered owner. Contact an administrator.'
          });
        }
        if (device.createdBy !== req.user.id) {
          return res.status(403).json({
            error: 'Forbidden: you do not have permission to delete readings for this device.'
          });
        }
      }

      await reading.destroy();
      return res.json({ message: 'Reading deleted successfully.' });
    }

    const device = await Device.findOne({ where: { deviceId } });
    if (!device) return res.status(404).json({ error: 'Device not found.' });

    if (req.user.role !== 'admin') {
      if (device.createdBy === null) {
        return res.status(403).json({
          error: 'Forbidden: this device has no registered owner. Contact an administrator.'
        });
      }
      if (device.createdBy !== req.user.id) {
        return res.status(403).json({
          error: 'Forbidden: you do not have permission to delete readings for this device.'
        });
      }
    }

    const deletedCount = await Reading.destroy({ where: { deviceId: device.id } });
    res.json({ message: `Deleted ${deletedCount} readings for device ${deviceId}.` });
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

module.exports = router;