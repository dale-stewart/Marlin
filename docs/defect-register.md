# Defect register

Behaviour found while rescuing code, recorded rather than changed. Every entry is
pinned by a test carrying a `LEGACY-BEHAVIOR:` comment, so the current behaviour cannot
drift unnoticed — and so that fixing one starts from a failing test rather than a
guess.

Nothing here has been fixed. Changing any of it changes what the firmware does, which is
a decision for the maintainer, not a side effect of adding tests.

**Status values:** `open` — decision needed · `by-design` — recorded as intended
· `blocked` — fix requires work that is not yet safe.

## Genuine defects

| # | Symptom | Where | Impact | Status |
|---|---|---|---|---|
| 1 | `i16tostr3left()` cannot represent a negative. Digits come from `'0' + (n % 10)`, so `-5` renders as `"+"` (ASCII 43) and `-1` as `"/"`. | `libs/numtostr.cpp:143` | Punctuation on the display instead of a number. Ten call sites (DOGM, TFTGLCD, tft, extui, menu_info) all pass non-negative values today, so latent. The signature takes `int16_t` and promises otherwise. | open |
| 2 | `ftostr42_52(99.999)` returns `"00.00"`. The dispatch tests the value *before* rounding, picks the narrow field, then rounding carries to 100.00 and the leading digit is dropped. | `libs/numtostr.cpp:225` | A value just under a hundred displays as zero. | open |
| 3 | `i16tostr4signrj(-1000)` returns `"-000"`. The four-digit branch is chosen on the signed value, then the sign overwrites the thousands digit. | `libs/numtostr.cpp:155` | Reads as zero rather than a thousand. | open |
| 4 | `Stopwatch::resume()` zeroes `startTimestamp` via `reset()`, then sets `RUNNING` without setting it again, so `duration()` computes `millis() - 0`. | `libs/stopwatch.cpp:82` | A print resumed after power loss is reported as older than it is, by the controller's uptime. Affects print time display and remaining-time estimates. | open |
| 5 | `Stopwatch::resume(0)` leaves the watch stopped — `if ((accumulator = with_time)) state = RUNNING;` only starts on a non-zero time. | `libs/stopwatch.cpp:86` | A job resumed with zero accumulated time runs untimed. Power-loss recovery normally restores a non-zero time, so latent. | open |
| 6 | `GCodeParser::parse()` assigns `string_arg` after skipping spaces, so for a valueless parameter followed by another token it points at the space before the *next* token: `"G0 X Y"` yields `" Y"`, not `"X Y"`. | `gcode/parser.cpp:348` | The string argument is off by one token. `"G0 X"` is unaffected. M-code string handling may depend on the current shape. | open |
| 7 | `i8tostr3rj(-128)` returns `"-28"` — the sign takes the hundreds column and the hundreds digit is lost. | `libs/numtostr.cpp:81` | Only affects the single value `-128`. | open |
| 8 | A stray continuation byte (a sequence starting mid-character) is skipped: the decoder consumes it, leaves the value at 0, and returns. | `lcd/utf8.cpp:157` | A corrupted or mis-sliced string silently loses a character rather than showing a replacement glyph. Slicing by byte offset rather than character can produce this. | open |
| 9 | A lead byte claiming more than four bytes (`0xFE`, `0xFF` — never valid UTF-8) is skipped the same way, with no indication. | `lcd/utf8.cpp:160` | As above. | open |

## Recorded as intended

These follow from fixed-width display fields and are consistent across the library. They
are listed so that a future change knows it is changing something deliberate.

| # | Behaviour | Where |
|---|---|---|
| 10 | A value too wide for its field is truncated to the low digits with no marker: `ui16tostr3rj(1000)` is `"000"`, `ui8tostr2(100)` is `"00"`, `ftostr72rj(123456.7)` is `"23456.70"`. | `libs/numtostr.cpp` |
| 11 | The `ns` (no sign) conversions drop the sign, so `-1.5` renders identically to `1.5`. Likewise `ftostr5rj`. | `libs/numtostr.cpp` |
| 12 | `pcttostrpctrj()` does not clamp, so a value above 100 prints as-is (`123%`). | `libs/numtostr.cpp:73` |
| 13 | `ftostr52sprj()` and `utostr3()` *clamp* at their maximum while everything else truncates — an inconsistency within the library, not a fault in either. | `libs/numtostr.cpp` |

## Design corrections, blocked

Not defects; structural problems whose fix is gated on the test frontier
(see `CLAUDE.md`).

| # | Correction | Blocked until |
|---|---|---|
| 14 | `GCodeParser` is static-only with global mutable state (`codenum`, `codebits`, `string_arg`), read directly across the codebase. | its consumers are rescued — Phase 2 |
| 15 | Every `numtostr` conversion writes into one shared `char conv[9]` and returns a pointer into it, so two calls in one expression clobber each other. Fan-in of 23 files. | its consumers are rescued |

## Working on one of these

1. The pinning test already exists and passes against current behaviour. Start there.
2. Invert it: assert the intended behaviour, watch it fail.
3. Fix, then remove the `LEGACY-BEHAVIOR:` comment and the entry here.
4. Check the call sites — several of these are latent only because callers happen to
   avoid the input that triggers them.
