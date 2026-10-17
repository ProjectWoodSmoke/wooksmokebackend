const { validationResult, body, param } = require('express-validator');

/**
 * Runs express-validator results and returns 422 with structured errors
 * if any validation rule failed. Place this after your validation chains.
 */
const handleValidationErrors = (req, res, next) => {
    const errors = validationResult(req);
    if (!errors.isEmpty()) {
        return res.status(422).json({
            error: 'Validation failed',
            details: errors.array().map(e => ({ field: e.path, message: e.msg }))
        });
    }
    next();
};

/** Safe plain-text string: printable chars only, no angle brackets or control chars */
const safeName = (field, maxLen = 100) =>
    body(field)
        .trim()
        .notEmpty().withMessage(`${field} is required`)
        .isLength({ max: maxLen }).withMessage(`${field} must be at most ${maxLen} characters`)
        .matches(/^[^<>"'%;()&+\x00-\x1F\x7F]+$/)
        .withMessage(`${field} contains invalid characters`);

const optionalSafeText = (field, maxLen = 500) =>
    body(field)
        .optional({ nullable: true, checkFalsy: false })
        .trim()
        .isLength({ max: maxLen }).withMessage(`${field} must be at most ${maxLen} characters`)
        .matches(/^[^<>"'%;()&+\x00-\x1F\x7F]*$/)
        .withMessage(`${field} contains invalid characters`);

const validateRegister = [
    body('username')
        .trim()
        .notEmpty().withMessage('username is required')
        .isLength({ min: 3, max: 50 }).withMessage('username must be 3–50 characters')
        .matches(/^[a-zA-Z0-9_-]+$/).withMessage('username may only contain letters, numbers, underscores, and hyphens'),

    body('email')
        .trim()
        .notEmpty().withMessage('email is required')
        .isEmail().withMessage('email must be a valid email address')
        .isLength({ max: 254 }).withMessage('email must be at most 254 characters')
        .normalizeEmail(),

    body('password')
        .notEmpty().withMessage('password is required')
        .isLength({ min: 8, max: 128 }).withMessage('password must be 8–128 characters'),

    handleValidationErrors
];

const validateLogin = [
    body('email')
        .optional({ nullable: true })
        .trim()
        .isEmail().withMessage('email must be a valid email address')
        .normalizeEmail(),

    body('username')
        .optional({ nullable: true })
        .trim()
        .isLength({ max: 50 }).withMessage('username must be at most 50 characters')
        .matches(/^[a-zA-Z0-9_-]+$/).withMessage('username contains invalid characters'),

    body('password')
        .notEmpty().withMessage('password is required')
        .isLength({ max: 128 }).withMessage('password must be at most 128 characters'),

    handleValidationErrors
];

const validateCreateApiKey = [
    safeName('name', 100),
    optionalSafeText('description', 500),
    handleValidationErrors
];

const validateCreateDevice = [
    body('deviceId')
        .trim()
        .notEmpty().withMessage('deviceId is required')
        .isLength({ max: 100 }).withMessage('deviceId must be at most 100 characters')
        .matches(/^[a-zA-Z0-9_:-]+$/).withMessage('deviceId may only contain letters, numbers, underscores, colons, and hyphens'),

    safeName('name', 100),

    body('type')
        .trim()
        .notEmpty().withMessage('type is required')
        .isLength({ max: 50 }).withMessage('type must be at most 50 characters')
        .matches(/^[a-zA-Z0-9_-]+$/).withMessage('type may only contain letters, numbers, underscores, and hyphens'),

    body('locationId')
        .optional({ nullable: true })
        .isInt({ min: 1 }).withMessage('locationId must be a positive integer'),

    body('status')
        .optional({ nullable: true })
        .isIn(['active', 'inactive', 'maintenance', 'hibernation'])
        .withMessage('status must be one of: active, inactive, maintenance, hibernation'),

    handleValidationErrors
];

const validatePatchDeviceStatus = [
    param('id')
        .isInt({ min: 1 }).withMessage('Device id must be a positive integer'),

    body('status')
        .trim()
        .notEmpty().withMessage('status is required')
        .isIn(['active', 'inactive', 'maintenance', 'hibernation'])
        .withMessage('status must be one of: active, inactive, maintenance, hibernation'),

    handleValidationErrors
];

const validateAcknowledgeAlert = [
    param('id')
        .isInt({ min: 1 }).withMessage('Alert id must be a positive integer'),

    optionalSafeText('notes', 1000),

    handleValidationErrors
];

/**
 * Matches the firmware's batched send format:
 *   { "d": [ [deviceId, [ [epochSeconds, [ [typeIndex, value], ... ]], ... ]], ... ] }
 *
 * Readings taken in the same sensor-read cycle share one snapshot timestamp.
 * A reading pair may carry a third unit-override element, which the mapping
 * layer handles and this chain does not require.
 */
const validateSensorDataUltraCompact = [
    body('d')
        .isArray({ min: 1 }).withMessage('d (data) must be a non-empty array'),

    body('d.*')
        .isArray({ min: 2, max: 2 }).withMessage('each device entry in d must be an array of [deviceId, snapshots]'),

    body('d.*.0')
        .isString().trim().notEmpty().withMessage('deviceId must be a string'),

    body('d.*.1')
        .isArray({ min: 1 }).withMessage('snapshots must be a non-empty array'),

    body('d.*.1.*')
        .isArray({ min: 2, max: 2 }).withMessage('each snapshot must be an array of [epoch, readings]'),

    body('d.*.1.*.0')
        .isInt({ min: 0 }).withMessage('snapshot timestamp must be a non-negative integer (Unix timestamp)'),

    body('d.*.1.*.1')
        .isArray({ min: 1 }).withMessage('snapshot readings must be a non-empty array'),

    body('d.*.1.*.1.*')
        .isArray({ min: 2 }).withMessage('each reading must be an array of at least [typeIndex, value]'),

    body('d.*.1.*.1.*.0')
        .isInt({ min: 0 }).withMessage('reading type index must be a non-negative integer'),

    body('d.*.1.*.1.*.1')
        .isNumeric().withMessage('reading value must be a number'),

    handleValidationErrors
];

const validateSensorData = [
    body('requests')
        .isArray({ min: 1 }).withMessage('requests must be a non-empty array'),

    body('requests.*.deviceId')
        .trim()
        .notEmpty().withMessage('each request must have a deviceId')
        .isLength({ max: 100 }).withMessage('deviceId must be at most 100 characters')
        .matches(/^[a-zA-Z0-9_:-]+$/).withMessage('deviceId contains invalid characters'),

    body('requests.*.readings')
        .isArray({ min: 1 }).withMessage('each request must have a non-empty readings array'),

    body('requests.*.readings.*.value')
        .isFloat().withMessage('reading value must be a number'),

    body('requests.*.readings.*.unit')
        .trim()
        .notEmpty().withMessage('reading unit is required')
        .isLength({ max: 20 }).withMessage('unit must be at most 20 characters')
        .matches(/^[^\x00-\x1F\x7F<>"]+$/).withMessage('unit contains invalid characters'),

    body('requests.*.readings.*.readingType')
        .trim()
        .notEmpty().withMessage('readingType is required')
        .isLength({ max: 50 }).withMessage('readingType must be at most 50 characters')
        .matches(/^[a-zA-Z0-9_-]+$/).withMessage('readingType contains invalid characters'),

    body('requests.*.readings.*.timestamp')
        .optional({ nullable: true })
        .isISO8601().withMessage('timestamp must be a valid ISO 8601 date-time'),

    handleValidationErrors
];

const validateSensorDataCompact = [
    body('requests')
        .isArray({ min: 1 }).withMessage('requests must be a non-empty array'),

    body('requests.*.deviceId')
        .trim()
        .notEmpty().withMessage('each request must have a deviceId')
        .isLength({ max: 100 }).withMessage('deviceId must be at most 100 characters')
        .matches(/^[a-zA-Z0-9_:-]+$/).withMessage('deviceId contains invalid characters'),

    body('requests.*.v')
        .isArray({ min: 1 }).withMessage('v (values) must be a non-empty array'),

    body('requests.*.u')
        .isArray({ min: 1 }).withMessage('u (units) must be a non-empty array'),

    body('requests.*.t')
        .isArray({ min: 1 }).withMessage('t (types) must be a non-empty array'),

    body('requests.*.ts')
        .optional()
        .isArray().withMessage('ts (timestamps) must be an array'),

    handleValidationErrors
];

module.exports = {
    validateRegister,
    validateLogin,
    validateCreateApiKey,
    validateCreateDevice,
    validatePatchDeviceStatus,
    validateAcknowledgeAlert,
    validateSensorData,
    validateSensorDataCompact,
    validateSensorDataUltraCompact
};