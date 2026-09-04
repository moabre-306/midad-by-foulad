#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // Wrapped, not truncated: these are whole sentences asking the user to decide
  // something, and "Log out of Midad? You'll need to sign in ..." cut at the screen
  // edge is exactly the half that does not tell them what happens.
  safeHeading = heading;
  safeBody = body;

  // Text sits in the upper part of the screen so the confirmation popup
  // (centered) doesn't cover it.
  startY = renderer.getScreenHeight() / 6;

  // No title on the popup: the question is already on screen above it, wrapped and
  // in full. Passing it here as well drew it twice -- once truncated behind, once
  // unwrapped inside a dialog whose width is clamped to the screen, so it spilled
  // out of both edges of the box.
  const char* options[] = {I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM)};
  confirmPopup.show("", options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  int currentY = startY;
  if (!safeHeading.empty()) {
    currentY +=
        renderer.drawCenteredTextWrapped(fontId, currentY, maxWidth, safeHeading.c_str(), 4, true, EpdFontFamily::BOLD);
    currentY += spacing;
  }

  if (!safeBody.empty()) {
    renderer.drawCenteredTextWrapped(fontId, currentY, maxWidth, safeBody.c_str(), 4, true, EpdFontFamily::REGULAR);
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
