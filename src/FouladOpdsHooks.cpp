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

  // One heartbeat per connect. Since registerDevice was deleted this is the only
  // thing that refreshes device_tokens.firmware_version, so an account otherwise
  // shows whatever version the device paired with, forever. Cheap: a small JSON
  // body, well inside Core's 30/min.
  //
  // Reading stats and the debug-log upload are NOT restored here. Their Core
  // replacements (/reading/v1/events, /devices/v1/logs) need work this change
  // does not do -- events need a trustworthy wall clock the X3 does not have.
  FouladTelemetryGate::resetForNewConnection();
  if (!FouladTelemetryGate::allow(millis())) return;
  FouladDeviceTracking::sendHeartbeat(server.username, server.password);
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
