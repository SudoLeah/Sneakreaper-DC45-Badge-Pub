#pragma once

#include <Arduino.h>

// Initializes the PN532 and starts its dedicated FreeRTOS worker task.
// Failure is reported through getNfcStateJson() and does not stop the LED or
// Wi-Fi controller.
void setupNFC();

// JSON state consumed by the NFC webpage.
String getNfcStateJson();

// Queue external-tag operations. The user has 15 seconds to present a tag.
bool queueNfcRead();
bool queueNfcWrite(const String &recordType, const String &payload);

// Make the PN532 emulate a read-only NFC Forum Type 4 NDEF tag so an NFC
// reader, such as a phone, can scan a Text or URL record from the board.
// External-tag operations are unavailable until tag emulation is stopped.
bool startNfcTagEmulation(const String &recordType, const String &payload);

// Starts a standards-based Wi-Fi Simple Configuration NDEF record. Android
// devices that support application/vnd.wfa.wsc can offer to join the network.
bool startNfcWifiOnboarding(const String &ssid,
                            const String &password,
                            const uint8_t apMac[6]);

// Used by the Wi-Fi settings API so credential changes rebuild only the
// active Wi-Fi onboarding record and never replace a manual Text/URL record.
bool isNfcWifiOnboardingActive();

bool stopNfcTagEmulation();
