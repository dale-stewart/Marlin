# Defect register

Behaviour found while rescuing code, recorded rather than changed. Every entry is
pinned by a test carrying a `LEGACY-BEHAVIOR:` comment, so the current behaviour cannot
drift unnoticed — and so that fixing one starts from a failing test rather than a
guess.

Nothing here has been fixed, with three recorded exceptions (#16, #18 and #20) where the
defect was in the measuring instrument itself. Changing any of the rest changes what the
firmware does, which is a decision for the maintainer, not a side effect of adding tests.

**Status values:** `open` — decision needed · `by-design` — recorded as intended
· `blocked` — fix requires work that is not yet safe.

## Genuine defects

| # | Symptom | Where | Impact | Status |
|---|---|---|---|---|
| 1 | `i16tostr3left()` cannot represent a negative. Digits come from `'0' + (n % 10)`, so `-5` renders as `"+"` (ASCII 43) and `-1` as `"/"`. | `libs/numtostr.cpp:143` | Punctuation on the display instead of a number. Ten call sites (DOGM, TFTGLCD, tft, extui, menu_info) all pass non-negative values today, so latent. The signature takes `int16_t` and promises otherwise. | open |
| 2 | `ftostr42_52(99.999)` returns `"00.00"`. The dispatch tests the value *before* rounding, picks the narrow field, then rounding carries to 100.00 and the leading digit is dropped. | `libs/numtostr.cpp:225` | A value just under a hundred displays as zero. | open |
| 3 | `i16tostr4signrj(-1000)` returns `"-000"`. The four-digit branch is chosen on the signed value, then the sign overwrites the thousands digit. | `libs/numtostr.cpp:155` | Reads as zero rather than a thousand. | open |
| 4 | `Stopwatch::resume()` zeroes `startTimestamp` via `reset()`, then sets `RUNNING` without setting it again, so `duration()` computes `millis() - 0`. | `libs/stopwatch.cpp:82` | A print resumed after power loss is reported as older than it is, by the controller's uptime. The error is an **offset, not a drift**: `millis()` is both the uptime and the origin the elapsed time is measured from, so the clock still advances one second per second — it is simply too high for ever, by the uptime at the instant of the resume. That is why nobody watching the number climb would notice. Affects print time display and remaining-time estimates. Now reachable end to end and pinned through the feature that causes it, by `power_loss___a_resumed_job_reports_the_uptime_as_well_as_the_resumed_time` — `PrintJobRecovery::resume()` issues `M24 T<seconds>`, and `M24` hands that straight to `Stopwatch::resume()`. | open |
| 5 | `Stopwatch::resume(0)` leaves the watch stopped — `if ((accumulator = with_time)) state = RUNNING;` only starts on a non-zero time. | `libs/stopwatch.cpp:86` | A job resumed with zero accumulated time runs untimed. Power-loss recovery normally restores a non-zero time, so latent. | open |
| 6 | `GCodeParser::parse()` assigns `string_arg` after skipping spaces, so for a valueless parameter followed by another token it points at the space before the *next* token: `"G0 X Y"` yields `" Y"`, not `"X Y"`. | `gcode/parser.cpp:348` | The string argument is off by one token. `"G0 X"` is unaffected. M-code string handling may depend on the current shape. | open |
| 7 | `i8tostr3rj(-128)` returns `"-28"` — the sign takes the hundreds column and the hundreds digit is lost. | `libs/numtostr.cpp:81` | Only affects the single value `-128`. | open |
| 8 | A stray continuation byte (a sequence starting mid-character) is skipped: the decoder consumes it, leaves the value at 0, and returns. | `lcd/utf8.cpp:157` | A corrupted or mis-sliced string silently loses a character rather than showing a replacement glyph. Slicing by byte offset rather than character can produce this. | open |
| 9 | A lead byte claiming more than four bytes (`0xFE`, `0xFF` — never valid UTF-8) is skipped the same way, with no indication. | `lcd/utf8.cpp:160` | As above. | open |
| 24 | **Null pointer dereference on `M502` with no card mounted.** `CardReader::jobRecoverFileExists()` calls `recovery.file.open(&root, …)` without checking `isMounted()`. With no volume mounted, `root`'s `SdVolume*` is null, `SdBaseFile::openRoot()` passes it straight to `SdVolume::fatType()`, and that dereferences null. `openJobRecoveryFile()` five lines below **does** have the `if (!isMounted()) return;` guard, so this is an omission in one of a pair, the same shape as #17. | `sd/cardreader.cpp:1677` (missing guard), crashes in `sd/SdVolume.h:107` | Reached by an ordinary user action: `M502` → `MarlinSettings::reset()` → `PrintJobRecovery::enable(false)` → `changed()` → `purge()` → `removeJobRecoveryFile()` → here. So **factory-resetting a printer that has power-loss recovery enabled and no SD card inserted dereferences a null pointer** — undefined behaviour, a hard fault on most targets. Requires `SDSUPPORT` + `POWER_LOSS_RECOVERY`, which is why the default test configuration never saw it. Found by building the media configuration for Phase 4b; it segfaults the test binary, which is what exposed it. | open |
| 22 | `static_assert(nullptr == strstr(EVENT_GCODE_SD_ABORT, "G27"), …)` is not a constant expression. `strstr` is only foldable when the compiler treats it as a builtin, which it does at `-O1` and above but not at `-O0`. | `inc/SanityCheck.h:449` | A configuration with media but without `NOZZLE_PARK_FEATURE` fails an unoptimised build with "non-constant condition for static assertion" instead of the message the check exists to print — so a user hits a compiler diagnostic about `strstr` rather than being told to enable nozzle parking. Optimised builds are unaffected, which is why it has survived: debug builds are the ones that break. Found while adding the media test configuration, which builds at `-O0`. | open |
| 23 | Enabling media auto-enables `REINIT_NOISY_LCD`, and `Warnings.cpp` then reports it as unnecessary on any board without an SD-detect pin. | `Configuration_adv.h:1842`, `inc/Warnings.cpp:1007` | Not wrong, but the default and the warning disagree: a board with media and no SD-detect always warns unless the user turns off an option they never turned on. Harmless where warnings are advisory; fatal where the build uses `-Werror`, which is how the test environments build. Worked around in `test/004-sd_powerloss.ini` rather than changed. | open |
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
| 20 | The test suite's result depended on the order its objects were linked in, and `mutation_test.py:link_inputs()` collected them with `rglob('*.o')` — filesystem order, neither stable nor reproducible. Measured on `linux_native_test`: sorted order **segfaulted before the first test**, filesystem order **hung part-way through**. **Three independent causes, all now fixed.** (1) `all_marlin_tests` was a namespace-scope `std::list` in `unit_tests.cpp`, and every `MARLIN_TEST` registers into it from another translation unit's static initialiser — so any test initialised first pushed into an unconstructed list. Now constructed on first use. (2) `Clock::startup` was a class static with a dynamic initialiser; a caller reading the clock before `Clock.cpp` was initialised got a zero baseline, i.e. time since the epoch, which overflows once scaled. Now constructed on first use. (3) **The one that actually caused the hang:** `Stepper::init()` under this HAL arms a real POSIX interval timer whose handler is the stepper ISR, and the ISR programs the next interval as its last act, so it self-perpetuates. `Timer::disable()` only masks the signal — it leaves the timer armed — so a test that woke the stepper left a ~1 kHz signal source running for the rest of the process. Every later `sleep_for()` was then interrupted faster than it could complete (`si_overrun` 12-14, 780k signals in 25 s), and `Endstops::resync()`'s `safe_delay(2)` never returned. Added `Timer::stop()`, which **blocks the signal before disarming** — the other order leaves one queued signal to re-arm the timer it just stopped — plus `HAL_timer_stop_all()`, called from the test framework after every test. | `test/unit_tests.cpp`, `HAL/LINUX/hardware/Clock.{h,cpp}`, `HAL/LINUX/hardware/Timer.{h,cpp}`, `HAL/LINUX/timers.{h,cpp}`, `buildroot/share/scripts/mutation_test.py` | Mutation testing under `linux_native_test` works again, and `link_inputs()` now sorts, so a run is reproducible. Verified by linking the suite in **sorted, reversed, filesystem and four shuffled orders — all 377 pass in every one**. `crc16` re-measures at **97.1%**, exactly the figure recorded before the breakage. | fixed |
| 18 | `Timer::enable()` calls `schedule()`, which restarts the period: `next_fire_ns = now + period`. Real hardware does not — `HAL_timer_enable_interrupt()` sets an interrupt-enable bit and leaves the counter running, so a pending compare match still happens when it always would have. Under this HAL, anything that disables and re-enables a timer more often than that timer's own period starves it forever. `Stepper::endstop_triggered()` is exactly that: its `ATOMIC_SECTION_START/END` is a `suspend()`/`wake_up()` pair on MF_TIMER_STEP, and while a closed switch sits in front of a moving axis `Endstops::poll()` calls it from *every* temperature interrupt (~1 ms), against a step interval at the head of a block of ~2.8 ms. | `HAL/TEST/hardware/Timer.h:52` | A move that *begins* with its endstop already closed never completes: the abort the ISR was asked to perform is never carried out, `axis_did_move` is never cleared, and the next temperature interrupt asks again. Simulated time keeps advancing, so it presents as a hang, not a failure. A switch that closes *during* a move is unaffected, because by then the step interval is far shorter than the temperature period. The behaviour this costs is homing an axis that is already sitting on its switch — i.e. a second `G28` without `HOMING_BACKOFF_POST_MM`. **Fixed** by deleting `schedule()` from `enable()`. Now pinned the other way, by `endstops___a_move_that_starts_against_a_closed_switch_is_abandoned_at_once`. | fixed |

| 25 | Executing a serial-received command hangs `GCodeQueue::advance()` in the `emergency_parser` + `advanced_pause_feature` configuration, once the line is long enough. Short lines — everything the suite already sends, up to roughly twenty characters — are fine; a 39-character line hangs, as does an 87-character one, with both a real command (`M117 <padding>`) and a string that is not a command at all. Not the over-long-line path: 39 characters is well inside `MAX_CMD_SIZE` (96). Not the receive buffer either: 88 bytes fits the port's 128. The hang is inside `advance()`, after the characters have been read and the command committed — narrowed to that by tracing either side of the call, not inferred. **Cause not established.** Found while writing a test for the over-long-line branch of the serial reader (`queue.cpp:387`); that test was removed rather than left disabled, so the branch is currently unasserted and its ten surviving mutants are recorded as unbought rather than unkillable. | `gcode/queue.cpp`, reproduced under `test/002-extruders_1_runout.ini` | Unknown. It may be an instrument problem — a report filling a transmit buffer on a port the fixture does not silence is the shape that has caused every previous hang here — or a real fault in a configuration real machines use. Until it is diagnosed, no test in this suite may execute a long command through the queue, which is a restriction on what can be covered rather than a known firmware defect. | open, undiagnosed |

| 26 | Stack corruption during a probing move, traced to a 4 KB write through an invalid pointer. `Stepper::pulse_phase_isr()` pulses the X/Y/Z step pins; `Gpio::set()` dispatches each write to that pin's registered peripheral, and on one of them it reaches a **`SimulatedHeater`** — although heaters are only ever attached to `HEATER_0_PIN` (10, versus step pins 54/60/46) and the pin table is verified clean at every test boundary in every configuration. The heater it reaches is not valid: `SimulatedHeater::advance()` calls `curve()`, which lazily builds `float celsius[1024]` — **4 KB written through that pointer**, which AddressSanitizer reports as a stack-buffer-overflow WRITE at `simulated_heaters.h:66`. The array indexing itself is in bounds; it is `this` that is wrong. | `tests/support/simulated_heaters.h`, `HAL/TEST/hardware/Gpio.h`, the probing path | **The original framing was backwards.** This was recorded as "a peripheral outlives the object it points at", because that is what the first ASan report showed — a stack-use-after-return. Chasing it further showed that to be a *consequence*: the 4 KB write destroys whatever frames it lands in, taking the stack canary and the cleanup records with it. That is why the `SimulatedBed` in the probing test has its constructor run and its destructor silently skipped, even inside an explicit scope, while its siblings in the same frame are destroyed normally — the compiler emitted the call, and the corrupted frame meant it never happened. Blocks probing, so `probe.cpp` and `G29` stay at 0%; the fixture they need is written and tested. Reproduce with `make unit-test-asan UNIT_TEST_CONFIG=bed_leveling` plus `ASAN_OPTIONS=detect_stack_use_after_return=0` (which is needed to see past the first report to the write that causes it) and a restored `probe_at_point` test. **Next:** find how a heater pointer reaches a step pin's table entry. Independently, a fixture doing a 4 KB lazy write from inside an interrupt callback is worth removing whatever the cause — building the curve eagerly would confine the damage and make the real fault visible where it happens. | open, harness |
| 27 | `types___SString` asserts the printed form of `1234.5 * 2345.602` and gets a different answer depending on how the suite is compiled: `2895645.67` under `-O2` and under `-O0` with coverage, `2895645.75` under AddressSanitizer. Both are correct roundings of the same product — the first is the nearest `double`, the second the nearest `float` — so what the assertion actually pins is whether the compiler folded the multiply at full precision before narrowing it. That is a property of the optimiser, not of `SString`. Upstream test, pre-existing; found by the first ASan run. The comparison was changed from `strcmp_P(...) == 0` to `TEST_ASSERT_EQUAL_STRING`, which prints both sides — the reason this took one build to diagnose rather than several. | `Marlin/tests/core/test_types.cpp:658` | The one known failure under `make unit-test-asan`. Everything else in the default configuration is clean under the sanitizer. | open, by-design elsewhere |

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
| 21 | **The HAL is selected by build-time file substitution, not by dependency inversion.** A platform is chosen with `-D__PLAT_*` and a `build_src_filter` that compiles one `src/HAL/<PLATFORM>` directory, and the firmware reaches hardware through macros (`WRITE`, `READ`, `HAL_timer_*`) that are textual substitution. So the HAL is not a dependency that can be injected — it is a directory swapped at build time, which is why exactly one can exist per binary, why each new platform is a wholesale rewrite of ~30-50 files, and why the real HALs (AVR, STM32, …) have no tests at all: nothing can stand in for them. Wanted instead: compile-time dependency inversion — the modules take their hardware access as a template parameter, so a real and a fake can be instantiated side by side in one binary at no runtime cost. | its consumers are rescued — this is every module that touches hardware, so it is the largest entry here. See the sequencing note below. |
| 19 | The thermal limit checks have no observable outcome other than shutting the machine down. `Temperature::_temp_error()` ends in `marlin.kill()`, whose last act (`minkill()`) is `while (!kill_state()) hal.watchdog_refresh();` — a spin waiting for an operator to press the kill button, with `watchdog_refresh()` a no-op and nothing able to change the pin from inside the loop. So no test can drive a sensor out of range and then *assert* anything: the call does not return. Measured rather than assumed — a probe driving `TEMP_0_PIN` to raw 1023 with a target set hangs the binary. The consequence is that `updateTemperaturesFromRawValues()`'s MINTEMP/MAXTEMP checks (`temperature.cpp:2973-2999`) can only be detected by a mutant *hanging*, never by a value. What is wanted is a seam that lets the shutdown be observed and returned from under test — the same shape as the `killed` flag the function already keeps, but reachable. That is a change to a production surface `kill()`'s many callers depend on. | `MarlinCore.cpp` and `temperature.cpp` are rescued — Phase 4a item 4 |

### Sequencing note for #21 — inverting the HAL

Two constraints shape any design, both measured rather than assumed.

**The language level is set by the oldest target, not the newest.** AVR builds with
`-std=gnu++1z` (C++17) on the avr-gcc that PlatformIO ships, and several platforms
`build_unflags` down to `gnu++11`/`gnu++14`. **C++20 concepts are therefore not available
on the targets that most need the abstraction.** That is not fatal: compile-time
dependency inversion at zero runtime cost is fully expressible in C++17 with class
templates, policy parameters, CRTP where static dispatch must go the other way, and
`static_assert` to check a policy's shape. Concepts are the ergonomic layer — better
error messages and named requirements — and can be added later, behind a feature test
macro, on platforms whose toolchain allows them. Designing *around* concepts today would
either exclude AVR or force a toolchain decision that has nothing to do with testability.

**It cannot be one change.** #21's consumers are every module that touches hardware, and
the ordering rule in `CLAUDE.md` forbids editing untested call sites. The migration that
respects it:

1. Add the templated seam **alongside** the existing macros. Purely additive — no call
   site moves, nothing is blocked by anything.
2. Redefine the macros as thin forwards to a default instantiation, so the existing call
   sites keep working unchanged and the two paths cannot diverge.
3. Migrate one module at a time, and only once *that module* is covered and mutation
   tested. The gate is per-module, not global: the whole firmware does not have to be
   rescued before any of it can move.
4. A platform HAL becomes a policy type rather than a directory, at which point a real
   and a fake can be instantiated in the same binary.

Step 4 is what makes the real HALs testable, and it would also retire the
`testhal`/`linux` split recorded in the plan document: a fake would no longer need its
own build environment, because it would no longer need to be the *only* HAL in the binary.

## Working on one of these

1. The pinning test already exists and passes against current behaviour. Start there.
2. Invert it: assert the intended behaviour, watch it fail.
3. Fix, then remove the `LEGACY-BEHAVIOR:` comment and the entry here.
4. Check the call sites — several of these are latent only because callers happen to
   avoid the input that triggers them.
