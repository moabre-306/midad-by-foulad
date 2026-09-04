#include "FouladDeviceTracking.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "FouladTelemetryGate.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MidadAppSettings.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "network/HttpDownloader.h"
#include "network/OtaUpdater.h"
#include "reading/ReadingStatsStore.h"
#include "util/DebugLog.h"
#include "util/DebugLogging.h"
#include "util/RollingSdLog.h"

namespace FouladDeviceTracking {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

namespace {
constexpr char TAG[] = "FDT";

// Reserved for the device-stats upload returning on /devices/v1/heartbeat: skip
// the books/reading-days arrays unless free heap comfortably covers
// BOTH the actual current data size AND this fixed margin for whatever OPDS
// browsing/WiFi/TLS is already holding -- scaled to the CURRENT book/day
// counts rather than the worst case (40 books + 750 days) once and for all,
// since a fixed worst-case threshold would never pass for a typical user
// with a handful of books, even though their real payload only needs a few
// KB. Per-entry estimates are deliberately generous (ArduinoJson's internal
// variant-slot overhead plus the final serialized string, per entry).
constexpr uint32_t READING_SNAPSHOT_SAFETY_MARGIN_BYTES = 40000;
constexpr uint32_t ESTIMATED_BYTES_PER_BOOK = 350;
constexpr uint32_t ESTIMATED_BYTES_PER_READING_DAY = 90;

// Last-reported READING_STATS total, to decide whether the (larger) reading
// snapshot needs resending -- device telemetry is cheap and always sent, but
// the reading snapshot is only worth another POST when something actually
// changed. Sentinel value ensures the first call after boot always sends it.
uint64_t lastReportedTotalReadingMs = UINT64_MAX;

// Matches the literal path HalSystem::checkPanic() writes to.
constexpr char CRASH_REPORT_PATH[] = "/crash_report.txt";

std::string modelName() { return gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4"; }
std::string displayName() { return std::string("Foulad eInk (") + (gpio.deviceIsX3() ? "X3" : "X4") + ")"; }

// Tagged into the shared on-SD debug log (see util/DebugLog.h) so a
// registration/reporting problem with the LIVE server can be diagnosed from
// the SD card, the same way OPDS browsing/downloading already can.
void diagLog(const std::string& line) { RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES); }

// For the crash_report.txt lifecycle specifically -- forces through RollingSdLog's
// debugLoggingEnabled gate. Routine FDT chatter stays opt-in via diagLog(), but these
// lines are the only local record of an actual panic once crash_report.txt is deleted,
// and per CrossPointSettings::debugLoggingEnabled's own carve-out, crash reporting must
// survive regardless of the debug-logging preference.
// Single point every network-touching entry below returns through when the gate refuses.
// Logs the reason at most once per reason per boot: the whole point of this change is that
// a refused upload must not become its own source of SD writes and heap churn.
bool telemetryRefused(const char* what) {
  const FouladTelemetryGate::Block b = FouladTelemetryGate::check(millis());
  if (b == FouladTelemetryGate::Block::None) return false;
  static uint8_t loggedReasons = 0;
  const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(b));
  if ((loggedReasons & bit) == 0) {
    loggedReasons = static_cast<uint8_t>(loggedReasons | bit);
    LOG_DBG(TAG, "%s skipped: %s", what, FouladTelemetryGate::describe(b));
  }
  return true;
}

void criticalDiagLog(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES, /*force=*/true);
}

// Web (and on-device) Arabic font picker index -> actual stored
// ARABIC_FONT_FAMILY value. NOT a 1:1 mapping: AMIRI(=1) was dropped from
// flash and is skipped by both UIs (see SettingsList.h's
// buildArabicFontFamilySetting()/kSelectableFonts, which this must stay in
// sync with) -- picker index 1 ("Uthmani Hafs") is stored value 2, not 1.
// Applying the raw web index directly here would silently select the wrong
// font.
constexpr uint8_t kArabicFontDisplayToStored[] = {
    CrossPointSettings::NOTONASKHARABIC,  // 0: Noto Naskh Arabic
    CrossPointSettings::UTHMANICHAFS,     // 1: Uthmani Hafs
    CrossPointSettings::TAJAWAL,          // 2: Tajawal
};
constexpr uint8_t kArabicFontDisplayCount = sizeof(kArabicFontDisplayToStored) / sizeof(kArabicFontDisplayToStored[0]);

uint8_t arabicFontFamilyToDisplayIndex(const uint8_t stored) {
  for (uint8_t i = 0; i < kArabicFontDisplayCount; i++) {
    if (kArabicFontDisplayToStored[i] == stored) return i;
  }
  return 0;  // stale/legacy value (e.g. removed Amiri=1) or out of range
}

}  // namespace

std::string getSerialNumber() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return std::string("XTE-") + mac.c_str();
}

// True when the queued KOSync push cannot possibly authenticate, so attempting it
// only spends a TLS handshake to earn a 401.
//
// /syncs/* lives on the legacy host and authenticates with an x-auth-key that ONLY
// legacy QR pairing mints. A device paired against Core is never issued one and
// Core serves no /syncs route, so that pairing 401s forever by construction.
//
// Deliberately narrow: the KOSync base URL is user-configurable, and someone
// pointing it at their own KOSync-compatible server must keep working. Only the
// midad.one + Core-paired combination is refused, and only for the automatic push
// on connect -- a manual sync the user asked for still runs and still reports what
// happened, rather than silently doing nothing.
bool koSyncCannotAuthenticate() {
  if (!isFouladEbooksUrl(KOREADER_STORE.getBaseUrl())) return false;  // user's own server
  const auto& servers = OPDS_STORE.getServers();
  return std::any_of(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
}

void flushPendingKOReaderSync() {
  if (!wifiConnected()) return;
  if (!KOREADER_STORE.hasPendingSync()) return;
  if (koSyncCannotAuthenticate()) {
    // Leave the entry queued rather than dropping it: pointing KOSync at a server
    // that can authenticate should deliver it, not discard it.
    return;
  }
  // Credentials may have been cleared (sign-out, re-pairing) since the item was
  // queued; a stale queue entry would otherwise retry forever with NO_CREDENTIALS.
  if (!KOREADER_STORE.hasCredentials()) {
    KOREADER_STORE.clearPendingSync();
    return;
  }

  const PendingKOReaderSync& pending = *KOREADER_STORE.getPendingSync();

  KOReaderProgress progress;
  progress.document = pending.documentHash;
  progress.progress = pending.xpath;
  progress.percentage = pending.percentage;

  // Rich CrossPoint position, same as the manual sync's live upload -- see
  // KOReaderSyncActivity::performUpload().
  KOReaderRichPosition pos;
  const float pct = pending.percentage < 0.0f ? 0.0f : pending.percentage > 1.0f ? 1.0f : pending.percentage;
  pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
  pos.spineIndex = pending.spineIndex;
  pos.pageNumber = pending.pageNumber;
  pos.totalPages = pending.totalPages;
  pos.paragraphIndex = pending.paragraphIndex;
  pos.xpath = pending.xpath;
  progress.position = std::move(pos);

  const auto result = KOReaderSyncClient::updateProgress(progress);
  if (result == KOReaderSyncClient::OK) {
    KOREADER_STORE.clearPendingSync();
    diagLog("KOReader sync flushed (doc=" + pending.documentHash + ")");
  }
  // Failure (offline mid-request, server error): leave it queued for the next
  // reconnect, same as every other flushPending* function here.
}

// First line of crash_report.txt is "CrossPoint version: X" (HalSystem::checkPanic).
// Empty when absent -- an older report, or a truncated write -- and the caller falls
// back to the running version rather than sending nothing.
std::string readCrashReportVersion() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "";
  char buf[96] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "";
  const std::string head(buf, static_cast<size_t>(read));
  constexpr const char* kPrefix = "CrossPoint version: ";
  const size_t at = head.find(kPrefix);
  if (at == std::string::npos) return "";
  const size_t start = at + strlen(kPrefix);
  const size_t end = head.find_first_of("\r\n", start);
  return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Condenses the "Panic reason" and "Heap before panic" lines out of crash_report.txt
// for the debug log -- readCrashReportVersion()'s technique (small bounded read, not
// the full multi-KB report with its backtrace/last-logs sections). This is what lets
// a "crash happened but I can't find crash_report.txt" report get diagnosed at all:
// the SD copy is deleted once an upload succeeds, which can be a LATER boot than the
// one that crashed -- so a user checking the SD card afterward finds nothing, even
// though the crash was real. Logging this summary into debug_log.txt (the file users
// already pull off the device) keeps the panic reason visible locally.
std::string readCrashReportSummary() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "(unreadable)";
  char buf[512] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "(unreadable)";
  const std::string head(buf, static_cast<size_t>(read));

  auto extractLine = [&](const char* prefix) -> std::string {
    const size_t at = head.find(prefix);
    if (at == std::string::npos) return "?";
    const size_t start = at + strlen(prefix);
    const size_t end = head.find_first_of("\r\n", start);
    return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };

  return "reason=[" + extractLine("Panic reason: ") + "] heap=[" + extractLine("Heap before panic: ") + "]";
}

bool uploadCrashReport(const std::string& username, const std::string& password) {
  if (telemetryRefused("crash-report upload")) return false;

  if (!wifiConnected() || username.empty() || password.empty()) return false;
  if (!Storage.exists(CRASH_REPORT_PATH)) return false;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", CRASH_REPORT_PATH}};
  // The version that CRASHED, not the one uploading. A crash report queues on the SD
  // card until the device is next online, by which time it may have updated -- so
  // stamping CROSSPOINT_VERSION here attributes an old panic to a new release, which
  // is exactly backwards for "did yesterday's RC break this". Observed: a 1.8.1 panic
  // arriving labelled 1.8.5-rc. HalSystem writes the real one into the report at
  // panic time; read it back out.
  const std::string crashedVersion = readCrashReportVersion();
  const std::vector<std::pair<std::string, std::string>> fields = {
      {"serial_number", getSerialNumber()},
      {"type", "crash"},
      {"firmware_version", crashedVersion.empty() ? CROSSPOINT_VERSION : crashedVersion}};
  std::string response;
  const bool ok = HttpDownloader::postFilesMultipart(FOULAD_EBOOKS_DEVICE_LOG_URL, files, fields, response, 20000,
                                                     username, password);

  if (!ok) {
    LOG_ERR(TAG, "Crash report upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
  // Safe either way: this writes to /debug_log.txt, not the file just uploaded.
  criticalDiagLog(ok ? "crash report upload -> ok"
                     : "crash report upload -> FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail));
  return ok;
}

}  // namespace FouladDeviceTracking
