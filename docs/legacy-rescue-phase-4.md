# Phase 4 — reaching the code the unit tests cannot compile

A detailed plan for the last phase of `docs/legacy-rescue-plan.md`, written after
Phases 2 and 3 reached their ceiling. Phases 0-3 took repository line coverage from
7.8% to 51.7% across 372 tests; this document is about the two separate walls that
stopped it going further, and what each would cost to remove.

## What actually blocks progress

Two limits, with different causes and different fixes. The plan's original Phase 4
treated them as one item, which was wrong.

### Wall 1 — code that compiles but cannot run

Six behaviours were verified unreachable by probing, not assumed:

| Behaviour | Why it never returns |
|---|---|
| `G28` homing | moves until an endstop triggers; only the stepper interrupt can trigger one |
| `G4` dwell | waits for the planner to drain |
| `G2`/`G3` arcs | fill the block buffer, then `buffer_line` waits for space |
| `M400` | waits for the planner to drain |
| `M109`/`M190` with a real target | wait for a temperature nothing advances |
| endstop *triggering* | `endstops.update()` only records a hit while the axis is moving |

All six share one cause: **the LINUX HAL has no interrupts**. Nothing advances a
heater, steps a motor, or drains the planner. The same cause accounts for most of the
uncovered lines in the largest modules:

| Module | Coverage | Largest uncovered functions |
|---|---|---|
| `stepper.cpp` | 24% | **`isr` 127 lines**, `init` 14, `apply_directions` 11 |
| `temperature.cpp` | 24% | `isr` 70, `mintemp_error` 52, `init` 38, `task` 36 |
| `motion.cpp` | 20% | `prepare_line_to_destination` 68, `blocking_move` 42 |

`stepper::isr` alone is the single largest uncovered function in the firmware.
`thermalManager.init()` cannot even be called — it crashes with SIGFPE in this build.

### Wall 2 — code that never compiles

Only **62 of 798** platform-agnostic source files are in the test build. The rest are
excluded by the feature flags in `test/001-default.ini`:

| Area | Files | In the build |
|---|---|---|
| `lcd/` | 475 | ~2 |
| `gcode/` | 188 | ~44 |
| `feature/` | 81 | ~1 |
| `module/` | 24 | ~10 |
| `sd/` | 12 | 0 |

Nothing is structurally untestable about most of it. It simply is not compiled, so no
test can reach it. Measured against all platform-agnostic code, current real coverage
is on the order of 1-2%.

**These are independent.** Wall 1 is one build-environment change that multiplies the
value of the 372 tests already written. Wall 2 is a matrix expansion that multiplies
the *cost* of everything already written. They should be sequenced, not merged.

---

## Phase 4a — run the unit tests against a simulated machine

Goal: make the six stalled behaviours testable and open `stepper::isr`,
`temperature::isr`, homing, probing and real motion.

The NATIVE_SIM HAL already exists and the simulator uses it. The question is what
supplies the machine underneath it, and here the investigation turned up something
that changes the decision.

### What NATIVE_SIM actually depends on

`Marlin/src/HAL/NATIVE_SIM` does not contain a machine model. It calls `Kernel::` —
the interrupt scheduler — which lives in **MarlinSimUI**, an external library fetched
from a GitHub archive and pinned by commit hash in `ini/native.ini`. The physical
models live there too: `Heater.h` (a real thermal model — 12 V, 3.6 Ω, 13 g hotend
mass), `StepperDriver.h`, `EndStop.h`, `bed_probe.h`, wired together by
`virtual_printer.cpp`.

Three details decide the options below:

- `execution_control.h` (the Kernel) and `Heater.h` have **no UI dependency**.
- `StepperDriver.h` and `EndStop.h` each `#include <imgui.h>` — for a debug panel, not
  for the model.
- `virtual_printer.cpp`, which instantiates everything, includes `imgui.h` too.

So the machine model is *nearly* headless already, and the coupling to the UI is
shallow — a handful of includes rather than a design entanglement.

### Option A1 — depend on MarlinSimUI as it ships

Point a new test environment at NATIVE_SIM and let PlatformIO pull the whole library.

- **For:** no new code; the model is maintained upstream and already matches the
  simulator people actually run.
- **Against:** unit tests would require SDL2, SDL2_net, OpenGL and GLM in CI, and
  build ImGui and ImPlot to run a test suite with no interface. It also puts a
  third-party GitHub archive on the critical path of the test suite — pinned by hash,
  but a dead URL becomes a red build. `.github/workflows/ci-unit-tests.yml` currently
  needs nothing but PlatformIO.

### Option A2 — compile the headless subset of MarlinSimUI

Take `Kernel`, `Heater`, `StepperDriver`, `EndStop` and a cut-down `virtual_printer`,
excluding `window`, `user_interface`, `renderer` and the vendored ImGui.

- **For:** the real thermal and motion models, no SDL or OpenGL, much faster builds.
- **Against:** needs the `imgui.h` includes stubbed or the debug panels compiled out —
  four files, mechanical but a fork of upstream in effect. Every future upgrade of the
  pinned hash must be re-checked against the exclusion list. This is the option most
  likely to rot quietly.

### Option A3 — a minimal in-repo simulation kernel

Write, in `Marlin/tests/support/`, only what the tests need: a clock that advances on
demand, a timer that dispatches `Temperature::isr` and `Stepper::isr` a chosen number
of times, a heater that approaches its target when its pin is driven, and an endstop
that trips at a chosen position.

- **For:** no external dependency at all; the test suite stays buildable from a bare
  checkout. Time becomes *explicit* — `advance(3, SECONDS)` rather than a sleep — which
  makes the tests fast and deterministic rather than timing-dependent, the same
  property that made the `Stopwatch` tests work. It also matches the seam that already
  exists: the LINUX HAL reads sensors through `Gpio`, which tests can already drive.
- **Against:** it is a second model of the machine. If it disagrees with the real
  physics, tests pass against a fiction. The mitigation is to keep it deliberately
  crude — a heater that moves toward its target and a stepper that counts steps is
  enough to unblock all six behaviours, and anything more should be questioned.

### Recommendation

**A3, scoped hard.** The purpose is not to simulate a printer; it is to make interrupts
happen and let time pass on demand. A crude model does that with no third-party
dependency and no fork to maintain, and it keeps the property that has made every
useful test in this rescue work: the test says what happens, rather than waiting to see.

A2 is the fallback if the crude model turns out to be too crude — specifically if
thermal protection or PID behaviour needs real physics to be meaningful. That decision
can be made later without redoing 4a, because both options sit behind the same test
fixtures.

A1 should be rejected: building ImGui and OpenGL to run a headless test suite is a cost
paid on every CI run forever, to avoid writing roughly two hundred lines of fake.

### What the first experiment found

Running it changed two assumptions.

**The real stepper ISR works when driven by hand — no fake needed for motion.** The
blocker was never that the ISR could not run; it was that `HAL_timer_init()` is called
only from `main()`, which a unit test build excludes, leaving `Timer::frequency` at zero
and every timer call dividing by it. Initialising the timers and then calling
`stepper.isr()` in a loop drains the real planner and moves the real steppers: a 1 mm
move at 80 steps/mm leaves the stepper at exactly 80 steps. `stepper.cpp` went from 24%
to **87%** and `planner.cpp` from 59% to **74%** on the strength of five tests.

So option A3 shrinks: the machine model does not need writing, only the *scheduling*.

**Initialising the real HAL timers has lasting, process-wide side effects.** They are
POSIX interval timers delivering SIGRTMIN. Masking the signal is not enough — a masked
signal is still queued, so the moment anything re-enables interrupts the backlog is
delivered and the ISR runs behind the test's back. Pushing the compare far into the
future did not fully settle it either: tests that passed before the timers were
initialised (`M109 S0`) hang afterwards. This is a strong argument for the plan's own
recommendation over A1/A2, and a caution that even A3 should avoid `HAL_timer_init()` if
a narrower way to set the timer frequency can be found.

**Threading is the wrong way to run a blocking command.** Running the command on one
thread while the test steps the machine on another works for `M400`, which only waits,
but not for anything that writes to the planner while the stepper reads it — the real
firmware has an interrupt that *preempts*, not a second thread that runs alongside. The
race showed up as intermittent hangs and the approach was withdrawn.

**The tests need their own HAL.**

The next attempt was a single-threaded pump: a hook in `Marlin::idle()` so `stepper.isr()`
runs from inside the wait rather than beside it. It worked — `M400` passed, driven by the
real ISR from inside `planner.synchronize()`. But getting there took four separate
`#ifdef UNIT_TEST` branches in production code: one in `MarlinCore::idle()`, and three in
the LINUX HAL to stop `Timer` creating POSIX interval timers, arming them, and
segfaulting on an uninitialised handle. And it still did not reach `G4`, because `dwell()`
waits on wall-clock `millis()`, which nothing outside the HAL can control.

Every one of those patches was working around the same thing: **the LINUX HAL is a HAL
for running Marlin on a workstation, not for testing it.** Its timers are real POSIX
timers because it is meant to run in real time. A unit test wants the opposite — time
that only moves when asked.

Marlin already has the seam for this. There are fifteen HALs, chosen per build
environment; adding one for tests uses the architecture as designed rather than
special-casing an existing HAL with conditional compilation. All four patches were
reverted.

### What a test HAL provides

- **Timers that are state, not signals.** No `sigaction`, no `timer_create`, nothing
  asynchronous — which removes the queued-signal races and the storm entirely.
- **A clock the test owns.** `millis()` and `micros()` read a counter that advances only
  when a test says so. `dwell()`, `M109`'s residency wait and homing timeouts then
  complete instantly and deterministically instead of waiting on a wall clock.
- **Interrupts that fire when time advances.** Advancing the clock past a timer's compare
  value calls its handler — so `Stepper::isr` and `Temperature::isr` run because time
  passed, which is the real relationship, and no hook in `idle()` is needed.
- **Pins and ADC a test can drive**, which the LINUX HAL already offers through `Gpio`
  and can be carried over unchanged.

Most of the LINUX HAL can be reused as-is; what changes is `Clock`, `Timer` and the two
timer entry points. The estimate is a few hundred lines, against four permanent
`#ifdef UNIT_TEST` branches in production code and a category of failure — asynchronous
signals arriving between two assertions — that no amount of care in the tests can rule
out.

This is what Phase 4a should have proposed. The A1/A2/A3 framing above was about *which
machine model* to use and missed that the more basic question was *which HAL*.

### Where 4a actually stands

The test HAL exists (`Marlin/src/HAL/TEST/`), and four of the six behaviours are done.

**Motion** runs the real planner and stepper; `test_simulated_motion.cpp` asserts exact
step counts on both environments.

**Waiting, dwelling and arcs** are done — `test_blocking_commands.cpp` covers `M400`
with and without a queued move, `G4 P`/`G4 S`, `G4` after a queued move, and an arc
longer than the block buffer. Getting there needed `hal.idletask()` to advance the
clock, so that waiting by spinning on `idle()` costs simulated time the way it costs
real time on hardware.

Two diagnoses along the way were recorded confidently and were **wrong**, which is worth
keeping because both cost hours:

- *"`Timer::getCount()` needs re-entrancy protection."* It did not. It never moved the
  clock, so Marlin's `AWAIT_TIMED_PULSE` spin never ended, and it returned an absolute
  count where hardware returns ticks since the last restart.
- *"A finished block leaves `next_fire_ns` stale in the far future, so queueing a new
  block never re-arms the timer."* Also wrong; `Timer` needed no change at all. Five
  `idle()` calls producing zero steps was simply too short a look —
  `Planner::get_current_block()` withholds the first block for `BLOCK_DELAY_FOR_1ST_MOVE`
  (100) interrupts while fewer than three moves are queued. The actual hang was a **pin**:
  `KILL_PIN` reads HIGH from reset on a board because it has a pull-up, but every
  simulated pin reads LOW, and LOW is `KILL_PIN_STATE`. The firmware saw the kill button
  held from the first instruction, debounced it over 250 passes, and the 250th
  `marlin.idle()` call reached `kill()` — which spins forever waiting for a release.
  Only commands that wait ever call `idle()` that many times, which is exactly why only
  those commands hung.

The general lesson, now a rule in `Marlin/src/HAL/TEST/AGENTS.md`: **a simulated pin
powers up in a state no board is ever in.** `Marlin::setup()` configures the pull-ups and
does not run in a test build, so the fixture has to stand in for it. Expect more of these.

**`M109`/`M190` with a real target are now done too** — the heaters are first-order
models attached as `Gpio` peripherals to the heater pins, so they tick from the ISR's own
soft PWM with no polling and no production seam. The commands reach their targets because
the firmware heated them. **Homing and endstop triggering remain.**

### What the measurement says

`testhal_native_coverage` (added for this, since these behaviours only run there):

| | Lines | At 4a start | Now |
|---|---|---|---|
| Platform-agnostic total | | 58.4% | **70.0%** (3024/4323) |
| `stepper.cpp` | | 24% | **87.8%** |
| `temperature.cpp` | | 24% | **79.2%** |
| `planner.cpp` | | 59% | **78.3%** |
| `motion.cpp` | | 20% | **20.8%** |

`stepper.cpp` cleared the gate by a wide margin — the phase paid for itself there.
`temperature.cpp` has since been brought up the same way and is now **79.2%**, taking the
platform-agnostic total to **70.0%**. Two obstacles had been recorded as facts and both
were wrong in the same direction — attributed to the component rather than to its
bring-up:

- The SIGFPE was never in `Temperature::init()`. That function ends with
  `HAL_timer_start(MF_TIMER_TEMP, ...)`, which divides by `Timer::frequency` — zero until
  `HAL_timer_init()` has run. It is the *same* uninitialised-frequency fault that once
  looked like a bug in `stepper.init()`. Order the bring-up correctly and it runs cleanly
  in both environments.
- The ADC held the exact analogue of the `KILL_PIN` trap, as predicted. A simulated pin
  reads 0; a raw count of 0 on a thermistor divider converts to **320 °C**, above both
  `MAXTEMP` limits, so the first `Temperature::task()` reached `kill()` and never
  returned. Bring-up now drives every analog input to a room-temperature count before
  `Temperature::init()`.

That is three separate faults now — kill button, timer frequency, thermistor divider —
all the same shape: **the simulated machine powers up in a state no board is ever in, and
the production `setup()` that would fix it does not run in a test build.** Expect it
again for any new peripheral. `motion.cpp` is low for the same reason at
one remove: `prepare_line_to_destination` and `blocking_move` are homing paths.

### Exit gate — met in full

- ✅ **all six** behaviours have tests — homing and endstop triggering completed the set
- ✅ `stepper.cpp` above 60% — **94.8%**
- ✅ `temperature.cpp` above 60% — **79.8%**
- ✅ no new external dependency in the test build

444 tests under the test HAL, 377 under LINUX, 18 acceptance scenarios. Final coverage:

| | At 4a start | End of 4a |
|---|---|---|
| Platform-agnostic total | 58.4% | **74.3%** (3213/4326) |
| `stepper.cpp` | 24% | **94.8%** |
| `planner.cpp` | 59% | **79.8%** |
| `temperature.cpp` | 24% | **79.8%** |
| `motion.cpp` | 20% | **59.7%** |
| `G28.cpp` | 0% | **85.7%** |
| `endstops.cpp` | ~0% | **51.6%** |

Two defects found in the instrument itself were fixed rather than recorded (register #16
and #18), on the reasoning that a characterization test cannot usefully pin a race or a
livelock, and that `HAL/TEST` exists precisely to behave like the hardware it replaces.

**Homing needed no HAL work**, and the predicted pin-in-an-impossible-state hazard did
*not* apply: a simulated pin reads LOW and `X_MIN_ENDSTOP_HIT_STATE` is HIGH, so every
switch reads open from reset — which is the state a real board powers up in. What it
needed was a switch that closes *because the carriage arrived*, since `G28` never returns
to the test body. `simulated_endstops.h` attaches a peripheral to the STEP pin and counts
pulses through DIR; it keeps its own carriage position deliberately, because
`do_homing_move()` zeroes the stepper count before each of the three moves in one `G28`.

### The coverage is reach, not protection — measured

Both new targets were mutation tested against `testhal_native_test`, mutants restricted
to gcov-covered lines:

| Target | Lines covered | Testable mutants | Killed | Timed out | Survived | Score |
|---|---|---|---|---|---|---|
| `stepper.cpp` | 87.8% | 559 | 86 | 75 | 398 | **28.8%** |
| `temperature.cpp` | 79.2% | 1433 | 139 | 322 | 972 | **32.2%** |

After a round of tests written against those survivors:

| Target | Testable | Killed | Timed out | Survived | Score | Killed-by-assertion |
|---|---|---|---|---|---|---|
| `stepper.cpp` | 559 | 182 | 80 | 297 | **46.9%** | 15.4% → **32.6%** |
| `temperature.cpp` | 1436 | 361 | 414 | 661 | **54.0%** | 9.7% → **25.1%** |

`stepper.cpp` is measured against an identical population (559 testable, 202 covered
lines), so its two scores are directly comparable. `temperature.cpp` is **not** quite:
the new tests reached three more lines, so the population moved from 1433/397 to
1436/400. The gain is far larger than that drift, but the two numbers are not the same
measurement and should not be quoted as though they were.

The number that matters is killed-by-assertion, which roughly doubled for `stepper.cpp`
and more than doubled for `temperature.cpp`. Timeouts still account for a large share of
`temperature.cpp`'s detections (414 of 775), so that suite still notices stoppage more
than wrongness.

A second round on `stepper.cpp` took it further, to **396/588 = 67.3%** with
killed-by-assertion at **53.7%** and timeouts unchanged at exactly 80 — so none of that
gain came from hangs. It also corrected the diagnosis below: the multistepping cluster did
need an *input* rather than an assertion, but not for the stated reason. With
`OLD_ADAPTIVE_MULTISTEPPING` disabled, `steps_per_isr` is not derived from the step rate
at all. It is a ladder, climbed one rung when an interrupt overruns its own interval and
given back when one finds itself waiting, so 3200 steps/mm at 200 mm/s leaves it at 1 for
the whole move — the exact configuration the first round's "multistepping" tests use.
Sustained overrun does reach 16, but saturates the ISR, which is then forced to
`min_ticks` and the computed interval becomes unobservable. What works is two buffered
moves: a fast one to climb the ladder, a slower collinear one to hold the rung without
saturating. Of the original 69 survivors in that cluster, 28 remain and all are
equivalent — `MULTISTEPPING_LIMIT` is the literal 16 and `loops` is always a power of two,
so every mutation of either comparand evaluates identically.

**A caution this produced, since fixed:** three tests from the first round were named for
multistepping and explained a mechanism they do not exercise. They still asserted
something true — pulses really do bunch up — but via the adaptive `max_loops` path, not
`steps_per_isr`. A test whose comment explains the wrong mechanism is worse than one with
no comment, because it is believed.

They are now named for what they actually check — that a high step rate loses no steps,
keeps to the commanded feedrate, and converts four times the feedrate into most of four
times the speed — and `with_resolution`'s comment now says outright that a single move
stays at one step per interrupt whatever its feedrate, with a pointer to `move_x_twice`
for the tests that do climb the ladder. No assertion changed; all three still pass, and
the suite totals are unmoved at 444/377/18.

The suspicion was right and the gap is wider than guessed. Two caveats make it wider
still:

- **Most detections are timeouts, not assertions.** Killed-by-assertion alone is 15.4%
  for `stepper.cpp` and **9.7%** for `temperature.cpp`. A timeout counts as detected —
  the suite would not pass — but it means the mutant broke a wait loop and the test hung,
  not that anything checked a value. Timeouts are a weaker signal, and a suite whose
  detections are mostly timeouts is one that notices *stoppage* rather than *wrongness*.
- The denominators exclude build failures (625 and 1371), which is correct, but the
  populations are large enough that the two scores are only comparable to future runs
  with the same covered-line set.

**The survivors are a handful of missing classes, not two thousand problems.** Every
large cluster says the same thing: *the tests assert the destination, never the journey.*

| Where | Cluster | What is unasserted |
|---|---|---|
| `stepper.cpp` 2442-2444, 2538, 1884-1920 | ~137 | multistepping and adaptive-ISR timing — and every test move is slow enough that `steps_per_isr` is always 1, so these branches are never even entered |
| `stepper.cpp` 2583, 2642, 2722-2729, 3083 | ~58 | the trapezoidal profile: `accelerate_before`, `decelerate_start`, `acceleration_time`, `ticks_nominal`. A mutant that corrupts acceleration still delivers the same total step count, and total step count is all the tests check |
| `temperature.cpp` 795-985 | ~123 | PID autotune. `M303` runs, but nothing asserts the constants it computes — `Ku`, `Tu`, `bias`, `d`, the `df` divisor |
| `temperature.cpp` 4821, 5015 | ~30 | the seconds-remaining arithmetic in the `M109`/`M190` progress reports |
| `temperature.cpp` 2973, 2999 | ~42 | sensor-range and direction checks on the paths that end in `kill()` |

So the work is three or four input/assertion classes. The first and third are **done** and
produced the gains above; what the round taught about the other two is worth recording:

1. ✅ **Assert timing, not just totals.** Derived relationships turned out to be the
   productive form — halving the acceleration stretches a move by √2, a triangular move
   peaks at its midpoint, acceleration and deceleration take equal time. A relationship
   that follows from the physics is hard to satisfy by accident, which is exactly the
   property that kills mutants; a recorded output is not.
2. ⚠️ **A move fast enough to need multistepping** — partly done, and the remainder is now
   precisely specified. The cluster only fell from 80 survivors to 69, because
   `MULTISTEPPING_LIMIT` is **16**: line 2442 needs `steps_per_isr >= 16`, sixteen pulses
   inside one interrupt, and the new tests reach only 2× or 4×. Roughly 3200 steps/mm at
   200 mm/s should get there. No assertion can substitute for the input.
3. ✅ **Assert what autotune computes.** The Ziegler-Nichols relations themselves —
   `Kp = 0.6·Ku`, `Ki = 2·Kp/Tu` — plus the relay levels being a mirrored pair within the
   power limits and every cycle straddling the target.
4. The `kill()` clusters remain alive as expected (`temperature.cpp` 2973-2999, ~53
   survivors), and stay that way until that seam exists (item 4 below).

Remaining clusters after the round: `stepper.cpp` 2442-2444 (69, needs the fast input),
2538 and 1884-1920 (~48, adaptive-ISR timing); `temperature.cpp` 3608 (17), the
`kill()` paths (~53), autotune internals at 804-954 (~42), and 4846/5040 (21) — the last
being the region of defect #17, where the behaviour is degenerate by definition and some
survivors are likely equivalent as a result.

### What remains in 4a

1. **Another survivor round on `temperature.cpp` — done.** The survivor set was clustered
   from `.pio/mutation/temperature2.json` (2808 run: 361 KILLED, 414 TIMEOUT, 661
   SURVIVED, 1372 BUILD_FAIL — 54.0%) and split into two independent bodies of work,
   taken by two agents on two files without contention.

   Settled by one clean run on an unloaded machine after both landed
   (`.pio/mutation/temperature4.json`):

   | | Before | After |
   |---|---|---|
   | Killed by assertion | 361 (25.1%) | **479 (33.2%)** |
   | Timed out | 414 | 426 |
   | Survived | 661 | 538 |
   | Detection | 54.0% | **62.7%** |
   | Line coverage | 79.8% | **81%** |

   **The two ends are not strictly comparable**, and the run says so itself: the new tests
   reach seven lines nothing reached before, so the covered set grew 400 → 407 and the
   testable population 1436 → 1443. The direction and magnitude are unambiguous; the
   decimal is not. Compare like with like by re-running the baseline, not by subtracting.

   - **`PID_autotune()`, lines 780-1010 — 199 survivors → 142. Done.** Eleven tests plus
     one instrument, in `test_pid_autotune.cpp`.

     What moved it was not eleven assertions but **`TracedHeater`**, which overrides the
     heater model's ISR callback to record every change of applied PWM with its timestamp
     and the model's own temperature. Every previous test read what autotune *said*;
     nothing checked what it *did*. That gives three measurements independent of the
     report — applied relay levels, the wall-clock length of each half-cycle, and the
     temperature at each switch — and it killed most of the switching and arithmetic
     cluster by itself. The rest came from input classes the fixtures could not produce
     until `SimulatedHeater` gained an optional *cooling* time constant: a heater that
     overshoots before it can switch, one that cools far slower than it heats so the bias
     is driven to its lower clamp, one that never falls back below target so the
     twenty-minute timeout fires.

     Of the 142 remaining: **99 equivalent** (mostly `PIDTEMPBED`/`PIDTEMPCHAMBER`
     compile-time `false`, plus values dead before they are read), **30 blocked behind
     `kill()`** — the whole `WATCH_PID` block, same cause as the range checks below —
     and **12 needing inputs that cannot be constructed** in this configuration.
   - **The limit and shutdown paths — 133 survivors** — **done.** `test_thermal_limits.cpp`,
     nine tests, on the same 400-line covered set so the two ends are comparable:

     | | Before | After |
     |---|---|---|
     | Killed by assertion | 361 | **419** |
     | Timed out | 414 | 418 |
     | Survived | 661 | 599 |
     | Detection | 54.0% | **58.3%** |

     The gain is +58 assertions; the timeout figure moved by 4, which is inside this
     harness's run-to-run variation near the timeout boundary. Per cluster:
     `disable_all_heaters()` 24 → 2, the print-timer threshold 43 → 7, both remainders
     equivalent (single-hotend argument swaps, and `TERN0`/`TERN1` arms the compiler folds
     away because the chamber is disabled — the disabled-build-flag class, reached through
     a `constexpr` array rather than an `#if`).

     The productive assertion was the threshold *relation*: `auto_job_over_threshold()` is
     false at exactly `EXTRUDE_MINTEMP / 2` and true one degree above. That single pair of
     assertions killed all 17 survivors on line 3608, including every arithmetic variant of
     the `/2` and every comparison variant. Asserting that a 200 °C target starts the timer
     would have killed none of them — every plausible threshold agrees at 200.

   - **The MINTEMP/MAXTEMP range checks at `2973-2999` stayed at 63, and this is a
     finding rather than a shortfall.** Those six lines have no observable outcome other
     than shutting the machine down: `_temp_error()` ends in `minkill()`, which spins on
     `while (!kill_state()) hal.watchdog_refresh();` — `watchdog_refresh()` is a no-op
     under this HAL and nothing inside the loop can change the pin, so the call never
     returns. Measured, not inferred: a probe driving `TEMP_0_PIN` to raw 1023 with a
     target set hangs the binary. Recorded as blocked correction **#19**.

     Of the 63: **24 equivalent**, **22 needing the `kill()` seam** (the mutant suppresses
     an error that should fire, observable only as a shutdown that did not happen), and
     **17 detectable only as a hang** (the mutant fires an error the original does not).
     Those last 17 were deliberately *not* written. They would be honest characterization
     tests, but they buy zero killed-by-assertion and cost a full timeout each on every
     future run of this target. Converting survivors into timeouts is not progress.

     This is **not** an instrument defect, so the #16/#18 precedent does not apply: on a
     real board `minkill()` genuinely does wait for an operator to press the kill button.
     The test HAL is being faithful. What is wanted is a production seam that lets the
     shutdown be observed and returned from — gated on `MarlinCore.cpp` being rescued.
3. **Reconsider the heater model's tuning.** The models are tuned, not derived, and the
   bed constant is load-bearing rather than cosmetic: the bed is bang-bang and
   `manage_heated_bed()` only reconsiders every `BED_CHECK_INTERVAL` (5 s), so a faster
   model swings past `TEMP_BED_HYSTERESIS` every cycle and `M190` never settles. A
   fixture parameter that must be inside a window for firmware logic to converge is a
   fragility worth either documenting precisely or removing.
4. **Decide about `kill()`.** The remaining ~20% of `temperature.cpp` is mostly paths
   that end in `kill()` — `maxtemp_error`, `mintemp_error`, the thermal-runaway state
   machine. These are the *safety* paths, so they are the ones most worth testing and the
   ones currently unreachable, because `kill()` never returns. Reaching them needs a seam
   that lets it return under test. That is a production surface change, so under the
   test-frontier rule it is a blocked correction, not a quick fix.

---

## Phase 4b — widen the configuration matrix

> **Status: underway.** Three configurations became eight, each added to reach one named
> file rather than to sweep up files in bulk — `006-eeprom` for `settings.cpp`,
> `007-i2c_encoders` and `008-extui` for two consumers of `planner.settings` that no
> configuration compiled. See the Phase 5 section of `legacy-rescue-plan.md` for what that
> unblocked and where it stopped. The section below is the original analysis.

Goal: compile the other 736 files so tests can reach them.

Each configuration is a separate `test/NNN-name.ini`, and each one is a **separate
coverage denominator and a separate mutation population**. This is the phase where cost
scales, so the choice of configurations matters more than the count.

### What each candidate would buy

| Configuration | Unlocks | Notes |
|---|---|---|
| **SD + media** | `sd/` (12 files), the SD half of `queue.cpp`, `M20`-`M34` | The queue is already 67% covered; this closes most of the rest. Needs a fake filesystem — the simulator uses a FAT image, which tests could too. |
| **LCD + menus** | a slice of `lcd/` (475 files) | The largest area by far, but most of it is per-display drivers. A single menu backend would cover the menu *logic* and leave the drivers untouched. Highest file count, lowest value per file. |
| **Bed leveling** | `feature/bedlevel/`, `vector_3.cpp`, `G29`, probing | The most algorithmically interesting code in the firmware: matrix maths, mesh interpolation, probe sequences. Pure functions behind a hardware-shaped API. Needs 4a for probing to run. |
| **TMC drivers** | `feature/tmc_util`, `M122`, `M906`-`M917` | Mostly register plumbing over SPI/UART; low behavioural density. |
| **Power-loss recovery** | `feature/powerloss` | Small, self-contained, and safety-relevant — it decides whether a print resumes correctly. Pairs naturally with SD. |

### Recommendation

Two configurations, not five:

1. **SD + power-loss recovery.** Small, closes the known gap in `queue.cpp`, and
   power-loss recovery is behaviour a user would notice going wrong. Defect #4 in the
   register — `Stopwatch::resume()` adding the controller's uptime — sits exactly here
   and cannot be properly exercised without it.
2. **Bed leveling.** The best ratio of behaviour to lines in the codebase, and it makes
   `vector_3.cpp` reachable, which Phase 1 had to defer for exactly this reason.

LCD is deliberately excluded despite being 60% of the files. Covering 475 display
drivers is a different project with a different value proposition; the menu *logic*
could be picked up later behind one backend if it proves worth it.

### Cost

Each configuration adds a full build and test run to CI, and a full mutation population
if mutation is run against it. At current sizes a configuration costs roughly 4 seconds
of test time and 1-3 minutes of mutation time per target.

---

## The three open questions, in detail

### 1. Does the simulated environment replace `linux_native_test` or sit beside it?

**Replace.** One environment, switched to the fake kernel.

The instinct is to keep both — the current environment is proven and 372 tests depend
on it. But two environments means every test file must state which one it belongs to,
and the mutation runner, coverage target and CI job all fork. The value of the fake
kernel is that time and interrupts become explicit; a test that does not use them is
unaffected by its presence. There is no behaviour in the current environment that the
fake kernel removes.

The exception worth allowing: keep the ability to build *without* it for one release
cycle, as a bisect tool. If a test starts failing after 4a, being able to run the same
binary without the kernel answers "is this the fake or the firmware?" in one command.
That is a build flag, not a second environment.

**Cost of being wrong:** if the fake kernel destabilises the suite, reverting is a
one-line environment change, because the fixtures are what tests talk to, not the HAL.

### 2. Which configurations matter most?

Answered above — SD + power-loss recovery, then bed leveling. The reasoning worth
making explicit is the *selection rule*, since it is the thing that generalises:

**Choose configurations by behavioural density, not file count.** Phase 1 picked targets
by `wc -l` and two of the four turned out to be nearly empty. The same mistake at
configuration scale is more expensive, because a configuration that pulls in 400 files
of display drivers commits every future mutation run to carrying them.

A useful proxy: how many *decisions* does the code make per line? Bed leveling is dense
with them; a display driver is mostly transcription.

### 3. Should mutation testing run across all configurations, or only the default?

**Only the default, plus the configuration that owns the target.**

Mutation cost is per configuration per target, and most targets are only compiled in one
configuration anyway. Running `numtostr` mutants under four configurations measures the
same code four times and reports the same survivors.

The rule that follows: a target's mutation score belongs to the configuration that
enables it. Where a target is compiled in several — `parser.cpp`, `queue.cpp` — run
mutation under the default, and treat the others as coverage-only unless a defect turns
up that is specific to one.

The runner already records `covered_lines`, `env` and `suite` in its results for exactly
this reason: two runs are only comparable within the same population, and that will
matter much more once configurations multiply.

---

## What this phase does not do

It does not reach the 475 LCD files, the vendor UIs, or the per-board pin
configurations. After 4a and 4b, most of the firmware by file count is still
unexercised. What changes is that the code that *decides things* — motion, temperature,
levelling, the command path — is under test, and the remainder is largely transcription
between a decision and a device.

That is the honest end state of this plan: not a covered codebase, but a covered
machine.
