#pragma once

// Template for include/secrets.h: device identity and credentials. Copy it and
// fill in the real values:
//
//     cp include/secrets.example.h include/secrets.h
//
// include/secrets.h is gitignored so the auth token is not committed.

#define PRODUCT_UID "com.example.you:your_product"

static constexpr char AUTH_TOKEN[]  = "sk_replace_me";
static constexpr char DEVICE_ID[]   = "sensor-001";
static constexpr char DEVICE_NAME[] = "Air Quality Monitor 1";
static constexpr char DEVICE_TYPE[] = "air-quality-monitor";
static constexpr char DEVICES_URL[] = "https://api.ubitechino.com/devices";
