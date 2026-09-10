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
