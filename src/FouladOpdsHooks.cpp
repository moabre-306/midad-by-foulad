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

  // Core serves no device-telemetry routes: /opds/reading-stats, /opds/device-log
  // and /opds/device-stats were legacy-only and were not carried over, so the
  // uploads that used to round-robin here all answered 404. Crash reports and the
  // debug log stay on SD untouched, waiting for /devices/v1/logs.
  (void)server;
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
