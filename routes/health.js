const express = require('express');
const router = express.Router();
const { Op } = require('sequelize');
const ApiHealth = require('../models/ApiHealth');
const sequelize = require('../config/database');

/**
 * @swagger
 * tags:
 *   name: Health
 *   description: API health monitoring and statistics
 */

/**
 * @swagger
 * components:
 *   schemas:
 *     HealthStats:
 *       type: object
 *       properties:
 *         totalRequests:
 *           type: integer
 *         successfulRequests:
 *           type: integer
 *         failedRequests:
 *           type: integer
 *         serverErrors:
 *           type: integer
 *         clientErrors:
 *           type: integer
 *         errorRate:
 *           type: number
 *         serverErrorRate:
 *           type: number
 *         averageLatency:
 *           type: integer
 *           description: Average response time in ms
 *         minLatency:
 *           type: integer
 *         maxLatency:
 *           type: integer
 *         p95Latency:
 *           type: integer
 *           description: 95th percentile response time in ms
 *         p99Latency:
 *           type: integer
 *           description: 99th percentile response time in ms
 *         timeRange:
 *           type: string
 *         startTime:
 *           type: string
 *           format: date-time
 *         endTime:
 *           type: string
 *           format: date-time
 *     EndpointStat:
 *       type: object
 *       properties:
 *         endpoint:
 *           type: string
 *         method:
 *           type: string
 *         requestCount:
 *           type: integer
 *         avgLatency:
 *           type: integer
 *         minLatency:
 *           type: integer
 *         maxLatency:
 *           type: integer
 *         errorCount:
 *           type: integer
 *         serverErrorCount:
 *           type: integer
 *         errorRate:
 *           type: number
 *         serverErrorRate:
 *           type: number
 *     LatencyTrend:
 *       type: object
 *       properties:
 *         timeSlot:
 *           type: string
 *           example: "2024-01-15 14:00:00"
 *         avgLatency:
 *           type: integer
 *         requestCount:
 *           type: integer
 *         errorCount:
 *           type: integer
 *         serverErrorCount:
 *           type: integer
 *         errorRate:
 *           type: number
 *         serverErrorRate:
 *           type: number
 */

/**
 * @swagger
 * /api/health/stats:
 *   get:
 *     summary: Get overall API health statistics
 *     description: Returns aggregate request counts, error rates, and latency percentiles for a given time range.
 *     tags: [Health]
 *     parameters:
 *       - in: query
 *         name: timeRange
 *         schema:
 *           type: string
 *           enum: [1h, 24h, 7d, 30d]
 *           default: 24h
 *         description: Time window to aggregate stats over
 *     responses:
 *       200:
 *         description: Health statistics returned successfully
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/HealthStats'
 *       500:
 *         description: Failed to fetch API health statistics
 */

router.get('/api/health/stats', async (req, res) => {
  try {
    const { timeRange = '24h' } = req.query;
    
    const now = new Date();
    let startTime = new Date();
    
    switch(timeRange) {
      case '1h':
        startTime.setHours(now.getHours() - 1);
        break;
      case '24h':
        startTime.setHours(now.getHours() - 24);
        break;
      case '7d':
        startTime.setDate(now.getDate() - 7);
        break;
      case '30d':
        startTime.setDate(now.getDate() - 30);
        break;
      default:
        startTime.setHours(now.getHours() - 24);
    }

    const requests = await ApiHealth.findAll({
      where: {
        timestamp: {
          [Op.gte]: startTime
        }
      }
    });

    if (requests.length === 0) {
      return res.json({
        totalRequests: 0,
        successfulRequests: 0,
        failedRequests: 0,
        serverErrors: 0,
        clientErrors: 0,
        errorRate: 0,
        serverErrorRate: 0,
        averageLatency: 0,
        minLatency: 0,
        maxLatency: 0,
        p95Latency: 0,
        p99Latency: 0,
        timeRange,
        startTime,
        endTime: now
      });
    }

    const totalRequests = requests.length;
    const successfulRequests = requests.filter(r => r.statusCode >= 200 && r.statusCode < 400).length;
    const failedRequests = totalRequests - successfulRequests;
    const serverErrors = requests.filter(r => r.isServerError).length;
    const clientErrors = requests.filter(r => r.statusCode >= 400 && r.statusCode < 500).length;
    const errorRate = ((failedRequests / totalRequests) * 100).toFixed(2);
    const serverErrorRate = ((serverErrors / totalRequests) * 100).toFixed(2);

    const latencies = requests.map(r => r.latency).sort((a, b) => a - b);
    const averageLatency = Math.round(latencies.reduce((a, b) => a + b, 0) / latencies.length);
    const minLatency = latencies[0];
    const maxLatency = latencies[latencies.length - 1];
    const p95Index = Math.floor(latencies.length * 0.95);
    const p99Index = Math.floor(latencies.length * 0.99);
    const p95Latency = latencies[p95Index] || maxLatency;
    const p99Latency = latencies[p99Index] || maxLatency;

    res.json({
      totalRequests,
      successfulRequests,
      failedRequests,
      serverErrors,
      clientErrors,
      errorRate: parseFloat(errorRate),
      serverErrorRate: parseFloat(serverErrorRate),
      averageLatency,
      minLatency,
      maxLatency,
      p95Latency,
      p99Latency,
      timeRange,
      startTime,
      endTime: now
    });

  } catch (error) {
    console.error('Error fetching API health stats:', error);
    res.status(500).json({ error: 'Failed to fetch API health statistics' });
  }
});
/**
 * @swagger
 * /api/health/endpoints:
 *   get:
 *     summary: Get per-endpoint health statistics
 *     description: Returns request count, latency, and error rates broken down by each endpoint and HTTP method.
 *     tags: [Health]
 *     parameters:
 *       - in: query
 *         name: timeRange
 *         schema:
 *           type: string
 *           enum: [1h, 24h, 7d, 30d]
 *           default: 24h
 *         description: Time window to aggregate stats over
 *     responses:
 *       200:
 *         description: Per-endpoint statistics returned successfully
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 endpoints:
 *                   type: array
 *                   items:
 *                     $ref: '#/components/schemas/EndpointStat'
 *                 timeRange:
 *                   type: string
 *                 startTime:
 *                   type: string
 *                   format: date-time
 *                 endTime:
 *                   type: string
 *                   format: date-time
 *       500:
 *         description: Failed to fetch endpoint statistics
 */

router.get('/api/health/endpoints', async (req, res) => {
  try {
    const { timeRange = '24h' } = req.query;
    
    const now = new Date();
    let startTime = new Date();
    
    switch(timeRange) {
      case '1h':
        startTime.setHours(now.getHours() - 1);
        break;
      case '24h':
        startTime.setHours(now.getHours() - 24);
        break;
      case '7d':
        startTime.setDate(now.getDate() - 7);
        break;
      case '30d':
        startTime.setDate(now.getDate() - 30);
        break;
      default:
        startTime.setHours(now.getHours() - 24);
    }

    const endpointStats = await ApiHealth.findAll({
      attributes: [
        'endpoint',
        'method',
        [sequelize.fn('COUNT', sequelize.col('id')), 'requestCount'],
        [sequelize.fn('AVG', sequelize.col('latency')), 'avgLatency'],
        [sequelize.fn('MIN', sequelize.col('latency')), 'minLatency'],
        [sequelize.fn('MAX', sequelize.col('latency')), 'maxLatency'],
        [sequelize.fn('SUM', sequelize.literal(
          "CASE WHEN statusCode >= 400 THEN 1 ELSE 0 END"
        )), 'errorCount'],
        [sequelize.fn('SUM', sequelize.literal(
          "CASE WHEN isServerError = 1 THEN 1 ELSE 0 END"
        )), 'serverErrorCount']
      ],
      where: {
        timestamp: {
          [Op.gte]: startTime
        }
      },
      group: ['endpoint', 'method'],
      order: [[sequelize.fn('COUNT', sequelize.col('id')), 'DESC']]
    });

    const formattedStats = endpointStats.map(stat => {
      const data = stat.get({ plain: true });
      const errorRate = data.requestCount > 0 
        ? ((data.errorCount / data.requestCount) * 100).toFixed(2)
        : 0;
      const serverErrorRate = data.requestCount > 0 
        ? ((data.serverErrorCount / data.requestCount) * 100).toFixed(2)
        : 0;
      
      return {
        endpoint: data.endpoint,
        method: data.method,
        requestCount: parseInt(data.requestCount),
        avgLatency: Math.round(parseFloat(data.avgLatency)),
        minLatency: parseInt(data.minLatency),
        maxLatency: parseInt(data.maxLatency),
        errorCount: parseInt(data.errorCount),
        serverErrorCount: parseInt(data.serverErrorCount),
        errorRate: parseFloat(errorRate),
        serverErrorRate: parseFloat(serverErrorRate)
      };
    });

    res.json({
      endpoints: formattedStats,
      timeRange,
      startTime,
      endTime: now
    });

  } catch (error) {
    console.error('Error fetching endpoint stats:', error);
    res.status(500).json({ error: 'Failed to fetch endpoint statistics' });
  }
});
/**
 * @swagger
 * /api/health/errors:
 *   get:
 *     summary: Get recent API errors
 *     description: Returns a list of recent 4xx/5xx requests and a breakdown of error counts by status code.
 *     tags: [Health]
 *     parameters:
 *       - in: query
 *         name: timeRange
 *         schema:
 *           type: string
 *           enum: [1h, 24h, 7d, 30d]
 *           default: 24h
 *         description: Time window to look back over
 *       - in: query
 *         name: limit
 *         schema:
 *           type: integer
 *           default: 50
 *         description: Maximum number of recent errors to return
 *     responses:
 *       200:
 *         description: Error data returned successfully
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 recentErrors:
 *                   type: array
 *                   items:
 *                     type: object
 *                 errorsByStatus:
 *                   type: array
 *                   items:
 *                     type: object
 *                     properties:
 *                       statusCode:
 *                         type: integer
 *                       count:
 *                         type: integer
 *                 totalServerErrors:
 *                   type: integer
 *                 timeRange:
 *                   type: string
 *                 startTime:
 *                   type: string
 *                   format: date-time
 *                 endTime:
 *                   type: string
 *                   format: date-time
 *       500:
 *         description: Failed to fetch error data
 */
router.get('/api/health/errors', async (req, res) => {
  try {
    const { timeRange = '24h', limit = 50 } = req.query;
    
    const now = new Date();
    let startTime = new Date();
    
    switch(timeRange) {
      case '1h':
        startTime.setHours(now.getHours() - 1);
        break;
      case '24h':
        startTime.setHours(now.getHours() - 24);
        break;
      case '7d':
        startTime.setDate(now.getDate() - 7);
        break;
      case '30d':
        startTime.setDate(now.getDate() - 30);
        break;
      default:
        startTime.setHours(now.getHours() - 24);
    }

    const errors = await ApiHealth.findAll({
      where: {
        timestamp: {
          [Op.gte]: startTime
        },
        statusCode: {
          [Op.gte]: 400
        }
      },
      order: [['timestamp', 'DESC']],
      limit: parseInt(limit)
    });

    const errorsByStatus = await ApiHealth.findAll({
      attributes: [
        'statusCode',
        [sequelize.fn('COUNT', sequelize.col('id')), 'count']
      ],
      where: {
        timestamp: {
          [Op.gte]: startTime
        },
        statusCode: {
          [Op.gte]: 400
        }
      },
      group: ['statusCode'],
      order: [[sequelize.fn('COUNT', sequelize.col('id')), 'DESC']]
    });

    const serverErrorCount = await ApiHealth.count({
      where: {
        timestamp: {
          [Op.gte]: startTime
        },
        isServerError: true
      }
    });

    res.json({
      recentErrors: errors,
      errorsByStatus: errorsByStatus.map(e => ({
        statusCode: e.statusCode,
        count: parseInt(e.get('count'))
      })),
      totalServerErrors: serverErrorCount,
      timeRange,
      startTime,
      endTime: now
    });

  } catch (error) {
    console.error('Error fetching error data:', error);
    res.status(500).json({ error: 'Failed to fetch error data' });
  }
});
/**
 * @swagger
 * /api/health/latency-trends:
 *   get:
 *     summary: Get latency and error trends over time
 *     description: Returns time-bucketed latency averages and error counts, useful for charting performance over a time window.
 *     tags: [Health]
 *     parameters:
 *       - in: query
 *         name: timeRange
 *         schema:
 *           type: string
 *           enum: [1h, 24h, 7d, 30d]
 *           default: 24h
 *         description: Time window to trend over
 *       - in: query
 *         name: interval
 *         schema:
 *           type: string
 *           enum: [minute, hour, day]
 *           default: hour
 *         description: Bucket size for grouping (note - actual bucketing is driven by timeRange)
 *     responses:
 *       200:
 *         description: Latency trends returned successfully
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 trends:
 *                   type: array
 *                   items:
 *                     $ref: '#/components/schemas/LatencyTrend'
 *                 timeRange:
 *                   type: string
 *                 interval:
 *                   type: string
 *                 startTime:
 *                   type: string
 *                   format: date-time
 *                 endTime:
 *                   type: string
 *                   format: date-time
 *       500:
 *         description: Failed to fetch latency trends
 */

router.get('/api/health/latency-trends', async (req, res) => {
  try {
    const { timeRange = '24h', interval = 'hour' } = req.query;
    
    const now = new Date();
    let startTime = new Date();
    let dateFormat;
    
    switch(timeRange) {
      case '1h':
        startTime.setHours(now.getHours() - 1);
        dateFormat = '%Y-%m-%d %H:%i:00';
        break;
      case '24h':
        startTime.setHours(now.getHours() - 24);
        dateFormat = '%Y-%m-%d %H:00:00';
        break;
      case '7d':
        startTime.setDate(now.getDate() - 7);
        dateFormat = '%Y-%m-%d 00:00:00';
        break;
      case '30d':
        startTime.setDate(now.getDate() - 30);
        dateFormat = '%Y-%m-%d 00:00:00';
        break;
      default:
        startTime.setHours(now.getHours() - 24);
        dateFormat = '%Y-%m-%d %H:00:00';
    }

    const trends = await ApiHealth.findAll({
      attributes: [
        [sequelize.fn('DATE_FORMAT', sequelize.col('timestamp'), dateFormat), 'timeSlot'],
        [sequelize.fn('AVG', sequelize.col('latency')), 'avgLatency'],
        [sequelize.fn('COUNT', sequelize.col('id')), 'requestCount'],
        [sequelize.fn('SUM', sequelize.literal(
          "CASE WHEN statusCode >= 400 THEN 1 ELSE 0 END"
        )), 'errorCount'],
        [sequelize.fn('SUM', sequelize.literal(
          "CASE WHEN isServerError = 1 THEN 1 ELSE 0 END"
        )), 'serverErrorCount']
      ],
      where: {
        timestamp: {
          [Op.gte]: startTime
        }
      },
      group: [sequelize.fn('DATE_FORMAT', sequelize.col('timestamp'), dateFormat)],
      order: [[sequelize.fn('DATE_FORMAT', sequelize.col('timestamp'), dateFormat), 'ASC']]
    });

    const formattedTrends = trends.map(t => {
      const data = t.get({ plain: true });
      return {
        timeSlot: data.timeSlot,
        avgLatency: Math.round(parseFloat(data.avgLatency)),
        requestCount: parseInt(data.requestCount),
        errorCount: parseInt(data.errorCount),
        serverErrorCount: parseInt(data.serverErrorCount),
        errorRate: data.requestCount > 0 
          ? ((data.errorCount / data.requestCount) * 100).toFixed(2)
          : 0,
        serverErrorRate: data.requestCount > 0 
          ? ((data.serverErrorCount / data.requestCount) * 100).toFixed(2)
          : 0
      };
    });

    res.json({
      trends: formattedTrends,
      timeRange,
      interval,
      startTime,
      endTime: now
    });

  } catch (error) {
    console.error('Error fetching latency trends:', error);
    res.status(500).json({ error: 'Failed to fetch latency trends' });
  }
});

module.exports = router;