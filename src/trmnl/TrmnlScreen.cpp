#include "TrmnlScreen.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include "TrmnlClient.h"
#include "TrmnlStore.h"

namespace TrmnlScreen {

bool draw(GfxRenderer& renderer) {
  HalFile file;
  if (!Storage.openFileForRead("TRMNL", TrmnlClient::SCREEN_PATH, file)) return false;
  Bitmap bitmap(file, false);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("TRMNL", "Cached screen is not a valid BMP");
    return false;
  }

  const bool flip = TRMNL_STORE.flipScreen;
  const bool landscape = bitmap.getWidth() > bitmap.getHeight();
  const auto previousOrientation = renderer.getOrientation();
  if (landscape) {
    renderer.setOrientation(flip ? GfxRenderer::LandscapeClockwise : GfxRenderer::LandscapeCounterClockwise);
  } else {
    renderer.setOrientation(flip ? GfxRenderer::PortraitInverted : GfxRenderer::Portrait);
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int x = bitmap.getWidth() < screenWidth ? (screenWidth - bitmap.getWidth()) / 2 : 0;
  const int y = bitmap.getHeight() < screenHeight ? (screenHeight - bitmap.getHeight()) / 2 : 0;

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight);

  // Same OEM grayscale sequence as the sleep screen bitmaps: a HALF base frame,
  // then the two gray planes.
  if (bitmap.hasGreyscale()) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  renderer.setOrientation(previousOrientation);
  return true;
}

}  // namespace TrmnlScreen
