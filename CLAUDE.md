# Working in this fork

`AGENTS.md` is the canonical orientation document for Marlin itself — architecture,
pipeline, components, code structure. Read it for anything about how the firmware
works. This file covers only what is specific to **this fork**.

## What this fork is for

This is a personal fork (`dale-stewart/Marlin`, branch `unit-test-coverage`). It exists
to exercise and refine the **`legacy-rescue`** skill (`.claude/skills/legacy-rescue/`),
with the intent of eventually applying it across the codebase.

**Nothing here is intended to go upstream.** That changes what "good" means:

- Upstream acceptability, minimal diffs, and matching MarlinFirmware conventions are
  not constraints.
- Correcting bad practice is in scope and wanted — propose the real design correction,
  not the smallest upstream-safe change.
- The skill is the primary deliverable. Findings from running it belong back in
  `SKILL.md`, not just in the commit log.

**The skill must stay portable.** `SKILL.md` is meant to be lifted into other projects,
so it states the *general* rule and never names Marlin, a G-code command, or a firmware
symbol. When a run here teaches something, split it: the transferable principle goes to
`SKILL.md` in neutral terms, the concrete instance stays in this file. If an example only
makes sense to someone who knows this firmware, it belongs here.

## Order of operations: refactor only behind the test frontier

A refactor may change whatever is **inside** a rescued target. It must **not** change
that target's **public surface** until the code calling it is itself under test, fully
covered, and mutation tested.

Editing hundreds of untested call sites is refactoring untested code at one remove —
exactly what the cover-first rule forbids. So cross-cutting corrections are **sequenced
migrations**, not refactors: rescue the target, refactor its internals behind the
existing API, record the surface change as a blocked follow-on, rescue each consumer,
and move call sites only as each consumer becomes covered.

The ordering is what makes the eventual correction safe. The correction is still wanted
— do not let a blocked surface change become an argument for skipping cover-first, and
do not let it be quietly forgotten either.

Behaviour found and recorded rather than changed is listed in
`docs/defect-register.md` — genuine defects, deliberate design limits, and blocked
design corrections, each pinned by a test.

### Known blocked correction

`GCodeParser` is a static-only class with global mutable state (`parser.codenum`,
`parser.codebits`, `parser.string_arg`), read directly across the codebase. Removing
that global state is wanted, and is **blocked** until its consumers are rescued.
`Marlin/src/gcode/parser.cpp` itself has been rescued (95% line coverage, ~83% mutation
detection excluding equivalents, 18 acceptance scenarios).

## What the skill's general rules look like here

These are the concrete instances behind rules stated abstractly in `SKILL.md`. Read them
as worked examples, not as additional rules.

**"Assert derived relationships, not recorded outputs."** In this firmware the productive
assertions are the physics: halving the acceleration stretches a move by √2; a triangular
move peaks at its midpoint; acceleration and deceleration take equal time; a trapezoid comes
off its plateau as it went on; the applied PID gains are the Ziegler-Nichols relations of the
measured `Ku` and `Tu`; the autotune relay levels are a mirrored pair inside the power limits.
Asserting a final step count instead is what once left `stepper.cpp` at 87.8% line coverage
and 28.8% mutation detection; the physics assertions took it to **96% line, 68.9% mutation**
(default config, 222 covered lines). See `Marlin/tests/module/test_step_timing.cpp` and
`test_pid_autotune.cpp`.

**"Separate needs-an-assertion from needs-an-input."** `MULTISTEPPING_LIMIT` is 16, so
`stepper.cpp:2442` needs `steps_per_isr >= 16` — sixteen pulses inside one interrupt —
before any assertion can touch it. Roughly 3200 steps/mm at 200 mm/s gets there, and
`with_resolution()`/`move_x_twice()` in `test_step_timing.cpp` now do: `steps_per_isr` is a
ladder climbed when an interrupt overruns, so it takes two buffered moves rather than one fast
one. What is left on those three lines is equivalent, and equivalent *because of this build*:
`MULTISTEPPING_LIMIT` is a constant 16, so every mutation of the left operand
(`>= 0`, `<= 16`, `== 16`, `(1==1)`) has the same value, and `steps_per_isr` only ever takes
powers of two, so the `loops >= 15` and `loops == 2` near-misses are unreachable. A build with
`MULTISTEPPING_LIMIT` of 4 would distinguish the first group.

**"Watch for an assertion that is self-consistent rather than correct."** An acceptance
test once asserted that the `M105` reply contained the formatted value of
`thermalManager.degHotend(0)` — the same accessor `M105` formats its output from. It
would have passed with the sensor pipeline delivering any value at all. Assert against
`SimulatedSensors::hotend_would_read()`, which predicts independently of the pipeline.

**"Assert the channel a message came out on."** A failed probe writes `Error:Probing Failed`
via `SERIAL_ERROR_MSG`, and `LCD_MESSAGE(MSG_LCD_PROBING_FAILED)` puts the identical words on
the status line, which this build echoes to the host. A test searching for `"Probing Failed"`
passed with `probe.cpp:1100` deleted; `"Error:" STR_ERR_PROBING_FAILED` does not. The mutation
run is what said so.

**"When the only thing a branch changes is speed, time is the assertion."** `probe.cpp:833`
drops the nozzle at `Z_PROBE_FEEDRATE_FAST` before probing whenever it starts above
`Z_CLEARANCE_DEPLOY_PROBE + 5`. The measurement is identical either way, so 13 mutants of that
threshold survived every assertion on the reading. Probing from a millimetre either side of it
and subtracting locates it: `a_probe_from_high_up_covers_the_first_part_quickly` in
`test_probe.cpp`. Under the test HAL a blocking move costs about 0.1 s of fixed overhead on top
of its travel, which is why that test asserts bounds and not `1/fast + 1/slow`.

**"A magnitude needs bracketing from both sides."** An untrusted Z gets exactly 10 mm more
depth (`probe.cpp:812`). A test showing only that it reached further left `*1`, `*9`, `*11` and
`%10` alive; probing with the limit 9.5 mm and 10.5 mm above the bed killed all four.

**"A cluster that should have died and did not may mean the test is not there."** Editing
`test_homing_the_machine.cpp` by replacing a span between two markers silently deleted
`Z_is_homed_over_the_middle_of_the_bed`, which sat between them. The suite still passed with
one fewer test, and the only thing that noticed was `G28.cpp:168` — the probe XY offset at the
safe homing point — still showing four survivors after the test that killed them was written.

**"A shortcut and the exact computation agree wherever the shortcut is valid."** `planner.cpp:2555`
picks between normalising the junction vector across all of XYZE and scaling it by the
already-computed `inverse_millimeters`, which is `1/`the XYZ length. For a travel move the two are
the same vector, so 15 mutants of `esteps > 0` survived every corner test — every corner test used
travel moves. An extruding corner distinguishes them, and the amount matters: with `e` mm of
filament per 10 mm leg the planner sees `cos θ = -e²/(100 + e²)`, so a right angle becomes a 120°
bend at `e = 10` but barely moves at `e = 0.1`. See `extruding_through_a_corner_widens_it` in
`test_planner.cpp`. Note `normalize_junction_vector()` returns **false** when it normalised —
the return is "was it marginal", not "did it work".

### State of the `planner.settings` seam

The blocked correction in `planner.h` — removing the public `settings`/`mm_per_step` and the raw
`block_buffer` — needs its *callers* covered first. Measured against the default build (word
boundaries; a prefix grep wrongly puts `marlinui.cpp` in this list, because its only reference is
the `block_buffer_runtime()` accessor):

| caller | line | mutation | note |
|---|---|---|---|
| `module/tool_change.cpp` | 97% | 41.2% | measured under `extruders_3_runout`; not compiled by default |
| `gcode/config/M92.cpp` | 100% | 66.1% | |
| `gcode/config/M200-M205.cpp` | 94% | 57.6% | |
| `module/stepper.cpp` | 96% | 68.9% | |
| `gcode/calibrate/G28.cpp` | 91% | 66.4% | |
| `gcode/motion/G2_G3.cpp` | 91% | 58.4% | |
| `module/settings.cpp` | 95% | 38.1% raw / **60.8% killable** | measured under `006-eeprom`; 51 covered lines become 303 |
| `module/motion.cpp` | 76% | 57.8% raw / 75.3% killable | |
| `module/temperature.cpp` | — | — | **not a blocker**: its three references are inside `MPCTEMP`, which is off here |

**`settings.cpp` is closed at 38.1% raw / 60.8% killable.** The raw figure is low and stays low
for a reason worth knowing: **256 of its 424 survivors are placeholder constants**. When a
feature is compiled out, `save()` still writes something in its slot to keep the block layout
stable across builds — `const xyze_pos_t planner_max_jerk = LOGICAL_AXIS_ARRAY(5, 10, 10, …)`
with `CLASSIC_JERK` off, `autoretract_defaults` with `FWRETRACT` off — and `load()` reads the
same slot into `dummyf` and discards it. Verified by changing the values wholesale: nothing
observes them. They are killable only by a build that has the feature, which is the whole point
of writing them.

What is left after that is reporting: `report()` on boot, `report_position()` when a load moved
the machine, and the debug lines around the CRC and version messages. No cluster above eight.

The behaviours that matter are pinned by `test_settings_storage.cpp`: the round trip, a second
save replacing the first, a reset leaving the stored block alone, and three ways a bad block is
refused — checksum, version, and a stored value that is not a number. Each failure is asserted to
be *reported* as well as refused, because a machine that quietly forgets its calibration is worse
than one that says so.

### Where the `planner.settings` migration stopped, and why

Every write to `axis_steps_per_mm`, `max_acceleration_mm_per_s2` and `max_feedrate_mm_s` in this
build now goes through `Planner`, single-axis or bulk. Reads too, for resolution. What stops the
fields becoming private is two things, and only one of them is about coverage.

**Consumers outside every test configuration.** Four LCD drivers and the I2C position encoder
assign these arrays and are compiled by no configuration under `test/`. Two of them are already
suspected wrong (register #33). Covering them needs configurations that build them, which is the
same problem `006-eeprom` solved for `settings.cpp` and the same solution.

**The setters conflated two operations — now separated.** `set_max_acceleration(axis, v)` and
`set_max_feedrate(axis, v)` clamp and warn under `LIMITED_MAX_*_EDITING`, which is right for *a
user naming a limit* and wrong for *the firmware restating one*. `override_max_acceleration()` and
`override_max_feedrate()` are the second operation: taken as given, not announced, and still
keeping the derived step-rate limits in step. `set_*` delegates to `override_*`, so the invariant
lives in one place and the difference between the two is exactly the clamping.

`G28`'s `begin_slow_homing()` and `M92`'s low-`E` compensation are the two firmware overrides, and
both now say so. Enabling `improve_homing_reliability` in `005-bed_leveling` is what made the
first of those safe to touch: that block was compiled by no configuration, and the existing homing
tests exercise it the moment it is built (G28 96% there). It is executed rather than strongly
asserted — nothing measures homing acceleration — which is worth knowing before leaning on it.

With that, **every raw write to the three per-axis arrays in compiled code is gone**.

**And there the migration ends, permanently, short of making the fields private.** The five
remaining consumers — `sovol_rts`, `creality/dwin`, `mks_ui/draw_number_key`, `extui/ui_api` and
`encoder_i2c` — are not merely untested. They cannot be built for the host at all, which was
checked rather than assumed:

- `SOVOL_SV06_RTS` fails to compile: it wants Arduino's `String`, and its `sendData(int, …)` and
  `sendData(int32_t, …)` overloads are the same signature on a 64-bit target.
- `EXTENSIBLE_UI` **is now buildable**. `ui_api.cpp` links only against a concrete UI supplying
  twenty-two `ExtUI::on*` callbacks; `tests/support/stub_extui.cpp` is the smallest one that
  satisfies the linker, and it *records* rather than discards — empty bodies would compile just as
  well and would make the interesting half untestable, since the contract of ExtUI is that the
  firmware tells the display when things happen and silence is the failure that matters. Config
  `008-extui`.

  Note the two directions need different tests. Homing, resets and status messages are the
  firmware calling *out*, and those live at the firmware's call sites, not in `ui_api.cpp` — three
  scenarios of that left the file at 0%. `ui_api.cpp` is what a display calls *in*, so covering it
  means a test standing in for the display. Only the migrated setters are covered here (2%);
  covering the rest of that 226-line API is a separate and much larger job with no defect behind
  it.
- `I2C_POSITION_ENCODERS` **is now buildable and tested** — it needed `<Wire.h>` (a stub bus, on
  the same footing as `HAL/TEST/spi.cpp`) and Arduino's legacy `Bxxxxxxxx` binary-literal macros,
  both now in `HAL/TEST/include/`. Config `007-i2c_encoders`. The bus grew an `I2CDevice`
  attachment seam and `tests/support/simulated_i2c_encoder.h` answers on it, reading its count
  from a `SimulatedAxisWithLimit` — the carriage, not `stepper.position()`, because an encoder
  driven by the firmware's own belief could never disagree with it, which is the one thing an
  encoder is for. 2% → 13%.

  Note `passes_test()` reports the field strength recorded by the last `get_raw_count()` and does
  not fetch one itself. A test that asks without reading gets "never seen" whatever is on the bus,
  which is how the first version of the bad-field test here passed against a perfectly healthy
  encoder. `ConfiguredEncoder::passes_its_test()` in `test_i2c_encoders.cpp` reads first.

Worth knowing before picking the next one: **effort and value are anti-correlated here.** The
cheapest to make buildable — `extui/ui_api.cpp` — is the one that already gets the refresh right.
The ones with suspected defects (#33, #34) are the display drivers, which are the expensive ones,
and `mks_ui` needs a third-party graphics library.

So this is not a coverage gap and no amount of testing closes it. Making those drivers
host-portable is a real project and a separate one, and it would have to be sequenced *before*
this migration rather than inside it. Until then the fields stay public with the reason written
where they are declared, and register #33 records the two drivers already suspected of the
mistake the encapsulation exists to prevent.

**Migration in progress — slice 1 of the `planner.settings` correction is done.**
`Planner::steps_per_mm(axis)` and `Planner::set_steps_per_mm(axis, value)` now exist alongside
the public array, and `M92` uses them. The defect being corrected is a public mutable field with
a derived cache: `mm_per_step` is the reciprocal and the stepper counts in steps, so changing the
resolution invalidates both — and until now keeping them in step was the caller's job to
remember, with nothing connecting the array to `refresh_positioning()` but a comment. Forgetting
it does not fail; the machine keeps moving at a scale that no longer matches what it reports.

The array is still public because `settings.cpp` still writes it during a bulk restore, which is
a different pattern (set everything, finalise once) and a separate consumer. That is the
sequenced migration working as prescribed: new API alongside the old, old retired as consumers
arrive.

The refactor touched **three production files and no test file**, and the 24 acceptance scenarios
stayed green and unedited throughout — which is the only thing that makes it evidence.

**What the acceptance suite protects on its own (Step 7).** Measured with the unit tests
excluded, `pio run -t marlin_eeprom -e acceptance_native_coverage`:

| consumer of `planner.settings` | acceptance-only line coverage |
|---|---|
| `module/settings.cpp` | 90% |
| `gcode/config/M92.cpp` | 68% |
| `gcode/config/M200-M205.cpp` | 50% |
| `module/stepper.cpp` | 74% |
| `module/motion.cpp` | 57% |
| `gcode/calibrate/G28.cpp` | 91% |
| `module/planner.cpp` | 62% |

Those motion figures were 13%, 5%, 0% and 11% until `moving_the_tool.feature` was written, and
two separate things had to change to fix that. The obvious one was six scenarios. The other was
that **`acceptance_native_test` extended `env:linux_native_test`** — the LINUX HAL, where time is
the wall clock, so a scenario could not wait for the machine to arrive anywhere and motion was
not expressible at all. Everything else in this fork moved to the test HAL; the acceptance envs
were left behind, and the effect was a silent ceiling on what the acceptance suite was allowed to
be about. They now extend `testhal_*`, and `[acceptance_only]` pulls in `tests/support` because a
scenario that homes needs rails and switches to home against.

`keeping_its_settings.feature`, `moving_the_tool.feature` and their steps name no C++ symbol,
teardown included. That is what lets them stay unedited while `planner.settings` is migrated
underneath them — the unit tests cannot do that job, because 117 of their references name the
symbol being removed and a net that moves with the code is not a net.

**`motion.cpp` is closed at 57.8% raw / 75.3% killable** (204/353; 82 of 149 survivors are
equivalent), 76% line coverage. Reason categories, all checked: 24 preprocessor-erased because
this build has no probe, 23 outside their variable's reachable range (`axis_home_dir` is always
`-1`, and no `HOMING_BUMP_DIVISOR` entry is below 1), 14 where an early-return shortcut agrees
with the general formula it skips — `get_move_distance` returns `ABS(diff.z)` for a Z-only move
and `SQRT(sq(dx)+sq(dy)+sq(dz))` gives the same — 12 masked by the duplicated extrusion guard
(register #30), 5 zero-length moves filtered by `MIN_STEPS_PER_SEGMENT`, and 4 on
`final_approach`, which is read only inside a `HOMING_Z_WITH_PROBE` block. The remaining 67 are
spread across 44 lines with **no cluster larger than three**, which is the signal to stop.

**`planner.cpp` is closed at 60.5% raw / 73.2% killable** (612/1012; 176 of the 400 survivors
are equivalent). The reason categories, all checked rather than inferred: 76 masked by the
junction-deviation cap (register #29), 47 erased by the preprocessor (the fan guards and the
`TERN0(FTM_CONSTANT_JOLT, …)` at `:1777`, which never evaluates true here), 19 outside their
variable's reachable range — `:834`'s `accel` was measured across the whole suite and spans
120000..640000000, so every relational mutant of `!= 0` agrees with it — 15 direction pins for
axes that are not moving, 11 guards that only skip redundant work, and 8 at `:2432`, where the
integer and floating-point acceleration limits are two forms of one formula and **both arms
run** (209 and 40 times), so the split is a shortcut, not dead code. What is left has no cluster
larger than seven.

**"A source-level mutator edits text; check the text still means something different."**
`planner.cpp:1223-1226` sets eight fans with eight hand-written `TERN_(HAS_FANn, FAN_SET(n))`
pairs, and had 56 survivors. Thirty-eight of them are erased by the preprocessor:
`TERN_(HAS_FAN-1, ...)` expands `HAS_FAN` to `1`, and `ENABLED()`'s token paste matches
`_ISENA_1` before the `-1` is reached, so the guard is still enabled. `HAS_FAN(4+1)` goes the
same way. The tell was that every remaining survivor mutated only the *guard* and none touched
the `FAN_SET` argument. A throwaway `MARLIN_TEST` printing `TERN_(HAS_FAN-1, hits += 1)` settled
it in one build; reading the `ENABLED` machinery would not have.

**"Ask the build what it compiled."** In the same cluster I first classified 45 survivors as dead
arms for fans this build lacks. It has **eight** — `FAN_COUNT` is 8 and `HAS_FAN1` is 1 — so every
arm is live, and the hand-applied mutant that swapped `HAS_FAN0` for `HAS_FAN1` was not a mutant
at all. `every_fan_is_driven_at_its_own_speed` in `test_planner.cpp` is what the line actually
needed. Print the resolved constant; the default config is not the whole story.

**`tool_change.cpp` is at 97% line / 41.2% mutation (47/114) — measured against
`extruders_3_runout`, not the default config**, where it is not compiled at all. It read 0% for
a long time for exactly that reason. `gcode/control/T.cpp` went 0% → 75% in the same pass. Quote
the configuration with any figure for these two or it means nothing.

**"Two points and a radius determine four arcs, so assert the property, not the number."**
`G2_G3.cpp:434-444` solves for an arc centre from the two endpoints and `R`. The centre can sit
either side of the chord and the tool can take the short way or the long way; `G2`/`G3` picks the
direction and the *sign* of `R` picks minor/major. None of that is readable from outside, but the
centre must be exactly `R` from both ends, which fixes the bulge at `R - √(R² - half_chord²)` for
the minor arc and `R + √(...)` for the major — and the bulge is something the carriage physically
does. `test_arcs.cpp` measures it with `SimulatedAxisWithLimit::highest_reached()` /
`lowest_reached()`, added for this. Measured, not assumed: `G2` bulges toward −X here, and a
negative `R` bulges the **same** way as a positive one, much further — the centre crosses the
chord and the long way round comes back over the same side. The first version of that test
asserted the opposite side and was wrong.

**"A counter the system re-bases cannot measure the movement it re-bases."** A tool change calls
`sync_plan_position()`, which re-references the firmware's step counters to the shifted
coordinate *without the carriage moving* — then moves the carriage back by the same amount. So
`stepper.position(X_AXIS)` reads identical before and after, and an assertion built on it reports
that nothing moved. Measure the physical carriage with a `SimulatedAxisWithLimit`, which counts
pulses on the pin and is not re-referenced by anything. This is the second instance of the same
trap; `simulated_endstops.h` documents the first, which is why the bed surface is driven from the
simulated rails rather than from `stepper.position()`.

**"A survivor may mean the test exists but not in the build you measured."** `motion.cpp:2271`'s
`is_home_dir` had 21 survivors and the test that kills them was already written —
`homing_leaves_no_endstop_hit_outstanding` in `test_homing_the_machine.cpp`, whose whole file is
guarded on `HAS_BED_PROBE && ENABLED(Z_SAFE_HOMING)`. Neither is in the default config, and the
mutation runner only ever measures `--suite default`. Inverting `is_home_dir` passes all 525
default-config tests and fails immediately under `005-bed_leveling`. Before treating a homing,
probing or levelling survivor here as unasserted, grep the whole suite — the answer is often
that the behaviour is covered in a configuration the measurement cannot see.

The fix was to make the basic case testable in the measured build: the default config has plain
X/Y/Z minimum endstops and **had never homed anything but X** — `do_homing_move` was called ten
times in the whole suite, every one of them on axis 0. `homing_leaves_no_switch_recorded_as_hit`
and `each_axis_homes_against_its_own_switch` in `test_homing.cpp` (unguarded) now do. Note the
latter asserts `lowest_reached()` rather than the final position, because `Z_SAFE_HOMING` sends
the carriage to the middle of the bed after homing X and the final position is therefore
configuration-dependent; where the axis *went* is not.

The 12 that remain on that line are equivalent by reachable range: every axis homes to its
minimum so `axis_home_dir` is always `-1`, making `> 1`, `> -1`, `>= 0` and `== 0` all agree
with `> 0`; and `distance` only ever takes ±300, +5 and -10, so no mutant of the `> 0` on the
right-hand side separates either. A configuration with an axis homing to *maximum* would
distinguish the first group.

**"When the only thing a branch changes is the order, the sequence is the assertion."**
`motion.cpp:1001` and `:1010` raise Z before crossing the bed and lower it only after arriving,
so the nozzle passes over a printed part rather than through it. Both orderings end at identical
coordinates, so nothing about the final position separates them.
`Marlin/tests/support/step_order.h` records the first and last rising edge per axis step pin,
which makes the assertion `StepOrder::finished_before(order.z, order.x)` — a relation between
two spans, not a timestamp. Note it reports false for an axis that never moved, so that an
ordering assertion cannot pass against a machine standing still.

**"A negative assertion is satisfied by every cause of nothing."** `do_z_clearance` returns
early when `zdest == position.z`, and `asking_for_the_height_it_is_already_at_moves_nothing`
passes with that guard deleted: without it the machine issues a zero-length move, which
`MIN_STEPS_PER_SEGMENT` (6) discards before it becomes a block, so no steps are taken either
way. All five survivors left on `:1001`, `:1010` and `:1091` are this — every one widens a
guard to include the equal case, and the equal case is a zero-length move that gets filtered
downstream. The test is kept and its docstring says what it does not test.

**"A guard that only avoids redundant work has no wrong answer."** The planner's forward pass is
gated twice — `planner.cpp:1098` skips a block whose predecessor was not accelerating, and
`:1167` skips one already at its optimised speed. Both are pure optimisations: when the guard is
false the kernel's own `new_exit_speed_sqr < entry_speed_sqr` cannot hold, so running it anyway
changes nothing. All nine remaining survivors on those two lines widen the guard (`<=`, `!=`,
`>=`, deleted, `(1==1)`), and the same goes for the `NOLESS` at `:1106` and the write at `:1109`
that `:1113` overwrites a line later. The three that *narrow* it were killed by
`a_move_cannot_enter_faster_than_the_run_up_allows`.

**"Do not state a precondition in terms of a value the code under test may have rewritten."**
That test first said "this run-up is only interesting if it falls short of
`second.max_entry_speed_sqr`" — and `planner.cpp:1109` writes the corrected speed back into
`max_entry_speed_sqr` precisely so a second pass will not redo the work, so the precondition
could never hold. It reads `sq(second.nominal_speed)` now. Also worth knowing: the first block of
a plan enters at `minimum_planner_speed_sqr` (18.75 here), not zero, so the assertion is
`v² = u² + 2as` with the block's own `entry_speed_sqr` for `u²` — `2as` alone is out by that much.

**"When a value is folded over a collection, move the deciding element away from the end."**
`planner.cpp` scales a whole move down until its worst axis is at its own feedrate limit, taking
the minimum over `LOOP_NUM_AXES`. Z is both the slowest axis on a cartesian machine and the last
one checked, so every natural test has the binding axis last, and replacing the `NOMORE` with a
plain assignment passes all of them. `the_order_the_axes_are_checked_in_does_not_decide` in
`test_planner.cpp` gives X the tighter limit instead; it is the only one of the five that fails.
Note `SimulatedMachine` flattens every `NUM_AXES` feedrate to 300 and acceleration to 3000, so a
test about per-axis limits has to state them itself.

**"Equivalence is often a property of the type."** `esteps` is a `uint32_t`, so `esteps >= 0`,
`esteps != 0` and `(1==1)` are all equivalent to `esteps > 0` on sight; `> 1` differs only for a
single 1/500 mm step, which no assertion can separate from zero. That plus the two `ANY()`
argument reorderings and the `IS_CORE` arm this build compiles out accounts for all 8 survivors
left on that line.

**"Pin the build configuration."** Here that is `restore_configs`; see the gotchas below.

**What validating the agents taught (2026-08-11).** The split skill and its two agents were
tested before being trusted, control first: `harness-validator` was run once against the
correct apparatus and once with `.pio/build/testhal_native_coverage` moved aside — the
documented fault where the runner warns and mutates every line. It returned `TRUSTWORTHY`
and `NOT TRUSTWORTHY` respectively, named the missing coverage build as the cause, and
refused to compare its unrestricted 16.8% against the restricted 71.9%. Its numbers on the
good run reproduced exactly when re-run here (41/16/50 of 528, 21 covered lines).

Two things it got wrong are worth more than the passes:

- **A false equivalence inside the evidence for a pass.** It certified the harness partly on
  a "control" pair of mutants it called equivalent because `target_extruder` is "always 0".
  It is not: `get_target_extruder_from_command()` returns **-1** for a `T` the build does not
  have (`gcode.cpp:138`), so `> 0` is killable and only `!= 0` is genuinely equivalent. No
  existing test took that path, which is why it looked unreachable. The next agent killed it.
- **"The suite passed" meant one config of eight.** `mutant-killer` measured against
  `001-default` as instructed and reported green truthfully; its new test used the literal
  `T1`, which is out of range with `EXTRUDERS` 1 and an ordinary request with `EXTRUDERS` 3.
  It failed immediately under `003-extruders_3_runout`. Fixed by deriving the index from
  `EXTRUDERS` itself, so the test states "the first extruder this build does not have".

Both agent definitions now carry the corresponding rule. The pattern behind both: **an agent
is most dangerous where it is most confident**, and both errors were in claims nothing
downstream would normally re-check.

**Delegating to subagents in this repo.** One agent per step of the skill:
`rescue-surveyor` (0-2), `harness-validator` (3), `mutant-killer` (4-5) and
`acceptance-author` (6-7), plus `hal-debugger` for escalation. The first four are
stack-neutral and lift with the skill; `hal-debugger` is specific to this firmware's HAL.
Step 8 is deliberately not delegated.

`rescue-surveyor` exists because of this fork's most repeated mistake: a file reading 0%
because nothing compiles it looks exactly like a file nobody tested. `tool_change.cpp`,
`temperature.cpp` and `settings.cpp` each cost a wrong claim before that was understood,
so the surveyor must classify a low figure as untested / not-compiled / not-host-buildable
/ not-linked, and say whether it measured or inferred it.

`acceptance-author` grep-checks its own scenarios for implementation symbols rather than
asserting they are clean — the property that makes `keeping_its_settings.feature` and
`moving_the_tool.feature` able to hold still through the `planner.settings` migration,
and one an agent can violate while believing it has not. Each states a **return contract** — an agent that hands back its raw
output has saved no context, only relocated it, so the contract is the point rather than
the prose. Give any agent that builds its own
worktree — all PlatformIO environments share `.pio/build/<env>`, so two agents building
concurrently overwrite each other's binary and interleave `restore_configs`, and an agent
measuring a flake over hundreds of runs will silently measure someone else's build.

Worktrees here have been created from `d58bae7e9a` (upstream, Sept 2025) rather than from
the branch under test, which has none of `Marlin/src/HAL/TEST`, `Marlin/tests/support`,
or the module tests. Tell the agent the commit its work builds on and give it the exact
baseline test counts, so a mismatch shows up as "the tree is wrong" instead of as a
mysterious build failure. Both agents that hit this reset the worktree branch themselves
and reported it.

**Test counts as of `unit-test-coverage`:** `testhal_native_test` 553,
`acceptance_native_test` 18 — each measured with `pio run -t marlin_default -e <env>`, i.e.
against the **default config only**.

Say which of those two axes you mean whenever you quote a count. `make unit-test-all-local`
varies the *config* and holds the env fixed: it runs `testhal_native_test` against all
**eight** configs in `test/`, reporting **553, 554, 561, 618, 624, 562, 559, 558**. The counts above vary
the *env* and hold the config fixed. Give an agent a bare number as a baseline without saying
which, and a correct tree reports a mismatch.

The sixth config, `006-eeprom.ini`, exists because `EEPROM_SETTINGS` is off everywhere else, so
`M500`/`M501` are not compiled and `settings.cpp` showed 51 covered lines instead of 298. A file
that is only reachable under an optional feature is a file whose common path nobody is checking;
adding the configuration was cheaper than any test.

## Which HAL the tests run against

**The test HAL is the working loop. The LINUX HAL is an integration check.**

`testhal_native_test` is a strict superset — verified by comparing the test names the two
binaries actually register, and no test exists under LINUX that does not exist here. It is
also the only env where time advances on request, so motion, blocking commands and the
interrupt handlers are reachable at all; every coverage and mutation figure in `docs/`
comes from it.

The LINUX HAL backs its peripherals with real OS facilities — wall-clock sleeps, POSIX
timers, signals — which is why it is slower and why every instrument defect found so far
(register #16, #18, #20) lived there rather than in the firmware. It still earns its place
as the only thing exercising that HAL, and #20 was a genuine order-dependence it caught
that the test HAL could not. Run it deliberately, with `make unit-test-integration`, not
on every change.

## Test, coverage, and mutation tooling

```bash
make unit-test-all-local                    # test HAL, all 3 configs in test/
make unit-test-integration                  # LINUX HAL, all 3 configs — the slow check
make unit-test-coverage                     # test HAL + gcov/gcovr report
make unit-test-mutation TARGET=<file.cpp>   # mutation-test one source file, test HAL
pio run -t marlin_default -e acceptance_native_test        # acceptance suite alone
pio run -t marlin_default -e acceptance_native_coverage    # ... with coverage
```

**Everything defaults to the test HAL now.** `UNIT_TEST_ENV`, `COVERAGE_ENV` and
`MUTATION_ENV` all name `testhal_*`, so the bare commands above measure the suite the
documents quote. Nothing needs saying twice any more:

```bash
make unit-test-all-local                                  # test HAL, all three configs
make unit-test-coverage                                   # test HAL + gcov
make unit-test-mutation TARGET=<file.cpp>                 # test HAL
make unit-test-mutation TARGET=<file.cpp> RERUN=.pio/mutation/results.json
make unit-test-integration                                # the LINUX HAL, run deliberately
```

**Coverage and mutation must still name the same suite** whenever either is overridden —
mutants are restricted to the lines the coverage build marked covered, so a mismatched
pair measures one suite against another's reach. `MUTATION_ENV` must also be repeated on
a `RERUN=`. Getting it wrong is not loud: a self-consistent pair produces a perfectly
plausible report, just of a different suite, and under LINUX most of the interesting
lines never execute — so *unasserted* and *unreachable* become indistinguishable, which
is the one distinction this whole exercise exists to make.

`make unit-test-coverage` prints both a whole-tree number and the **platform-agnostic**
one (excluding `Marlin/src/HAL/`), which is the figure the plan documents quote — 74.3%
against 73.8% for the same build, so quoting the wrong one looks like a small regression.

`buildroot/share/scripts/mutation_test.py` drives the compiler and linker directly
rather than invoking `platformio test` per mutant, and runs mutants in parallel: about
75 seconds for a target that previously took 20 minutes. Useful options:

- `MUTATION_ENV=acceptance_native_test` — measure the acceptance suite on its own
- `RERUN=.pio/mutation/results.json` — re-run only the previous survivors (~10s)
- `MUTATION_JOBS=N` — worker count, defaults to cores minus one

Run `make unit-test-coverage` first: mutants are restricted to gcov-covered lines, and
without a coverage build every line is mutated, which is slower and reports survivors on
lines no test can reach. The runner derives the coverage build from the env name
(`_test` → `_coverage`), so `MUTATION_ENV=testhal_native_test` looks for
`.pio/build/testhal_native_coverage`; if that build is absent it **warns and mutates
every line anyway** rather than stopping. Read the warning.

Environments added by this fork, in `ini/native.ini`:

| Env | Purpose |
|---|---|
| `linux_native_coverage` | `linux_native_test` + gcov instrumentation (integration only) |
| `acceptance_native_test` | acceptance suite only, unit tests excluded |
| `acceptance_native_coverage` | the same, with coverage |
| `testhal_native_test` | **the default suite** — unit tests against `HAL/TEST`, time advances only on request |
| `testhal_native_coverage` | the same, with coverage; the only env that measures motion and blocking commands |

Configurations in `test/`:

| Config | Why it exists |
|---|---|
| `001-default` | the baseline every figure is quoted against unless another is named |
| `002-extruders_1_runout`, `003-extruders_3_runout` | filament runout; 003 is the only build with `tool_change.cpp` in it |
| `004-sd_powerloss` | media and power-loss recovery |
| `005-bed_leveling` | probing, `Z_SAFE_HOMING`, and the whole of `test_homing_the_machine.cpp` |
| `006-eeprom` | `EEPROM_SETTINGS`, so `M500`/`M501` and most of `settings.cpp` are compiled at all |
| `007-i2c_encoders` | `I2C_POSITION_ENCODERS`, the first consumer of `planner.settings` from outside the build to be made buildable |
| `008-extui` | `EXTENSIBLE_UI`, which links only against a concrete display — `tests/support/stub_extui.cpp` is that display, and it records rather than discards |

`gcovr` is required for coverage reports (`uv tool install gcovr` — `pip install --user`
is blocked by PEP 668 on this machine).

### Gotchas that have cost real time

- **A cold nozzle makes the *planner* drop the E part of a move, silently.**
  `planner.cpp:1850` — `tooColdToExtrude()` sets `position.e = target.e` and zeroes
  `steps_dist.e`, so the firmware's idea of the filament position advances as though the
  extrusion happened. A test asserting on E steps then sees a machine that never moved, with no
  complaint on any channel and `buffer_line()` still returning true. This is separate from the
  guard in `prepare_line_to_destination()`, so calling `buffer_line` directly does *not* avoid
  it. Whether it bites depends on `thermalManager.allow_cold_extrude`, which is machine state
  another test's `M302` may have left either way — which is exactly how the E-direction tests
  passed in the default configuration and failed under bed levelling. Any test about E must say
  which side of that guard it wants: see `PlainExtrusion` in `test_simulated_motion.cpp`.
  Related: `MIN_STEPS_PER_SEGMENT` is 6, so a move of one E step is dropped before it becomes a
  block at all and has to ride along with a travel long enough to survive.
- **`planner.buffer_line()` returns true and queues nothing while the machine is not
  running.** `marlin.state` is `MF_RUNNING` only because `SimulatedMachine` sets it, so a test
  that plans moves without that fixture gets `true` from every `buffer_line` and
  `movesplanned() == 0`. Nothing reports it. The tell is a queue-related assertion failing in a
  way that makes no sense — a block never delivered, a buffer never filling — and the first
  thing to print is `movesplanned()`.
- **Never `git add -A` after a coverage or mutation run.** Both rewrite
  `Marlin/Configuration.h`, `Configuration_adv.h` and `config.ini` for the suite they measure
  and leave them rewritten. Cleaning them before the *test* run is not enough if a measurement
  runs afterwards — this has now committed a generated bed-levelling config twice. The tell is
  every configuration reporting the same test count as the one last measured, because
  `restore_configs` restores to whatever is checked in. Re-run `git checkout --` on those four
  paths immediately before `git add`.
- **`restore_configs` reverts your config.** Every test target runs it before and after,
  which does `git checkout` on `Marlin/Configuration.h`, `Configuration_adv.h`,
  `Marlin/config.ini`, and `Marlin/src/pins/*/pins_*.h`. Uncommitted config work is
  discarded silently, and a mutation run started afterward will fail to build every
  mutant. Re-apply the suite's config before running anything directly:
  ```bash
  cp -f test/001-default.ini Marlin/config.ini
  python3 ./buildroot/share/PlatformIO/scripts/configuration.py
  ```
- **PlatformIO prints a summary line even when the build ERRORs.** `1 test cases: 0
  succeeded` does not mean a test ran. Classify build failure on `ERRORED`/`error:`, or
  every non-compiling mutant scores as killed.
- **The unit test binary exits 0 even when assertions fail.** Marlin's Unity `main` does
  not propagate failures into the exit status; PlatformIO decides pass/fail by parsing
  `N Tests M Failures K Ignored`. Anything judging the binary by its exit code alone
  will call every failing mutant a survivor.
- **`pio run -t compiledb` must come before the test build.** `preflight-checks.py`
  deletes `M115.o` and `Warnings.o` on every build to refresh their timestamps; running
  compiledb afterwards leaves them missing and the link fails on undefined references.
- **Serial output hangs the unit test binary.** `HAL/LINUX/include/serial.h` implements
  `write()` as `while (!transmit_buffer.free());` over a 128-byte buffer. The simulator
  drains that buffer from its UI; the test binary has nothing draining it, so the first
  report that overflows it spins forever — the symptom is a test that passes and then
  the run never finishes. Any test that dispatches a G-code command must mark the port
  as having no host attached for the duration (`MYSERIAL1.host_connected = false`), which
  makes `write()` return immediately. See the `NoHostAttached` helper in
  `Marlin/tests/gcode/test_gcode_commands.cpp`. Draining afterwards does not work: the
  spin happens part-way through a single report.
- **Media is faked at the block device, not at the wire.** Neither native HAL defined the
  `spi*` functions, so a configuration with media would not link — `Marlin/src/HAL/TEST/spi.cpp`
  now provides them as an idle bus (reads return `0xFF`, writes discard), which is what a
  controller sees with an empty slot. Tests do not use that path: `DiskIODriver` is a pure
  virtual interface and `CardReader::changeMedia()` is public, so a fake block device goes
  in at the level the firmware already abstracts. Simulating a card over SPI would mean
  implementing SD's command protocol to test code sitting well above it.
- **Commands that wait for hardware need a stand-in sensor.** `M109`/`M190` loop until a
  temperature is reached and nothing advances a heater here, so a non-zero target would
  never return. No production seam was needed: `thermalManager.temp_hotend`/`temp_bed`
  are public and the ADC pipeline that would overwrite them is dormant in this build
  (it only refreshes when the temperature ISR has produced a full sample set, and that
  ISR does not run). Tests say what the sensor reads via
  `Marlin/tests/gcode/simulated_sensors.h`. Note `thermalManager.init()` crashes with
  SIGFPE in this build — do not call it. The same caution applies to anything calling
  `planner.synchronize()` with queued moves.
- **Simulated pins power up in a state no board is ever in.** Every `Gpio` pin reads LOW
  at reset. On a board `KILL_PIN` has a pull-up and reads HIGH — released — so the
  firmware in a test build sees the kill button held from the first instruction,
  debounces it over 250 passes of `manage_inactivity()`, and the 250th `marlin.idle()`
  call reaches `kill()`, which spins forever waiting for a release. Only commands that
  wait call `idle()` that many times, so this presents as "blocking commands hang" and
  looks convincingly like a timer bug. `Marlin::setup()` configures those pull-ups and
  does not run in a test build, so the fixture stands in for it
  (`SimulatedMachine::release_kill_button()`). Expect other pins with the same problem.
- **Object link order used to matter — fixed, and the suite is now checked against it.**
  Three independent order dependencies made the binary segfault or hang depending on how
  its objects were linked (register #20): a test registry and a clock baseline that were
  both statics with dynamic initialisers, and — the one that caused the hangs — a
  simulated timer left armed by whichever test woke the stepper, whose signal then
  starved every later `sleep_for()`. All three are fixed, `link_inputs()` sorts so runs
  are reproducible, and the suite is verified under sorted, reversed, filesystem and four
  shuffled orders.
- **A test must leave the simulated peripherals quiet.** Under the LINUX HAL the timers
  are POSIX timers delivering real signals, so one left running is not merely untidy — it
  interrupts every blocking call in the process from then on. The framework now calls
  `HAL_timer_stop_all()` after each test; `HAL_timer_disable_interrupt()` is *not*
  sufficient, because it only masks the signal and leaves the timer armed.
- **Mull is not viable here.** Its IR plugin forces clang across the whole build, which
  needs `-stdlib=libc++`, which then rejects Marlin's own `types.h` and `temperature.h`.
  Use a source-level mutator (`universalmutator`) that builds with the project's own
  toolchain.
- **Mutation runs used to take ~20 minutes per target** when each mutant went through
  `platformio test`. With `mutation_test.py` a full target is ~75 seconds and a survivor
  re-run ~10 seconds, so authoring tests is now the slower half again.
- **A mutation run writes one full copy of the target per mutant.** `temperature.cpp` is
  ~31k mutants and about 6 GB, kept after the run because `RERUN=` reads them back — and
  it is per worktree, so three concurrent runs filled this machine's root volume. Set
  `MUTATION_MUTANT_DIR=/mnt/md0/marlin-mutants/<target>` to put them on the array
  instead; a `RERUN` must name the same directory as the run that produced its results.
- **Do not take a mutation measurement while another agent is running one.** Timeouts
  count as detected and the threshold is wall-clock, so a loaded machine turns surviving
  mutants into false kills. Proven here by building a suspect mutant in directly: it
  completes in seconds and survives, but scored TIMEOUT under a parallel run. The runner
  now derives the timeout from the baseline it measures at startup and records both in
  the results JSON — but the fix does not make concurrent runs comparable, it only makes
  the threshold visible. Publish the number from a run taken alone.
- **`/tmp/mut_*` is `mutation_test.py`'s per-worker scratch.** An agent clearing it to
  recover disk space will delete another run's in-flight compiles, which surface as
  BUILD_FAIL in code that is fine. One agent did exactly this here.
- **`preflight-checks.py` gates env/board compatibility.** Envs whose names end in
  `_native_test` or `_native_coverage` are exempt, because the test targets rewrite the
  board per suite. New measurement envs should follow that naming.
