const express = require('express');
const sequelize = require('./config/database');
const cors = require('cors');
const chalk = require('chalk');
require('./models');
const trackApiHealth = require('./middleware/trackApiHealth');
const authRoutes = require('./routes/auth');
const submitRoutes = require('./routes/submit');
const devicesRoutes = require('./routes/devices');
const readingsRoutes = require('./routes/readings');
const alertsRoutes = require('./routes/alerts');
const powerStatusRoutes = require('./routes/powerStatus');
const healthRoutes = require('./routes/health');
const swaggerUi = require('swagger-ui-express');
const swaggerSpec = require('./swagger');

const app = express();

app.use(cors());
app.use(express.json());
app.use(trackApiHealth);

app.use('/api-docs', swaggerUi.serve, swaggerUi.setup(swaggerSpec));
app.get('/api-docs.json', (req, res) => res.json(swaggerSpec));

app.use((req, res, next) => {
  const start = Date.now();
  res.on('finish', () => {
    const duration = Date.now() - start;
    const methodColor =
        req.method === 'GET'    ? chalk.cyan    :
            req.method === 'POST'   ? chalk.green   :
                req.method === 'PUT'    ? chalk.yellow  :
                    req.method === 'PATCH'  ? chalk.magenta :
                        req.method === 'DELETE' ? chalk.red     : chalk.white;
    const statusColor =
        res.statusCode >= 500 ? chalk.redBright    :
            res.statusCode >= 400 ? chalk.yellowBright :
                res.statusCode >= 300 ? chalk.cyanBright   : chalk.greenBright;
    console.log(
        `${chalk.gray(new Date().toLocaleTimeString())} | ` +
        `${methodColor(req.method)} ${chalk.white(req.originalUrl)} ` +
        `${statusColor(res.statusCode)} ` +
        `${chalk.gray(`(${duration} ms)`)}`
    );
  });
  next();
});

app.use((req, res, next) => {
  if (!app.locals.dbReady) {
    return res.status(503).json({
      error: 'Service temporarily unavailable — database is reconnecting, please try again shortly.',
    });
  }
  next();
});

app.use('/', authRoutes);
app.use('/', submitRoutes);
app.use('/', devicesRoutes);
app.use('/', readingsRoutes);
app.use('/', alertsRoutes);
app.use('/', powerStatusRoutes);
app.use('/', healthRoutes);

const PORT = process.env.PORT || 3000;

sequelize.setDbReadyCallback(async (ready, alreadySynced) => {
  app.locals.dbReady = ready;
  if (ready && !alreadySynced) {
    try {
      await sequelize.sync({ alter: true });
      sequelize.setDbSynchronized();
      console.log(chalk.green(' Database synchronized.'));

      // Runs on every boot, not just on an empty table, so defaults added to
      // the seeder later still reach existing deployments. The seeder only
      // inserts, so operator-tuned thresholds survive.
      const { seed } = require('./seeders/alert-thresholds');
      await seed();
    } catch (err) {
      console.error(chalk.red(` Database sync error: ${err.message}`));
    }
  }
});

if (require.main === module) {
  app.listen(PORT, async () => {
    console.log(chalk.blue('\n Initializing server...'));
    console.log(chalk.yellow(`✓ Server running at:      ${chalk.bold(`http://localhost:${PORT}`)}`));
    console.log(chalk.cyan(`✓ API Health Dashboard:   ${chalk.bold(`http://localhost:${PORT}/api/health/stats`)}`));
    console.log(chalk.magenta(`✓ API Docs:               ${chalk.bold(`http://localhost:${PORT}/api-docs`)}\n`));
  });
}

module.exports = { app, sequelize };