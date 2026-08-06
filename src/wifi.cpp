#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <cstring>
#include "badge_wifi.h"
#include "badge_settings.h"
#include "nfc.h"

// Implemented in main.cpp. wifi.cpp only transports and parses LED requests;
// main.cpp remains responsible for LED state, validation, and animation logic.
String getLedStateJson();
void applyLedWebSettings(const String &pattern,
                         int red,
                         int green,
                         int blue,
                         int brightness,
                         int speed);

namespace {

constexpr char AP_SSID_PREFIX[] = "Sneakreaper Badge";
// IEEE 802.11 SSIDs can contain up to 32 bytes. This buffer holds the
// 17-character prefix, a hyphen, the complete 12-digit MAC suffix, and '\0'.
char apSsid[33] = {};
uint64_t deviceMac = 0;

const IPAddress AP_IP(10, 10, 10, 100);
const IPAddress AP_GATEWAY(10, 10, 10, 100);
const IPAddress AP_SUBNET(255, 255, 255, 0);
constexpr uint16_t HTTP_PORT = 80;

WebServer server(HTTP_PORT);
bool fileSystemReady = false;
bool accessPointRestartPending = false;
bool refreshWifiNfcAfterRestart = false;
uint32_t accessPointRestartAt = 0;

bool activeApMatchesExpectedSettings(const char *expectedSsid,
                                    const char *expectedPassword,
                                    bool expectedHidden) {
  wifi_config_t config = {};
  if (esp_wifi_get_config(WIFI_IF_AP, &config) != ESP_OK) {
    return false;
  }

  const size_t expectedSsidLength = strlen(expectedSsid);
  const size_t expectedPasswordLength = strlen(expectedPassword);

  const size_t activeSsidLength =
      config.ap.ssid_len != 0
          ? config.ap.ssid_len
          : strnlen(reinterpret_cast<const char *>(config.ap.ssid),
                    sizeof(config.ap.ssid));

  const bool ssidMatches =
      activeSsidLength == expectedSsidLength &&
      memcmp(config.ap.ssid, expectedSsid, expectedSsidLength) == 0;

  const bool passwordMatches =
      expectedPasswordLength < sizeof(config.ap.password) &&
      memcmp(config.ap.password,
             expectedPassword,
             expectedPasswordLength) == 0 &&
      config.ap.password[expectedPasswordLength] == '\0';

  const bool hiddenMatches =
      (config.ap.ssid_hidden != 0) == expectedHidden;

  return ssidMatches && passwordMatches && hiddenMatches;
}

bool configureVerifiedAccessPoint() {
  const char *password = getPersistentWifiPassword();
  const bool hidden = getPersistentWifiHidden();
  if (!password || strlen(password) != 12) {
    Serial.println(
        "[WIFI] ERROR: No verified 12-character Wi-Fi password is available");
    return false;
  }

  // WiFi.mode(WIFI_AP) may restore the Wi-Fi driver's previous AP settings.
  // Force a clean stop before applying the NVS credential resolved above.
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(75);

  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    WiFi.mode(WIFI_AP);
    delay(50);

    if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
      Serial.println("[WIFI] ERROR: Could not configure access point IP");
    } else if (!WiFi.softAP(apSsid, password, 1, hidden ? 1 : 0, 4)) {
      Serial.println("[WIFI] ERROR: Could not start access point");
    } else {
      delay(75);
      if (activeApMatchesExpectedSettings(apSsid, password, hidden)) {
        Serial.println(
            "[WIFI] Active access-point credentials verified");
        return true;
      }

      Serial.println(
          "[WIFI] WARNING: Active AP credentials did not match stored "
          "settings; restarting Wi-Fi");
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  Serial.println(
      "[WIFI] ERROR: Access point credential verification failed");
  return false;
}

void createMacDerivedSsid() {
  // ESP.getEfuseMac() returns the factory-programmed base MAC value. Keeping
  // all 48 bits makes the visible SSID stable and effectively unique per PCB.
  deviceMac = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;

  snprintf(apSsid,
           sizeof(apSsid),
           "%s-%012llX",
           AP_SSID_PREFIX,
           static_cast<unsigned long long>(deviceMac));
}

void addNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
}

int boundedRequestArg(const String &name, int low, int high) {
  if (!server.hasArg(name)) return -1;
  return constrain(server.arg(name).toInt(), low, high);
}

void serveLittleFsFile(const char *path, const char *contentType) {
  addNoCacheHeaders();

  if (!fileSystemReady || !LittleFS.exists(path)) {
    String error = path;
    error += " is missing. Upload the LittleFS data image and try again.";
    server.send(500, "text/plain; charset=utf-8", error);
    return;
  }

  File page = LittleFS.open(path, "r");
  if (!page) {
    String error = "Could not open ";
    error += path;
    server.send(500, "text/plain; charset=utf-8", error);
    return;
  }

  server.streamFile(page, contentType);
  page.close();
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char character = value[i];
    switch (character) {
      case '\\': escaped += F("\\\\"); break;
      case '"': escaped += F("\\\""); break;
      case '\n': escaped += F("\\n"); break;
      case '\r': escaped += F("\\r"); break;
      case '\t': escaped += F("\\t"); break;
      default: escaped += character; break;
    }
  }
  return escaped;
}

String wifiSettingsJson(bool ok, const String &message) {
  String json;
  json.reserve(220);
  json += F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"ssid\":\"");
  json += jsonEscape(apSsid);
  json += F("\",\"password\":\"");
  json += jsonEscape(getPersistentWifiPassword());
  json += F("\",\"hidden\":");
  json += getPersistentWifiHidden() ? F("true") : F("false");
  json += F(",\"restartPending\":");
  json += accessPointRestartPending ? F("true") : F("false");
  json += F(",\"message\":\"");
  json += jsonEscape(message);
  json += F("\"}");
  return json;
}

void handleWifiSettingsGet() {
  addNoCacheHeaders();
  server.send(200, "application/json", wifiSettingsJson(true, String()));
}

void handleWifiSettingsSet() {
  addNoCacheHeaders();

  if (!server.hasArg("password") || !server.hasArg("hidden")) {
    server.send(
        400,
        "application/json",
        wifiSettingsJson(false, "Password and hidden fields are required."));
    return;
  }

  const String password = server.arg("password");
  const String hiddenValue = server.arg("hidden");
  const bool hidden =
      hiddenValue == "1" || hiddenValue == "true" || hiddenValue == "on";

  String error;
  if (!setPersistentWifiSettings(password, hidden, error)) {
    server.send(400, "application/json", wifiSettingsJson(false, error));
    return;
  }

  // Preserve the current NFC mode. Only an active Wi-Fi onboarding record is
  // rebuilt after the AP restart; manual Text/URL emulation is untouched.
  refreshWifiNfcAfterRestart = isNfcWifiOnboardingActive();
  accessPointRestartPending = true;
  accessPointRestartAt = millis() + 700;

  server.send(
      202,
      "application/json",
      wifiSettingsJson(
          true,
          hidden
              ? "Saved. Reconnect by manually entering the hidden SSID and new password."
              : "Saved. Reconnect to the badge using the updated password."));
}

void servicePendingAccessPointRestart() {
  if (!accessPointRestartPending ||
      static_cast<int32_t>(millis() - accessPointRestartAt) < 0) {
    return;
  }

  accessPointRestartPending = false;
  Serial.println("[WIFI] Applying dashboard Wi-Fi settings");

  if (!configureVerifiedAccessPoint()) {
    Serial.println(
        "[WIFI] ERROR: Dashboard settings were stored, but the access point "
        "could not restart");
    refreshWifiNfcAfterRestart = false;
    return;
  }

  Serial.printf("[WIFI] SSID visibility: %s\n",
                getPersistentWifiHidden() ? "hidden" : "visible");
  Serial.printf("[WIFI] Password: %s\n", getPersistentWifiPassword());

  if (refreshWifiNfcAfterRestart) {
    uint8_t accessPointMac[6] = {0};
    const bool haveAccessPointMac = getBadgeWifiApMac(accessPointMac);
    if (startNfcWifiOnboarding(
            getBadgeWifiSsid(),
            getBadgeWifiPassword(),
            haveAccessPointMac ? accessPointMac : nullptr)) {
      Serial.println(
          "[NFC][WIFI] Onboarding records refreshed with dashboard settings");
    } else {
      Serial.println(
          "[NFC][WIFI] WARNING: Could not refresh onboarding records");
    }
  }

  refreshWifiNfcAfterRestart = false;
}

// -----------------------------------------------------------------------------
// Dashboard, LED webpage, and LED API
// -----------------------------------------------------------------------------
void handleDashboardPage() {
  serveLittleFsFile("/index.html", "text/html; charset=utf-8");
}

void handleLedPage() {
  serveLittleFsFile("/led.html", "text/html; charset=utf-8");
}

void handleLedState() {
  addNoCacheHeaders();
  server.send(200, "application/json", getLedStateJson());
}

void handleLedSet() {
  const String pattern = server.hasArg("pattern") ? server.arg("pattern") : String();

  applyLedWebSettings(pattern,
                      boundedRequestArg("r", 0, 255),
                      boundedRequestArg("g", 0, 255),
                      boundedRequestArg("b", 0, 255),
                      boundedRequestArg("brightness", 0, 255),
                      boundedRequestArg("speed", 1, 100));

  handleLedState();
}

// -----------------------------------------------------------------------------
// NFC webpage and API
// -----------------------------------------------------------------------------
void handleNfcPage() {
  serveLittleFsFile("/nfc.html", "text/html; charset=utf-8");
}

// -----------------------------------------------------------------------------
// CTF webpage and image
// -----------------------------------------------------------------------------
void handleCtfPage() {
  serveLittleFsFile("/CTF.html", "text/html; charset=utf-8");
}

void handleCtfImage() {
  serveLittleFsFile("/ctf-image.png", "image/png");
}

void handleNfcState() {
  addNoCacheHeaders();
  server.send(200, "application/json", getNfcStateJson());
}

void sendNfcQueueResponse(bool accepted) {
  addNoCacheHeaders();
  server.send(accepted ? 202 : 409, "application/json", getNfcStateJson());
}

void handleNfcRead() {
  sendNfcQueueResponse(queueNfcRead());
}

void handleNfcWrite() {
  const String recordType = server.hasArg("recordType") ? server.arg("recordType") : String();
  const String payload = server.hasArg("payload") ? server.arg("payload") : String();
  sendNfcQueueResponse(queueNfcWrite(recordType, payload));
}


void handleNfcTagEmulationStart() {
  const String recordType =
      server.hasArg("recordType") ? server.arg("recordType") : String();
  const String payload =
      server.hasArg("payload") ? server.arg("payload") : String();
  sendNfcQueueResponse(startNfcTagEmulation(recordType, payload));
}

void handleNfcTagEmulationStop() {
  sendNfcQueueResponse(stopNfcTagEmulation());
}

void handleNotFound() {
  if (server.uri().startsWith("/api/")) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
  } else {
    server.send(404, "text/plain; charset=utf-8",
                "Page not found. Open http://10.10.10.100/ for the dashboard, "
                "http://10.10.10.100/led for LEDs, "
                "http://10.10.10.100/nfc for NFC tools, or "
                "http://10.10.10.100/ctf for the CTF page.");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleDashboardPage);
  server.on("/index.html", HTTP_GET, handleDashboardPage);
  server.on("/led", HTTP_GET, handleLedPage);
  server.on("/led.html", HTTP_GET, handleLedPage);
  server.on("/nfc", HTTP_GET, handleNfcPage);
  server.on("/nfc.html", HTTP_GET, handleNfcPage);

  server.on("/ctf", HTTP_GET, handleCtfPage);
  server.on("/CTF.html", HTTP_GET, handleCtfPage);
  server.on("/ctf-image.png", HTTP_GET, handleCtfImage);

  server.on("/api/state", HTTP_GET, handleLedState);
  server.on("/api/set", HTTP_GET, handleLedSet);

  server.on("/api/wifi/settings", HTTP_GET, handleWifiSettingsGet);
  server.on("/api/wifi/settings", HTTP_POST, handleWifiSettingsSet);

  server.on("/api/nfc/state", HTTP_GET, handleNfcState);
  server.on("/api/nfc/read", HTTP_POST, handleNfcRead);
  server.on("/api/nfc/write", HTTP_POST, handleNfcWrite);
  server.on("/api/nfc/emulation/start", HTTP_POST, handleNfcTagEmulationStart);
  server.on("/api/nfc/emulation/stop", HTTP_POST, handleNfcTagEmulationStop);

  server.on("/favicon.ico", HTTP_GET, []() { server.send(204, "text/plain", ""); });
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[WEB] HTTP server started");
}

}  // namespace

const char *getBadgeWifiSsid() {
  return apSsid;
}

const char *getBadgeWifiPassword() {
  return getPersistentWifiPassword();
}

bool getBadgeWifiApMac(uint8_t outMac[6]) {
  if (!outMac || apSsid[0] == '\0') return false;
  return WiFi.softAPmacAddress(outMac) != nullptr;
}

void setupWiFiAccessPoint() {
  Serial.println("[WIFI] Mounting LittleFS");
  fileSystemReady = LittleFS.begin(true);
  if (!fileSystemReady) {
    Serial.println("[WIFI] ERROR: LittleFS mount failed");
  } else {
    Serial.printf("[WIFI] /index.html: %s\n",
                  LittleFS.exists("/index.html") ? "ready" : "missing");
    Serial.printf("[WIFI] /led.html: %s\n",
                  LittleFS.exists("/led.html") ? "ready" : "missing");
    Serial.printf("[WIFI] /nfc.html: %s\n",
                  LittleFS.exists("/nfc.html") ? "ready" : "missing");
    Serial.printf("[WIFI] /CTF.html: %s\n",
                  LittleFS.exists("/CTF.html") ? "ready" : "missing");
    Serial.printf("[WIFI] /ctf-image.png: %s\n",
                  LittleFS.exists("/ctf-image.png") ? "ready" : "missing");
  }

  createMacDerivedSsid();

  // Resolve and verify the persistent password before starting Wi-Fi.
  if (!initializeBadgeSettings()) {
    Serial.println(
        "[WIFI] ERROR: Persistent credentials unavailable; access point "
        "was not started");
    return;
  }

  Serial.println("[WIFI] Starting access point");
  if (!configureVerifiedAccessPoint()) {
    return;
  }

  setupWebServer();

  Serial.printf("[WIFI] SSID: %s\n", apSsid);
  Serial.printf("[WIFI] SSID visibility: %s\n",
                getPersistentWifiHidden() ? "hidden" : "visible");
  Serial.printf("[WIFI] Password: %s\n", getPersistentWifiPassword());
  Serial.print("[WIFI] Dashboard: http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("[WIFI] LED controller: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/led");
  Serial.print("[WIFI] NFC tools: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/nfc");
  Serial.print("[WIFI] CTF page: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/ctf");
}

void updateWebServer() {
  server.handleClient();
  servicePendingAccessPointRestart();
}
