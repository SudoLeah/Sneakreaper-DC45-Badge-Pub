#pragma once

#include <stdint.h>

// Starts the ESP32 access point and HTTP server.
void setupWiFiAccessPoint();

// Services pending HTTP requests. Call this frequently from loop().
void updateWebServer();

// Returns the unique access-point credentials generated during startup.
// These remain valid for the lifetime of the badge.
const char *getBadgeWifiSsid();
const char *getBadgeWifiPassword();

// Copies the six-byte SoftAP MAC address into outMac. Returns false if the
// access point has not been initialized or the destination is null.
bool getBadgeWifiApMac(uint8_t outMac[6]);
