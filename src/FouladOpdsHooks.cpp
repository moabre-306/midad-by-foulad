#include "FouladOpdsHooks.h"

#include <Epub.h>
#include <HalStorage.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "FouladTelemetryGate.h"
#include "RecentBooksStore.h"

namespace {}  // namespace

namespace FouladOpdsHooks {

void reportDeviceTrackingOnConnect(const OpdsServer& server) {
  // Not gated on server.url below like the rest of this function: KOReader
  // Sync's server is independently configured (any KOSync-compatible host,
  // not necessarily midad.one), so this is worth attempting on WiFi coming up
  // for ANY OPDS server, Foulad eBooks or not.
  FouladDeviceTracking::flushPendingKOReaderSync();

  if (server.url != FOULAD_EBOOKS_URL) return;
  FouladTelemetryGate::resetForNewConnection();

  // ONE upload per connect, round-robin, never a burst.
  //
  // These five used to run back-to-back the moment WiFi came up. Each opens its own TLS
  // session needing a ~16.7KB contiguous buffer, so on a device with ~40KB free and a
  // largest block well under that, they starved each other and left the heap at 3264
  // bytes free -- the "slow and stuck" report of 2026-09-01. Whatever is queued still
  // gets sent, just one connection at a time: crash reports and the debug log are
  // preserved on SD until their upload actually succeeds, so nothing is lost by waiting.
  //
  // Ordered by how irreplaceable the data is. A crash report exists once and explains a
  // failure nobody else can reproduce; a stats snapshot is regenerated every session.
  static uint8_t nextTask = 0;
  constexpr uint8_t TASK_COUNT = 4;
  const uint8_t task = nextTask;
  nextTask = static_cast<uint8_t>((nextTask + 1) % TASK_COUNT);
  switch (task) {
    case 0:
      FouladDeviceTracking::flushPendingCrashReport(server.username, server.password);
      break;
    case 1:
      FouladDeviceTracking::flushPendingReadingStats(server.username, server.password);
      break;
    case 2:
      FouladDeviceTracking::uploadDebugLog(server.username, server.password);
      break;
    default:
      FouladDeviceTracking::reportDeviceStats(server.username, server.password);
      break;
  }
}

void removeExistingNewsDownload(const std::string& filename) {
  if (!Storage.exists(filename.c_str())) return;
  Storage.remove(filename.c_str());
  // The cache directory is keyed on the FILE PATH (Epub.h: "epub_" +
  // hash(filepath)) and is only validated against the cache format version --
  // never against the file's size or contents. A feed keeps the same path
  // every time, so without this the reader would find yesterday's book.bin
  // and rendered sections still valid and read the old articles out of
  // cache, no matter how fresh the download was. Progress goes too, which is
  // correct: page 4 of yesterday's articles means nothing in today's.
  const std::string cacheDir = Epub(filename, "/.crosspoint").getCachePath();
  if (Storage.exists(cacheDir.c_str())) Storage.removeDir(cacheDir.c_str());
}

void tagFouladBookId(const std::string& filename, const OpdsEntry& book, const OpdsServer& server) {
  if (server.url != FOULAD_EBOOKS_URL) return;
  const std::string fouladBookId = extractFouladBookId(book.id);
  if (fouladBookId.empty()) return;
  RECENT_BOOKS.addBook(filename, book.title, book.author, "", fouladBookId);
}

}  // namespace FouladOpdsHooks
