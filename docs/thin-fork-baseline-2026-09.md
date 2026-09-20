# Thin-fork baseline record — X3/X4 Pro convergence, September 2026

Records the Midad-side divergence the owner approved when the X3 consolidated
line and the X4 Pro convergence line were merged into one shared codebase.

**This is a baseline record, not an exemption.** It does not change
`scripts/thin-fork-guard.sh`, does not add a path allowlist, and does not
authorise any future divergence. It states what was reviewed and accepted, so
the guard's verdict can be read against a recorded decision instead of being
re-derived by hand each time.

## Scope

    integration branch  integration/1.6.22-x4pro-touch
    upstream baseline   7db14a015ca4b1d74d3cbed131a748f3029a4517
                        (CrossPoint 1.6.0rc-80-g7db14a01, 2026-09-06)
    guard base used     72b153fd (the 1.6.22 line, this merge's first parent)

## Correction to the file count

The guard lists **54** files. Ten of those are **not divergences**: their content
at HEAD is byte-identical to upstream at `7db14a01`. The merge moved them
*toward* upstream, and the guard reports them only because they differ from the
base ref, without checking whether the new content equals upstream's.

    lib/EpdFont/EpdFontData.h
    src/activities/settings/OpdsServerListActivity.h
    src/activities/settings/TextSettingsActivity.h
    src/activities/settings/TextSettingsPreview.cpp
    src/activities/settings/TextSettingsPreview.h
    src/util/DictHtmlPages.h
    src/util/DictionaryRegistry.cpp
    src/util/DictionaryRegistry.h
    src/util/HtmlToPlainText.cpp
    src/util/HtmlToPlainText.h

**The approved divergence set is therefore 44 files, not 54.** Of those 44, all
were already divergent from `7db14a01` on the X4 Pro convergence line before
this merge — no file in the set is newly hand-authored divergence.

## The 44, by why the difference is necessary

- **TOUCH-UI** — upstream ships no touch Settings, preview or option popup.
  These are the single shared architecture that lets one build serve X3's
  buttons and X4 Pro's touch panel. Explicitly *not* a second architecture.
- **ARABIC/RTL** — kashida justification, RTL shaping, Arabic-aware
  HTML-to-text and the dictionary above them. Upstream has no Arabic support.
- **PLATFORM** — board/capability plumbing on CrossPoint's own
  `FREEINK_CAP_*` and `BoardConfig::` abstractions; no Midad board switch.
- **CORE+OTA** — the Midad Core client (`api.midad.one`, pinned at GTS Root
  R4) and the digest-authenticated OTA path. Upstream OTA is unpinned.
- **HARDENING** — Wi-Fi connect-policy state machine and CSS cache-version
  reconciliation, both answering field-observed failures.
- **READER** — toolbar/overlay, long-press page skip, dictionary entry,
  KOReader sync.
- **FONTS** — SD font catalogue and the split kerning-class record layout.

Per-file assignment: `midad-verification-package/reports/divergence-classification.md`.

## What this record does NOT do

The guard still reports FAIL, and that is correct. Two things gate it, and
both need a repo admin — neither can be done from a working tree:

1. **The baseline has to actually move.** The guard measures against the PR's
   base branch. Against `origin/develop` this branch shows 222 divergent files,
   because develop carries neither the 1.6.22 sync nor either RC line. The
   divergence becomes "pre-existing" only once this work is on develop.
2. **`.github/workflows/ci.yml` is a security-boundary file** and differs from
   develop. It is NOT touched by this integration — it is identical to the
   1.6.22 line — but the boundary gate is path-based, not authorship-based, so
   it fires regardless. Clearing it requires the documented procedure in
   `docs/upstream-sync-architecture.md` ("Security-boundary file maintenance"):
   admin relaxes the required check, merges, restores immediately.

Recording the decision does not substitute for either step.

## Addendum — CrossPoint 1.6.5rc merge

    upstream merged     a1ceb6333f907856e4dcde21bd0a94f5be84452b (1.6.5rc)
    merge base          7db14a015ca4b1d74d3cbed131a748f3029a4517

Divergences **deliberately kept** in this merge. Each was reached by reading both
sides, not by taking a whole file from one of them; a future sync should not
re-open them without a product decision.

| Area | Kept | Why |
| --- | --- | --- |
| `RecentBooksActivity.{h,cpp}` | ours (upstream deleted it) | Midad's Home hero/recents row. Five dependents: `HomeActivity`, `ActivityManager`, `GridNav`, `QuranBook`, `GymCatalog`. |
| `OpdsBookBrowserActivity.cpp` | ours | Midad renders the catalogue as a direct cover grid via `render(RenderLock&&)`; upstream moved to declarative `UiScreen` builders this fork's header does not declare. |
| `HomeActivity` tile list | ours | Midad's Home is four tiles (Foulad eBooks, Stats, Apps, Settings). `menuItemToIndex`/`indexToMenuItem` must stay mutual inverses of that list. |
| `SettingInfo` location | ours (`SettingsTypes.h`) | This fork extracted it; upstream still defines it inside `SettingsActivity.h`. Taking upstream's copy would be a redefinition. Upstream's two new `SettingAction` values were ported across. |
| `uiTheme` settings row | absent | Single-theme firmware (`uiTheme = FOULAD`). |
| `main.cpp` idle loop | ours | BLE-aware light sleep; upstream's simpler loop cannot express the BLE RF-floor guard, and the code after it reads `blePreventsLowPower`/`idleMs`. |
| `test/minibidi_arabic` | not carried | Asserts `BidiUtils` emits Arabic presentation forms. This fork shapes at draw time in `lib/ArabicShaper`; `test/arabic_shaper` covers the same forms in the right architecture. |
| `PngToBmpConverter` internal | both parameters | Both sides added a 7th positional `bool`. `allowUpscale` keeps position 7 so existing callers bind unchanged; upstream's `originalThresholds` is appended at 8. |

Defects this merge found and fixed, rather than carried forward:

- `EpdFont::getLigature` was missing upstream's #3294 guard, so the font's
  Lam-Alef pairs re-collapsed already-shaped Arabic presentation forms and
  transposed letters. `test/ligature_guard` existed but was never registered.
- `SdCardFont`'s underuse hysteresis ended up evaluated **twice** (both sides had
  moved it, to different functions), retiring font buffers a scope early.
  Upstream's cache-miss placement is the one kept.
- `SdCardFont`'s arena retry (`PREWARM_ARENA_TOO_LARGE`, `measuredBytesPerGlyph`)
  had been dropped by the earlier 1.6.22 merge (`1a8fb18c`) and is restored.
- `KeyboardLayoutsActivity` compiled but was unreachable: its settings row and
  dispatch case were lost.
- OTA asset naming: the release workflows publish `crosspoint-<version>-<device>.bin`
  while the firmware still searched `firmware.bin`. All three publishing workflows
  and `OtaUpdater` now agree, including the leading-`v` strip Midad's tags need.
