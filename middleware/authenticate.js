const jwt = require('jsonwebtoken');
const bcrypt = require('bcrypt');
const { User, ApiKey } = require('../models');

const JWT_SECRET = process.env.JWT_SECRET || 'your-secret-key-change-in-production';

const authenticate = async (req, res, next) => {
  try {
    const authHeader = req.headers.authorization;

    if (!authHeader) {
      return res.status(401).json({ error: 'No authorization header' });
    }

    const token = authHeader.replace('Bearer ', '');

    if (token.startsWith('sk_')) {

      const prefix = token.substring(0, 8);
      const candidate = await ApiKey.findOne({
        where: { prefix },
        include: [{ model: User, as: 'user' }]
      });

      if (!candidate || !(await bcrypt.compare(token, candidate.key))) {
        return res.status(401).json({ error: 'Invalid API key' });
      }

      const matchedApiKey = candidate;

      await matchedApiKey.update({ lastUsedAt: new Date() });

      req.user = matchedApiKey.user;
      req.authType = 'apikey';
      return next();
    } else {
      const decoded = jwt.verify(token, JWT_SECRET);
      const user = await User.findByPk(decoded.id);

      if (!user) {
        return res.status(401).json({ error: 'User not found' });
      }

      req.user = user;
      req.authType = 'jwt';
      return next();
    }
  } catch (error) {
    if (error.name === 'JsonWebTokenError') {
      return res.status(401).json({ error: 'Invalid token' });
    }
    if (error.name === 'TokenExpiredError') {
      return res.status(401).json({ error: 'Token expired' });
    }
    return res.status(500).json({ error: error.message });
  }
};

module.exports = { authenticate, JWT_SECRET };