#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "network/WifiConnectPolicy.h"

// Regression coverage for the WiFi connect-timing defect:
//
//   1. The auto-connect budget was 7000 ms while the operation it bounds had a measured
//      median of 6778 ms and a worst case of 7122 ms on the same clock -- a timeout
//      inside its own distribution, so 11 of 75 logged attempts were cut off on networks
//      that a manual retry then joined in 6506 ms.
//   2. The clock started ~684 ms before WiFi.begin(), behind a rolling-log SD append and
//      a blocking panel repaint, so display and storage time was charged to the radio.
//
// The measurements are pinned here, next to the budgets derived from them, so a future
// edit to either has to face the evidence -- same intent as test/x3_memory_budget.

namespace {

using namespace wifi_connect_policy;

// The value that shipped before this test existed, kept as an explicit negative case.
constexpr unsigned long OLD_AUTO_CONNECTION_TIMEOUT_MS = 7000;
// Raw device figures, on the old clock basis (which began before ~684 ms of SD + repaint).
constexpr unsigned long LOGGED_WORST_ELAPSED_MS = 7122;
constexpr unsigned long LOGGED_MEDIAN_ELAPSED_MS = 6778;
constexpr unsigned long LOGGED_PRE_BEGIN_OVERHEAD_MS = 684;

TEST(WifiConnectPolicy, MeasurementsMatchTheDeviceLogOnTheWifiBeginBasis) {
  EXPECT_EQ(MEASURED_WORST_ASSOCIATION_MS, LOGGED_WORST_ELAPSED_MS - LOGGED_PRE_BEGIN_OVERHEAD_MS);
  EXPECT_EQ(MEASURED_MEDIAN_ASSOCIATION_MS, LOGGED_MEDIAN_ELAPSED_MS - LOGGED_PRE_BEGIN_OVERHEAD_MS);
}

TEST(WifiConnectPolicy, AutoBudgetClearsTheMeasuredWorstCaseWithHeadroom) {
  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, MEASURED_WORST_ASSOCIATION_MS);
  // At least 50% margin over the worst success actually observed: 60 samples cannot
  // have found the true tail, and a marginal-signal association is slower still.
  EXPECT_GE(AUTO_CONNECTION_TIMEOUT_MS, MEASURED_WORST_ASSOCIATION_MS + MEASURED_WORST_ASSOCIATION_MS / 2);
}

TEST(WifiConnectPolicy, RejectsTheBudgetThatCausedTheFalseTimeouts) {
  // The regression itself, stated against the clock 7000 ms was actually applied to:
  // it fell below the worst success observed, and cleared the median by 3%.
  EXPECT_LT(OLD_AUTO_CONNECTION_TIMEOUT_MS, LOGGED_WORST_ELAPSED_MS);
  EXPECT_LT(OLD_AUTO_CONNECTION_TIMEOUT_MS - LOGGED_MEDIAN_ELAPSED_MS, LOGGED_MEDIAN_ELAPSED_MS / 20);

  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, OLD_AUTO_CONNECTION_TIMEOUT_MS);
  // Has to survive the raw (pre-fix) numbers too, so the fix does not rest on the clock
  // move alone being correct.
  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, LOGGED_WORST_ELAPSED_MS);
}

TEST(WifiConnectPolicy, AutoBudgetStaysShorterThanAManualConnect) {
  // Auto-connect may work through several saved networks, so it must still fail faster
  // than a user-initiated attempt the user is watching.
  EXPECT_LT(AUTO_CONNECTION_TIMEOUT_MS, CONNECTION_TIMEOUT_MS);
}

TEST(WifiConnectPolicy, HintRetryCannotOutlastAPlainManualAttempt) {
  // A hinted attempt costs the hint window plus a full retry; that whole sequence must
  // stay inside the manual budget, or the "fast path" would be the slow one.
  EXPECT_LT(HINT_ATTEMPT_TIMEOUT_MS, AUTO_CONNECTION_TIMEOUT_MS);
  EXPECT_LE(HINT_ATTEMPT_TIMEOUT_MS + AUTO_CONNECTION_TIMEOUT_MS, CONNECTION_TIMEOUT_MS);
}

TEST(WifiConnectPolicy, ClockSyncWaitIsBounded) {
  // Matches HalClock::quickSyncSystemTime's own 50 x 100 ms ceiling.
  EXPECT_EQ(TIME_SYNC_TIMEOUT_MS, 5000UL);
}

TEST(WifiConnectPolicy, BssidRoundTripsThroughItsTextForm) {
  const uint8_t cases[][BSSID_LEN] = {
      {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01},
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, {0x0a, 0xb0, 0x0c, 0xd0, 0x0e, 0xf0},
  };

  for (const auto& bssid : cases) {
    char text[BSSID_TEXT_LEN + 1];
    formatBssid(bssid, text);
    ASSERT_EQ(std::string(text).size(), BSSID_TEXT_LEN);

    uint8_t parsed[BSSID_LEN] = {};
    ASSERT_TRUE(parseBssid(text, parsed)) << text;
    for (size_t i = 0; i < BSSID_LEN; ++i) {
      EXPECT_EQ(parsed[i], bssid[i]) << "byte " << i << " of " << text;
    }
  }
}

TEST(WifiConnectPolicy, FormatsLowercaseHexWithoutSeparators) {
  const uint8_t bssid[BSSID_LEN] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
  char text[BSSID_TEXT_LEN + 1];
  formatBssid(bssid, text);
  EXPECT_STREQ(text, "a1b2c3d4e5f6");
}

TEST(WifiConnectPolicy, ParsesUppercaseHexToo) {
  uint8_t parsed[BSSID_LEN] = {};
  ASSERT_TRUE(parseBssid("A1B2C3D4E5F6", parsed));
  const uint8_t expected[BSSID_LEN] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
  for (size_t i = 0; i < BSSID_LEN; ++i) EXPECT_EQ(parsed[i], expected[i]);
}

TEST(WifiConnectPolicy, RejectsMalformedBssidTextWithoutTouchingTheOutput) {
  // A corrupted or hand-edited JSON value must be ignored, never half-applied: a
  // partially-parsed BSSID would pin WiFi.begin() to an AP that does not exist.
  const char* bad[] = {
      "",
      "a1b2c3d4e5f",        // one digit short
      "a1b2c3d4e5f6a",      // one digit long
      "a1b2c3d4e5fg",       // non-hex digit
      "a1:b2:c3:d4:e5:f6",  // separators
      "a1b2 c3d4e5f6",
  };

  for (const char* text : bad) {
    uint8_t out[BSSID_LEN] = {9, 9, 9, 9, 9, 9};
    EXPECT_FALSE(parseBssid(text, out)) << "accepted: '" << text << "'";
    for (size_t i = 0; i < BSSID_LEN; ++i) EXPECT_EQ(out[i], 9) << "clobbered on '" << text << "'";
  }

  uint8_t out[BSSID_LEN] = {9, 9, 9, 9, 9, 9};
  EXPECT_FALSE(parseBssid(nullptr, out));
}

TEST(WifiConnectPolicy, TreatsAnAllZeroBssidAsNothingRemembered) {
  const uint8_t none[BSSID_LEN] = {};
  EXPECT_FALSE(isKnownBssid(none));
  EXPECT_FALSE(isUsableHint(none, 6));

  const uint8_t some[BSSID_LEN] = {0, 0, 0, 0, 0, 1};
  EXPECT_TRUE(isKnownBssid(some));
  EXPECT_TRUE(isUsableHint(some, 6));
}

TEST(WifiConnectPolicy, AcceptsOnlyTwoPointFourGigahertzChannels) {
  // The ESP32-C3 has no 5 GHz radio; anything else in the file is corruption.
  EXPECT_FALSE(isValidChannel(0));
  EXPECT_TRUE(isValidChannel(1));
  EXPECT_TRUE(isValidChannel(14));
  EXPECT_FALSE(isValidChannel(15));
  EXPECT_FALSE(isValidChannel(36));
  EXPECT_FALSE(isValidChannel(-1));
  EXPECT_FALSE(isValidChannel(255));
}

TEST(WifiConnectPolicy, NeedsBothHalvesOfAHintToUseIt) {
  const uint8_t bssid[BSSID_LEN] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
  EXPECT_FALSE(isUsableHint(bssid, 0));  // BSSID without a channel
  EXPECT_TRUE(isUsableHint(bssid, 11));
}

}  // namespace
