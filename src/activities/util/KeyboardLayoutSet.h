#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
};

// Table position is the persisted bit assignment. Keep existing rows in place
// and append new layouts so SDK enum changes cannot reinterpret saved masks.
inline constexpr LayoutInfo ALL[] = {
    // Only layouts whose Language this fork ships. Midad carries EN/AR/BG/FA
    // (the rest were dropped to cut firmware size), and the SDK has no Arabic,
    // Bulgarian or Persian keyboard -- Arabic input goes through
    // KeyboardEntryActivity's own panel instead. Adding an Arabic layout to the
    // SDK, mirroring HebrewIl, would let that panel retire; see the sync notes.
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
// Symbol layers have no Latin letters, so credentials and URLs require at
// least one of these layouts to remain enabled.
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
