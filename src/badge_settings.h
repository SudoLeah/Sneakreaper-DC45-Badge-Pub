#pragma once

#include <Arduino.h>

// Opens the badge settings namespace. If no Wi-Fi credential exists, creates
// one password and commits it to NVS with read-back verification. If a stored
// credential exists but cannot be validated, this returns false and does not
// generate a replacement.
bool initializeBadgeSettings();

// Returns the verified 12-character NVS password, or an empty string when
// settings initialization failed.
const char *getPersistentWifiPassword();

// Returns whether the SoftAP SSID is hidden. This setting is stored in NVS.
bool getPersistentWifiHidden();

// Validates, stores, and reads back the dashboard Wi-Fi settings. The password
// must be exactly 12 characters and use the same safe character policy as the
// generated credential. On failure, the previous settings remain active.
bool setPersistentWifiSettings(const String &password,
                               bool hidden,
                               String &error);

// Wi-Fi onboarding is the default boot behavior until the user selects
// Stop Tag Emulation for the first time.
bool isWifiOnboardingDismissed();
bool dismissWifiOnboarding();

// Stores the last valid manual Text or URL record. Empty/invalid records are
// never saved. Unchanged records are not rewritten, reducing NVS wear.
bool savePersistedTagEmulationRecord(const String &recordType,
                                     const String &payload);

// Returns false when no valid manual record has ever been saved.
bool loadPersistedTagEmulationRecord(String &recordType, String &payload);
