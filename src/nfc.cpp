#include <Arduino.h>
#include <algorithm>
#include <SPI.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_SPIDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <Adafruit_PN532.h>

#include "nfc.h"
#include "badge_settings.h"

// Single switch for every advanced NFC diagnostic: APDU byte dumps, Type 2
// pages, TLV/NDEF internals, polling detail, and write preparation detail.
#ifndef NFC_DEBUG_VERBOSE
#define NFC_DEBUG_VERBOSE true
#endif

namespace {

// -----------------------------------------------------------------------------
// PCB PN532 hardware
// -----------------------------------------------------------------------------
constexpr uint8_t SEL0 = 40;
constexpr uint8_t SEL1 = 39;
constexpr int8_t RSTPD_N = -1;  // Not connected to a valid ESP32-S3 GPIO
constexpr uint8_t PN532_SCK = 35;
constexpr uint8_t PN532_MISO = 36;
constexpr uint8_t PN532_MOSI = 37;
constexpr uint8_t PN532_SS = 38;

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

constexpr uint32_t OPERATION_TIMEOUT_MS = 15000;
constexpr uint32_t POLL_INTERVAL_MS = 140;
// Adafruit's timeout covers both command acknowledgement and the final
// InListPassiveTarget response. Twenty-five milliseconds was too short for
// reliable tag activation on the PN532.
constexpr uint16_t TAG_POLL_TIMEOUT_MS = 500;
constexpr uint8_t PASSIVE_ACTIVATION_RETRIES = 0x10;
constexpr uint32_t POLL_PROGRESS_INTERVAL_MS = 1000;
constexpr size_t MAX_TYPE2_USER_BYTES = 888;  // NTAG216 user memory
constexpr size_t MAX_WEB_PAYLOAD_BYTES = 700;
constexpr size_t MAX_TAG_EMULATION_PAYLOAD_BYTES = 220;
constexpr size_t MAX_EMULATED_NDEF_BYTES = 240;
constexpr uint8_t MAX_TARGET_READ_BYTES = 59;

// Wi-Fi Simple Configuration (WSC) attribute identifiers. Values are encoded
// big-endian inside an application/vnd.wfa.wsc MIME NDEF record.
constexpr uint16_t WSC_CREDENTIAL = 0x100E;
constexpr uint16_t WSC_NETWORK_INDEX = 0x1026;
constexpr uint16_t WSC_SSID = 0x1045;
constexpr uint16_t WSC_AUTH_TYPE = 0x1003;
constexpr uint16_t WSC_ENCRYPTION_TYPE = 0x100F;
constexpr uint16_t WSC_NETWORK_KEY = 0x1027;
constexpr uint16_t WSC_MAC_ADDRESS = 0x1020;
constexpr uint16_t WSC_AUTH_WPA2_PSK = 0x0020;
constexpr uint16_t WSC_ENCRYPTION_AES = 0x0008;
constexpr char WSC_MIME_TYPE[] = "application/vnd.wfa.wsc";
constexpr uint32_t TARGET_RETRY_INTERVAL_MS = 160;
constexpr uint8_t TYPE2_PAGE_BYTES = 4;
constexpr uint8_t TYPE2_READ_RETRIES = 3;
constexpr uint8_t TYPE2_WRITE_RETRIES = 3;
constexpr uint16_t TYPE2_RETRY_DELAY_MS = 4;
constexpr uint16_t TYPE2_WRITE_SETTLE_MS = 12;
constexpr size_t NFC_DEBUG_MAX_BYTES = 192;

constexpr uint8_t NFC_TASK_CORE = 0;
constexpr UBaseType_t NFC_TASK_PRIORITY = 1;
constexpr uint32_t NFC_TASK_STACK_SIZE = 8192;
constexpr UBaseType_t NFC_COMMAND_QUEUE_DEPTH = 4;
constexpr size_t NFC_WIFI_SSID_BUFFER_SIZE = 33;
constexpr size_t NFC_WIFI_PASSWORD_BUFFER_SIZE = 65;

uint8_t type2Buffer[MAX_TYPE2_USER_BYTES + 8];
uint8_t emulatedNdefFile[MAX_EMULATED_NDEF_BYTES + 2];

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
enum class NfcOperation : uint8_t {
  NONE,
  READ,
  WRITE_TEXT,
  WRITE_URL
};

const char *operationName(NfcOperation operation) {
  switch (operation) {
    case NfcOperation::READ: return "READ";
    case NfcOperation::WRITE_TEXT: return "WRITE_TEXT";
    case NfcOperation::WRITE_URL: return "WRITE_URL";
    default: return "NONE";
  }
}

void logNfcBytes(const char *label, const uint8_t *data, size_t length,
                 size_t maxBytes = NFC_DEBUG_MAX_BYTES) {
#if NFC_DEBUG_VERBOSE
  const size_t shown = std::min(length, maxBytes);
  Serial.printf("[NFC][DATA] %s (%u byte%s): ", label,
                static_cast<unsigned>(length), length == 1 ? "" : "s");
  for (size_t i = 0; i < shown; ++i) {
    if (i) Serial.print(' ');
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
  if (shown < length) Serial.print(" ...");
  Serial.println();
#else
  (void)label;
  (void)data;
  (void)length;
  (void)maxBytes;
#endif
}

void logType2Page(const char *action, uint16_t page,
                  const uint8_t data[TYPE2_PAGE_BYTES]) {
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][PAGE] %s page %u: %02X %02X %02X %02X | ",
                action, static_cast<unsigned>(page), data[0], data[1], data[2],
                data[3]);
  for (size_t i = 0; i < TYPE2_PAGE_BYTES; ++i) {
    const uint8_t c = data[i];
    Serial.print(c >= 32 && c <= 126 ? static_cast<char>(c) : '.');
  }
  Serial.println();
#else
  (void)action;
  (void)page;
  (void)data;
#endif
}

struct NfcState {
  bool readerReady = false;
  bool busy = false;
  bool writable = false;
  String status = "offline";
  String message = "PN532 has not been initialized.";
  String uid;
  String tagType;
  String recordType;
  String payload;
  String raw;
  uint16_t capacity = 0;
  uint32_t updatedAt = 0;

  bool tagEmulationEnabled = false;
  bool emulationReaderConnected = false;
  String emulatedRecordType;
  String emulatedPayload;
  String emulationMessage = "Tag emulation is stopped.";
  uint32_t tagScans = 0;
  uint32_t lastTagScanAt = 0;

  // Internal published-state field used by the Wi-Fi settings workflow.
  bool wifiOnboardingActive = false;
};

enum class NfcCommandType : uint8_t {
  READ,
  WRITE_TEXT,
  WRITE_URL,
  START_TEXT_EMULATION,
  START_URL_EMULATION,
  START_WIFI_ONBOARDING,
  STOP_EMULATION
};

struct NfcCommand {
  NfcCommandType type = NfcCommandType::READ;
  char payload[MAX_WEB_PAYLOAD_BYTES + 1] = {};
  char ssid[NFC_WIFI_SSID_BUFFER_SIZE] = {};
  char password[NFC_WIFI_PASSWORD_BUFFER_SIZE] = {};
  uint8_t apMac[6] = {};
  bool hasApMac = false;
};

NfcState state;
NfcState publishedState;
QueueHandle_t nfcCommandQueue = nullptr;
SemaphoreHandle_t nfcStateMutex = nullptr;
TaskHandle_t nfcTaskHandle = nullptr;
bool commandPending = false;
NfcOperation pendingOperation = NfcOperation::NONE;
String pendingPayload;
String type2IoError;
uint32_t operationDeadline = 0;
uint32_t lastPoll = 0;
uint32_t lastPollProgress = 0;
uint32_t pollAttempts = 0;

enum class EmulatedFile : uint8_t {
  NONE,
  CAPABILITY_CONTAINER,
  NDEF
};

// This profile selects which NDEF content is served. All profiles use the
// same proven Adafruit PN532 AsTarget() activation methodology.
enum class TagEmulationProfile : uint8_t {
  WIFI_WSC_ANDROID,
  GENERIC_NDEF
};

TagEmulationProfile tagEmulationProfile =
    TagEmulationProfile::GENERIC_NDEF;

enum class NdefApplicationVersion : uint8_t { NONE, V1_0, V2_0 };
NdefApplicationVersion selectedNdefApplicationVersion =
    NdefApplicationVersion::NONE;
bool tagEmulationSessionActive = false;
bool tagScanCounted = false;
bool ndefApplicationSelected = false;
EmulatedFile selectedEmulatedFile = EmulatedFile::NONE;
uint16_t emulatedNdefLength = 0;
uint16_t highestEmulatedReadOffset = 0;
uint32_t lastTargetAttempt = 0;

constexpr uint8_t TYPE4_CAPABILITY_CONTAINER_V1[] = {
    0x00, 0x0F,        // CCLEN: 15 bytes
    0x10,              // NFC Forum Type 4 Tag mapping version 1.0
    0x00, 0x3B,        // MLe: reader may read up to 59 bytes per command
    0x00, 0x34,        // MLc: maximum command data length
    0x04, 0x06,        // NDEF File Control TLV
    0xE1, 0x04,        // NDEF file ID
    0x00, 0xF0,        // maximum NDEF file size: 240 bytes
    0x00,              // read access: always allowed
    0xFF               // write access: never allowed
};

constexpr uint8_t TYPE4_CAPABILITY_CONTAINER_V2[] = {
    0x00, 0x0F,        // CCLEN: 15 bytes
    0x20,              // NFC Forum Type 4 Tag mapping version 2.0
    0x00, 0x3B,        // MLe: reader may read up to 59 bytes per command
    0x00, 0x34,        // MLc: maximum command data length
    0x04, 0x06,        // NDEF File Control TLV
    0xE1, 0x04,        // NDEF file ID
    0x00, 0xF0,        // maximum NDEF file size: 240 bytes
    0x00,              // read access: always allowed
    0xFF               // write access: never allowed
};

// -----------------------------------------------------------------------------
// Formatting helpers
// -----------------------------------------------------------------------------
String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    switch (c) {
      case '"': escaped += F("\\\""); break;
      case '\\': escaped += F("\\\\"); break;
      case '\b': escaped += F("\\b"); break;
      case '\f': escaped += F("\\f"); break;
      case '\n': escaped += F("\\n"); break;
      case '\r': escaped += F("\\r"); break;
      case '\t': escaped += F("\\t"); break;
      default:
        if (c < 0x20) {
          char encoded[7];
          snprintf(encoded, sizeof(encoded), "\\u%04X", c);
          escaped += encoded;
        } else {
          escaped += static_cast<char>(c);
        }
    }
  }
  return escaped;
}

String bytesToHex(const uint8_t *data, size_t length, size_t maxBytes = static_cast<size_t>(-1)) {
  const size_t shown = std::min(length, maxBytes);
  String output;
  output.reserve(shown * 3 + 4);
  char byteText[4];
  for (size_t i = 0; i < shown; ++i) {
    if (i) output += ' ';
    snprintf(byteText, sizeof(byteText), "%02X", data[i]);
    output += byteText;
  }
  if (shown < length) output += F(" ...");
  return output;
}

String uidToString(const uint8_t *uid, uint8_t uidLength) {
  String output;
  output.reserve(uidLength * 3);
  char byteText[4];
  for (uint8_t i = 0; i < uidLength; ++i) {
    if (i) output += ':';
    snprintf(byteText, sizeof(byteText), "%02X", uid[i]);
    output += byteText;
  }
  return output;
}

String bytesToString(const uint8_t *data, size_t length) {
  String output;
  output.reserve(length);
  for (size_t i = 0; i < length; ++i) output += static_cast<char>(data[i]);
  return output;
}

const char *uriPrefix(uint8_t code) {
  static const char *const prefixes[] = {
      "", "http://www.", "https://www.", "http://", "https://", "tel:",
      "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://",
      "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:",
      "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:",
      "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
      "tcpobex://", "irdaobex://", "file://", "urn:epc:id:",
      "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
      "urn:nfc:"};
  return code < (sizeof(prefixes) / sizeof(prefixes[0])) ? prefixes[code] : "";
}

uint8_t selectUriPrefix(const String &uri, String &remainder) {
  struct PrefixChoice {
    const char *text;
    uint8_t code;
  };
  static const PrefixChoice choices[] = {
      {"https://www.", 0x02}, {"http://www.", 0x01}, {"https://", 0x04},
      {"http://", 0x03},     {"mailto:", 0x06},    {"tel:", 0x05}};

  for (const auto &choice : choices) {
    if (uri.startsWith(choice.text)) {
      remainder = uri.substring(strlen(choice.text));
      return choice.code;
    }
  }
  remainder = uri;
  return 0x00;
}

void clearTagResult() {
  state.writable = false;
  state.uid = String();
  state.tagType = String();
  state.recordType = String();
  state.payload = String();
  state.raw = String();
  state.capacity = 0;
}

void finishSuccess(const String &message) {
  Serial.printf("[NFC][DONE] %s: %s\n", operationName(pendingOperation),
                message.c_str());
  state.busy = false;
  state.status = "success";
  state.message = message;
  state.updatedAt = millis();
  pendingOperation = NfcOperation::NONE;
  pendingPayload = String();
}

void finishError(const String &message) {
  Serial.printf("[NFC][ERROR] %s: %s\n", operationName(pendingOperation),
                message.c_str());
  state.busy = false;
  state.status = "error";
  state.message = message;
  state.updatedAt = millis();
  pendingOperation = NfcOperation::NONE;
  pendingPayload = String();
}

bool configurePassiveReader(const char *reason) {
  Serial.printf("[NFC][READER] Configuring passive reader mode: %s\n", reason);

  if (!nfc.SAMConfig()) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "PN532 SAM configuration failed while entering reader mode.";
    state.updatedAt = millis();
    Serial.println("[NFC][READER] ERROR: SAMConfig failed");
    return false;
  }

  if (!nfc.setPassiveActivationRetries(PASSIVE_ACTIVATION_RETRIES)) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "PN532 passive-target retry configuration failed.";
    state.updatedAt = millis();
    Serial.println("[NFC][READER] ERROR: setPassiveActivationRetries failed");
    return false;
  }

  delay(20);
  state.readerReady = true;
  Serial.printf("[NFC][READER] Ready; retries=0x%02X pollTimeout=%u ms\n",
                PASSIVE_ACTIVATION_RETRIES, TAG_POLL_TIMEOUT_MS);
  return true;
}

bool beginOperation(NfcOperation operation, const String &message) {
  if (!state.readerReady) {
    Serial.printf("[NFC][REJECT] %s: PN532 reader is not available\n",
                  operationName(operation));
    state.status = "error";
    state.message = "PN532 reader is not available.";
    state.updatedAt = millis();
    return false;
  }
  if (state.tagEmulationEnabled) {
    Serial.printf("[NFC][REJECT] %s: tag emulation is active\n",
                  operationName(operation));
    state.status = "error";
    state.message = "Stop tag emulation before reading or writing an external tag.";
    state.updatedAt = millis();
    return false;
  }
  if (state.busy) {
    Serial.printf("[NFC][REJECT] %s: %s is already active\n",
                  operationName(operation), operationName(pendingOperation));
    state.message = "Another NFC operation is already waiting for a tag.";
    state.updatedAt = millis();
    return false;
  }

  // Target/card-emulation mode and aborted commands can leave the PN532 in a
  // state where a new passive scan does not start cleanly. Reassert normal SAM
  // reader mode before every web-requested external-tag operation.
  if (!configurePassiveReader("starting external-tag operation")) return false;

  pendingOperation = operation;
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][QUEUE] %s queued; timeout=%lu ms\n",
                operationName(operation),
                static_cast<unsigned long>(OPERATION_TIMEOUT_MS));
#endif
  state.busy = true;
  state.status = "waiting";
  state.message = message;
  state.updatedAt = millis();
  operationDeadline = millis() + OPERATION_TIMEOUT_MS;
  lastPoll = 0;
  lastPollProgress = 0;
  pollAttempts = 0;
#if NFC_DEBUG_VERBOSE
  Serial.println("[NFC][POLL] Reader armed. Present the tag and keep it stationary.");
#endif
  return true;
}

// -----------------------------------------------------------------------------
// NDEF parsing and Type 2 tag access
// -----------------------------------------------------------------------------
bool readType2Page(uint16_t page, uint8_t output[TYPE2_PAGE_BYTES]) {
  if (page > 230U) {
    type2IoError = "Type 2 page address is outside the NTAG2xx range.";
    return false;
  }

  for (uint8_t attempt = 1; attempt <= TYPE2_READ_RETRIES; ++attempt) {
    memset(output, 0, TYPE2_PAGE_BYTES);
    if (nfc.ntag2xx_ReadPage(static_cast<uint8_t>(page), output)) {
      type2IoError = String();
      logType2Page("READ", page, output);
      return true;
    }
#if NFC_DEBUG_VERBOSE
    Serial.printf("[NFC][PAGE] Read page %u failed (attempt %u/%u)\n",
                  static_cast<unsigned>(page), attempt, TYPE2_READ_RETRIES);
#endif
    delay(TYPE2_RETRY_DELAY_MS);
    yield();
  }

  type2IoError = "Failed to read Type 2 page " + String(page) +
                 " after " + String(TYPE2_READ_RETRIES) + " attempts.";
  Serial.printf("[NFC] ERROR: %s\n", type2IoError.c_str());
  return false;
}

bool writeType2PageVerified(uint16_t page,
                            const uint8_t data[TYPE2_PAGE_BYTES]) {
  if (page < 4U || page > 225U) {
    type2IoError = "Refusing to write outside the NTAG2xx user-page range.";
    return false;
  }

  for (uint8_t attempt = 1; attempt <= TYPE2_WRITE_RETRIES; ++attempt) {
    uint8_t writableData[TYPE2_PAGE_BYTES];
    memcpy(writableData, data, TYPE2_PAGE_BYTES);
    logType2Page("WRITE request", page, writableData);

    if (nfc.ntag2xx_WritePage(static_cast<uint8_t>(page), writableData)) {
      delay(TYPE2_WRITE_SETTLE_MS);

      uint8_t verifyData[TYPE2_PAGE_BYTES] = {0, 0, 0, 0};
      if (readType2Page(page, verifyData) &&
          memcmp(verifyData, data, TYPE2_PAGE_BYTES) == 0) {
        logType2Page("WRITE verified", page, verifyData);
        type2IoError = String();
        return true;
      }
#if NFC_DEBUG_VERBOSE
      logType2Page("VERIFY expected", page, data);
      logType2Page("VERIFY actual", page, verifyData);
#endif
    } else {
#if NFC_DEBUG_VERBOSE
      Serial.printf("[NFC][PAGE] Write page %u command failed (attempt %u/%u)\n",
                    static_cast<unsigned>(page), attempt, TYPE2_WRITE_RETRIES);
#endif
    }

    delay(TYPE2_RETRY_DELAY_MS);
    yield();
  }

  type2IoError = "Failed to write and verify Type 2 page " + String(page) + ".";
  Serial.printf("[NFC] ERROR: %s\n", type2IoError.c_str());
  return false;
}

bool loadType2Bytes(size_t requiredBytes, size_t capacity,
                    size_t &loadedBytes) {
  if (requiredBytes > capacity || requiredBytes > MAX_TYPE2_USER_BYTES) {
    type2IoError = "The Type 2 TLV length exceeds the tag's advertised capacity.";
    return false;
  }

  while (loadedBytes < requiredBytes) {
    const uint16_t page = 4U + static_cast<uint16_t>(loadedBytes / TYPE2_PAGE_BYTES);
    uint8_t pageData[TYPE2_PAGE_BYTES];
    if (!readType2Page(page, pageData)) return false;

    const size_t copyLength =
        std::min<size_t>(TYPE2_PAGE_BYTES, capacity - loadedBytes);
    memcpy(type2Buffer + loadedBytes, pageData, copyLength);
    loadedBytes += copyLength;
    yield();
  }
  return true;
}

bool parseNdefRecord(const uint8_t *message, size_t messageLength) {
  if (messageLength < 4) {
    state.recordType = "Unknown";
    state.payload = "The NDEF message is too short to decode.";
    return false;
  }

  size_t offset = 0;
  const uint8_t header = message[offset++];
  const bool shortRecord = (header & 0x10) != 0;
  const bool hasId = (header & 0x08) != 0;
  const uint8_t tnf = header & 0x07;
  const uint8_t typeLength = message[offset++];

  uint32_t payloadLength = 0;
  if (shortRecord) {
    if (offset >= messageLength) return false;
    payloadLength = message[offset++];
  } else {
    if (offset + 4 > messageLength) return false;
    payloadLength = (static_cast<uint32_t>(message[offset]) << 24) |
                    (static_cast<uint32_t>(message[offset + 1]) << 16) |
                    (static_cast<uint32_t>(message[offset + 2]) << 8) |
                    static_cast<uint32_t>(message[offset + 3]);
    offset += 4;
  }

  uint8_t idLength = 0;
  if (hasId) {
    if (offset >= messageLength) return false;
    idLength = message[offset++];
  }

  if (offset + typeLength + idLength + payloadLength > messageLength) return false;

  const uint8_t *type = message + offset;
  offset += typeLength + idLength;
  const uint8_t *payload = message + offset;

#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][NDEF] header=%02X tnf=%u typeLen=%u payloadLen=%lu short=%s id=%s\n",
                header, tnf, typeLength, static_cast<unsigned long>(payloadLength),
                shortRecord ? "yes" : "no", hasId ? "yes" : "no");
  logNfcBytes("NDEF record", message, messageLength);
#endif

  if (tnf == 0x01 && typeLength == 1 && type[0] == 'T') {
    state.recordType = "Text";
    if (payloadLength < 1) {
      state.payload = String();
      return true;
    }
    const uint8_t status = payload[0];
    const bool utf16 = (status & 0x80) != 0;
    const uint8_t languageLength = status & 0x3F;
    if (1U + languageLength > payloadLength) return false;
    if (utf16) {
      state.payload = "UTF-16 text record detected; this UI currently displays UTF-8 text only.";
      return true;
    }
    state.payload = bytesToString(payload + 1 + languageLength,
                                  payloadLength - 1 - languageLength);
#if NFC_DEBUG_VERBOSE
    Serial.printf("[NFC][NDEF] Decoded Text: %s\n", state.payload.c_str());
#endif
    return true;
  }

  if (tnf == 0x01 && typeLength == 1 && type[0] == 'U') {
    state.recordType = "URL";
    if (payloadLength < 1) {
      state.payload = String();
      return true;
    }
    state.payload = String(uriPrefix(payload[0]));
    state.payload += bytesToString(payload + 1, payloadLength - 1);
#if NFC_DEBUG_VERBOSE
    Serial.printf("[NFC][NDEF] Decoded URL: %s\n", state.payload.c_str());
#endif
    return true;
  }

  state.recordType = "NDEF";
  state.payload = "Unsupported NDEF record type. Raw message bytes are shown below.";
  return true;
}

bool readUnformattedType2Preview() {
  constexpr size_t PREVIEW_PAGES = 6;
  uint8_t preview[PREVIEW_PAGES * TYPE2_PAGE_BYTES] = {0};
  size_t bytesRead = 0;

  for (size_t index = 0; index < PREVIEW_PAGES; ++index) {
    uint8_t pageData[TYPE2_PAGE_BYTES];
    if (!readType2Page(4U + index, pageData)) break;
    memcpy(preview + bytesRead, pageData, TYPE2_PAGE_BYTES);
    bytesRead += TYPE2_PAGE_BYTES;
  }

  state.recordType = "Raw";
  state.payload =
      "The tag responds like Type 2 memory, but it is not NFC Forum NDEF formatted. "
      "The first available user pages are shown as raw bytes.";
  state.raw = bytesToHex(preview, bytesRead);
  state.capacity = 0;
  state.writable = false;
  return bytesRead != 0;
}

bool readType2Tag(bool &type2MemoryResponded) {
  type2MemoryResponded = false;
  type2IoError = String();

  uint8_t capability[TYPE2_PAGE_BYTES];
  if (!readType2Page(3, capability)) return false;
  type2MemoryResponded = true;

  logType2Page("CAPABILITY", 3, capability);

  if (capability[0] != 0xE1) {
    state.tagType = "Type 2-compatible memory (not NDEF formatted)";
    state.message = "Type 2 memory was detected without a valid capability container.";
    return readUnformattedType2Preview();
  }

  state.tagType = "NFC Forum Type 2 (NTAG / Ultralight)";
  state.capacity = static_cast<uint16_t>(capability[2]) * 8U;
  state.capacity = std::min<uint16_t>(state.capacity,
                                     static_cast<uint16_t>(MAX_TYPE2_USER_BYTES));
  state.writable = (capability[3] & 0x0F) != 0x0F;

#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC] Type 2 CC: %02X %02X %02X %02X, data area %u bytes\n",
                capability[0], capability[1], capability[2], capability[3],
                static_cast<unsigned>(state.capacity));
#endif

  if (state.capacity == 0) {
    state.recordType = "None";
    state.payload = "The tag reports no user memory.";
    return true;
  }

  // Parse the Type 2 TLV stream incrementally. This is intentionally not a
  // full-memory dump: a typical short NDEF record can be decoded after only a
  // few pages. The previous implementation read every advertised page first,
  // causing 124 separate RF reads on an NTAG215 even for a tiny record.
  // JoinOurDiscord-discord.gg/thesafehouse-Ed-Loves-Furries-Bring-Us-Backpack-Beers-Next-Year
  size_t loadedBytes = 0;
  size_t offset = 0;

  while (offset < state.capacity) {
    if (!loadType2Bytes(offset + 1U, state.capacity, loadedBytes)) return false;
    const uint8_t tlvType = type2Buffer[offset++];
#if NFC_DEBUG_VERBOSE
    Serial.printf("[NFC][TLV] offset=%u type=%02X\n",
                  static_cast<unsigned>(offset - 1U), tlvType);
#endif

    if (tlvType == 0x00) continue;  // NULL TLV
    if (tlvType == 0xFE) break;     // Terminator TLV

    if (!loadType2Bytes(offset + 1U, state.capacity, loadedBytes)) return false;
    size_t tlvLength = type2Buffer[offset++];
    if (tlvLength == 0xFF) {
      if (!loadType2Bytes(offset + 2U, state.capacity, loadedBytes)) return false;
      tlvLength = (static_cast<size_t>(type2Buffer[offset]) << 8) |
                  static_cast<size_t>(type2Buffer[offset + 1]);
      offset += 2;
    }

    if (tlvLength > state.capacity - offset) {
      type2IoError = "The tag contains a TLV length larger than its data area.";
      return false;
    }

#if NFC_DEBUG_VERBOSE
    Serial.printf("[NFC][TLV] type=%02X length=%u payloadOffset=%u\n", tlvType,
                  static_cast<unsigned>(tlvLength),
                  static_cast<unsigned>(offset));
#endif

    if (tlvType == 0x03) {
      if (tlvLength == 0) {
        state.recordType = "Empty";
        state.payload = "This tag contains an empty NDEF message.";
        state.raw = String();
        return true;
      }

      if (!loadType2Bytes(offset + tlvLength, state.capacity, loadedBytes)) {
        return false;
      }

      state.raw = bytesToHex(type2Buffer + offset, tlvLength, 320);
      if (!parseNdefRecord(type2Buffer + offset, tlvLength)) {
        state.recordType = "NDEF";
        state.payload = "NDEF data was found but could not be decoded.";
      }

#if NFC_DEBUG_VERBOSE
      Serial.printf("[NFC] NDEF read completed after %u user bytes (%u pages)\n",
                    static_cast<unsigned>(loadedBytes),
                    static_cast<unsigned>((loadedBytes + 3U) / 4U));
#endif
      return true;
    }

    offset += tlvLength;
  }

  state.recordType = "None";
  state.payload = "No NDEF message was found on the tag.";
  state.raw = String();
  return true;
}

bool buildNdef(const String &recordType, const String &input, size_t capacity,
               size_t &totalLength, String &error) {
  if (input.length() == 0) {
    error = "Enter text or a URL before writing.";
    return false;
  }
  if (input.length() > MAX_WEB_PAYLOAD_BYTES) {
    error = "The payload exceeds the 700-byte controller limit.";
    return false;
  }

  String recordPayload = input;
  uint8_t prefixCode = 0;
  uint8_t typeByte = 0;
  size_t payloadLength = 0;

  if (recordType == "url") {
    typeByte = 'U';
    prefixCode = selectUriPrefix(input, recordPayload);
    payloadLength = 1U + recordPayload.length();
  } else {
    typeByte = 'T';
    payloadLength = 3U + input.length();  // status byte + "en" + text
  }

  const bool shortRecord = payloadLength <= 255U;
  const size_t recordHeaderLength = shortRecord ? 4U : 7U;
  const size_t ndefLength = recordHeaderLength + payloadLength;
  const size_t tlvHeaderLength = ndefLength < 255U ? 2U : 4U;
  totalLength = tlvHeaderLength + ndefLength + 1U;  // terminator TLV

  if (totalLength > capacity || totalLength > MAX_TYPE2_USER_BYTES) {
    error = "The NDEF record is too large for this tag.";
    return false;
  }

  memset(type2Buffer, 0, totalLength + 4U);
  size_t offset = 0;
  type2Buffer[offset++] = 0x03;  // NDEF Message TLV
  if (ndefLength < 255U) {
    type2Buffer[offset++] = static_cast<uint8_t>(ndefLength);
  } else {
    type2Buffer[offset++] = 0xFF;
    type2Buffer[offset++] = static_cast<uint8_t>((ndefLength >> 8) & 0xFF);
    type2Buffer[offset++] = static_cast<uint8_t>(ndefLength & 0xFF);
  }

  type2Buffer[offset++] = shortRecord ? 0xD1 : 0xC1;  // MB + ME + TNF well-known
  type2Buffer[offset++] = 0x01;                       // type length
  if (shortRecord) {
    type2Buffer[offset++] = static_cast<uint8_t>(payloadLength);
  } else {
    type2Buffer[offset++] = static_cast<uint8_t>((payloadLength >> 24) & 0xFF);
    type2Buffer[offset++] = static_cast<uint8_t>((payloadLength >> 16) & 0xFF);
    type2Buffer[offset++] = static_cast<uint8_t>((payloadLength >> 8) & 0xFF);
    type2Buffer[offset++] = static_cast<uint8_t>(payloadLength & 0xFF);
  }
  type2Buffer[offset++] = typeByte;

  if (recordType == "url") {
    type2Buffer[offset++] = prefixCode;
    memcpy(type2Buffer + offset, recordPayload.c_str(), recordPayload.length());
    offset += recordPayload.length();
  } else {
    type2Buffer[offset++] = 0x02;  // UTF-8, two-character language code
    type2Buffer[offset++] = 'e';
    type2Buffer[offset++] = 'n';
    memcpy(type2Buffer + offset, input.c_str(), input.length());
    offset += input.length();
  }

  type2Buffer[offset++] = 0xFE;
  totalLength = offset;
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][NDEF] Built %s record: input=%u bytes, encoded=%u bytes\n",
                recordType.c_str(), static_cast<unsigned>(input.length()),
                static_cast<unsigned>(totalLength));
#endif
  logNfcBytes("Encoded Type 2 TLV", type2Buffer, totalLength);
  return true;
}

bool writeType2Buffer(size_t totalLength) {
  type2IoError = String();

  // NFC Forum transaction-safe update: publish a zero-length NDEF TLV first,
  // write the remainder, then commit the real first page last.
  const uint8_t emptyTag[TYPE2_PAGE_BYTES] = {0x03, 0x00, 0xFE, 0x00};
  if (!writeType2PageVerified(4, emptyTag)) return false;

  const size_t pages = (totalLength + TYPE2_PAGE_BYTES - 1U) / TYPE2_PAGE_BYTES;
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][WRITE] Storing %u bytes across %u page(s); commit page 4 last\n",
                static_cast<unsigned>(totalLength),
                static_cast<unsigned>(pages));
#endif
  for (size_t pageIndex = 1; pageIndex < pages; ++pageIndex) {
    uint8_t pageData[TYPE2_PAGE_BYTES] = {0, 0, 0, 0};
    const size_t sourceOffset = pageIndex * TYPE2_PAGE_BYTES;
    const size_t remaining =
        totalLength > sourceOffset ? totalLength - sourceOffset : 0;
    memcpy(pageData, type2Buffer + sourceOffset,
           std::min<size_t>(TYPE2_PAGE_BYTES, remaining));

    if (!writeType2PageVerified(4U + pageIndex, pageData)) return false;
    yield();
  }

  uint8_t firstPage[TYPE2_PAGE_BYTES] = {0, 0, 0, 0};
  memcpy(firstPage, type2Buffer,
         std::min<size_t>(TYPE2_PAGE_BYTES, totalLength));
  if (!writeType2PageVerified(4, firstPage)) return false;

  Serial.printf("[NFC] NDEF write committed and verified across %u pages\n",
                static_cast<unsigned>(pages));
  return true;
}

bool prepareType2ForWrite(uint16_t &capacity, String &error) {
  type2IoError = String();
  uint8_t capability[TYPE2_PAGE_BYTES];
  if (!readType2Page(3, capability)) {
    error = type2IoError.length() ? type2IoError
                                  : "The Type 2 capability page could not be read.";
    return false;
  }
  logType2Page("WRITE capability", 3, capability);
  if (capability[0] != 0xE1) {
    error =
        "This tag is not NFC Forum Type 2 formatted. Automatic formatting is "
        "disabled because writing the capability container without positively "
        "identifying the chip could permanently alter an incompatible tag.";
    return false;
  }
  if ((capability[3] & 0x0F) == 0x0F) {
    error = "This tag reports that its NDEF data is read-only.";
    return false;
  }
  capacity = std::min<uint16_t>(static_cast<uint16_t>(capability[2]) * 8U,
                                static_cast<uint16_t>(MAX_TYPE2_USER_BYTES));
  if (capacity == 0) {
    error = "The tag reports no writable user memory.";
    return false;
  }
  return true;
}


// -----------------------------------------------------------------------------
// NFC Forum Type 4 Tag emulation
// -----------------------------------------------------------------------------
bool appendWscAttribute(uint8_t *buffer, size_t capacity, size_t &offset,
                        uint16_t attributeId, const uint8_t *value,
                        size_t valueLength) {
  if (!buffer || valueLength > 0xFFFFU ||
      offset + 4U + valueLength > capacity) {
    return false;
  }

  buffer[offset++] = static_cast<uint8_t>((attributeId >> 8) & 0xFF);
  buffer[offset++] = static_cast<uint8_t>(attributeId & 0xFF);
  buffer[offset++] = static_cast<uint8_t>((valueLength >> 8) & 0xFF);
  buffer[offset++] = static_cast<uint8_t>(valueLength & 0xFF);
  if (valueLength && value) {
    memcpy(buffer + offset, value, valueLength);
    offset += valueLength;
  }
  return true;
}

bool buildWifiOnboardingNdef(const String &ssid, const String &password,
                             const uint8_t apMac[6], String &error) {
  if (ssid.length() == 0 || ssid.length() > 32U) {
    error = "The generated Wi-Fi SSID is empty or exceeds 32 bytes.";
    return false;
  }
  if (password.length() < 8U || password.length() > 63U) {
    error = "The Wi-Fi password must contain 8 to 63 characters.";
    return false;
  }

  // Keep the Android Wi-Fi Simple Configuration credential payload unchanged.
  // This is the same WSC attribute order and content used by the working
  // single-record Android implementation.
  uint8_t credential[160] = {0};
  size_t credentialLength = 0;
  const uint8_t networkIndex = 1;
  const uint8_t authType[] = {
      static_cast<uint8_t>((WSC_AUTH_WPA2_PSK >> 8) & 0xFF),
      static_cast<uint8_t>(WSC_AUTH_WPA2_PSK & 0xFF)};
  const uint8_t encryptionType[] = {
      static_cast<uint8_t>((WSC_ENCRYPTION_AES >> 8) & 0xFF),
      static_cast<uint8_t>(WSC_ENCRYPTION_AES & 0xFF)};
  const uint8_t zeroMac[6] = {0};
  const uint8_t *mac = apMac ? apMac : zeroMac;

  const bool attributesBuilt =
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_NETWORK_INDEX, &networkIndex, 1) &&
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_SSID,
                         reinterpret_cast<const uint8_t *>(ssid.c_str()),
                         ssid.length()) &&
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_AUTH_TYPE, authType, sizeof(authType)) &&
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_ENCRYPTION_TYPE, encryptionType,
                         sizeof(encryptionType)) &&
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_NETWORK_KEY,
                         reinterpret_cast<const uint8_t *>(password.c_str()),
                         password.length()) &&
      appendWscAttribute(credential, sizeof(credential), credentialLength,
                         WSC_MAC_ADDRESS, mac, 6);

  if (!attributesBuilt) {
    error = "The Wi-Fi credential attributes exceed the emulated tag buffer.";
    return false;
  }

  uint8_t wscPayload[176] = {0};
  size_t wscPayloadLength = 0;
  if (!appendWscAttribute(wscPayload, sizeof(wscPayload), wscPayloadLength,
                          WSC_CREDENTIAL, credential, credentialLength)) {
    error = "The Wi-Fi credential could not be wrapped in a WSC record.";
    return false;
  }

  // The second NDEF record is a standard NFC Forum Well Known Text record.
  // Dedicated iPhone NFC apps can enumerate the message and decode this record.
  const String textPayload =
      "Wi-Fi Network: " + ssid + "\nPassword: " + password;
  const size_t textPayloadLength = 3U + textPayload.length();

  constexpr size_t mimeTypeLength = sizeof(WSC_MIME_TYPE) - 1U;
  const size_t wscRecordLength = 3U + mimeTypeLength + wscPayloadLength;
  const size_t textRecordLength = 4U + textPayloadLength;
  const size_t ndefLength = wscRecordLength + textRecordLength;

  if (wscPayloadLength > 255U || textPayloadLength > 255U ||
      ndefLength > MAX_EMULATED_NDEF_BYTES) {
    error = "The combined Wi-Fi and Text NDEF message is too large.";
    return false;
  }

  memset(emulatedNdefFile, 0, sizeof(emulatedNdefFile));
  emulatedNdefFile[0] = static_cast<uint8_t>((ndefLength >> 8) & 0xFF);
  emulatedNdefFile[1] = static_cast<uint8_t>(ndefLength & 0xFF);

  size_t offset = 2;

  // Record 1: Android WSC MIME record. Its MIME type, WSC payload bytes, and
  // record order are unchanged. Only ME is cleared because a second record
  // follows in the same NDEF message.
  emulatedNdefFile[offset++] = 0x92;  // MB + SR + TNF MIME media; ME clear
  emulatedNdefFile[offset++] = static_cast<uint8_t>(mimeTypeLength);
  emulatedNdefFile[offset++] = static_cast<uint8_t>(wscPayloadLength);
  memcpy(emulatedNdefFile + offset, WSC_MIME_TYPE, mimeTypeLength);
  offset += mimeTypeLength;
  memcpy(emulatedNdefFile + offset, wscPayload, wscPayloadLength);
  offset += wscPayloadLength;

  // Record 2: iPhone-readable NFC Forum Text RTD.
  emulatedNdefFile[offset++] = 0x51;  // ME + SR + TNF well-known; MB clear
  emulatedNdefFile[offset++] = 0x01;  // type length
  emulatedNdefFile[offset++] = static_cast<uint8_t>(textPayloadLength);
  emulatedNdefFile[offset++] = 'T';
  emulatedNdefFile[offset++] = 0x02;  // UTF-8, 2-byte language code
  emulatedNdefFile[offset++] = 'e';
  emulatedNdefFile[offset++] = 'n';
  memcpy(emulatedNdefFile + offset, textPayload.c_str(), textPayload.length());
  offset += textPayload.length();

  if (offset != ndefLength + 2U) {
    error = "The combined Wi-Fi NDEF message length is inconsistent.";
    return false;
  }

  emulatedNdefLength = static_cast<uint16_t>(ndefLength);
  state.emulatedRecordType = "Wi-Fi + Text";
  state.emulatedPayload =
      "SSID: " + ssid + "\nPassword: " + password;

#if NFC_DEBUG_VERBOSE
  Serial.printf(
      "[NFC][WIFI] Built dual-record onboarding message: "
      "SSID=%s total=%u bytes WSC=%u bytes Text=%u bytes\n",
      ssid.c_str(), static_cast<unsigned>(ndefLength),
      static_cast<unsigned>(wscPayloadLength),
      static_cast<unsigned>(textPayloadLength));
#endif
  logNfcBytes("Wi-Fi onboarding NDEF", emulatedNdefFile + 2, ndefLength);
  return true;
}

bool buildTagEmulationNdef(const String &recordType, const String &input,
                         String &error) {
  String normalized = recordType;
  normalized.toLowerCase();
  if (normalized != "text" && normalized != "url") {
    error = "Tag-emulation record type must be text or url.";
    return false;
  }
  if (input.length() == 0) {
    error = "Enter text or a URL before starting tag emulation.";
    return false;
  }
  if (input.length() > MAX_TAG_EMULATION_PAYLOAD_BYTES) {
    error = "Tag-emulation payload exceeds the 220-byte limit.";
    return false;
  }

  String recordPayload = input;
  uint8_t typeByte = 0;
  uint8_t prefixCode = 0;
  size_t payloadLength = 0;

  if (normalized == "url") {
    typeByte = 'U';
    prefixCode = selectUriPrefix(input, recordPayload);
    payloadLength = 1U + recordPayload.length();
  } else {
    typeByte = 'T';
    payloadLength = 3U + input.length();  // status byte + "en" + UTF-8 text
  }

  // The configured limit keeps the record in short-record form, simplifying
  // Type 4 Tag reads and keeping every APDU response inside the PN532 buffer.
  const size_t ndefLength = 4U + payloadLength;
  if (ndefLength > MAX_EMULATED_NDEF_BYTES || payloadLength > 255U) {
    error = "The NDEF record is too large for tag emulation.";
    return false;
  }

  memset(emulatedNdefFile, 0, sizeof(emulatedNdefFile));
  emulatedNdefFile[0] = static_cast<uint8_t>((ndefLength >> 8) & 0xFF);
  emulatedNdefFile[1] = static_cast<uint8_t>(ndefLength & 0xFF);

  size_t offset = 2;
  emulatedNdefFile[offset++] = 0xD1;  // MB + ME + SR + TNF well-known
  emulatedNdefFile[offset++] = 0x01;  // type length
  emulatedNdefFile[offset++] = static_cast<uint8_t>(payloadLength);
  emulatedNdefFile[offset++] = typeByte;

  if (normalized == "url") {
    emulatedNdefFile[offset++] = prefixCode;
    memcpy(emulatedNdefFile + offset, recordPayload.c_str(), recordPayload.length());
    offset += recordPayload.length();
  } else {
    emulatedNdefFile[offset++] = 0x02;  // UTF-8 with a 2-byte language code
    emulatedNdefFile[offset++] = 'e';
    emulatedNdefFile[offset++] = 'n';
    memcpy(emulatedNdefFile + offset, input.c_str(), input.length());
    offset += input.length();
  }

  emulatedNdefLength = static_cast<uint16_t>(ndefLength);
  state.emulatedRecordType = normalized == "url" ? "URL" : "Text";
  state.emulatedPayload = input;
  return true;
}

void logTagEmulationFrame(const char *direction, const uint8_t *data,
                         size_t length) {
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][EMULATION] %s (%u): ", direction,
                static_cast<unsigned>(length));
  for (size_t i = 0; i < length; ++i) {
    if (i) Serial.print(' ');
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
  Serial.println();
#else
  (void)direction;
  (void)data;
  (void)length;
#endif
}

bool sendTargetResponse(const uint8_t *data, size_t dataLength,
                        uint8_t statusHigh = 0x90,
                        uint8_t statusLow = 0x00) {
  if (dataLength > MAX_TARGET_READ_BYTES) return false;

  uint8_t response[MAX_TARGET_READ_BYTES + 3] = {0};
  response[0] = PN532_COMMAND_TGSETDATA;
  if (dataLength && data) memcpy(response + 1, data, dataLength);
  response[dataLength + 1] = statusHigh;
  response[dataLength + 2] = statusLow;

  // Adafruit PN532 1.3.4 transmits TgSetData correctly, but its
  // setDataTarget() return-value check compares the response-command byte with
  // 0x15 (the SAMConfiguration response) instead of checking TgSetData's 0x8F
  // response/status. A valid R-APDU is therefore sent even though the helper
  // returns false. Do not tear down the tag-emulation session based on that broken
  // return value; the next TgGetData call is the reliable session-health test.
  logTagEmulationFrame("TX", response + 1, dataLength + 2);
  (void)nfc.setDataTarget(response, static_cast<uint8_t>(dataLength + 3));
  return true;
}

bool selectNdefApplication(const uint8_t *apdu, uint8_t length) {
  // NFC Forum Type 4 Tag applications exist with both the original 1.0 AID
  // (...0100) and the current 2.0 AID (...0101). Android normally selects
  // 2.0; iPhone/Core NFC readers and third-party apps may probe either.
  static const uint8_t NDEF_APPLICATION_ID_V1[] = {
      0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x00};
  static const uint8_t NDEF_APPLICATION_ID_V2[] = {
      0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};

  if (!apdu || length < 12 || apdu[1] != 0xA4 || apdu[2] != 0x04 ||
      apdu[4] != 0x07) {
    return false;
  }

  if (memcmp(apdu + 5, NDEF_APPLICATION_ID_V2,
             sizeof(NDEF_APPLICATION_ID_V2)) == 0) {
    selectedNdefApplicationVersion = NdefApplicationVersion::V2_0;
#if NFC_DEBUG_VERBOSE
    Serial.println("[NFC][EMULATION] Selected NDEF Tag Application v2.0");
#endif
    return true;
  }
  if (memcmp(apdu + 5, NDEF_APPLICATION_ID_V1,
             sizeof(NDEF_APPLICATION_ID_V1)) == 0) {
    selectedNdefApplicationVersion = NdefApplicationVersion::V1_0;
#if NFC_DEBUG_VERBOSE
    Serial.println("[NFC][EMULATION] Selected NDEF Tag Application v1.0");
#endif
    return true;
  }
  return false;
}

void resetTagEmulationSession() {
  tagEmulationSessionActive = false;
  tagScanCounted = false;
  ndefApplicationSelected = false;
  selectedNdefApplicationVersion = NdefApplicationVersion::NONE;
  selectedEmulatedFile = EmulatedFile::NONE;
  highestEmulatedReadOffset = 0;
  state.emulationReaderConnected = false;
}

bool processTagEmulationApdu(const uint8_t *apdu, uint8_t length) {
  if (length < 2) return sendTargetResponse(nullptr, 0, 0x67, 0x00);

  // SELECT NDEF Tag Application: D2760000850101
  if (apdu[1] == 0xA4 && apdu[2] == 0x04) {
    if (!selectNdefApplication(apdu, length)) {
      return sendTargetResponse(nullptr, 0, 0x6A, 0x82);
    }
    ndefApplicationSelected = true;
    selectedEmulatedFile = EmulatedFile::NONE;
    return sendTargetResponse(nullptr, 0);
  }

  // SELECT FILE: E103 is the Capability Container; E104 is the NDEF file.
  if (apdu[1] == 0xA4 && (apdu[2] == 0x00 || apdu[2] == 0x02)) {
    if (!ndefApplicationSelected) {
      return sendTargetResponse(nullptr, 0, 0x69, 0x85);
    }
    if (length < 7 || apdu[4] != 0x02) {
      return sendTargetResponse(nullptr, 0, 0x67, 0x00);
    }

    const uint16_t fileId =
        (static_cast<uint16_t>(apdu[5]) << 8) | apdu[6];
    if (fileId == 0xE103) {
      selectedEmulatedFile = EmulatedFile::CAPABILITY_CONTAINER;
      return sendTargetResponse(nullptr, 0);
    }
    if (fileId == 0xE104) {
      selectedEmulatedFile = EmulatedFile::NDEF;
      return sendTargetResponse(nullptr, 0);
    }
    return sendTargetResponse(nullptr, 0, 0x6A, 0x82);
  }

  // READ BINARY from the currently selected file.
  if (apdu[1] == 0xB0) {
    if (length < 5 || selectedEmulatedFile == EmulatedFile::NONE) {
      return sendTargetResponse(nullptr, 0, 0x69, 0x85);
    }

    const uint16_t readOffset =
        (static_cast<uint16_t>(apdu[2]) << 8) | apdu[3];

    // Accept both short and extended Le encodings. The Type 4 mapping normally
    // uses short APDUs, but some iPhone NFC applications issue extended-form
    // READ BINARY commands while probing an ISO7816 tag.
    uint16_t requested = 0;
    if (apdu[4] != 0) {
      requested = apdu[4];
    } else if (length >= 7) {
      requested = (static_cast<uint16_t>(apdu[5]) << 8) | apdu[6];
      if (requested == 0) requested = 256U;
    } else {
      requested = 256U;
    }

    const uint8_t *fileData = nullptr;
    size_t fileLength = 0;
    if (selectedEmulatedFile == EmulatedFile::CAPABILITY_CONTAINER) {
      if (selectedNdefApplicationVersion == NdefApplicationVersion::V1_0) {
        fileData = TYPE4_CAPABILITY_CONTAINER_V1;
        fileLength = sizeof(TYPE4_CAPABILITY_CONTAINER_V1);
      } else {
        fileData = TYPE4_CAPABILITY_CONTAINER_V2;
        fileLength = sizeof(TYPE4_CAPABILITY_CONTAINER_V2);
      }
    } else {
      fileData = emulatedNdefFile;
      fileLength = static_cast<size_t>(emulatedNdefLength) + 2U;
    }

    if (readOffset >= fileLength) {
      return sendTargetResponse(nullptr, 0, 0x6B, 0x00);
    }

    const size_t remaining = fileLength - readOffset;
    const size_t responseLength =
        std::min<size_t>(std::min<size_t>(requested, remaining),
                         MAX_TARGET_READ_BYTES);
    if (!sendTargetResponse(fileData + readOffset, responseLength)) return false;

    if (selectedEmulatedFile == EmulatedFile::NDEF) {
      const uint16_t readEnd = static_cast<uint16_t>(readOffset + responseLength);
      highestEmulatedReadOffset = std::max<uint16_t>(highestEmulatedReadOffset, readEnd);
      if (!tagScanCounted && highestEmulatedReadOffset >= fileLength) {
        tagScanCounted = true;
        ++state.tagScans;
        state.lastTagScanAt = millis();
        state.emulationMessage = "NDEF record scanned from the emulated tag.";
        state.message = state.emulationMessage;
        state.updatedAt = millis();
        Serial.printf("[NFC] Emulated %s record scanned (%u bytes)\n",
                      state.emulatedRecordType.c_str(),
                      static_cast<unsigned>(emulatedNdefLength));
      }
    }
    return true;
  }

  // iPhone readers can issue discovery/probing commands before selecting the
  // NFC Forum NDEF application. These responses are ISO7816-compliant and do
  // not change the existing Android/WSC read sequence.
  if (apdu[1] == 0x70) {  // MANAGE CHANNEL is not supported
    return sendTargetResponse(nullptr, 0, 0x6A, 0x81);
  }
  if (apdu[1] == 0xC0) {  // GET RESPONSE: no pending response data
    return sendTargetResponse(nullptr, 0);
  }

  // This emulated tag is deliberately read-only.
  if (apdu[1] == 0xD6) {
    return sendTargetResponse(nullptr, 0, 0x69, 0x82);
  }

  return sendTargetResponse(nullptr, 0, 0x6D, 0x00);
}

void serviceTagEmulation() {
  const uint32_t now = millis();
  uint8_t apdu[64] = {0};
  uint8_t apduLength = 0;

  if (!tagEmulationSessionActive) {
    if (now - lastTargetAttempt < TARGET_RETRY_INTERVAL_MS) return;
    lastTargetAttempt = now;

    // Use the same target activation methodology that has proven reliable for
    // the dual-record Wi-Fi credential emulation on both Android and iPhone.
    //
    // Adafruit PN532 1.3.4's AsTarget() success check is incorrect: after
    // TgInitAsTarget it checks for 0x15 instead of the TgInitAsTarget response
    // code. The PN532 can already be activated by the phone while AsTarget()
    // still returns false. Start target mode, then use the first successfully
    // received C-APDU as proof that a tag-emulation session is active.
    (void)nfc.AsTarget();

    if (!nfc.getDataTarget(apdu, &apduLength) || apduLength == 0) {
      state.emulationReaderConnected = false;
      return;
    }

    tagEmulationSessionActive = true;
    tagScanCounted = false;
    ndefApplicationSelected = false;
    selectedEmulatedFile = EmulatedFile::NONE;
    highestEmulatedReadOffset = 0;
    state.emulationReaderConnected = true;
    state.emulationMessage = "NFC reader detected. Serving the configured NDEF record...";
    state.message = state.emulationMessage;
    state.updatedAt = millis();
    if (tagEmulationProfile == TagEmulationProfile::WIFI_WSC_ANDROID) {
      Serial.println("[NFC] NFC reader connected to WSC emulated tag");
    } else {
      Serial.println(
          "[NFC] NFC reader connected to user-defined NDEF emulated tag");
    }
  } else if (!nfc.getDataTarget(apdu, &apduLength) || apduLength == 0) {
    resetTagEmulationSession();
    if (state.tagEmulationEnabled) {
      state.emulationMessage =
          "Ready. Hold an NFC reader or phone over the PN532 antenna to scan the record.";
      state.message = state.emulationMessage;
      state.updatedAt = millis();
    }
    return;
  }

  logTagEmulationFrame("RX", apdu, apduLength);
  if (!processTagEmulationApdu(apdu, apduLength)) {
    resetTagEmulationSession();
    state.emulationMessage =
        "The NFC reader exchange ended before the emulated record was fully scanned.";
    state.message = state.emulationMessage;
    state.updatedAt = millis();
  }
}

void identifyTag(const uint8_t *uid, uint8_t uidLength) {
  clearTagResult();
  state.uid = uidToString(uid, uidLength);
  if (uidLength == 7) {
    state.tagType = "ISO14443A (7-byte UID)";
  } else if (uidLength == 4) {
    state.tagType = "ISO14443A (4-byte UID)";
  } else {
    state.tagType = "ISO14443A";
  }
}

void processDetectedTag(uint8_t *uid, uint8_t uidLength) {
  identifyTag(uid, uidLength);
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][TAG] Detected UID=%s length=%u operation=%s\n",
                state.uid.c_str(), uidLength, operationName(pendingOperation));
#endif

  if (pendingOperation == NfcOperation::READ) {
    bool type2MemoryResponded = false;
    if (readType2Tag(type2MemoryResponded)) {
      finishSuccess("Tag read successfully.");
      return;
    }

    if (type2MemoryResponded || uidLength == 7) {
      const String detail = type2IoError.length()
                                ? " " + type2IoError
                                : String();
      finishError("The tag was detected, but its Type 2 memory could not be read." +
                  detail);
      return;
    }

    state.recordType = "UID only";
    state.payload =
        "The UID was read successfully. This build reads and writes NDEF on "
        "NFC Forum Type 2 memory. MIFARE Classic requires sector authentication "
        "and is not modified by the web interface.";
    finishSuccess("Tag UID read successfully.");
    return;
  }

  uint16_t capacity = 0;
  String error;
  if (!prepareType2ForWrite(capacity, error)) {
    finishError(error);
    return;
  }
  state.capacity = capacity;
  state.writable = true;
  state.tagType = "NFC Forum Type 2 (NTAG / Ultralight)";

  const String recordType =
      pendingOperation == NfcOperation::WRITE_URL ? "url" : "text";
#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][WRITE] Preparing %s payload (%u bytes): %s\n",
                recordType.c_str(), static_cast<unsigned>(pendingPayload.length()),
                pendingPayload.c_str());
#endif
  size_t totalLength = 0;
  if (!buildNdef(recordType, pendingPayload, capacity, totalLength, error)) {
    finishError(error);
    return;
  }
  if (!writeType2Buffer(totalLength)) {
    finishError("Writing failed before the complete NDEF record was stored. " +
                type2IoError);
    return;
  }

  // Incremental verification reads only through the committed NDEF TLV instead
  // of dumping the complete tag capacity.
  bool type2MemoryResponded = false;
  if (!readType2Tag(type2MemoryResponded)) {
    finishError("The tag was written, but the verification read failed. " +
                type2IoError);
    return;
  }
  finishSuccess(recordType == "url" ? "URL written and verified."
                                     : "Text written and verified.");
}

// -----------------------------------------------------------------------------
// Dedicated NFC worker and command queue
// -----------------------------------------------------------------------------
void publishNfcState() {
  if (!nfcStateMutex) {
    publishedState = state;
    return;
  }

  if (xSemaphoreTake(nfcStateMutex, portMAX_DELAY) == pdTRUE) {
    publishedState = state;
    xSemaphoreGive(nfcStateMutex);
  }
}

NfcState getPublishedStateSnapshot() {
  NfcState snapshot;
  if (!nfcStateMutex) return publishedState;

  if (xSemaphoreTake(nfcStateMutex, portMAX_DELAY) == pdTRUE) {
    snapshot = publishedState;
    xSemaphoreGive(nfcStateMutex);
  }
  return snapshot;
}

void setPublishedErrorLocked(const String &message) {
  publishedState.status = "error";
  publishedState.message = message;
  publishedState.updatedAt = millis();
}

void setPublishedError(const String &message) {
  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  setPublishedErrorLocked(message);
  xSemaphoreGive(nfcStateMutex);
}

bool copyCommandString(const String &source, char *destination,
                       size_t destinationSize) {
  if (!destination || destinationSize == 0 ||
      source.length() >= destinationSize) {
    return false;
  }

  memcpy(destination, source.c_str(), source.length());
  destination[source.length()] = '\0';
  return true;
}

bool sendCommandWhileLocked(const NfcCommand &command) {
  if (!nfcCommandQueue || commandPending) return false;

  commandPending = true;
  if (xQueueSend(nfcCommandQueue, &command, 0) != pdTRUE) {
    commandPending = false;
    return false;
  }
  return true;
}

void markCommandDequeued() {
  if (!nfcStateMutex) {
    commandPending = false;
    return;
  }

  if (xSemaphoreTake(nfcStateMutex, portMAX_DELAY) == pdTRUE) {
    commandPending = false;
    xSemaphoreGive(nfcStateMutex);
  }
}

bool executeQueueNfcRead() {
  if (!beginOperation(NfcOperation::READ,
                      "Hold an NFC tag over the PCB reader to scan it.")) {
    return false;
  }
  pendingPayload = String();
  clearTagResult();
  return true;
}

bool executeQueueNfcWrite(const NfcCommand &command) {
  const bool isUrl = command.type == NfcCommandType::WRITE_URL;
  const NfcOperation operation =
      isUrl ? NfcOperation::WRITE_URL : NfcOperation::WRITE_TEXT;
  const char *message =
      isUrl
          ? "Hold a writable Type 2 tag over the reader to write the URL."
          : "Hold a writable Type 2 tag over the reader to write the text.";

  if (!beginOperation(operation, message)) return false;

  pendingPayload = String(command.payload);
  clearTagResult();
  return true;
}

bool executeStartTagEmulation(const NfcCommand &command) {
  if (!state.readerReady) {
    state.status = "error";
    state.message = "PN532 reader is not available.";
    state.updatedAt = millis();
    return false;
  }
  if (pendingOperation != NfcOperation::NONE ||
      (state.busy && !state.tagEmulationEnabled)) {
    state.status = "error";
    state.message = "Wait for the current external-tag operation to finish.";
    state.updatedAt = millis();
    return false;
  }

  const String recordType =
      command.type == NfcCommandType::START_URL_EMULATION ? "url" : "text";
  const String payload(command.payload);
  String error;
  if (!buildTagEmulationNdef(recordType, payload, error)) {
    state.status = "error";
    state.message = error;
    state.emulationMessage = error;
    state.updatedAt = millis();
    return false;
  }

  tagEmulationProfile = TagEmulationProfile::GENERIC_NDEF;
  resetTagEmulationSession();
  state.tagScans = 0;
  state.lastTagScanAt = 0;
  state.tagEmulationEnabled = true;
  state.wifiOnboardingActive = false;
  state.busy = true;
  state.status = "emulating";
  state.emulationMessage =
      "Ready. Hold an NFC reader or phone over the PN532 antenna to scan the record.";
  state.message = state.emulationMessage;
  state.updatedAt = millis();
  lastTargetAttempt = 0;

  if (!savePersistedTagEmulationRecord(recordType, payload)) {
    Serial.println(
        "[NFC][PERSIST] WARNING: Tag Emulation started, but its record "
        "could not be saved for reboot");
  }

  Serial.printf("[NFC] Tag emulation started: %s, %u payload bytes\n",
                state.emulatedRecordType.c_str(),
                static_cast<unsigned>(payload.length()));
  return true;
}

bool executeStartWifiOnboarding(const NfcCommand &command) {
  if (!state.readerReady) {
    state.status = "error";
    state.message = "PN532 reader is not available for Wi-Fi onboarding.";
    state.updatedAt = millis();
    return false;
  }
  if (pendingOperation != NfcOperation::NONE ||
      (state.busy && !state.tagEmulationEnabled)) {
    state.status = "error";
    state.message = "Wait for the current NFC operation to finish.";
    state.updatedAt = millis();
    return false;
  }

  const String ssid(command.ssid);
  const String password(command.password);
  String error;
  if (!buildWifiOnboardingNdef(
          ssid,
          password,
          command.hasApMac ? command.apMac : nullptr,
          error)) {
    state.status = "error";
    state.message = error;
    state.emulationMessage = error;
    state.updatedAt = millis();
    Serial.printf("[NFC][WIFI] ERROR: %s\n", error.c_str());
    return false;
  }

  tagEmulationProfile = TagEmulationProfile::WIFI_WSC_ANDROID;
  resetTagEmulationSession();
  state.tagScans = 0;
  state.lastTagScanAt = 0;
  state.tagEmulationEnabled = true;
  state.wifiOnboardingActive = true;
  state.busy = true;
  state.status = "emulating";
  state.emulationMessage =
      "Wi-Fi onboarding ready. Scan the badge to connect to its network.";
  state.message = state.emulationMessage;
  state.updatedAt = millis();
  lastTargetAttempt = 0;

  Serial.printf("[NFC][WIFI] Tag emulation started for SSID: %s\n",
                ssid.c_str());
  return true;
}

bool executeStopTagEmulation() {
  const bool wasEnabled = state.tagEmulationEnabled;

  if (!isWifiOnboardingDismissed()) {
    if (dismissWifiOnboarding()) {
      Serial.println(
          "[NFC][PERSIST] First stop recorded; future boots will not restore "
          "the Wi-Fi onboarding record");
    } else {
      Serial.println(
          "[NFC][PERSIST] WARNING: First-stop state could not be stored");
    }
  }

  state.tagEmulationEnabled = false;
  state.wifiOnboardingActive = false;
  resetTagEmulationSession();
  state.busy = false;
  state.status = "idle";
  state.emulationMessage = "Tag emulation is stopped.";
  state.message = "Reader ready. Choose an operation, then present a tag.";
  state.updatedAt = millis();

  if (state.readerReady &&
      !configurePassiveReader("stopping tag emulation")) {
    state.message = "PN532 could not return to reader mode after tag emulation.";
    state.emulationMessage = state.message;
    state.updatedAt = millis();
    return false;
  }

  if (wasEnabled) Serial.println("[NFC] Tag emulation stopped");
  return true;
}

void executeNfcCommand(const NfcCommand &command) {
  switch (command.type) {
    case NfcCommandType::READ:
      (void)executeQueueNfcRead();
      break;
    case NfcCommandType::WRITE_TEXT:
    case NfcCommandType::WRITE_URL:
      (void)executeQueueNfcWrite(command);
      break;
    case NfcCommandType::START_TEXT_EMULATION:
    case NfcCommandType::START_URL_EMULATION:
      (void)executeStartTagEmulation(command);
      break;
    case NfcCommandType::START_WIFI_ONBOARDING:
      (void)executeStartWifiOnboarding(command);
      break;
    case NfcCommandType::STOP_EMULATION:
      (void)executeStopTagEmulation();
      break;
  }
}

void serviceNfcWorker() {
  if (!state.readerReady) return;

  if (state.tagEmulationEnabled) {
    serviceTagEmulation();
    return;
  }

  if (pendingOperation == NfcOperation::NONE) return;

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - operationDeadline) >= 0) {
    Serial.printf("[NFC][TIMEOUT] %s expired without detecting a tag\n",
                  operationName(pendingOperation));
    finishError("No tag was detected before the 15-second timeout.");
    return;
  }
  if (now - lastPoll < POLL_INTERVAL_MS) return;
  lastPoll = now;

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  ++pollAttempts;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength,
                               TAG_POLL_TIMEOUT_MS)) {
#if NFC_DEBUG_VERBOSE
    const uint32_t afterPoll = millis();
    if (lastPollProgress == 0 ||
        afterPoll - lastPollProgress >= POLL_PROGRESS_INTERVAL_MS) {
      lastPollProgress = afterPoll;
      const uint32_t remaining =
          static_cast<int32_t>(operationDeadline - afterPoll) > 0
              ? operationDeadline - afterPoll
              : 0;
      Serial.printf("[NFC][POLL] No target yet; attempts=%lu remaining=%lu ms\n",
                    static_cast<unsigned long>(pollAttempts),
                    static_cast<unsigned long>(remaining));
    }
#endif
    return;
  }

#if NFC_DEBUG_VERBOSE
  Serial.printf("[NFC][POLL] Target activated after %lu attempt(s); UID length=%u\n",
                static_cast<unsigned long>(pollAttempts), uidLength);
#endif
  state.status = "working";
  state.message = "Tag detected. Processing...";
  state.updatedAt = millis();
  processDetectedTag(uid, uidLength);
}

void nfcWorkerTask(void *parameter) {
  (void)parameter;
  Serial.printf("[NFC][TASK] Worker started on core %d\n", xPortGetCoreID());

  for (;;) {
    NfcCommand command;
    if (nfcCommandQueue &&
        xQueueReceive(nfcCommandQueue, &command, 0) == pdTRUE) {
      markCommandDequeued();
      executeNfcCommand(command);
      publishNfcState();
    }

    serviceNfcWorker();
    publishNfcState();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

String buildNfcStateJson(const NfcState &snapshot) {
  String json;
  json.reserve(900 + snapshot.payload.length() + snapshot.raw.length() +
               snapshot.emulatedPayload.length() +
               snapshot.emulationMessage.length());
  json += F("{\"readerReady\":");
  json += snapshot.readerReady ? F("true") : F("false");
  json += F(",\"busy\":");
  json += snapshot.busy ? F("true") : F("false");
  json += F(",\"writable\":");
  json += snapshot.writable ? F("true") : F("false");
  json += F(",\"status\":\"");
  json += jsonEscape(snapshot.status);
  json += F("\",\"message\":\"");
  json += jsonEscape(snapshot.message);
  json += F("\",\"uid\":\"");
  json += jsonEscape(snapshot.uid);
  json += F("\",\"tagType\":\"");
  json += jsonEscape(snapshot.tagType);
  json += F("\",\"recordType\":\"");
  json += jsonEscape(snapshot.recordType);
  json += F("\",\"payload\":\"");
  json += jsonEscape(snapshot.payload);
  json += F("\",\"raw\":\"");
  json += jsonEscape(snapshot.raw);
  json += F("\",\"capacity\":");
  json += snapshot.capacity;
  json += F(",\"tagEmulationEnabled\":");
  json += snapshot.tagEmulationEnabled ? F("true") : F("false");
  json += F(",\"emulationReaderConnected\":");
  json += snapshot.emulationReaderConnected ? F("true") : F("false");
  json += F(",\"emulatedRecordType\":\"");
  json += jsonEscape(snapshot.emulatedRecordType);
  json += F("\",\"emulatedPayload\":\"");
  json += jsonEscape(snapshot.emulatedPayload);
  json += F("\",\"emulationMessage\":\"");
  json += jsonEscape(snapshot.emulationMessage);
  json += F("\",\"tagScans\":");
  json += snapshot.tagScans;
  json += F(",\"lastTagScanAt\":");
  json += snapshot.lastTagScanAt;
  json += F(",\"updatedAt\":");
  json += snapshot.updatedAt;
  json += '}';
  return json;
}

}  // namespace

void setupNFC() {
  nfcStateMutex = xSemaphoreCreateMutex();
  nfcCommandQueue =
      xQueueCreate(NFC_COMMAND_QUEUE_DEPTH, sizeof(NfcCommand));

  if (!nfcStateMutex || !nfcCommandQueue) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "NFC task synchronization could not be initialized.";
    state.updatedAt = millis();
    publishedState = state;
    Serial.println("[NFC] ERROR: Could not create worker queue or state mutex");
    return;
  }

  Serial.println("[NFC] Configuring PN532 interface");
  pinMode(SEL0, OUTPUT);
  pinMode(SEL1, OUTPUT);
  digitalWrite(SEL0, HIGH);
  digitalWrite(SEL1, LOW);
  if (RSTPD_N >= 0) {
    pinMode(RSTPD_N, OUTPUT);
    digitalWrite(RSTPD_N, HIGH);
  }
  delay(100);

  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

  if (!nfc.begin()) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "PN532 initialization failed.";
    state.updatedAt = millis();
    publishNfcState();
    Serial.println("[NFC] ERROR: nfc.begin() failed");
    return;
  }

  const uint32_t version = nfc.getFirmwareVersion();
  if (!version) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "PN532 was not found. Check power, SPI selection, and wiring.";
    state.updatedAt = millis();
    publishNfcState();
    Serial.println("[NFC] ERROR: PN532 not found");
    return;
  }

  if (!configurePassiveReader("startup")) {
    publishNfcState();
    return;
  }

  state.readerReady = true;
  state.status = "idle";
  state.message = "Reader ready. Choose an operation, then present a tag.";
  state.updatedAt = millis();
  publishNfcState();

  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
      nfcWorkerTask,
      "nfc-worker",
      NFC_TASK_STACK_SIZE,
      nullptr,
      NFC_TASK_PRIORITY,
      &nfcTaskHandle,
      NFC_TASK_CORE);

  if (taskCreated != pdPASS) {
    state.readerReady = false;
    state.status = "offline";
    state.message = "The dedicated NFC worker task could not be started.";
    state.updatedAt = millis();
    publishNfcState();
    Serial.println("[NFC] ERROR: Could not create dedicated NFC task");
    return;
  }

  Serial.printf("[NFC] Found PN5%02X firmware %u.%u\n", (version >> 24) & 0xFF,
                (version >> 16) & 0xFF, (version >> 8) & 0xFF);
  Serial.printf("[NFC] Advanced NFC serial diagnostics: %s\n",
                NFC_DEBUG_VERBOSE ? "enabled" : "disabled");
}

String getNfcStateJson() {
  return buildNfcStateJson(getPublishedStateSnapshot());
}

bool queueNfcRead() {
  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (!publishedState.readerReady) {
    setPublishedErrorLocked("PN532 reader is not available.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if (publishedState.tagEmulationEnabled) {
    setPublishedErrorLocked(
        "Stop tag emulation before reading or writing an external tag.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if (publishedState.busy || commandPending) {
    setPublishedErrorLocked(
        "Another NFC operation is already waiting for a tag.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  const NfcState previous = publishedState;
  NfcCommand command;
  command.type = NfcCommandType::READ;

  publishedState.busy = true;
  publishedState.status = "queued";
  publishedState.message =
      "Hold an NFC tag over the PCB reader to scan it.";
  publishedState.updatedAt = millis();

  if (!sendCommandWhileLocked(command)) {
    publishedState = previous;
    setPublishedErrorLocked("The NFC command queue is full.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  xSemaphoreGive(nfcStateMutex);
  return true;
}

bool queueNfcWrite(const String &recordType, const String &payload) {
  String normalized = recordType;
  normalized.toLowerCase();
  if (normalized != "text" && normalized != "url") {
    setPublishedError("Record type must be text or url.");
    return false;
  }
  if (payload.length() == 0 || payload.length() > MAX_WEB_PAYLOAD_BYTES) {
    setPublishedError(
        payload.length() == 0
            ? "Enter text or a URL before writing."
            : "Payload exceeds the 700-byte controller limit.");
    return false;
  }

  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (!publishedState.readerReady) {
    setPublishedErrorLocked("PN532 reader is not available.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if (publishedState.tagEmulationEnabled) {
    setPublishedErrorLocked(
        "Stop tag emulation before reading or writing an external tag.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if (publishedState.busy || commandPending) {
    setPublishedErrorLocked(
        "Another NFC operation is already waiting for a tag.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  const NfcState previous = publishedState;
  NfcCommand command;
  command.type = normalized == "url"
                     ? NfcCommandType::WRITE_URL
                     : NfcCommandType::WRITE_TEXT;
  if (!copyCommandString(payload, command.payload, sizeof(command.payload))) {
    setPublishedErrorLocked("The NFC payload could not be queued.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  publishedState.busy = true;
  publishedState.status = "queued";
  publishedState.message =
      normalized == "url"
          ? "Hold a writable Type 2 tag over the reader to write the URL."
          : "Hold a writable Type 2 tag over the reader to write the text.";
  publishedState.updatedAt = millis();

  if (!sendCommandWhileLocked(command)) {
    publishedState = previous;
    setPublishedErrorLocked("The NFC command queue is full.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  xSemaphoreGive(nfcStateMutex);
  return true;
}

bool startNfcTagEmulation(const String &recordType, const String &payload) {
  String normalized = recordType;
  normalized.toLowerCase();
  if (normalized != "text" && normalized != "url") {
    setPublishedError("Record type must be text or url.");
    return false;
  }
  if (payload.length() == 0 ||
      payload.length() > MAX_TAG_EMULATION_PAYLOAD_BYTES) {
    setPublishedError(
        payload.length() == 0
            ? "Enter text or a URL before starting Tag Emulation."
            : "The Tag Emulation payload exceeds the 220-byte limit.");
    return false;
  }

  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (!publishedState.readerReady) {
    setPublishedErrorLocked("PN532 reader is not available.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if ((publishedState.busy && !publishedState.tagEmulationEnabled) ||
      commandPending) {
    setPublishedErrorLocked(
        "Wait for the current external-tag operation to finish.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  const NfcState previous = publishedState;
  NfcCommand command;
  command.type = normalized == "url"
                     ? NfcCommandType::START_URL_EMULATION
                     : NfcCommandType::START_TEXT_EMULATION;
  if (!copyCommandString(payload, command.payload, sizeof(command.payload))) {
    setPublishedErrorLocked("The emulation payload could not be queued.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  publishedState.tagEmulationEnabled = true;
  publishedState.wifiOnboardingActive = false;
  publishedState.emulationReaderConnected = false;
  publishedState.busy = true;
  publishedState.status = "emulating";
  publishedState.emulatedRecordType =
      normalized == "url" ? "URL" : "Text";
  publishedState.emulatedPayload = payload;
  publishedState.emulationMessage =
      "Preparing the configured NDEF record...";
  publishedState.message = publishedState.emulationMessage;
  publishedState.updatedAt = millis();

  if (!sendCommandWhileLocked(command)) {
    publishedState = previous;
    setPublishedErrorLocked("The NFC command queue is full.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  xSemaphoreGive(nfcStateMutex);
  return true;
}

bool startNfcWifiOnboarding(const String &ssid,
                            const String &password,
                            const uint8_t apMac[6]) {
  if (ssid.length() == 0 || password.length() == 0 ||
      ssid.length() >= NFC_WIFI_SSID_BUFFER_SIZE ||
      password.length() >= NFC_WIFI_PASSWORD_BUFFER_SIZE) {
    setPublishedError("The Wi-Fi onboarding credentials are invalid.");
    return false;
  }

  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (!publishedState.readerReady) {
    setPublishedErrorLocked(
        "PN532 reader is not available for Wi-Fi onboarding.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if ((publishedState.busy && !publishedState.tagEmulationEnabled) ||
      commandPending) {
    setPublishedErrorLocked(
        "Wait for the current NFC operation to finish.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  const NfcState previous = publishedState;
  NfcCommand command;
  command.type = NfcCommandType::START_WIFI_ONBOARDING;
  if (!copyCommandString(ssid, command.ssid, sizeof(command.ssid)) ||
      !copyCommandString(password, command.password,
                         sizeof(command.password))) {
    setPublishedErrorLocked("The Wi-Fi onboarding record could not be queued.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }
  if (apMac) {
    memcpy(command.apMac, apMac, sizeof(command.apMac));
    command.hasApMac = true;
  }

  publishedState.tagEmulationEnabled = true;
  publishedState.wifiOnboardingActive = true;
  publishedState.emulationReaderConnected = false;
  publishedState.busy = true;
  publishedState.status = "emulating";
  publishedState.emulatedRecordType = "Wi-Fi";
  publishedState.emulatedPayload = ssid;
  publishedState.emulationMessage =
      "Preparing the Wi-Fi onboarding records...";
  publishedState.message = publishedState.emulationMessage;
  publishedState.updatedAt = millis();

  if (!sendCommandWhileLocked(command)) {
    publishedState = previous;
    setPublishedErrorLocked("The NFC command queue is full.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  xSemaphoreGive(nfcStateMutex);
  return true;
}

bool isNfcWifiOnboardingActive() {
  const NfcState snapshot = getPublishedStateSnapshot();
  return snapshot.tagEmulationEnabled && snapshot.wifiOnboardingActive;
}

bool stopNfcTagEmulation() {
  if (!nfcStateMutex ||
      xSemaphoreTake(nfcStateMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (commandPending) {
    setPublishedErrorLocked("Another NFC command is still being queued.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  const NfcState previous = publishedState;
  NfcCommand command;
  command.type = NfcCommandType::STOP_EMULATION;

  publishedState.tagEmulationEnabled = false;
  publishedState.wifiOnboardingActive = false;
  publishedState.emulationReaderConnected = false;
  publishedState.busy = false;
  publishedState.status = "stopping";
  publishedState.emulationMessage = "Stopping tag emulation...";
  publishedState.message = publishedState.emulationMessage;
  publishedState.updatedAt = millis();

  if (!sendCommandWhileLocked(command)) {
    publishedState = previous;
    setPublishedErrorLocked("The NFC command queue is full.");
    xSemaphoreGive(nfcStateMutex);
    return false;
  }

  xSemaphoreGive(nfcStateMutex);
  return true;
}
