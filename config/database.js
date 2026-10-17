require('dotenv').config();
const { Sequelize } = require('sequelize');
const chalk = require('chalk');

const dbConfig = {
  name: process.env.DB_NAME || 'iot_monitoring',
  user: process.env.DB_USER || 'root',
  password: process.env.DB_PASSWORD || 'password',
  host: process.env.DB_HOST || 'localhost',
  port: parseInt(process.env.DB_PORT) || 3306,
  showSql: process.env.SHOW_SQL === 'true',
};

const sequelize = new Sequelize(dbConfig.name, dbConfig.user, dbConfig.password, {
  host: dbConfig.host,
  dialect: 'mysql',
  port: dbConfig.port,
  logging: dbConfig.showSql
      ? (msg) => console.log(chalk.gray(`[Sequelize] ${msg}`))
      : false,
  pool: {
    max: 5,
    min: 0,
    acquire: 30000,
    idle: 10000,
  },
});

console.log(chalk.cyanBright('\n Database Configuration:'));
console.table({
  'Database Name': dbConfig.name,
  'User': dbConfig.user,
  'Host': dbConfig.host,
  'Port': dbConfig.port,
  'Show SQL Logs': dbConfig.showSql,
});

const RECONNECT_DELAY_MS = 10_000;
let isConnected = false;
let isSynchronized = false;
let dbReadyCallback = null;

sequelize.setDbReadyCallback = function(callback) {
  dbReadyCallback = callback;
  if (isConnected && dbReadyCallback) {
    dbReadyCallback(true, isSynchronized);
  }
};

sequelize.setDbSynchronized = function() {
  isSynchronized = true;
};

async function connectWithRetry() {
  try {
    await sequelize.authenticate();
    if (!isConnected) {
      isConnected = true;
      console.log(chalk.greenBright('✔ Database connection established successfully.'));
    }
    if (dbReadyCallback) {
      dbReadyCallback(true, isSynchronized);
    }
  } catch (err) {
    isConnected = false;
    isSynchronized = false;
    if (dbReadyCallback) {
      dbReadyCallback(false, false);
    }
    console.error(chalk.redBright(`✘ Unable to connect to the database: ${err.message}`));
    console.log(chalk.yellow(`  ↻ Retrying in ${RECONNECT_DELAY_MS / 1000} seconds...`));
    setTimeout(connectWithRetry, RECONNECT_DELAY_MS);
  }
}

connectWithRetry();

module.exports = sequelize;