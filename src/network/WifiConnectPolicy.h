#pragma once

#include <cstddef>
#include <cstdint>

// Timing budgets and BSSID-hint encoding for the WiFi association path.
//
// Deliberately free of Arduino/ESP-IDF headers so test/wifi_connect_policy can assert
// the budgets against the device measurements that set them, the same way
// test/x3_memory_budget pins the heap numbers it was derived from.
namespace wifi_connect_policy {

// --- Measured association times (X3, 75 attempts / 60 successes) -------------
// Timed from WiFi.begin() to WL_CONNECTED. The device log that produced these timed
// from ~684 ms earlier -- before a rolling-log SD append and a blocking panel repaint --
// and recorded a 7122 ms worst case and a 6778 ms median there, so the radio's own
// figures are those minus that fixed pre-begin cost.
inline constexpr unsigned long MEASURED_WORST_ASSOCIATION_MS = 6438;
inline constexpr unsigned long MEASURED_MEDIAN_ASSOCIATION_MS = 6094;

// --- Timeouts ----------------------------------------------------------------

// User-initiated connect: the user picked one specific network and is watching it, so
// wait the attempt out rather than guessing on their behalf.
inline constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;

// Auto-connect budget. Shorter than a manual connect because auto-connect may work
// through several saved networks in sequence, but it must still clear the measured
// distribution with real margin. The previous 7000 ms had none: on the very clock it
// bounded it sat *below* the 7122 ms worst case observed and only 222 ms (3%) above the
// 6778 ms median, with 71% of successes past 6500 ms. 11 of 75 logged attempts were cut
// off, on networks a manual retry then joined in 6506 ms.
inline constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 10000;

// First attempt when a BSSID/channel hint is available. A hint that still matches the AP
// associates on one channel with no scan at all; if it has not landed by here the AP has
// moved (roamed, changed channel, or gone), and a full-scan retry is worth more than
// waiting out the whole budget on the wrong channel.
inline constexpr unsigned long HINT_ATTEMPT_TIMEOUT_MS = 3500;

// Bound on the post-association SNTP wait, so a blocked NTP path cannot strand the UI.
inline constexpr unsigned long TIME_SYNC_TIMEOUT_MS = 5000;

// --- BSSID hints -------------------------------------------------------------

inline constexpr size_t BSSID_LEN = 6;
inline constexpr size_t BSSID_TEXT_LEN = 12;  // lowercase hex, no separators

// 2.4 GHz only: the ESP32-C3 has no 5 GHz radio, so a persisted channel outside 1..14 is
// corrupt and must never reach WiFi.begin().
inline constexpr uint8_t MAX_CHANNEL = 14;

constexpr bool isValidChannel(const int channel) { return channel >= 1 && channel <= static_cast<int>(MAX_CHANNEL); }

// An all-zero BSSID is the "nothing remembered" marker, not an address.
constexpr bool isKnownBssid(const uint8_t* bssid) {
  for (size_t i = 0; i < BSSID_LEN; ++i) {
    if (bssid[i] != 0) return true;
  }
  return false;
}

// Both halves must be present: a channel without a BSSID (or the reverse) would make
// WiFi.begin() associate against a guess.
constexpr bool isUsableHint(const uint8_t* bssid, const uint8_t channel) {
  return isKnownBssid(bssid) && isValidChannel(channel);
}

// Writes BSSID_TEXT_LEN lowercase hex digits plus a terminator into out.
constexpr void formatBssid(const uint8_t* bssid, char* out) {
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < BSSID_LEN; ++i) {
    out[i * 2] = digits[bssid[i] >> 4U];
    out[i * 2 + 1] = digits[bssid[i] & 0x0FU];
  }
  out[BSSID_TEXT_LEN] = '\0';
}

// Accepts exactly BSSID_TEXT_LEN hex digits in either case. Leaves out untouched and
// returns false on anything else, so a hand-edited or truncated JSON value is ignored
// rather than half-applied.
constexpr bool parseBssid(const char* text, uint8_t* out) {
  if (text == nullptr) return false;

  uint8_t parsed[BSSID_LEN] = {};
  for (size_t i = 0; i < BSSID_TEXT_LEN; ++i) {
    const char ch = text[i];
    uint8_t nibble = 0;
    if (ch >= '0' && ch <= '9') {
      nibble = static_cast<uint8_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = static_cast<uint8_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = static_cast<uint8_t>(ch - 'A' + 10);
    } else {
      return false;  // also catches a short string: its terminator lands here
    }
    parsed[i / 2] = static_cast<uint8_t>((parsed[i / 2] << 4U) | nibble);
  }
  if (text[BSSID_TEXT_LEN] != '\0') return false;

  for (size_t i = 0; i < BSSID_LEN; ++i) out[i] = parsed[i];
  return true;
}

// Validates one persisted hint record, as read out of the hint file: the BSSID in its
// text form and the channel as an integer. Writes to outBssid/outChannel only once the
// whole record has passed, so a record that is part-valid -- a good channel beside a
// truncated BSSID, say -- can never leave a half-applied hint behind for WiFi.begin()
// to associate against.
constexpr bool parseHintRecord(const char* bssidText, const int channel, uint8_t* outBssid, uint8_t& outChannel) {
  if (!isValidChannel(channel)) return false;

  uint8_t parsed[BSSID_LEN] = {};
  if (!parseBssid(bssidText, parsed)) return false;
  if (!isUsableHint(parsed, static_cast<uint8_t>(channel))) return false;

  for (size_t i = 0; i < BSSID_LEN; ++i) outBssid[i] = parsed[i];
  outChannel = static_cast<uint8_t>(channel);
  return true;
}

}  // namespace wifi_connect_policy
