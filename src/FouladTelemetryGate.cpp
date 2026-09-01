#include "FouladTelemetryGate.h"

#include <Arduino.h>

namespace FouladTelemetryGate {
namespace {

// Core serves none of the four routes this module posts to. Flipping this to true is a
// deliberate act that must follow the routes actually existing -- check Core's
// route-inventory.ts, which CI holds against the live Hono route table.
constexpr bool ROUTES_IMPLEMENTED = false;

// The upload path runs on HttpDownloader's esp-tls transport, which does not verify the
// peer. Pairing was moved to wolfSSL with a pinned GTS Root R4 anchor
// (VerifiedTlsSession); telemetry never was. Flipping this to true requires routing these
// posts through VerifiedTlsSession first -- not merely editing this constant.
constexpr bool TLS_VERIFIED = false;

uint8_t consecutiveFailures = 0;
unsigned long nextAttemptAtMs = 0;

}  // namespace

Block check(const unsigned long nowMs) {
  if (!ROUTES_IMPLEMENTED) return Block::RoutesUnimplemented;
  if (!TLS_VERIFIED) return Block::UnverifiedTls;
  if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) return Block::Backoff;
  if (nextAttemptAtMs != 0 && static_cast<long>(nowMs - nextAttemptAtMs) < 0) return Block::Backoff;
  if (ESP.getMaxAllocHeap() < MIN_CONTIGUOUS_BYTES) return Block::LowContiguousHeap;
  return Block::None;
}

bool allow(const unsigned long nowMs) { return check(nowMs) == Block::None; }

void noteFailure(const unsigned long nowMs) {
  if (consecutiveFailures < MAX_CONSECUTIVE_FAILURES) consecutiveFailures++;
  unsigned long delay = BACKOFF_BASE_MS;
  for (uint8_t i = 1; i < consecutiveFailures && delay < BACKOFF_MAX_MS; ++i) delay *= 2;
  if (delay > BACKOFF_MAX_MS) delay = BACKOFF_MAX_MS;
  nextAttemptAtMs = nowMs + delay;
}

void noteSuccess() {
  consecutiveFailures = 0;
  nextAttemptAtMs = 0;
}

void resetForNewConnection() { noteSuccess(); }

const char* describe(const Block b) {
  switch (b) {
    case Block::None:
      return "ok";
    case Block::RoutesUnimplemented:
      return "routes-unimplemented";
    case Block::UnverifiedTls:
      return "unverified-tls";
    case Block::LowContiguousHeap:
      return "low-contiguous-heap";
    case Block::Backoff:
      return "backoff";
  }
  return "unknown";
}

}  // namespace FouladTelemetryGate
