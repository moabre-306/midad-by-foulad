#pragma once

class GfxRenderer;

namespace TrmnlScreen {

// Paints TrmnlClient::SCREEN_PATH full-screen (grayscale when the image has
// it). Landscape images use a landscape orientation, so the 800x480 TRMNL
// layout fills the panel; TRMNL_STORE.flipScreen turns it 180 degrees.
// Restores the renderer's orientation before returning. False if there is no
// usable image (nothing is painted then).
bool draw(GfxRenderer& renderer);

}  // namespace TrmnlScreen
