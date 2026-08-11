# Applying legacy-rescue to the rest of Marlin

A sequencing plan for `.claude/skills/legacy-rescue/`, based on measurements taken while
rescuing `Marlin/src/gcode/parser.cpp`. See `CLAUDE.md` for why this fork exists and for
the ordering rule that governs refactors.

**Where it stands.** Phases 0-3 and 4a are complete; 4b is underway (three to eight
configurations); Phase 5 took one cross-cutting correction all the way through the
workflow, including the acceptance and refactor steps that had never been run here.
Platform-agnostic line coverage is **77.7%** in the default configuration, with 547-618
tests depending on configuration and 31 acceptance scenarios. The most transferable
finding is in Phase 5: the acceptance suite is not a nicety before a refactor, it is the
only net that does not move when the code does.

## Two facts that shape everything

**Only 80 of 1,016 `.cpp` files compile into the native test build** — about 4,600 lines.
Everything else is unreachable by any test until new configuration profiles or HAL fakes
pull it in. "The remaining code" is therefore two problems: rescuing what is testable,
and expanding what is testable.

**The measurement loop is too slow to scale.** `parser.cpp` — 62 compiled lines — took
eight mutation runs at roughly 20 minutes each. Extrapolated to 4,600 lines that is on
the order of 150 hours of mutation compute, before any tests are written. This is the
binding constraint, and it is fixable once rather than paid 74 times.

## Phase 0 — Make the loop affordable — **DONE**

Everything else was gated on this. Delivered as
`buildroot/share/scripts/mutation_test.py` (`make unit-test-mutation`).

Measured on `parser.cpp`: a full run went from ~20 minutes to **75 seconds**, and
re-running only the survivors takes **11 seconds**. Validated against the previous
runner by comparing survivor sets, not just scores — both report the same 46 survivors.

Most of the gain was not parallelism. Over half the per-mutant cost was PlatformIO's own
startup, so the runner invokes the compiler and linker directly: compile the one mutated
translation unit (0.78s), relink the 88 objects with it substituted (0.37s), run (0.002s).
The remaining gain is running that pipeline across all cores.

Two bugs found by validating against a known-good result, neither of which the baseline
gate would have caught, since a green baseline looks identical under both:

- the test binary exits 0 even when assertions fail, so classifying on exit code alone
  reported 7.1% where the truth was 78.3%;
- `pio run -t compiledb` after the test build leaves objects deleted by
  `preflight-checks.py` unrebuilt, breaking every link.

Original plan for this phase, for the record:

| Work | Why | Expected gain |
|---|---|---|
| Parallel runner (N workers, isolated build artifacts) | runs are serial today | ~5-8x |
| Pre-filter non-compiling mutants with a syntax-only pass | 248 of 446 mutants (56%) did not compile; each cost a full build, link and run | ~2x |
| Restrict the mutator set to semantic operators | much of that 56% is invalid C++ the text mutator emits | fewer wasted mutants |
| Incremental mode — mutate only lines changed since the last run | re-running survivors should be the default, not a manual step | large on iteration |
| Promote the runner out of scratch into the repo with a `make` target | it holds load-bearing logic (baseline gate, failure classification) | reproducibility |

**Exit gate:** a full measurement of one target in under five minutes, with the runner
committed and subject to the same rules it enforces.

## Where the Gherkin lives

Scenarios describe user-facing features at the system boundary, not modules. Leaf
utilities do not get feature files of their own — they are exercised through whichever
feature uses them, and their edge cases stay in unit tests. So Phase 1 targets are
finished at step 5 (covered, mutation tested, survivors accounted for) and their
step 6-7 work happens later, as the features that call them are described.

Practically: `numtostr` is exercised by whatever renders a display or answers M105, and
gets its coverage from those features plus its own unit tests. The two suites need not
correspond one-to-one; between them they must leave nothing unexercised.

## Phase 1 — Leaf utilities

`libs/numtostr` (270 uncovered lines), `lcd/utf8` (81), `libs/vector_3`, `libs/crc16`,
`libs/stopwatch`, `core/utility`.

Pure functions, one to three includes, no hardware. `numtostr` alone is about 6% of the
uncovered compiled mass and is pure string formatting. These are cheap wins that prove
out the Phase 0 tooling and build fluency with the acceptance-suite idiom.

Roughly six targets, low risk. Unit tests only — see "Where the Gherkin lives" above.

**Correction: pick targets by coverable lines, not file size.** This list was drawn up
from `wc -l`, and two of its entries turned out to be nearly empty in the compiled
configuration: `libs/vector_3` is entirely behind `#if ABL_PLANAR ||
AUTO_BED_LEVELING_UBL` and has **zero** coverable lines here, and `core/utility` compiles
just five (a `serial_delay` that delegates to `safe_delay`). Both move to Phase 4, where
a configuration that enables bed leveling makes `vector_3` reachable. Read the target
list off the coverage report, not the directory listing.

**Correction: leaf does not mean seam-free.** `Stopwatch` reads the millisecond clock, so
its timing arithmetic is invisible to tests that complete in microseconds. The native HAL
already exposes `Clock::setTimeMultiplier()`, so the seam existed and no change to
`Stopwatch` was needed — but a "leaf utility" needing clock control was not anticipated
here. Check for a time, randomness or I/O dependency before assuming a phase needs no
seam work.

`lcd/utf8` is **done**: 100% line coverage, 90.4% mutation detection (95.4% once the
sixteen provably-equivalent mutants in the binary search are excluded). Most assertions
are against the UTF-8 standard rather than current output, and `pf_bsearch_r` is checked
exhaustively against a linear reference for every array size up to eight.

`libs/crc16` is **done**: 100% line coverage, 97.1% mutation detection. Its expected
values come from the published CRC-16 check vectors rather than from current output, so
these are correctness tests rather than characterization.

`libs/stopwatch` is **done**: 100% line coverage, 66.2% raw mutation detection — 78.2%
once the ten mutants on `debug()` calls are excluded, which are equivalent because
`DEBUG_STOPWATCH` compiles `debug()` to an empty inline. The remaining survivors are
assignments overwritten before they can be observed.

`libs/numtostr` is **done through step 5**: 18 characterization tests plus three
survivor-killing rounds, 98% line coverage and 96.5% mutation detection, with three
display-corrupting defects recorded as LEGACY-BEHAVIOR rather than changed
(`i16tostr3left` on negatives, `ftostr42_52(99.999)`, `i16tostr4signrj(-1000)`).

## Phase 2 — G-code handlers

The ~30 `gcode/*` files already in the build, then `gcode/queue.cpp` (171 uncovered) and
`gcode/gcode.cpp` (163).

These are `GCodeParser`'s consumers. Under the test-frontier rule, the blocked
global-state correction becomes legal exactly when they are covered. `parser.h` has a
fan-in of 59 files, about 30 of which are in the compiled set, so this phase clears most
of it and Phase 4 finishes the rest.

**Prerequisite found while starting this phase:** any command that reports to serial
hangs the test binary, because the native HAL busy-waits on a 128-byte transmit buffer
that nothing drains outside the simulator. Tests that dispatch commands must mark the
port as having no host attached; see `CLAUDE.md`. This affects every target in this
phase, not just the reporting ones — most handlers report when called with no arguments.

**Second prerequisite:** commands that wait on hardware (`M109`, `M190`, anything that
synchronises the planner with queued moves) cannot be tested in this build — the LINUX
HAL has no heater or stepper model to advance, so the wait never ends. Only their
already-satisfied paths are reachable. Testing the waiting itself means building the
unit tests against the NATIVE_SIM HAL, which does simulate those devices; that is a
Phase 4 item, not a reason to skip the commands now.

**Milestone:** the first blocked surface change becomes unblocked.

**Re-framing `parser.feature` — done.** It has been replaced by three feature files in
`Marlin/tests/gcode/features/`: receiving a job from a host, preparing the printer for a
job, and telling the host what the printer is doing. Nothing in them names the parser.
The steps put bytes on the serial port and the queue reads them, so the parser is
exercised because a real command passed through it.

Measured on its own, the acceptance suite reaches 77% of `parser.cpp` and 58% of
`queue.cpp` without ever calling either — which is the point of the exercise. The
remainder is edge-case parsing (malformed sequences, the checksum-position quirks, the
M32 path) that legitimately belongs in unit tests, and is where a restructuring of the
parser could not lean on the scenarios alone.

## Phase 3 — Core modules

| Target | Uncovered lines | Commits in 2 years |
|---|---|---|
| `module/temperature.cpp` | 501 | 53 |
| `module/planner.cpp` | 377 | 53 |
| `module/stepper.cpp` | 230 | 70 |
| `module/motion.cpp` | 219 | 38 |
| `module/settings.cpp` | — | 42 |
| `module/endstops.cpp` | 62 | — |

Highest churn and highest untested mass — the best regression value in the repository,
and the only phase that needs real step-4 seams (clock, ADC, GPIO, stepper ISR). Fan-in
is severe (`motion.h` 162, `planner.h` 151, `temperature.h` 124), so these surfaces stay
frozen for a long time. Budget the most time here; expect seam work to dominate.

## Decision: the test HAL is the working loop

Taken 2026-08-03, after `linux_native_test` produced its third instrument defect
(register #20) without ever having found a firmware one.

Everything from here — the suite, coverage, mutation — runs against
`testhal_native_test` by default. The LINUX HAL suite is **integration testing**: run
deliberately with `make unit-test-integration`, not on every change.

The precondition was checked rather than assumed: the two binaries were asked which tests
they actually register, and **no test exists under LINUX that does not also exist under
the test HAL**, so the switch drops nothing. The test HAL also runs all three configs
(464/465/465) in comparable wall-clock time.

The reasoning is cost and reliability. The LINUX HAL backs its peripherals with real
operating-system facilities — wall-clock sleeps, POSIX timers, signals — so it is slower
and its failures are less reproducible; #16 (a serial race), #18 (a timer that re-armed on
enable) and #20 (link-order dependence via a leaked POSIX timer) were all faults in the
instrument. The test HAL makes time a counter, which removes that entire class.

What this costs, stated so it is not discovered later: the LINUX HAL is the environment
upstream actually runs, and it is the only thing exercising that HAL's code. #20 was a
real order-dependence that the test HAL could not have caught, because its timers are not
real ones. Keeping the integration target — and running it before anything is called done
— is what stops this from becoming a blind spot.

## Phase 4 — Expand the frontier

Detailed in **`docs/legacy-rescue-phase-4.md`**, written after Phases 2 and 3 reached
their ceiling.

The short version: this is two phases, not one. **4a** switches the unit tests to a
simulated machine so interrupts happen and time can be advanced on demand — one change
that unblocks six verified-unreachable behaviours and the largest uncovered function in
the firmware (`stepper::isr`, 127 lines). **4b** widens the configuration matrix to
compile the 736 platform-agnostic files that no test can currently reach.

They are sequenced deliberately: 4a multiplies the value of the tests already written,
while 4b multiplies their cost.

**Status: 4a has met its exit gate.** The test HAL exists; motion, every blocking command
(`M400`, `G4`, arcs) and the temperature ISR all run on simulated time. `stepper.cpp`
24% → 87.8%, `temperature.cpp` 24% → 79.2%, platform-agnostic total 58.4% → **70.0%**,
398 tests under the test HAL and 377 under LINUX.

Mutation testing then confirmed that the coverage was reach rather than protection —
`stepper.cpp` scored **28.8%** and `temperature.cpp` **32.2%** — and one round of tests
written against the survivors has since taken them to **46.9%** and **54.0%**, with
killed-by-assertion roughly doubling in both. The lesson worth carrying: the productive
assertions were *derived relationships* (halving acceleration stretches a move by √2; the
applied PID gains are the Ziegler-Nichols relations of Ku and Tu), because a relationship
that follows from the physics is hard to satisfy by accident. Recorded outputs are not.

**4a is now complete.** All six behaviours have tests — homing and endstop triggering
closed the set, taking `motion.cpp` from 20.8% to 59.7% and `G28.cpp` to 85.7% with no
HAL work needed. A second survivor round took `stepper.cpp` to **67.3%** (53.7%
killed-by-assertion, timeouts unchanged, so none of the gain was hangs). Final state:
**74.3%** platform-agnostic line coverage, 444 tests under the test HAL, 377 under LINUX,
18 acceptance scenarios.

Two defects in the instrument were fixed rather than recorded (register #16, a serial
ring-buffer race; #18, a test-HAL timer that re-armed on enable and livelocked homing).
Both were fixed on the same reasoning: a characterization test cannot usefully pin a race
or a livelock, and the test HAL exists to behave like the hardware it replaces.

**The `temperature.cpp` survivor round is complete.** Both halves landed — the limit and
shutdown paths (`test_thermal_limits.cpp`) and the PID autotune internals
(`test_pid_autotune.cpp`) — taking the file from 54.0% to **62.7%** detection and 79.8%
to 81% line coverage, with killed-by-assertion 361 → **479**. Measured in one clean run
on an unloaded machine after both landed; the covered set grew 400 → 407 lines in the
process, so the endpoints are close rather than strictly comparable.

The safety paths that end in `kill()` are a blocked correction rather than a gap, and now
a measured one: those lines have no observable outcome other than shutting the machine
down, so no assertion can reach them at all. They need a seam that lets `kill()` return,
which is a production surface change. Recorded as register entry #19; details at the end
of the 4a section of the phase-4 document.

**4b is underway and is not what it looked like.** The configuration matrix has gone from
three to eight, and every addition was made to reach a *specific* file that no
configuration compiled — not to sweep up files in bulk:

| Config | Reached |
|---|---|
| `006-eeprom` | `EEPROM_SETTINGS`; `settings.cpp` went from 51 covered lines to 303 |
| `007-i2c_encoders` | `I2C_POSITION_ENCODERS`, which needed a stub I2C bus and a simulated device |
| `008-extui` | `EXTENSIBLE_UI`, which needed a stub display to link at all |

The lesson worth carrying from 4b so far: **a file compiled by no configuration reads as a
coverage gap and is usually a build problem.** `tool_change.cpp` showed 0% because it was
not in the default build, not because nothing tested it — it was already at 23% under
`003`. Whether a figure names its configuration turns out to matter more than the figure.

## Phase 5 — the first sequenced migration

The plan predicted that Phase 3 "may not reach 95% without seams that amount to surface
changes, which are themselves blocked by fan-in", and that some modules could not be
finished until their consumers moved. That is what happened, and this phase is the first
time the workflow ran all the way through it.

**The target.** `planner.settings` is a public mutable struct with two derived caches —
`mm_per_step` (the reciprocal of `axis_steps_per_mm`) and `max_acceleration_steps_per_s2`.
Keeping them in step was every caller's job to remember, connected to the field by nothing
but a comment. Forgetting it is silent: the machine keeps moving, at a scale that no longer
matches what it reports.

**The sequence, and what each step actually bought.**

1. *Rescue the consumers.* Eight files, closed in turn — `planner.cpp` 37.8% → 60.5%
   mutation (73.2% killable), `motion.cpp` → 57.8% (75.3% killable), `tool_change.cpp`
   0% → 41%, `settings.cpp` 16% → 38% (60.8% killable), `M92` → 100% line, `G2_G3` → 91%.
2. *Write the acceptance scenarios first.* This is the step that made the refactor possible
   and the one easiest to skip. **117 references to `planner.settings` live in the unit
   tests** — every one would have been edited by the migration, and a test rewritten by a
   refactor cannot show the refactor preserved anything. Two feature files
   (`keeping_its_settings`, `moving_the_tool`) name no C++ symbol, teardown included.
3. *Validate the acceptance suite alone (Step 7).* It reached `motion.cpp` at 5% and `G28`
   at 0% — and the reason was not missing scenarios. `acceptance_native_test` extended
   `env:linux_native_test`, where time is the wall clock, so motion was **not expressible**.
   Pointing it at the test HAL and adding six scenarios took `G28` 0% → 91%, `stepper.cpp`
   13% → 74%, `planner.cpp` 11% → 62%.
4. *Refactor, in five slices.* Accessors and an owner for the invariant; a dead write
   deleted (#31); bulk setters; and finally the distinction the API was missing —
   `set_*` clamps and warns because a *user* named a limit, `override_*` does not because
   the *firmware* is restating one. Two call sites had been assigning the arrays directly
   and were right to: the API had no way to say what they meant.

**Where it stopped, and why that is the interesting part.** Every write in every buildable
file now goes through `Planner`. The fields are still public because three display drivers
assign them and **cannot be built for the host at all** — `sovol_rts` and `creality/dwin`
are ill-formed on a 64-bit target (`sendData(int)` and `sendData(int32_t)` are one
signature here and two on the target), and `mks_ui` needs LVGL. That is not a coverage gap
and no amount of testing closes it; making those drivers host-portable is a real project
and it would have to come *before* this migration rather than inside it.

Two of the five did fall: `encoder_i2c` (a stub `Wire.h`, then a simulated encoder
answering on it) and `ui_api.cpp` (a stub display recording what it was told).

**The payoff, stated as evidence rather than argument.** The case for the encapsulation was
that direct assignment leaves a derived cache stale, silently. Three consumers were
*suspected* of exactly that (#33, #34) and none could be built, so none could be confirmed.
`calibrate_steps_mm()` was the fourth — and once `007-i2c_encoders` and a simulated encoder
made it drivable, the bug was **confirmed by watching a test fail**: calibration correctly
found 100 steps/mm while `mm_per_step` stayed at 1/80, the figure it had just proved wrong.
Register #35, fixed by routing the write through the owner. The defect existed because the
language could not express "you now owe a refresh", and stopped existing when the field had
an owner.

Across all five refactor slices **no test file was edited** and the acceptance suite stayed
green. That property is the whole reason any of it counts as evidence.

## Cross-cutting: a blocked-corrections register

`docs/defect-register.md`, 35 entries. It did not get forgotten, which was the risk, and
it earned its place in an unexpected way: entries #33, #34 and #35 are the same defect in
four different consumers, and writing them down separately as they were found is what made
the pattern visible. #35 is the one that could be run, and confirming it is what turned the
argument for the `planner.settings` migration from a design opinion into a measured fact.

Three instrument defects were fixed rather than recorded, on the standing reasoning that a
characterization test cannot pin a race, a livelock, or a store that depends on what a
previous run left behind: #16 (serial ring-buffer race), #18 (test-HAL timer re-arming on
enable), #32 (the emulated EEPROM was file-backed and shared across parallel mutation
workers — a flaky-test generator, and the suite passed only from the second run onwards).

## Per-target gates

Unchanged from the skill: line coverage at or above 95%, mutation detection at or above
80% excluding equivalents, acceptance-only parity with the unit suite, and every
survivor killed, documented as equivalent, or logged as an open question.

In practice the second of those has been met in the *killable* sense and not the raw one,
and the distinction has proved worth keeping. Equivalent mutants are a large and growing
share as a file gets better — 176 of 400 in `planner.cpp`, 256 of 424 in `settings.cpp`,
where the residue is placeholder constants written to keep a stored layout stable across
builds and read back into a variable that discards them. A raw score falling while the
killable score rises is normal, and a single number cannot show it. Every target closed
here reports both, with the reason categories checked rather than inferred.

## Caveats

- Phase 3 may not reach 95% without seams that amount to surface changes, which are
  themselves blocked by fan-in. Expect to discover that some modules cannot be finished
  until their consumers move, and to re-sequence.
- About 936 files remain unreachable after all four phases without substantial HAL
  simulation. A complete rescue is a multi-quarter effort; this plan covers the
  load-bearing code.
- Some of those files are unreachable for a reason no amount of simulation fixes: they do
  not compile for a 64-bit host, because overload sets that are distinct on AVR and 32-bit
  ARM collapse there. Making them buildable means editing untested driver code in order to
  compile it, and a change to an overload set can silently alter which function a call
  resolves to *on the target*. That is a hardware-in-the-loop job, not a testing one, and
  it is the honest ceiling on this approach.
- The estimate that matters is not file count. It is whether Phase 0 makes the following
  phases cost 20 hours of compute or 150.
