#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstring>

#include "badge_settings.h"

namespace {

constexpr char SETTINGS_NAMESPACE[] = "badgecfg";
constexpr char KEY_WIFI_CREDENTIAL[] = "wifi_cred";
constexpr char KEY_WIFI_HIDDEN[] = "wifi_hidden";
constexpr char KEY_ONBOARDING_DONE[] = "wifi_done";
constexpr char KEY_RECORD_VALID[] = "rec_valid";
constexpr char KEY_RECORD_TYPE[] = "rec_type";
constexpr char KEY_RECORD_DATA[] = "rec_data";

constexpr size_t WIFI_PASSWORD_LENGTH = 12;
constexpr uint32_t WIFI_CREDENTIAL_MAGIC = 0x42444745UL;  // "BDGE"
constexpr uint8_t WIFI_CREDENTIAL_VERSION = 1;

// The generated password is exactly 12 characters and always contains at
// least one uppercase letter, lowercase letter, digit, and special character.
// Accepted characters are the full ASCII alphabet, all decimal digits, and
// the existing WPA2-compatible special-character set.
constexpr char UPPERCASE_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char LOWERCASE_CHARS[] = "abcdefghijklmnopqrstuvwxyz";
constexpr char DIGIT_CHARS[] = "0123456789";
constexpr char SPECIAL_CHARS[] = "!_-";
constexpr char ALL_PASSWORD_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "!_-";

struct __attribute__((packed)) StoredWifiCredential {
  uint32_t magic;
  uint8_t version;
  char password[WIFI_PASSWORD_LENGTH + 1];
  uint32_t checksum;
};

static_assert(sizeof(StoredWifiCredential) == 22,
              "Unexpected StoredWifiCredential packing");

char cachedWifiPassword[WIFI_PASSWORD_LENGTH + 1] = {};
bool settingsInitialized = false;
bool cachedOnboardingDismissed = false;
bool cachedWifiHidden = false;

uint32_t updateFnv1a(uint32_t hash, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t credentialChecksum(const StoredWifiCredential &credential) {
  uint32_t hash = 2166136261UL;
  hash = updateFnv1a(
      hash,
      reinterpret_cast<const uint8_t *>(&credential.magic),
      sizeof(credential.magic));
  hash = updateFnv1a(hash, &credential.version, sizeof(credential.version));
  hash = updateFnv1a(
      hash,
      reinterpret_cast<const uint8_t *>(credential.password),
      sizeof(credential.password));
  return hash;
}

char randomCharacter(const char *characters) {
  return characters[esp_random() % strlen(characters)];
}

bool containsCharacterFrom(const char *value, const char *characters) {
  for (size_t i = 0; i < WIFI_PASSWORD_LENGTH; ++i) {
    if (strchr(characters, value[i]) != nullptr) return true;
  }
  return false;
}

bool isValidPassword(const char *password) {
  if (!password || password[WIFI_PASSWORD_LENGTH] != '\0') return false;

  for (size_t i = 0; i < WIFI_PASSWORD_LENGTH; ++i) {
    if (password[i] == '\0' ||
        strchr(ALL_PASSWORD_CHARS, password[i]) == nullptr) {
      return false;
    }
  }

  return containsCharacterFrom(password, UPPERCASE_CHARS) &&
         containsCharacterFrom(password, LOWERCASE_CHARS) &&
         containsCharacterFrom(password, DIGIT_CHARS) &&
         containsCharacterFrom(password, SPECIAL_CHARS);
}

void copyPassword(char destination[WIFI_PASSWORD_LENGTH + 1],
                  const char *source) {
  memcpy(destination, source, WIFI_PASSWORD_LENGTH);
  destination[WIFI_PASSWORD_LENGTH] = '\0';
}

void generateRandomWifiPassword(
    char password[WIFI_PASSWORD_LENGTH + 1]) {
  // Guarantee all required character classes.
  password[0] = randomCharacter(UPPERCASE_CHARS);
  password[1] = randomCharacter(LOWERCASE_CHARS);
  password[2] = randomCharacter(DIGIT_CHARS);
  password[3] = randomCharacter(SPECIAL_CHARS);

  for (size_t i = 4; i < WIFI_PASSWORD_LENGTH; ++i) {
    password[i] = randomCharacter(ALL_PASSWORD_CHARS);
  }

  password[WIFI_PASSWORD_LENGTH] = '\0';

  // Shuffle the guaranteed characters out of fixed positions.
  for (size_t i = WIFI_PASSWORD_LENGTH - 1; i > 0; --i) {
    const size_t swapIndex = esp_random() % (i + 1);
    const char temporary = password[i];
    password[i] = password[swapIndex];
    password[swapIndex] = temporary;
  }
}

StoredWifiCredential makeCredential(const char *password) {
  StoredWifiCredential credential = {};
  credential.magic = WIFI_CREDENTIAL_MAGIC;
  credential.version = WIFI_CREDENTIAL_VERSION;
  copyPassword(credential.password, password);
  credential.checksum = credentialChecksum(credential);
  return credential;
}

bool validateCredential(const StoredWifiCredential &credential) {
  return credential.magic == WIFI_CREDENTIAL_MAGIC &&
         credential.version == WIFI_CREDENTIAL_VERSION &&
         isValidPassword(credential.password) &&
         credential.checksum == credentialChecksum(credential);
}

bool readCredential(Preferences &preferences,
                    StoredWifiCredential &credential) {
  if (preferences.getBytesLength(KEY_WIFI_CREDENTIAL) !=
      sizeof(StoredWifiCredential)) {
    return false;
  }

  if (preferences.getBytes(
          KEY_WIFI_CREDENTIAL,
          &credential,
          sizeof(credential)) != sizeof(credential)) {
    return false;
  }

  return validateCredential(credential);
}

bool storeAndVerifyCredential(Preferences &preferences,
                              const char *password) {
  const StoredWifiCredential credential = makeCredential(password);

  if (preferences.putBytes(
          KEY_WIFI_CREDENTIAL,
          &credential,
          sizeof(credential)) != sizeof(credential)) {
    return false;
  }

  StoredWifiCredential readBack = {};
  return readCredential(preferences, readBack) &&
         memcmp(readBack.password,
                password,
                WIFI_PASSWORD_LENGTH + 1) == 0;
}

String wifiPasswordValidationError(const String &password) {
  if (password.length() != WIFI_PASSWORD_LENGTH) {
    return F("Password must be exactly 12 characters.");
  }

  char candidate[WIFI_PASSWORD_LENGTH + 1] = {};
  password.toCharArray(candidate, sizeof(candidate));

  for (size_t i = 0; i < WIFI_PASSWORD_LENGTH; ++i) {
    if (strchr(ALL_PASSWORD_CHARS, candidate[i]) == nullptr) {
      return F("Use only A-Z, a-z, 0-9, and ! _ - characters.");
    }
  }

  if (!containsCharacterFrom(candidate, UPPERCASE_CHARS) ||
      !containsCharacterFrom(candidate, LOWERCASE_CHARS) ||
      !containsCharacterFrom(candidate, DIGIT_CHARS) ||
      !containsCharacterFrom(candidate, SPECIAL_CHARS)) {
    return F(
        "Password needs uppercase, lowercase, a number from 0-9, and ! _ or -.");
  }

  return String();
}

bool validManualRecord(const String &recordType, const String &payload,
                       String &normalizedType) {
  normalizedType = recordType;
  normalizedType.toLowerCase();
  return (normalizedType == "text" || normalizedType == "url") &&
         payload.length() > 0 && payload.length() <= 220;
}

}  // namespace

bool initializeBadgeSettings() {
  if (settingsInitialized) return true;

  Preferences preferences;
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    Serial.println(
        "[SETTINGS] ERROR: Could not open NVS; Wi-Fi will not start");
    return false;
  }

  StoredWifiCredential credential = {};

  if (!preferences.isKey(KEY_WIFI_CREDENTIAL)) {
    // This is the only path that creates a password.
    char generatedPassword[WIFI_PASSWORD_LENGTH + 1] = {};
    generateRandomWifiPassword(generatedPassword);

    if (!storeAndVerifyCredential(preferences, generatedPassword)) {
      preferences.end();
      Serial.println(
          "[SETTINGS] ERROR: First-boot Wi-Fi password could not be stored "
          "and verified; Wi-Fi will not start");
      return false;
    }

    copyPassword(cachedWifiPassword, generatedPassword);
    Serial.println(
        "[SETTINGS] Generated, stored, and verified first-boot Wi-Fi password");
  } else {
    // Once the key exists, never regenerate it. An invalid record is treated
    // as a hard error so the badge cannot silently change passwords.
    if (!readCredential(preferences, credential)) {
      preferences.end();
      Serial.println(
          "[SETTINGS] ERROR: Stored Wi-Fi credential is invalid; refusing "
          "to generate a replacement password");
      return false;
    }

    copyPassword(cachedWifiPassword, credential.password);
    Serial.println("[SETTINGS] Loaded and verified stored Wi-Fi password");
  }

  cachedOnboardingDismissed =
      preferences.getBool(KEY_ONBOARDING_DONE, false);
  cachedWifiHidden = preferences.getBool(KEY_WIFI_HIDDEN, false);
  preferences.end();

  settingsInitialized = true;
  return true;
}

const char *getPersistentWifiPassword() {
  if (!settingsInitialized && !initializeBadgeSettings()) return "";
  return cachedWifiPassword;
}

bool getPersistentWifiHidden() {
  if (!settingsInitialized && !initializeBadgeSettings()) return false;
  return cachedWifiHidden;
}

bool setPersistentWifiSettings(const String &password,
                               bool hidden,
                               String &error) {
  error = String();
  if (!settingsInitialized && !initializeBadgeSettings()) {
    error = F("Persistent settings are unavailable.");
    return false;
  }

  error = wifiPasswordValidationError(password);
  if (error.length() > 0) return false;

  char requestedPassword[WIFI_PASSWORD_LENGTH + 1] = {};
  password.toCharArray(requestedPassword, sizeof(requestedPassword));

  const bool passwordChanged =
      memcmp(requestedPassword,
             cachedWifiPassword,
             WIFI_PASSWORD_LENGTH + 1) != 0;
  const bool hiddenChanged = hidden != cachedWifiHidden;
  if (!passwordChanged && !hiddenChanged) return true;

  Preferences preferences;
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    error = F("Could not open NVS to save Wi-Fi settings.");
    return false;
  }

  char previousPassword[WIFI_PASSWORD_LENGTH + 1] = {};
  copyPassword(previousPassword, cachedWifiPassword);
  const bool previousHidden = cachedWifiHidden;

  bool passwordStored = true;
  bool hiddenStored = true;

  if (passwordChanged) {
    passwordStored =
        storeAndVerifyCredential(preferences, requestedPassword);
  }

  if (passwordStored && hiddenChanged) {
    hiddenStored =
        preferences.putBool(KEY_WIFI_HIDDEN, hidden) != 0 &&
        preferences.getBool(KEY_WIFI_HIDDEN, !hidden) == hidden;
  }

  if (!passwordStored || !hiddenStored) {
    // Best-effort rollback keeps NVS and the running configuration aligned.
    if (passwordChanged) {
      (void)storeAndVerifyCredential(preferences, previousPassword);
    }
    if (hiddenChanged) {
      (void)preferences.putBool(KEY_WIFI_HIDDEN, previousHidden);
    }
    preferences.end();

    error = !passwordStored
                ? F("The new password could not be stored and verified.")
                : F("The hidden-SSID setting could not be stored and verified.");
    return false;
  }

  preferences.end();
  copyPassword(cachedWifiPassword, requestedPassword);
  cachedWifiHidden = hidden;

  Serial.printf("[SETTINGS] Wi-Fi settings updated: hidden=%s\n",
                cachedWifiHidden ? "true" : "false");
  return true;
}

bool isWifiOnboardingDismissed() {
  if (!settingsInitialized && !initializeBadgeSettings()) return true;
  return cachedOnboardingDismissed;
}

bool dismissWifiOnboarding() {
  if (!settingsInitialized && !initializeBadgeSettings()) return false;
  if (cachedOnboardingDismissed) return true;

  Preferences preferences;
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    Serial.println(
        "[SETTINGS] ERROR: Could not store Wi-Fi onboarding dismissal");
    return false;
  }

  const bool stored =
      preferences.putBool(KEY_ONBOARDING_DONE, true) != 0;
  preferences.end();

  if (stored) {
    cachedOnboardingDismissed = true;
    Serial.println(
        "[SETTINGS] Wi-Fi onboarding disabled for future reboots");
  } else {
    Serial.println(
        "[SETTINGS] ERROR: Wi-Fi onboarding dismissal was not stored");
  }
  return stored;
}

bool savePersistedTagEmulationRecord(const String &recordType,
                                     const String &payload) {
  String normalizedType;
  if (!validManualRecord(recordType, payload, normalizedType)) return false;

  Preferences preferences;
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    Serial.println(
        "[SETTINGS] ERROR: Could not open NVS to save Tag Emulation record");
    return false;
  }

  const bool existingValid =
      preferences.getBool(KEY_RECORD_VALID, false);
  const String existingType =
      preferences.getString(KEY_RECORD_TYPE, String());
  const String existingPayload =
      preferences.getString(KEY_RECORD_DATA, String());

  if (existingValid && existingType == normalizedType &&
      existingPayload == payload) {
    preferences.end();
    return true;
  }

  const bool invalidated =
      preferences.putBool(KEY_RECORD_VALID, false) != 0;
  bool typeStored = false;
  bool payloadStored = false;
  bool committed = false;

  if (invalidated) {
    typeStored =
        preferences.putString(KEY_RECORD_TYPE, normalizedType) != 0;
    payloadStored =
        preferences.putString(KEY_RECORD_DATA, payload) != 0;
    if (typeStored && payloadStored) {
      committed =
          preferences.putBool(KEY_RECORD_VALID, true) != 0;
    }
  }
  preferences.end();

  if (committed) {
    Serial.printf(
        "[SETTINGS] Stored persistent %s Tag Emulation record (%u bytes)\n",
        normalizedType.c_str(), static_cast<unsigned>(payload.length()));
  } else {
    Serial.println(
        "[SETTINGS] ERROR: Persistent Tag Emulation record was not committed");
  }
  return committed;
}

bool loadPersistedTagEmulationRecord(String &recordType, String &payload) {
  recordType = String();
  payload = String();

  Preferences preferences;
  if (!preferences.begin(SETTINGS_NAMESPACE, true)) {
    Serial.println(
        "[SETTINGS] WARNING: Could not open NVS to restore Tag Emulation");
    return false;
  }

  const bool valid = preferences.getBool(KEY_RECORD_VALID, false);
  if (valid) {
    recordType = preferences.getString(KEY_RECORD_TYPE, String());
    payload = preferences.getString(KEY_RECORD_DATA, String());
  }
  preferences.end();

  String normalizedType;
  if (!valid || !validManualRecord(recordType, payload, normalizedType)) {
    recordType = String();
    payload = String();
    return false;
  }

  recordType = normalizedType;
  return true;
}
