#include "FouladDeviceTracking.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "FouladTelemetryGate.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MidadAppSettings.h"
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

std::string readingStatsEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/reading-stats"; }
std::string deviceLogEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device-log"; }
std::string deviceStatsEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device-stats"; }

// reportDeviceStats() skips the books/reading-days arrays this connect
// (device telemetry alone still sends) unless free heap comfortably covers
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

// For the crash_report.txt upload/flush lifecycle specifically (see
// flushPendingCrashReport()) -- forces through RollingSdLog's debugLoggingEnabled gate.
// Routine FDT chatter (register/firmware-check/device-stats above) stays opt-in via
// diagLog(), but these three lines are the only local record of an actual panic once
// crash_report.txt itself is deleted, and per CrossPointSettings::debugLoggingEnabled's
// own carve-out, crash reporting must survive regardless of the debug-logging preference.
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

// Single attempt, no 404 handling -- reportReadingStats() (below) is the only
// caller and owns the retry-after-register decision.
bool postReadingStatsOnce(const std::string& username, const std::string& password, const std::string& fouladBookId,
                          const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();
  doc["book_id"] = atoi(fouladBookId.c_str());
  doc["progress_percent"] = progressPercent;
  if (!lastPosition.empty()) doc["last_position"] = lastPosition;
  doc["seconds_read"] = secondsRead;
  std::string body;
  serializeJson(doc, body);

  std::string response;
  const bool ok =
      HttpDownloader::postJson(readingStatsEndpoint(), body, username, password, response) == HttpDownloader::OK;
  diagLog("reading-stats book=" + fouladBookId + " progress=" + std::to_string(progressPercent) +
          " seconds=" + std::to_string(secondsRead) + " -> " +
          (ok ? "ok" : "FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail)));
  return ok;
}

// Single attempt, no 404 handling -- reportDeviceStats() owns the
// retry-after-register decision, same convention as postReadingStatsOnce().
// includeReading false sends `device` telemetry alone (unchanged since last
// send, or heap too tight for the larger arrays right now).
bool postDeviceStatsOnce(const std::string& username, const std::string& password, const bool includeReading) {
  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();

  JsonObject device = doc["device"].to<JsonObject>();
  device["battery_percent"] = powerManager.getBatteryPercentage();
  device["wifi_rssi"] = WiFi.RSSI();
  device["free_heap_bytes"] = ESP.getFreeHeap();
  device["uptime_seconds"] = millis() / 1000;

  if (includeReading) {
    JsonObject reading = doc["reading"].to<JsonObject>();
    reading["current_streak_days"] = READING_STATS.getCurrentStreakDays();
    reading["max_streak_days"] = READING_STATS.getMaxStreakDays();
    reading["today_reading_seconds"] = static_cast<uint32_t>(READING_STATS.getTodayReadingMs() / 1000);
    reading["total_reading_seconds"] = static_cast<uint32_t>(READING_STATS.getTotalReadingMs() / 1000);
    reading["books_started"] = READING_STATS.getBooksStartedCount();
    reading["books_finished"] = READING_STATS.getBooksFinishedCount();

    // book.title empty fallback matches StatsActivity's own bookTitle().
    JsonArray books = reading["books"].to<JsonArray>();
    for (const auto& book : READING_STATS.getBooks()) {
      JsonObject b = books.add<JsonObject>();
      b["title"] = book.title.empty() ? book.path : book.title;
      b["author"] = book.author;
      b["total_reading_seconds"] = static_cast<uint32_t>(book.totalReadingMs / 1000);
      b["sessions"] = book.sessions;
      b["progress_percent"] = book.lastProgressPercent;
      b["completed"] = book.completed;
      b["last_read_at"] = book.lastReadAt;
      b["estimated_time_left_seconds"] = book.estimatedTimeLeftSeconds;
    }

    JsonArray days = reading["reading_days"].to<JsonArray>();
    for (const auto& day : READING_STATS.getReadingDays()) {
      JsonObject d = days.add<JsonObject>();
      d["day_ordinal"] = day.dayOrdinal;
      d["reading_seconds"] = day.readingMs / 1000;
    }
  }

  std::string body;
  serializeJson(doc, body);

  std::string response;
  const bool ok =
      HttpDownloader::postJson(deviceStatsEndpoint(), body, username, password, response) == HttpDownloader::OK;
  diagLog(std::string("device-stats upload (reading=") + (includeReading ? "yes" : "no") + ") -> " +
          (ok ? "ok" : "FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail)));
  return ok;
}
}  // namespace

std::string getSerialNumber() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return std::string("XTE-") + mac.c_str();
}

void reportReadingStats(const std::string& username, const std::string& password, const std::string& fouladBookId,
                        const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  if (telemetryRefused("reading-stats")) return;

  if (!wifiConnected() || username.empty() || password.empty() || fouladBookId.empty()) return;

  if (postReadingStatsOnce(username, password, fouladBookId, progressPercent, lastPosition, secondsRead)) {
    return;
  }

  LOG_ERR(TAG, "Reading-stats report failed (status=%d)", HttpDownloader::getLastFailure().detail);
  diagLog("reading-stats book=" + fouladBookId +
          " gave up, status=" + std::to_string(HttpDownloader::getLastFailure().detail));
}

void flushPendingReadingStats(const std::string& username, const std::string& password) {
  if (telemetryRefused("reading-stats")) return;

  if (!wifiConnected() || username.empty() || password.empty()) return;

  // Driven by the stats store, not RecentBooksStore. Recents caps at 10 and
  // evicts, so iterating it silently skipped every catalog book that had been
  // read but pushed out of the top 10 -- those never produced a library_reading
  // row on the server no matter how often the device reconnected
  // (INSTRUCTIONS-14). The stats store holds up to MAX_BOOKS and now carries the
  // id itself, so the two lists can no longer disagree about what is reportable.
  READING_STATS.ensureLoaded();
  int flushed = 0;
  for (const auto& stats : READING_STATS.getBooks()) {
    if (stats.fouladBookId.empty()) continue;  // side-loaded, or read before v3
    if (stats.totalReadingMs == 0) continue;
    reportReadingStats(username, password, stats.fouladBookId, stats.lastProgressPercent, "",
                       static_cast<uint32_t>(stats.totalReadingMs / 1000));
    flushed++;
  }
  if (flushed > 0) diagLog("flush: reported " + std::to_string(flushed) + " book(s)");
}

void flushPendingKOReaderSync() {
  if (!wifiConnected()) return;
  if (!KOREADER_STORE.hasPendingSync()) return;
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

void uploadDebugLog(const std::string& username, const std::string& password) {
  if (telemetryRefused("debug-log upload")) return;

  if (!DebugLogging::enabled() || !wifiConnected() || username.empty() || password.empty()) return;
  if (!Storage.exists(DebugLog::PATH)) return;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", DebugLog::PATH}};
  const std::vector<std::pair<std::string, std::string>> fields = {{"serial_number", getSerialNumber()},
                                                                   {"firmware_version", CROSSPOINT_VERSION}};
  std::string response;
  const bool ok =
      HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  // Neither outcome is diagLog()'d, because diagLog() writes to DebugLog::PATH --
  // the very file this function just uploaded (see diagLog's definition above).
  //
  // On failure that would get picked up on the next attempt, and a persistent
  // failure (e.g. no signal) would spam a line into the log on every reconnect.
  //
  // On success it defeats server-side deduplication outright: the upload
  // endpoint skips storing a body whose hash matches the last one it holds for
  // this device, so an unchanged reader costs nothing to re-report. Appending
  // "log upload -> ok" after each successful send made every subsequent body
  // byte-different, so the hash never matched and every OPDS browse stored a
  // fresh ~50KB copy (see foulad-ebooks EINK_DEVICE_LOG_TASKS.md 2.4/5.4).
  //
  // If a success marker is ever wanted, it has to live somewhere other than the
  // file being uploaded.
  if (!ok) {
    LOG_ERR(TAG, "Debug log upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
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
// flushPendingCrashReport() deletes the SD copy once it's uploaded, which can be a
// LATER boot than the one that crashed -- so a user checking the SD card afterward
// finds nothing, even though the crash was real and the report made it to the server.
// Logging this summary into debug_log.txt (the file users already pull off the device)
// before deletion keeps the panic reason visible locally too.
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
  const bool ok =
      HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  if (!ok) {
    LOG_ERR(TAG, "Crash report upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
  // Unlike uploadDebugLog(), diagLog() here is safe either way: this writes to
  // /debug_log.txt, a different file than the one just uploaded.
  criticalDiagLog(ok ? "crash report upload -> ok"
                     : "crash report upload -> FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail));
  return ok;
}

void flushPendingCrashReport(const std::string& username, const std::string& password) {
  if (telemetryRefused("crash-report upload")) return;

  if (!Storage.exists(CRASH_REPORT_PATH)) return;  // nothing waiting
  if (!uploadCrashReport(username, password)) return;
  // See readCrashReportSummary(): capture the panic reason locally before the SD
  // copy is deleted below, since the upload+delete can happen on a boot after the
  // one the user actually has eyes on.
  criticalDiagLog("crash summary: " + readCrashReportSummary());
  // Delivered: drop the local copy so the next connection does not resend it.
  // Deliberately only on success -- a failed upload leaves the file for the next
  // attempt, which is the whole point of queueing it rather than requiring the
  // user to be looking at the crash screen at the moment they have WiFi.
  Storage.remove(CRASH_REPORT_PATH);
  criticalDiagLog("crash report flushed and cleared");
}

void reportDeviceStats(const std::string& username, const std::string& password) {
  if (telemetryRefused("device-stats")) return;

  if (!wifiConnected() || username.empty() || password.empty()) return;

  READING_STATS.ensureLoaded();
  const uint64_t totalMs = READING_STATS.getTotalReadingMs();
  const bool readingChanged = totalMs != lastReportedTotalReadingMs;
  const uint32_t estimatedReadingBytes =
      static_cast<uint32_t>(READING_STATS.getBooks().size()) * ESTIMATED_BYTES_PER_BOOK +
      static_cast<uint32_t>(READING_STATS.getReadingDays().size()) * ESTIMATED_BYTES_PER_READING_DAY;
  const bool includeReading =
      readingChanged && ESP.getFreeHeap() >= estimatedReadingBytes + READING_SNAPSHOT_SAFETY_MARGIN_BYTES;
  if (readingChanged && !includeReading) {
    diagLog("device-stats reading snapshot skipped: free heap=" + std::to_string(ESP.getFreeHeap()) +
            " need=" + std::to_string(estimatedReadingBytes + READING_SNAPSHOT_SAFETY_MARGIN_BYTES));
  }

  const bool ok = postDeviceStatsOnce(username, password, includeReading);

  // Only remember success -- a failed send (including one that skipped the
  // reading snapshot for heap reasons) should retry next connect, not be
  // treated as "already reported."
  if (ok && includeReading) lastReportedTotalReadingMs = totalMs;
}

}  // namespace FouladDeviceTracking
