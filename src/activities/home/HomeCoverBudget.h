#pragma once

#include <cstddef>
#include <cstdint>

// Affordability rule for HomeActivity's cover-tile frame snapshot.
//
// The snapshot is a pure redraw-cost cache: it holds the already-rendered cover
// pixels so a re-render (selector move, clock tick) can blit them back instead of
// re-decoding the thumbnail BMPs from SD. Covers are drawn identically either way,
// so refusing the snapshot costs SD reads, never the design.
//
// Its size is set by the theme's cover-tile height and the panel geometry, and it
// scales badly on the taller X3 panel: the Foulad tile (640px, the height its
// status line + 300px hero + Recents divider + 210px thumb row actually occupy)
// needs 42,768 bytes on an X3 in either portrait, and 51,282 bytes in either
// landscape -- against a measured clean-boot free heap of 49,040 with a largest
// free block of 38,900. So the portrait request cannot be served at all (it
// exceeds the largest block even on a pristine heap), and the landscape one would
// leave nothing behind if it could be. Buying it would starve everything that has
// to coexist with it on the Home screen, most visibly FontDecompressor's ~12KB
// per-group glyph buffers.
//
// Hence a reserve-based rule rather than a fixed cap: what matters is not the
// snapshot's absolute size but how much heap survives it. RESERVE_BYTES is sized
// so the largest working buffer Home legitimately needs afterwards still fits --
// the font decompressor's biggest observed hot group is ~12.2KB, and Home's
// own popup/label rendering and RecentBooksStore strings account for the rest.
// Cover *generation* (a 32KB PNG inflate ring buffer plus an EPUB metadata parse)
// deliberately releases the snapshot first, so it does not have to fit here.
namespace HomeCoverBudget {

// Free heap that must remain after the snapshot is taken.
constexpr uint32_t RESERVE_BYTES = 24u * 1024u;
// Headroom demanded on top of the request inside the largest free block, so the
// snapshot never lands as the single block that leaves the allocator with nothing.
constexpr uint32_t BLOCK_HEADROOM_BYTES = 2u * 1024u;

// `needed` bytes for the snapshot against a heap reporting `freeHeap` total free
// and `maxAllocHeap` in its largest contiguous block.
constexpr bool fits(const size_t needed, const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  if (needed == 0) return false;
  const uint64_t need = static_cast<uint64_t>(needed);
  return need + RESERVE_BYTES <= freeHeap && need + BLOCK_HEADROOM_BYTES <= maxAllocHeap;
}

}  // namespace HomeCoverBudget
