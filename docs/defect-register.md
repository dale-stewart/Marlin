# Defect register

Behaviour found while rescuing code, recorded rather than changed. Every entry is
pinned by a test carrying a `LEGACY-BEHAVIOR:` comment, so the current behaviour cannot
drift unnoticed — and so that fixing one starts from a failing test rather than a
guess.

Nothing here has been fixed, with two recorded exceptions (#16 and #18) where the defect was in
the measuring instrument itself. Changing any of the rest changes what the firmware does,
which is a decision for the maintainer, not a side effect of adding tests.

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
| 17 | `wait_for_hotend()` sets `residency_start_ms = now + SEC_TO_MS(TEMP_RESIDENCY_TIME) / 3` on the *first* pass of the wait loop, then tests it with the three-argument `PENDING(now, start, interval)`, which is `(now - start) < interval` in unsigned arithmetic. A start time in the future wraps the subtraction to near 2^32, so the condition is false immediately and the loop exits. | `module/temperature.cpp:4846` (hotend), `:5039` (bed — an identical copy, found by mutation testing rather than by reading) | M109 does no residency wait at all when the hotend is already within TEMP_WINDOW of the target as the command begins, and M190 does the same for the bed against TEMP_BED_WINDOW — it returns on the next pass instead of holding for TEMP_RESIDENCY_TIME. Reach the window one pass later and the offset is zero, the subtraction does not wrap, and the full wait happens, so the effect is confined to a hotend that is already at temperature. Pinned by `simulated_temperature___M109_does_not_hold_a_hotend_that_is_already_at_the_target`. | open |

## Fixed, with reason

| # | Symptom | Where | Impact | Status |
|---|---|---|---|---|
| 16 | `RingBuffer::write()` incremented `index_write` before storing the byte, and `read()` incremented `index_read` before loading it, so each side advertised a slot it had not finished with. A reader seeing the new index before the store landed took the previous lap's byte from that slot; the real byte then overwrote a slot the index had already passed. | `HAL/LINUX/include/serial.h:54`, `HAL/TEST/include/serial.h:54` | One substituted character, same length, on any report longer than the 128-byte buffer — where `HalSerial::write()` spins and the producer genuinely races the consumer. Affects the simulator, which drains the buffer from its UI thread, and any ISR/main-loop pairing. | fixed |

**Why this one was fixed rather than recorded.** The rule exists so that adding tests
does not quietly change behaviour. Here the defect was *in the instrument*: it corrupted
the serial output the acceptance tests assert against, at roughly 1 run in 40. A suite
with a random failure cannot serve as a mutation baseline — every mutant is scored partly
by chance — so leaving it pinned by a characterization test would have meant building the
rest of the rescue on a measurement known to be unreliable.

Pinning it instead of fixing it was considered and rejected: a test asserting "output is
occasionally corrupted" is not a test, and the fault is not deterministic enough to pin.

Evidence, measured on an isolated worktree at `525d497f` with the same binary otherwise:
**6 failures in 200 runs unfixed, 0 in 400 fixed**. The control was run first, because a
clean run of the fixed binary proves nothing unless the harness is known to detect the
fault.

## Test harness defects

Not firmware. These are faults in the instrument, listed here so that a behaviour the
suite cannot reach is not mistaken for a behaviour the firmware does not have.

| # | Symptom | Where | Impact | Status |
|---|---|---|---|---|
| 20 | The test binary has a latent static-initialisation-order dependency, and only the link order PlatformIO happens to use is good. `mutation_test.py:link_inputs()` collects objects with `rglob('*.o')` — **filesystem order**, which is neither stable nor reproducible — so the order it gets changes whenever test files are added. Measured on `linux_native_test`: the sorted order **segfaults before the first test**, and the current filesystem order **hangs after 355 of 377 tests**, in `stopwatch___duration_advances_while_running`. Only PlatformIO's own order runs to completion, and PlatformIO links through a response file, so that order is not recoverable from its verbose log. | `buildroot/share/scripts/mutation_test.py:link_inputs`, plus whatever static in the suite carries the dependency | **Mutation testing under `linux_native_test` cannot currently run at all** — the baseline gate refuses, correctly, rather than producing a number. That blocks re-measuring the Phase 1 targets (`numtostr`, `utf8`, `crc16`, `stopwatch`), whose recorded scores were taken before the order changed and are not reproducible today. `testhal_native_test` is unaffected — its order still works, and every figure in the phase-4 documents comes from it. The real fix is to remove the static-initialisation-order dependency from the test suite rather than keep hunting for a lucky order; capturing PlatformIO's order would only hide it. | open |
| 18 | `Timer::enable()` calls `schedule()`, which restarts the period: `next_fire_ns = now + period`. Real hardware does not — `HAL_timer_enable_interrupt()` sets an interrupt-enable bit and leaves the counter running, so a pending compare match still happens when it always would have. Under this HAL, anything that disables and re-enables a timer more often than that timer's own period starves it forever. `Stepper::endstop_triggered()` is exactly that: its `ATOMIC_SECTION_START/END` is a `suspend()`/`wake_up()` pair on MF_TIMER_STEP, and while a closed switch sits in front of a moving axis `Endstops::poll()` calls it from *every* temperature interrupt (~1 ms), against a step interval at the head of a block of ~2.8 ms. | `HAL/TEST/hardware/Timer.h:52` | A move that *begins* with its endstop already closed never completes: the abort the ISR was asked to perform is never carried out, `axis_did_move` is never cleared, and the next temperature interrupt asks again. Simulated time keeps advancing, so it presents as a hang, not a failure. A switch that closes *during* a move is unaffected, because by then the step interval is far shorter than the temperature period. The behaviour this costs is homing an axis that is already sitting on its switch — i.e. a second `G28` without `HOMING_BACKOFF_POST_MM`. **Fixed** by deleting `schedule()` from `enable()`. Now pinned the other way, by `endstops___a_move_that_starts_against_a_closed_switch_is_abandoned_at_once`. | fixed |

**Why this one was fixed rather than recorded.** Same reasoning as #16: the defect was in
the instrument, not the firmware. `HAL/TEST` is compiled only into the `testhal_*`
environments and ships to nobody, and its entire purpose is to behave like the hardware
it stands in for — so a divergence from hardware semantics is not behaviour to preserve,
it is the instrument being wrong. It also cost real coverage: homing an axis already on
its switch was unreachable, and presented as a hang rather than a failure.

Recording it instead was considered and rejected. A characterization test would have
pinned a livelock, and "this move never completes" is not behaviour any firmware
maintainer would want protected.

The test that pinned the stall carried a message saying what to do when the stall was
fixed, which is why the change surfaced immediately rather than silently passing.

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
| 19 | The thermal limit checks have no observable outcome other than shutting the machine down. `Temperature::_temp_error()` ends in `marlin.kill()`, whose last act (`minkill()`) is `while (!kill_state()) hal.watchdog_refresh();` — a spin waiting for an operator to press the kill button, with `watchdog_refresh()` a no-op and nothing able to change the pin from inside the loop. So no test can drive a sensor out of range and then *assert* anything: the call does not return. Measured rather than assumed — a probe driving `TEMP_0_PIN` to raw 1023 with a target set hangs the binary. The consequence is that `updateTemperaturesFromRawValues()`'s MINTEMP/MAXTEMP checks (`temperature.cpp:2973-2999`) can only be detected by a mutant *hanging*, never by a value. What is wanted is a seam that lets the shutdown be observed and returned from under test — the same shape as the `killed` flag the function already keeps, but reachable. That is a change to a production surface `kill()`'s many callers depend on. | `MarlinCore.cpp` and `temperature.cpp` are rescued — Phase 4a item 4 |

## Working on one of these

1. The pinning test already exists and passes against current behaviour. Start there.
2. Invert it: assert the intended behaviour, watch it fail.
3. Fix, then remove the `LEGACY-BEHAVIOR:` comment and the entry here.
4. Check the call sites — several of these are latent only because callers happen to
   avoid the input that triggers them.
