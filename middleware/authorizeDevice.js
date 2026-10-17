const { Device } = require('../models');

/**
 * Middleware factory for device-level authorization.
 *
 * Usage:
 *   router.delete('/devices/:id', authenticate, authorizeDevice('id'), handler)
 *   router.delete('/sensordata',  authenticate, authorizeDevice('deviceId', 'query', 'deviceId'), handler)
 *
 * @param {string} paramName     - Name of the param that identifies the device.
 * @param {string} paramSource   - Where to find it: 'params' (default), 'query', or 'body'.
 * @param {string} paramType     - 'id'  → look up by Device.id (PK)
 *                                 'deviceId' → look up by Device.deviceId (string UID)
 *                                 Defaults to 'id'.
 */
const authorizeDevice = (paramName = 'id', paramSource = 'params', paramType = 'id') => {
    return async (req, res, next) => {
        try {
            if (req.user.role === 'admin') {
                return next();
            }

            const value = req[paramSource][paramName];

            if (!value) {
                return next();
            }

            const whereClause = paramType === 'deviceId'
                ? { deviceId: value }
                : { id: value };

            const device = await Device.findOne({ where: whereClause });

            if (!device) {
                return next();
            }

            // Legacy rows have no owner, so deny non-admins rather than assume access
            if (device.createdBy === null) {
                return res.status(403).json({
                    error: 'Forbidden: this device has no registered owner. Contact an administrator.'
                });
            }

            if (device.createdBy !== req.user.id) {
                return res.status(403).json({
                    error: 'Forbidden: you do not have permission to modify this device.'
                });
            }

            req.device = device;
            next();
        } catch (error) {
            res.status(500).json({ error: error.message });
        }
    };
};

module.exports = { authorizeDevice };