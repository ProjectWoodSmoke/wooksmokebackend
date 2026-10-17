# Biobot Cloud API

A Node.js & Sequelize-based monitoring service for IoT devices.

## Getting Started

### Using Docker (Recommended for Development)

The easiest way to get the project running is using Docker and Docker Compose. This will set up the Node.js application and a MySQL database for you.

1.  **Prerequisites:**
    - Docker and Docker Compose installed on your machine.

2.  **Start the environment:**
    ```bash
    docker-compose up --build
    ```
    This command builds the development image, starts the MySQL database, waits for it to be healthy, and then starts the Node.js app with `nodemon` for hot-reloading.

3.  **Access the services:**
    - **API:** [http://localhost:3000](http://localhost:3000)
    - **API Docs:** [http://localhost:3000/api-docs](http://localhost:3000/api-docs)
    - **Health Stats:** [http://localhost:3000/api/health/stats](http://localhost:3000/api/health/stats)

4.  **Stopping the environment:**
    ```bash
    docker-compose down
    ```

### Local Development (Without Docker)

1.  **Install dependencies:**
    ```bash
    npm install
    ```

2.  **Configure environment variables:**
    Create a `.env` file in the root directory and add your database configuration:
    ```env
    DB_HOST=localhost
    DB_USER=root
    DB_PASSWORD=password
    DB_NAME=iot_monitoring
    DB_PORT=3306
    PORT=3000
    ```

3.  **Run the application:**
    ```bash
    # Development mode (with hot-reloading)
    npm run dev

    # Production mode
    npm start
    ```

## Project Features

- **Automatic Database Synchronization:** Sequelize will automatically create/update tables on startup.
- **Auto-Seeding:** Initial alert thresholds are seeded if the database is empty.
- **Health Monitoring:** Built-in middleware to track API health and performance.
- **Swagger Documentation:** Interactive API documentation available at `/api-docs`.
