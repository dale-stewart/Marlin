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

**"Compare against the values the system defines, never against another of its own readings."**
Two tests written in one session here each killed nothing, and neither was noticed until the
fault was injected by hand. `each_switch_is_reported_from_its_own_pin` closed each endstop in
turn and asserted that its report differed from the same switch's *open* report;
`M119_reports_each_switch_as_it_actually_is` did the same through the carriage. Both are
satisfied by a comparison inverted for every switch, because that inverts both readings. The
mutation score did not move across three runs, and the tell was exactly that — a new test
that changes nothing has usually asserted nothing. They now compare against `STR_ENDSTOP_HIT`
and `STR_ENDSTOP_OPEN`, the words the firmware publishes, and the same injection fails them.
The differential version was the more elegant code, which is why it survived review.

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

**`acceptance-author` validated by a refactor drill (2026-08-11).** A grep proves the
scenarios do not *name* the implementation; it cannot prove they do not *depend* on it. So
the check was empirical: it wrote `timing_the_job.feature` for the print job timer, and then
`Stopwatch` → `ElapsedClock` and `print_job_timer` → `job_elapsed_clock` were renamed across
**41 production files**, with the feature file and step definitions untouched.

The acceptance suite built and passed, 34/34. The unit tests did not compile —
`test_stopwatch.cpp`, `test_thermal_limits.cpp`, `test_media_commands.cpp` and
`test_powerloss.cpp` all failed on the renamed symbols. That contrast is the whole argument
for Steps 6-7 in one run: the same rename that a net must survive is the one that edits every
unit test naming it, and a net that moves with the code is not a net.

What made it work is visible in the steps — every one goes through `the_host_sends("M75")`
and `the_reply_to("M31")`. The only non-G-code call is the test HAL's clock, which is the
harness rather than the target, and renaming the target could not reach it. The four
`duration` hits its own grep reported are the English word, in prose and assertion messages.
Acceptance-only coverage verified rather than relayed: `M31.cpp` 100%, `M75-M78.cpp` 100%,
`stopwatch.cpp` 83%, all 0% before.

The drill is the reusable part. `acceptance_native_test` compiles exactly one test source
besides the framework and `tests/support`, so renaming a target's public surface in
`Marlin/src` and rebuilding *that env alone* is a cheap, honest test of whether a scenario
suite is really at the boundary — and `git checkout -- Marlin/src` puts it back.

**`rescue-surveyor` validated against `Marlin/src/feature/` (2026-08-11).** Chosen because
the directory is the four-way classification in concentrated form: 41 of 42 `.cpp` files sit
at 0% and *none* of them wants tests written. It led with the denominator rather than the
percentage — 95 of ~17,785 countable lines are compiled under `001-default`, so the honest
figure for the directory is ~0.07%, not the 13.7% that a coverage report shows — found
`host_actions.cpp` as the one genuine gap, and separated *not compiled in the baseline* from
**not compiled in any of the eight configurations** (mmu, mmu3, leds, resonance, password,
digipot, dac, tmc_util — about 10,800 lines dark everywhere). That last distinction was not
in the answer key and is the more useful one.

It also flagged that `test_runout.o` links under `001-default` while `feature/runout.o` does
not. Benign — `test_runout.cpp` is guarded on `FILAMENT_RUNOUT_SENSOR` and compiles to an
empty translation unit — and it reported the observation without claiming a defect, which is
the right handling of an unresolved lead. Its one real error was a structural count given
approximately (94 files; there are 85), now covered by a rule in its definition.

**First target rescued end to end with the new structure: `gcode.cpp` (2026-08-11).**
Surveyor picked it over better-covered candidates because it is the heaviest direct reader of
the parser globals among compiled files — 11 raw hits, the consumer the blocked `GCodeParser`
correction needs first. Line coverage was the least useful number available: 63% suggested
thin neglect, while the validated mutation baseline was **72.1% raw / 74.5% killable**, with
29% of the killable survivors inside one function.

Now **93.2% raw / 97.4% killable** (261/268), from 17 tests. `host_keepalive()` was the
cluster and it needed *an input*, not assertions — simulated time, plus busy and paused as
independent guard terms. What is left is 12 equivalents and 7 genuinely blocked, and the
blocked ones are worth knowing: `report_heading`'s `if (fstr)` false branch needs a null
`FSTR_P` that would segfault natively, and G2/G3 arc dispatch fills the planner buffer and
hangs. **`gcode.cpp` is now 93.8% raw / 97.8% killable over 119 covered lines** — not comparable
to the earlier 93.2%, which was measured over 110.

**Correction: `dwell()` was reported blocked here and is not.** The claim was that its
busy-wait never returns because `millis()` does not advance inside the loop — stated as
"verified by reading the loop, not attempted", and relayed by me into two commit messages
before anyone tried it. Under the test HAL `marlin.idle()` costs simulated time, which is the
whole reason that HAL exists, and `G4_P_dwells_for_milliseconds` in
`test_blocking_commands.cpp` has been passing all along. A blocked classification asserted
from reading is worth exactly as much as an equivalence asserted from reading, which is to say
it needs the same probe.

**Two verdicts fell to one probe.** `KEEPALIVE_STATE(IN_HANDLER)` at `:330` had also been
called observable only mid-call. A long `G4` is a handler holding the machine busy while
`idle()` advances the clock, so `a_long_dwell_reports_busy_to_the_host` drives the real
dispatch path and dies when the busy mark is deleted. One wrong sentence about `millis()` had
been hiding a reachable region, and it hid it in two places.

**The `GCodeParser` correction now has a net, and the net was tested (2026-08-11).**
Steps 6-7 for `gcode.cpp` were not bookkeeping: **239 references across 22 test files name the
`parser.` surface**, twice the `planner.settings` figure, so the unit tests would be edited by
the very refactor they are meant to protect. The acceptance step file names it **zero** times.

Proven rather than argued, by the same drill used on the print job timer: `codenum` →
`command_number`, `string_arg` → `command_text`, `codebits` → `seen_bits` across **20
production files**, feature files and steps untouched. The acceptance suite built and passed
37/37; the unit-test build failed to compile. That is the migration's safety net demonstrated
against the migration itself.

Acceptance-only coverage of `gcode.cpp` is 47% (was 43%), `parser.cpp` 79%, `queue.cpp` 58%.

Two spans are **unreachable from outside** and are not coverage gaps: `host_keepalive()`'s
reporting branches and `process_subcommands_now()`. Both need a handler that loops on
`marlin.idle()` past the keepalive interval while simulated time advances — the same state
`dwell()` needs and cannot get, because nothing drives the virtual clock while a handler spins
on `idle()`. That is a production seam, and it is the one thing standing between the acceptance
suite and the dispatcher's reporting paths.

**`queue.cpp` rescued (2026-08-11): 60.6% -> 77.2% raw, ~87% killable, 68% -> 76% line.**
The M112 cluster — 28 mutants at `queue.cpp:541` — is the taxonomy's third category, code with
no observable outcome: `Marlin::minkill()` ends in `for (;;) hal.watchdog_refresh()` and never
returns, so no assertion can run after it. Verified by reading `MarlinCore.cpp`, not assumed.
A real printer does stop dead on M112, so the substitute halting is accurate rather than
defective, and the cluster is a blocked seam rather than a gap.

**The over-long-line branch was defect #25 and no longer hangs (2026-08-12).** Nine shapes,
both HALs, all complete — see the register for what was tried and what remains unestablished
about why. It is now asserted from the narrowing side only, and the reason is the useful part:
the obvious companion assertion — a parameter one character *past* the cut, expected to be
lost — passes with the guard deleted, because without it the reader writes past the end of the
buffer rather than reading more of the line. Widening mutants there are undefined behaviour,
not different behaviour. Also worth knowing before writing a test like it: a test cannot
transmit more than the port's 128-byte receive buffer in one go, and the newline goes over the
side with everything else, so the symptom is no command at all — which looks exactly like a
line the firmware refused.

Two counting traps caught here, both worth remembering:

- **The agent reported 131 survivors killed; the true figure is 79.** It had subtracted one full
  run's kill count from another's (433 - 302) — but the new tests raised covered lines from 120
  to 135, so the second run had a larger mutant population, including new mutants that were born
  dead. Re-running the *previous run's survivor list* is the only way to count survivors killed,
  because that population is fixed. Its headline 77.2% was correct; the work-done figure was
  inflated by 65%.
- **A measurement taken straight after `make unit-test-all-local` is unrestricted and wrong** —
  see the gotcha above. This bit me in this very session, one hour after using the same fault
  deliberately to test `harness-validator`.

**`temperature.cpp` — real baseline, and slice one (2026-08-13).** The measurement on disk was
ten days stale and would have been quoted wrong. Fresh, taken alone:

| | |
|---|---|
| line coverage | 82% over 411 covered lines |
| raw mutation | **62.6%** (907/1450) |
| ...killed by an assertion | **441** |
| ...counted as detected by *timeout* | **466** |
| survived | 543 |

**Quote the killed-by-assertion figure for this file, not the raw one.** More than half the
"detected" total is timeouts, which the runner counts as kills — so the honest headline is
**30.4% killed by assertion**, and the raw 62.6% flatters it by a factor of two. The timeouts are
real rather than a loaded machine (this was run alone): a great many mutants here turn a bounded
wait into an unbounded one.

The 543 split cleanly, which is what makes this file sliceable: PID autotune 142, the `M109`/`M190`
waits 134, the MINTEMP/MAXTEMP checks 63 (register #19, blocked), the runaway state machine 30, and
174 spread thin.

**Slice one: the thermal runaway watchdog, which had never been tested at all.** 29 of the 30
survivors in that region are dead and nothing outside it moved — every kill is on 3423-3538.
`test_thermal_runaway.cpp`, 13 tests.

It had no tests because `tr_state_machine_t` was **private**. It is a value type with a pure
`run()` — current, target, heater, period and hysteresis all arrive as arguments and it touches
nothing but its own three fields — so a test can hold its own instance and drive it with no heater,
no sensor and no control loop. Making the type public is additive and moves no caller; the real
correction, moving it to namespace scope as a watchdog of its own, is recorded as blocked #50.

Three things worth carrying:

- **Asserting the state was not enough, and the mutation run is what said so.** The state does not
  change until the trip, and the trip is unreachable — `_TEMP_ERROR` ends in `kill()`. So every
  boundary test read `TRStable` on *both* sides, and replacing `running_temp - current` with
  `running_temp / current` survived all of them. That is not a subtle mutant: it turns a 50-degree
  shortfall into 1.33 and reports a runaway as perfectly healthy. What separates them is `timer`,
  the deadline the decision is carried in — pushed back on every pass inside the hysteresis, left
  standing outside it. **Where the outcome is blocked, assert the state the outcome will be
  computed from.** Plus one test that the deadline is exactly one configured period away, since
  "it moved" is satisfied by any amount at all, including an hour.
- **The tests found #49 by being the first non-static instance.** `running_temp` is the one member
  with no default initialiser. The firmware's instances are a zeroed static array, so it cannot
  bite there — but the arming test is `running_temp != target`, so an instance whose stack garbage
  happens to equal the target never arms, and the heater is watched by nothing, silently. Found
  because `two_heaters_are_watched_independently` failed against garbage that happened to be
  200.0f. Recorded rather than fixed; `fresh()` zeroes it and says why. Note the shape: **using a
  type the way its blocked correction would use it is itself a probe.**
- The two survivors left are equivalent with reasons. `else if` -> `if` at 3473, where the
  preceding branch is `TERN0(HEATER_IDLE_HANDLER, ...)` and `HEATER_IDLE_HANDLER` is undefined here
  (it needs `ADVANCED_PAUSE_FEATURE` or `PROBING_HEATERS_OFF`, neither set). And `target > 0` ->
  `!= 0`, which differs only for a negative target, which `setTargetHotend` cannot produce.

**The 63-survivor MINTEMP/MAXTEMP cluster has a way through that needs no production change.**
Register #19 asks for "a seam that lets the shutdown be observed and returned from under test".
`BOGUS_TEMPERATURE_GRACE_PERIOD` is exactly that and already exists: with it set, the first temp
errors record an expiry and **return** instead of calling `loud_kill`, so the reporting path
becomes assertable. It would be a configuration whose safety kill is deferred, which is a
deliberate choice rather than a free one — not taken yet.

**`013-bogus_temp_grace` partly unblocked register #19 (2026-08-13), and the seam was already in
the firmware.** `_temp_error()` ends in `kill()`, which does not return, so the MINTEMP/MAXTEMP
checks could be *reached* by a test but never asserted after — 63 survivors, recorded as blocked
since the register was written. `BOGUS_TEMPERATURE_GRACE_PERIOD` is Marlin's own answer to a
related problem (sensors are unreliable just after power-on) and within it a temperature error
disables the heaters and **returns**. That is exactly the seam #19 asks for. No production change,
nothing stubbed, one line of configuration.

The rule is in `survivor-taxonomy.md` now: **before recording a seam as needing new production
code, check whether the code already has one behind a build option.** Software that halts on a
fault usually has a mode where it does not, because its authors needed the same escape.

`test_temperature_errors.cpp`, 9 tests. What they assert is the safety property rather than a
message: a sensor that has stopped making sense is *detected* and every heater is *switched off* —
hotend and bed, both directions, target cleared as well as pin, and still off several control
passes later. A bed fault takes the hotend with it, because `_temp_error()` calls
`disable_all_heaters()` rather than disabling the one that failed.

**What the configuration actually bought, stated honestly.** Killed-by-assertion went **441 -> 642**
and timeouts fell **466 -> 348**; raw is 62.6% -> **65.5%** (990/1512) over a covered set that grew
411 -> 440, so the raw figures are not comparable and the kill count is. Most of that +201 is
*timeouts becoming assertion kills* — the same mutants detected properly instead of by hanging —
rather than new behaviour covered. That is worth having on its own: a suite whose detections are
hangs is slow and its score is soft. (The last 18 of those kills come from a survivor re-run after
two more bed tests were added, over the same 1512-mutant population.)

**And the min/max cluster only fell 63 -> 42, which turned out to be the more useful finding.**
The MAXTEMP checks are **masked**, not merely blocked: `updateTemperaturesFromRawValues()` compares
the raw ADC value against `raw_max` and `manage_hotends()`/`manage_heated_bed()` compare the degrees
against `maxtemp`/`BED_MAXTEMP`, and since `raw_max` is derived in `init()` by walking until
`analog_to_celsius(raw_max) <= tmax`, the two are the same predicate either side of the same
monotonic conversion. Any input reaching one reaches the other, so no assertion can separate them.
Register #51 — 24 of the 42 that remain are exactly those. The MINTEMP checks have no such twin,
which is why those *did* fall to tests: the bed's alone went 14 -> 5 once a test drove the bed cold
with its heater on.

**Two things this file said about the temperature fixture were wrong**, and both cost a wrong first
draft — see the gotcha, which is corrected. `thermalManager.init()` does **not** SIGFPE and
`SimulatedHardware::ensure_ready()` has been calling it all along, which is what narrows
`temp_range[]` to the configured limits and makes them brackettable. And the ADC pipeline is not
dormant; what is true is that **a changed reading takes ~300 ms of simulated time to arrive**,
because the ADC is oversampled 16 times. Asserting 50 ms after changing a sensor sees the old value,
which looks exactly like a fault that went undetected — four tests failed that way first.

One behaviour found and pinned rather than changed: **the cold limit sits one representable reading
above the configured minimum.** A nozzle reading exactly 5.00 C is shut down and 6.00 C is not,
because `init()` walks `raw_min` down in steps of `OVERSAMPLENR` from a value that is not a multiple
of it, so the boundary never lands on a reading the ADC can produce. Harmless — 5 C is below any
room a printer lives in — and now bracketed to a single count in both directions.

**`M206_M428.cpp` rescued (2026-08-13), and it took a configuration rather than a test:
40% -> 95% line and 57.4% raw under the default config, 100% line and 87.3% raw / 100%
killable under `012-max_endstops`.**

The gap was the whole of `M428` — the command that says "the spot I am at now is home".
The 29 survivors it left were all on one line, and classifying them is the useful part,
because the obvious diagnosis was wrong twice over:

    if (!WITHIN(diff[i], -20, 20) && motion.home_dir((AxisEnum)i) > 0)

**Two independent reasons, and fixing the first alone buys nothing.** Every axis here homed
to its minimum, so `home_dir` was always -1 and the branch was dead — that much is the usual
story. But `base_home_pos` was therefore always **0**, which makes the correction
`diff[i] = -position[i]` textually different from and numerically identical to the general
`diff[i] = base_home_pos(i) - position[i]` on the line above. So the branch is a *no-op even
when forced to run*, and a configuration that only reached it would have killed nothing. The
variant had to move `base_home_pos` as well, which `Z_HOME_DIR 1` does for free —
`Z_HOME_POS` becomes `Z_MAX_POS`. One line of configuration bought both. That rule is now in
`survivor-taxonomy.md`.

The behaviour it unblocks is the one the command's own comment describes: on a machine whose Z
switch is at the top, the useful place to stand when setting a Z offset is at the bed, and the
bed is the length of the axis away from the reference point. M428 measures from **zero** there
instead of from the endstop. Three tests state it — near the bed, near the switch, and the band
between them that is too far from both — and the third is what stops a firmware that corrects
unconditionally from passing.

**The nine that remain are equivalent, in two families.** Five widen the guard to include the
minimum-homing axes, where the correction is still a no-op because `X_HOME_POS` and
`Y_HOME_POS` are zero; that reason is now a `static_assert` in the test file rather than a
comment, so it fails the day it stops being true. Four are +/-1 mutants of the `-20, 20` bounds
on that line, and they are equivalent for a prettier reason: the band in which they differ is
`|diff|` just over 20, which puts the carriage near `Z_HOME_POS -/+ 20` — and the *corrected*
value there is `-179.x`, refused by the range check on the line below. Both arms refuse, so
nothing separates them. The bounds on line 89 are a separate copy and are bracketed from both
sides.

**Making Z home upward meant the simulated rail had to grow an end.**
`SimulatedAxisWithLimit::closed()` was `carriage_steps <= trip_steps` — a minimum switch, and
nothing else was expressible. It now takes a `SwitchEnd`, and `furthest_towards_switch()` is the
direction-aware companion to `lowest_reached()`, so "it reached its switch" means the same thing
on both machines. Note the other half of that: with `Z_HOME_DIR 1` the firmware does not define
`Z_MIN_ENDSTOP_HIT_STATE` **at all**, so a fixture naming it does not fail, it fails to compile.
`STR_Z_LIMIT` in `simulated_endstops.h` is how a test should name the Z switch from now on.

Two harness defects fell out, both found by the new configuration and both fixed —
**#47**, a home offset left behind by a failing test moving the origin for every test after it
(the same `longjmp` as the heater targets; two of that run's five failures were collateral), and
**#48**, which is worth reading in full: a heated nozzle starts the print job timer, a running
job arms the filament sensor, and the sensor injects `M600` into the next test that idles. Three
correct behaviours composing into contamination, latent until a new test happened to sit between
the heater tests and the queue tests. The tell was `queue___starts_empty` failing two files away
and the suite hanging after it, and what found it in one run was printing the injected string at
every test boundary rather than reasoning about which feature could have produced it.

**`011-shared_enable` unblocked the dead two thirds of `M17_M18_M84.cpp` (2026-08-12):
33% -> 84% line, and the mutant population over that file went from 38 testable to 104.**

The problem was never a missing test. `do_enable()` and `try_to_disable()` handle boards
where two axes share a driver enable pin — tracking which axes came on as a side effect,
and refusing to report an axis released when the shared pin is still driven — and
`any_enable_overlap()` is a `constexpr` over the pin assignments that was false in every
configuration. The branch calling them was dead. **The fix is a configuration, not a test**,
and it cost one `.ini` plus `#ifndef` guards on the enable pins in
`native/pins_RAMPS_NATIVE.h` so a configuration can point two axes at one driver.

The behaviour is a physical consequence rather than a policy: one pin cannot be high for X
and low for Y at once. So `M17 X` energises Y and has to say so, and `M18 X` cannot cut the
current while Y is held and has to say *that*.

**It found two defects immediately, and one of them by breaking a test that had always
passed.** `M17_naming_an_extruder_the_machine_lacks_does_nothing` failed the moment the new
configuration existed — register #45: the bound `if (e < EXTRUDERS)` is in the direct path
only, and the shared-enable path never reads the value at all on a single-extruder machine.
Same command, same argument, different answer depending on how the board is wired. A test
can only be as general as the builds it runs in, and this one had run in ten builds that all
took the same branch.

Register #46 came from the second test: `M18 X` on a shared board correctly reports "X not
disabled. Shared with Y" *and* clears X's enable flag, because `disable_axis()` marks before
it asks. The warning and the state disagree in the same breath. Not merely cosmetic —
`mark_axis_disabled()` under `Z_CAN_FALL_DOWN` throws away the homing.

Both are recorded and pinned; the tests state what each configuration actually does rather
than asserting the version that passes.

**And then the newly-reachable code was rescued: 59.6% -> 88.5% (92/104), 100% killable.**
Twelve survivors remain and all twelve are equivalent here — two redundant-work guards, six
needing an *extruder* that shares a pin (this board shares only X with Y), two on
`any_enable_overlap()` itself which is the branch already taken, one `LCD_MESSAGE` with no
display, one `REPEAT` over extruder overlap there is none of.

What the survivors asked for was mostly **exactness in the report**, and that is the lesson:

- *"Enabling X reports that Y also came on"* is not enough. Three input classes are needed,
  because the message is a mask accumulated, then filtered against what was asked for and
  what was already on — and each step has mutants only one class separates. One of a shared
  pair names the other; **both** of a shared pair name nothing, which is what kills a filter
  that ands where it should or; an axis with its own driver names nothing, which is what
  kills an accumulator seeded with a bit instead of with zero.
- *"The warning says 'not disabled'"* is not enough either. It is assembled from the axis
  letter, the words, the list of axes sharing, and the full stop, each with its own mutants.
  Asserting the whole sentence separates them. And the list must be asserted as **Y and not
  Z**: a version that listed every enabled axis contains Y too, and would tell the user that
  an axis on a driver of its own was somehow implicated.
- **Naming an axis is not naming an extruder.** `selected_axis_bits()` asks
  `parser.seen('E')` before anything else and walks the extruders in a separate loop;
  widening either test enables the hot end on every `M17`, which no assertion about X, Y or
  Z can see.

**`core/serial.cpp` rescued (2026-08-12): 42% -> 93% line, 90.5% raw, 100% killable
(134/134).** Chosen over larger gaps for a reason worth keeping: **it is the instrument every
other test asserts through.** Dozens of tests here search a captured serial stream for a
word; if the word is right and the prefix is wrong, or the axis labels are transposed, those
tests still pass and the machine still lies to its host. A fault here weakens assertions
everywhere at once while looking like a formatting detail.

The pure formatting helpers were the whole gap — `serial_offset()`, `serial_ternary()`,
`print_bin()`, `SERIAL_ECHO_SP()`, the `echo:`/`Error:`/`Warning:` prefixes, and
`print_xyz`/`print_xyze`. No fixtures, no hardware, no clock, so the assertions can be exact
strings rather than substrings, which is what makes them worth having.

Two things the mutation run asked for:

- **`serial_offset(v, sp)` needs the sign and the style varied *together*.** `sp` says how
  *zero* should be written — nothing, a space, or a plus — and the non-zero cases must ignore
  it. Relaxing either equality (`v == 0` to `<=` or `>=`) is invisible until a non-zero value
  is passed with a non-zero `sp`, which no caller in the firmware does and no obvious test
  would. Five values times three styles, and eleven mutants died.
- **`print_xyze` is a second call site with its own copy of everything** — its own argument
  list, its own suffix decision. Covering `print_xyz` left both unasserted. Same lesson as
  `prompt_do`'s four overloads.

**A short-circuit makes a later clause unreachable, and that is its own equivalence
category.** `else if (v > 0 || (v == 0 && sp == 2))` — mutating the second clause's `==` to
`>=` changes nothing, because every case where they differ (`v > 0`) is already taken by the
first clause. Not preprocessor-erased, not out of reachable range: *unreachable by
short-circuit*. Worth recognising on sight, because it looks exactly like a live boundary
mutant.

The other thirteen survivors: ten are `NUM_AXIS_LIST_` swaps of axes this build does not have
(`planner.cpp:1223` again), one is `count *= PROPORTIONAL_FONT_RATIO` mutated to `/=` with the
ratio at 1.0, one is `(0==1)` written where `0` was, and one is an `else` dropped where the
two branches are mutually exclusive anyway.

**`endstops.cpp` closed (2026-08-12): 79% line, 24.8% -> 34.4% raw, 100% killable (54/54).**
The raw figure is the lowest here by a distance and it is not a gap. **103 of the 157
testable mutants are erased by the preprocessor**, 77 of them on a single line:

    MAP(ES_REPORT, X_MIN, X2_MIN, X_MAX, X2_MAX, Y_MIN, ..., Z4_MAX);

sixteen switch names, of which this build has three. Every survivor there substitutes one of
the *absent* names — `X2_MIN` becomes `X0_MIN` — and `TERF(USE_X2_MIN, ...)` expands to
nothing either way. That is `planner.cpp:1223` again, with the same tell: every survivor
touches the names and none touches the comparison, which lives on the `#define` a line
above. The rest are the same story for `ENDSTOP_NOISE_THRESHOLD`, `USE_Z_MIN_PROBE`,
`BLTOUCH` and `JOYSTICK_DEBUG`, plus two on `__O2` — an optimisation attribute with no
semantics — and one in `validate_homing_move()` whose else calls `kill()`.

**Two lessons, and the first was my own mistake.** The first version of
`each_switch_is_reported_from_its_own_pin` compared each switch's closed reading against its
*open* reading and asserted they differed. It killed nothing, and the reason is the trap
this file has warned about since `M105`: a fault that inverts every switch inverts both
readings, so "they differ" still holds. Proven rather than argued — inverting the comparison
by hand left the test passing. It now asserts against `STR_ENDSTOP_HIT` and
`STR_ENDSTOP_OPEN`, the words the firmware defines, and the same injection fails it.

The second: **`resync()` is a third instance of "a call with no return value and no message
is asserted on the clock or not at all".** `enable()` and `enable_globally()` both end in it,
it returns void, and its body is a delay that waits for the sampling interrupt to run once —
so every mutant of it and of its guard survived everything. Timing `enable(true)` against
`enable(false)` killed eleven at a stroke. After `planner.synchronize()` in `M0` and `M18`,
and `M81`'s shutdown pause, that rule has earned its place.

**And the per-axis gap again.** `Endstops::update()` has a hand-written
`if (AXIS_IS_MOVING(n))` / `if (AXIS_DIR_REV(n))` block per axis, and the existing "a closed
switch is ignored while the axis moves away" test drove only X — so widening Y's or Z's copy
to always-true survived. Same shape as `each_axis_homes_against_its_own_switch`, one level
down: there it was which switch homing drives *against*, here which switch is *ignored*. The
failure it prevents is a printer that cannot be backed off its own limit.

**Four small 0% commands closed (2026-08-12).** `M119` 0 -> 100% line / 100% mutation,
`M155` the same, `M80_M81` 0 -> 80% line / 33.3% -> 66.7% mutation, and **`M876` was already
at 100%** — the `host_actions` tests had covered it, which is worth noticing before writing
anything: a file at 0% in yesterday's report may not be at 0% today.

`M80` itself is not compiled — `PSU_CONTROL` is off — so the 10 countable lines are `M81`.
What is left there is classified: two on an `LCD_MESSAGE` with no display to receive it, one
a +/-1 mutant of the shutdown delay that no assertion can resolve, and one on
`delayed_power_off`, which is constant-false without `POWER_OFF_TIMER`.

Two of the three tests exist because of a **bare statement with nothing to observe but the
clock** — `safe_delay(1000)` in `M81`, five survivors, killed by bounding how long the
command takes. That is now the third instance of the same shape after `planner.synchronize()`
in `M0` and in `M18`, and it is worth stating as a rule: *a call with no return value and no
message is asserted on the clock or not at all.*

**`M155` found register #44, and it found it by failing.** The test asserted that a period
beyond the 60-second limit is clamped; it failed, and the probe said why —
`set_interval()` clamps `report_interval` but schedules the *first* report from the
unclamped `seconds`. Nothing at 61 seconds, a report at 261. So `M155 S255` gives four and a
quarter minutes of silence before the clamp takes any effect, which is the silence the clamp
exists to prevent. Pinned from both sides so a fix to the broken half cannot break the
working one.

**`M119` is bracketed rather than shown once**, because a report that always said "open"
passes any test that closes nothing, and one that always said "TRIGGERED" passes any test
that closes something. The same switch is read both ways round, driven by moving the
simulated carriage onto its limit rather than by writing the pin — so what is asserted is
the whole path a person exercises with their finger, inversion setting included.

**`M17_M18_M84.cpp` rescued (2026-08-12): 17% -> 33% line, 50% -> 81.6% raw, 100% killable.**
Both figures are over the same 27-line covered set, so they are directly comparable, and the
line figure is the one that needs explaining: **two thirds of this file is dead on this
board.** `do_enable()` and `try_to_disable()` handle machines where two axes share one
enable pin — warning about the axis that came on, or stayed on, as a side effect — and
`any_enable_overlap()` is a `constexpr` over the pin assignments that is *false* here. That
is a configuration gap and no amount of testing closes it; it needs a board in `test/` whose
axes share a pin.

The behaviours that are reachable are worth having, because the failure modes are physical:
`M18 X` must leave Z holding or the gantry drops, and `M18 S<n>` is a *setting* rather than
a command to switch off now. Three survivors needed inputs nothing else in the suite
produces:

- **`reset_stepper_timeout()` is invisible until time has passed.** Deleting it survived
  every assertion on the stored period, because the period is right either way. What it
  changes is *when the clock starts*: without it, setting a timeout on an already-idle
  machine gives one that has effectively already expired.
- **`planner.synchronize()` in `M18 <axis>` needs a move long enough to see.** Same shape as
  `M0`'s, and the same consequence — release a motor with a move still queued and the
  carriage coasts while the firmware believes it arrived.
- **`E` on its own means every extruder, and only a multi-extruder build can say so.** On
  one extruder, "all of them" and "number zero" are the same outcome, so mutating the
  has-a-value test to *always* survives — reading a missing value gives 0, and extruder 0 is
  all of them. Third instance of "the behaviour needs a machine the measured build is not".

**A survivor turned out to be a redundant guard, and reading the callee is what settled it.**
`M17`'s `if (e < EXTRUDERS)` has mutants that widen it, and they survive because
`Stepper::enable_extruder()` is a `switch` over the valid indices — an out-of-range index
matches no case and does nothing. Register #30's category, verified by reading
`stepper.cpp:735` rather than inferred from the score.

**And that reading found #43.** `disable_extruder()` is *not* symmetric with it: it calls
`mark_axis_disabled()` — a `CBI` on a shift count taken straight from the command — *before*
the switch that would have ignored the index, and `M18`'s path has no bound check at all
where `M17`'s does. The consequence was measured rather than argued: `M18 E99` leaves the
mask unchanged here, because x86 masks the shift to 6 and bit 6 belongs to no stepper in
this build. Benign by accident, on a property of the host's shift instruction. The probe
that established that was written, run, and deleted.

**`host_actions.cpp` rescued (2026-08-12): 13% -> 76% line, 91.9% raw / 100% killable.**
The one genuine gap the `feature/` survey found, and it stayed a gap because it looked like
a formatting file. It is not: it is the protocol a machine with no screen uses to ask a
person for something, and **a prompt is a sequence, not a message** — end, begin with the
text, name each button, then show. A host builds a dialogue box by reading those in order,
so a button emitted after `show` is a button nobody can press, and nothing about the
individual lines says so. Nearly every assertion here is therefore about *position within
the output* rather than presence, which is what a test of a protocol has to be.

Four things worth carrying:

- **`//action:` is the whole agreement.** Assert the prefix separately from anything that
  uses it, so it fails for one reason.
- **`if (eol)` needs both directions.** Asserting only that an unterminated action runs on
  is satisfied by a mutant that *never* writes a newline, because then everything runs
  together. Two tests, opposite ways.
- **A duplicated body costs a test each.** `prompt_do` is four overloads — program-memory or
  runtime message, each with and without a trailing character — and each has its own copy of
  the two lines that open the prompt and emit the buttons. Covering three left the fourth's
  copies unasserted. And **two distinct buttons are needed**, not one: with a single button
  a mutant that names the first twice, or swaps the pair, produces output no assertion on
  that one button can separate.
- **The trailing character is not decoration.** `prompt_do(PROMPT_FILAMENT_RUNOUT,
  F("FilamentRunout T"), tool)` appends the extruder number. Lose it and the host tells the
  user a filament ran out without saying which, on exactly the machines where it matters.

**"Equivalent by construction on this platform" is a distinct category and this file has the
clearest instance of it.** The three survivors on `if (pgm)` choose between reading the
message from program memory and from RAM — and `PSTR(str)` is `(str)` here while
`pgm_read_byte(addr)` is a plain dereference, so **both arms are literally the same code**.
Not "no test reaches it", not "no assertion separates it": there is nothing to separate. On
AVR they differ and the wrong arm reads a pointer as an address in the other memory space.
Checked in the headers rather than inferred from the score.

The other three: `extra_char != '\0'` mutated to `> '\0'` is equivalent by reachable range
(every character passed is a positive ASCII digit), and two in `handle_response()` are cases
whose bodies the preprocessor erases in the default build — an empty case falling into
another empty case. Those two are killable under `003` and the runner only measures default,
which is the trap already recorded for homing and levelling.

### Sizing the `GCodeParser` migration (2026-08-11)

18 files read `parser.codenum` / `codebits` / `string_arg`. Measured rather than assumed, and
the shape changed twice in the measuring.

**Eight were compiled by an existing configuration and had simply never been measured.** Under
`004`: `M23` 100%, `M30` 100%, `M32` 100%, `M928` 100%, `M28_M29` 71%. Under `003`: `T.cpp` 75%.
Under `008`: `M117` 80%. `M118` turned out to be 92% in the default build and had never been
listed as rescued at all. That is nine of eighteen effectively closed by pointing a coverage run
at builds that already existed — no tests written.

**`M0_M1.cpp` is rescued (2026-08-12): 0% -> 100% line, 100% mutation (21/21) under `003`.**
It was the biggest remaining consumer — 11 parser reads — and had sat at 0% under `003`, `008`
*and* `010` (`HAS_DWIN_E3V2` is in the `HAS_RESUME_CONTINUE` list too, which the earlier survey
missed). Not neglect: it is a command whose entire job is to not return, so nothing in the
suite could reach it until the test HAL made `idle()` cost simulated time.

Three things it taught, none of them about M0:

- **The untimed branch needs a second thread.** With no `P` and no `S` there is no deadline,
  so the only thing that ends the loop is a person. The answer has to arrive from another
  thread — the `SerialCapture` arrangement — and its delay must be *wall-clock*, because
  simulated time only moves when the machine moves it.
- **`> before` is not enough, and the mutation run is what said so.** `ms = 0` mutated to
  `ms = 1` gives a one-millisecond deadline, which satisfies "time passed" perfectly. The
  untimed case has to be shown to outlast any trivial deadline. Two survivors, one bound.
- **A bare `planner.synchronize()` has no return value and no message.** Queueing a move that
  takes ten times the wait is what makes it observable: with the synchronize the command
  cannot return until the move is done, without it the wait is over first. This is the one
  behaviour that actually matters — `M0` is where a person reaches into the machine.

The `command_number()` read is asserted through the host prompt (`//action:prompt_begin M0 Stop`
against `M1 Stop`), which is the only place in the file where the two commands differ at all.

**Register #41 is fixed, and the fix has no test — deliberately.** `MString::ltrim()` called
`strcpy` on overlapping ranges; it is now `memmove`. The trim produces the right answer either
way, so no assertion in this suite can distinguish fixed from broken, and writing one would be
theatre. **The sanitizer is the pinning**: before, `make unit-test-asan` aborted under `009` after
331 tests; after, both `001` and `009` run to completion with only register #27's float-rounding
assertion failing. When the only instrument that can see a defect is the sanitizer, the sanitizer
run *is* the regression test, and it has to be run deliberately for that to mean anything.

**The five `009` consumers are closed, and the figures in this file were stale.** They read
0-7% when the configuration was new; `test_parser_consumers.cpp` was written since, and they are
**98% line** with mutation measured under `009`:

| file | raw | note |
|---|---|---|
| `sd/M33.cpp` | 100% | |
| `config/M550.cpp` | 92.3% | 1 survivor: `ui.reset_status(false)` deleted — no display in this build |
| `feature/macro/M810-M819.cpp` | 87.5% | 5 survivors, all equivalent |
| `geometry/G53-G59.cpp` | 83.8% | 11 survivors, all equivalent |
| `host/M16.cpp` | 75% | 1 survivor: the mismatched-name branch calls `kill()`, which never returns |

**Every one of those 16 "equivalent" survivors is the same shape, and it is worth naming: a
mutant whose only effect is undefined behaviour.** Widening a bounds check lets an out-of-range
index through, and the array read or write that follows is out of bounds — so the mutant does not
compute a *different* answer, it computes an *undefined* one, which on this platform happens to
read as zero. No assertion separates those, and reaching for one means asserting on memory layout.

**But a sanitizer does separate them, and that was checked rather than assumed.** With the guard
in `M810_819()` deleted, `make unit-test-asan` reports `global-buffer-overflow ...
M810-M819.cpp:52` from inside `a_macro_number_past_the_configured_slots_does_nothing`. So the
honest classification is *equivalent under the ordinary suite, detectable under the sanitizer
build* — not *unkillable*.

Two of the sixteen are a different and prettier equivalence: `command_number() - 54` mutated to
`% 54`, and `- 810` to `% 810`. Over the reachable domains (54..59 and 810..819) subtraction and
modulo agree exactly, so those are equivalent by reachable range rather than by undefined
behaviour.

**Getting to that answer took two false starts, both instructive.** The first sanitizer run
reported nothing, and it would have been easy to write down "the sanitizer does not see it" — but
it had aborted six test files earlier on an unrelated fault and never reached the macro tests. A
control that does not run is not a control. The fault it aborted on is register #41, a genuine
`strcpy`-on-overlapping-ranges in `MString::ltrim()`, and #42 is why it was invisible: PlatformIO
prints `331 test cases: 1 failed, 329 succeeded` beside `[ERRORED]` and none of the sanitizer's
output. Run `.pio/build/testhal_native_asan/program` directly to see it.

**`009-parser_consumers.ini` makes five more visible** — `M550`, `G53-G59`, `M16`, `M810-M819`,
`M33`, which no configuration compiled. They now read 0-7%, which is the point: 0% and measurable
is a rescue, invisible is not. Three traps on the way, two of which `004` had already hit and
documented — `CONFIGURABLE_MACHINE_NAME` needs `GCODE_QUOTED_STRINGS`; media defines
`EVENT_GCODE_SD_ABORT` and `SanityCheck` asserts on it with a **non-constexpr `strstr`**, which
can never compile; and `REINIT_NOISY_LCD` is on by default but unwanted on `BOARD_SIMULATED`,
which `-Werror` turns fatal. The third is new: **these `.ini` files take no inline `#` comments.**
The configuration script copies the rest of the line into the generated `#define`, and an em-dash
in a comment surfaced as "extended character is not valid in an identifier" in
`Configuration_adv.h` — a file I had not edited.

**Two remain genuinely out of reach, and only one is a wall.** `M485.cpp` (8 reads) pulls a
third-party RS485 library that wants `arduino/HardwareSerial.h`: not host-buildable, same class
as `sovol_rts` and `mks_ui`, a porting job. `mmu3.cpp` (1 read) is buildable in principle but
chains `MMU_MODEL` → exactly 5 extruders → `FILAMENT_RUNOUT_SENSOR`, which would turn `009` into
a different machine for the cheapest consumer in the set. It wants its own configuration, and it
is worth one read.

**The migration is now blocked on `M485` alone.** `M0_M1` is rescued (100% line, 100% mutation),
the five `009` files are closed, and **`M28_M29` is 100% line / 98.0% raw, 100% killable** under
`004` — its one survivor is `p[1] > '0'` mutated to `!= '0'`, which the `NUMERIC(p[1])` guard on
the line above makes equivalent by constraining the operand to `'0'..'9'`. Worth keeping as a
pattern: **a guard on the preceding line is part of the domain of the line after it**, and the
mutation runner does not know that.

The one test that was needed there is also worth keeping. `while (*p == ' ')` widened to `<= ' '`
survived everything, because with an ordinary `B0 name.gco` the two agree exactly — one space,
then a letter. They separate on any byte below a space, and a **tab** is the reachable one: a host
sends bytes and nothing upstream turns a tab into a space. The tab is not skipped, so it stays in
the filename, which the card then refuses — and the assertion is bracketed against the space
rather than left as "no file was opened", because that alone is satisfied by every cause of
nothing.

`M485` remains unbuildable for the host, which is a porting job rather than a testing one.

### Emulating the embedded platforms: what it would actually take

Every wall here that is *not* a testing problem is the same wall — code that cannot be compiled
or run for a 64-bit host: `sovol_rts`, `creality/dwin`, `mks_ui`, `M485.cpp`, `mmu3.cpp`.

**The important finding is that most of those are not hardware dependencies.** They are
toolchain and type-model differences. `sovol_rts` fails because `int32_t` is `int` on x86-64
and `long` on arm-none-eabi, which turns two distinct overloads into one signature with two
bodies (register #36). `dwin` and `M485` fail on missing Arduino headers. None of that needs a
CPU emulated; it needs the target's *compiler*.

So the work is staged, and the stages are wildly different in cost. Nothing below is installed
on this machine — checked: no `qemu-system-arm`, no `qemu-system-avr`, no `arm-none-eabi-gcc`,
and only the `native` PlatformIO platform.

**Stage 0 — done, and it ended the sovol question rather than advancing it.** Both toolchains
install into user space with no root: `pio pkg install -g -p atmelavr` and
`-t platformio/toolchain-gccarmnoneeabi`. The type-model story is confirmed exactly —
`int32_t` is `long` and `int` is 16-bit on AVR, so `sendData(int)` collides with the
**`int16_t`** overload there, while on x86-64 it collides with the **`int32_t`** one. The set is
well-formed only where `int` is 32-bit and `int32_t` is `long`, which is arm-none-eabi and
nothing else.

But cross-compiling the real `GD32F103RET6_sovol_maple` environment found something that makes
that moot: **`sovol_rts.cpp` references four identifiers that do not exist anywhere in the tree**
— `MARLINVERSION`, `MACVERSION`, `SOFTVERSION`, `CORP_WEBSITE_E` — so it cannot compile in any
configuration on any platform. Register #37. It is dead code, the defect recorded against it in
#33 cannot run on a machine built from this tree, and **no emulator helps**: undefined
identifiers fail at compile time, and QEMU has nothing to run.

The other two were then checked the same way, and the three drivers turn out to be in three
different states:

| driver | cross-compiled for its target | state |
|---|---|---|
| `lcd/sovol_rts` | **cannot, ever** — four undefined identifiers | dead code (#37) |
| `lcd/dwin/creality` | **builds clean** — `STM32F103RE_creality`, flash 25.5%, RAM 11.2% | live, and testable in principle |
| `lcd/extui/mks_ui` | **builds clean** — `mks_robin_nano_v1v2`, flash 56.7%, RAM 74.8% | live; register #34 now confirmed |

`mks_ui` took two goes, and the second one only worked after reading `Conditionals-2-LCD.h`
instead of guessing. Two traps, both worth knowing. **`TFT_LVGL_UI` is not something you set** —
it is auto-defined by the legacy path from `TFT_LVGL_UI_FSMC` or `TFT_LVGL_UI_SPI`, and setting
it directly alongside a panel is what pushes `LCD_ENABLED_COUNT` past one. And
**`TFT_RES_480x320` lives inside Configuration.h's `#if ENABLED(TFT_GENERIC)` block**, which is
evaluated *before* Conditionals auto-defines `TFT_GENERIC` — so the resolution silently does not
apply unless `TFT_GENERIC` is also set explicitly, and the build then insists on a resolution
that is right there in the file.

**Both live drivers had their recorded suspicion confirmed by their own object files**, which is
the cheapest form of confirmation available and needed no emulator at all. #33 for `dwin`: uses
`set_max_feedrate` and `set_max_acceleration`, writes steps-per-millimetre raw, references no
`refresh` symbol. #34 for `mks_ui`: references `refresh_positioning` but no
`refresh_acceleration_rates`, and writes `max_acceleration_mm_per_s2` raw — it remembers the
refresh derived from resolution and forgets the one derived from acceleration.

So the emulator was never what these two needed. A compiler that accepts the file and a symbol
table settled both.

**Stage 1 — `qemu-user`, not `qemu-system`.** A test binary cross-compiled for 32-bit ARM Linux
and run under `qemu-arm` gives a genuine 32-bit type model, real execution, and the existing
test HAL, without modelling a board at all. Far cheaper than system emulation. Caveat worth
checking before betting on it: glibc on ARM may define `int32_t` as `int`, in which case this
stage does *not* reproduce #36 even though it does reproduce pointer width and alignment.

**Stage 2 — `qemu-system-arm` with a Cortex-M machine and semihosting.** The only stage that
exercises the real platform HAL — timers, USART, SPI. Also the most fragile: QEMU's STM32
models implement a subset of peripherals, and Marlin's init touches many, so the likely first
result is a hang in `setup()` rather than a running firmware. A project, not a task.

**And the instrument problem applies to the emulator itself.** An emulator is a measuring
device, and this fork's whole discipline says an unvalidated measurement is a rumour. QEMU's
peripheral models are approximations; a test that passes under emulation and would fail on
hardware is exactly the self-consistent wrong measurement this workflow exists to catch. Any
QEMU result needs the same treatment as any other harness — inject a fault, prove it can fail,
and reproduce a known result before trusting a new one.

### What a host-buildable display driver actually buys

`SerialCapture` now takes the port to watch, so the drainer the host-facing tests already
used works on `LCD_SERIAL` too. That matters because a DWIN panel is not an interface the
firmware calls — it is a screen the firmware writes bytes at, so there is no `stub_extui`
equivalent to record and the byte stream is the only observable. The drain has to run on
another thread for the documented reason: the write busy-waits for room in 128 bytes, a
screen refresh is longer than that, and draining afterwards means draining a buffer whose
producer is already wedged.

**Half the seam works.** `a_status_message_reaches_the_panel` is the first assertion any LCD
driver has ever had here.

**The other half needed a hand, and now has one.** Register #33's write lives in
`hmiStepXYZE()` and happens only on an encoder click; `encoderReceiveAnalyze()` gates it on
`BUTTON_PRESSED(ENC)`, which reads `BTN_ENC` — and `BOARD_SIMULATED` defined **no encoder pins
at all**, because `HAS_DWIN_E3V2` is neither `HAS_WIRED_LCD` nor a TFT UI and fell through both
branches of `pins_RAMPS_NATIVE.h`. So the macro was a compile-time false and no call sequence
reached the assignment. Making a driver host-buildable is not the same as making it drivable.

Three GPIOs in the pins file and `tests/support/simulated_encoder.h` close that, and
**#33 is now confirmed by behaviour**:
`changing_the_resolution_at_the_panel_leaves_the_reciprocal_stale` turns the knob, clicks, and
asserts the stored resolution changed while `mm_per_step` did not follow. Nothing is set behind
the driver's back — the starting value is below the driver's own lower limit, so the committed
value is the driver's choice and not the test's.

The fixture is worth reading before writing another like it. The firmware, not the fixture,
does the sampling: a phase change is only seen by a call into the driver and the delta is
consumed by that same call, so every motion takes the caller's pump. And a phase change costs
two passes and 3 ms of simulated time, because `MarlinUI::get_encoder_delta()` debounces —
the first pass after an edge only starts the timer.

**Validated by injecting the fix.** Adding `planner.refresh_positioning()` after `dwin.cpp:1647`
makes that test — and only that test — fail. Doing it also broke the suite twice over and found
two harness defects worth more than the test (registers #39, #40): panel buttons power up held,
which cost nothing but turned an 8-second run into ten minutes with everything still passing;
and `SerialCapture`'s destructor is skipped by Unity's `longjmp`, leaving a port connected with
nothing draining it, so the *next* long write anywhere in the suite never returns. Both are
fixed in `quiesce_simulated_peripherals()`, which runs on the far side of the jump.

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

**Test counts as of `unit-test-coverage`:** `testhal_native_test` 697,
`acceptance_native_test` 37 — each measured with `pio run -t marlin_default -e <env>`, i.e.
against the **default config only**.

Say which of those two axes you mean whenever you quote a count. `make unit-test-all-local`
varies the *config* and holds the env fixed: it runs `testhal_native_test` against all
**thirteen** configs in `test/`, reporting **697, 711, 721, 768, 768, 706, 703, 723, 763, 758, 697, 700, 706**. The counts above vary
the *env* and hold the config fixed. Give an agent a bare number as a baseline without saying
which, and a correct tree reports a mismatch.

The sixth config, `006-eeprom.ini`, exists because `EEPROM_SETTINGS` is off everywhere else, so
`M500`/`M501` are not compiled and `settings.cpp` showed 51 covered lines instead of 298. A file
that is only reachable under an optional feature is a file whose common path nobody is checking;
adding the configuration was cheaper than any test.

## Which HAL the tests run against

**HAL/TEST, and only HAL/TEST.** As of 2026-08-12 there is no way to build the unit tests
against anything else: `linux_native_test` and `linux_native_coverage` are deleted, so is
`make unit-test-integration`, and `[native_unit_test]` — the section every test env now
extends — deliberately names no HAL. A test environment that descends from a HAL
environment is how a suite ends up built against the wall clock, which is the mistake this
prevents rather than documents.

`HAL/LINUX` is still built, as **firmware**, by `env:linux_native`. That is all it is for.

The reasoning is worth keeping because the arrangement failed in both directions:

- **Real time makes most of the suite unreachable.** Under wall-clock time a test cannot
  advance the clock, so every command that waits — `G4`, `M400`, `M109`, homing, arcs — is
  not expressible. Those tests were guarded out, which meant the LINUX build exercised the
  code *least* likely to be HAL-sensitive.
- **Nobody ran it.** `b301300403` (2026-08-05) appended four tests after
  `test_step_timing.cpp`'s closing `#endif`, and the LINUX build stopped compiling for
  **seven days and 56 commits** before anyone noticed. It was found by accident, while
  trying to reproduce a defect on that HAL. A check that can be broken for a week without
  anyone tripping over it is not a check.
- **And it did not finish.** Measured before deleting it: 193 tests in 800 seconds, then
  killed. At that rate a full run is half an hour.

Its historical value was real — registers #16, #18 and #20 were all found there — and it is
spent. #20, the link-order dependence, is now guarded by something cheaper and HAL-free: the
mutation runner sorts its link inputs, and the suite is verified under sorted, reversed,
filesystem and four shuffled orders.

Two things fell out of the removal that are worth knowing:

- **`preflight-checks.py` grants board compatibility by name, and used to grant it by
  inheritance.** `testhal_native_asan` qualified only because its `extends` chain reached
  `linux_native_test`, which the board's own annotation in `pins.h` names. Breaking that
  chain broke the asan env with a message pointing at an environment that no longer exists.
  The exemption is now `_native_test`, `_native_coverage`, `_native_asan` — **the name is
  the whole rule**, so a new measurement env must match it or it will not build at all.
- **The `__PLAT_TEST__` guards are gone from the test tree entirely.** They no longer switched
  anything, and a guard that cannot be false is a claim about the build that is not true. Files
  guarded on a *feature* keep that guard, narrowed: `#if defined(__PLAT_TEST__) && HAS_BED_PROBE`
  is now `#if HAS_BED_PROBE`. The trap that caused the seven-day breakage is therefore gone
  rather than avoided — there is no closing `#endif` left to append a test after.

  **Two of them had `#else` branches, and both were worse than dead code.** In
  `test/unit_tests.cpp` the fallback stubbed out the peripheral-leak check to nothing, so the
  LINUX build had no guard against the fault class of register #26 — the one that cost the most
  to diagnose. In `simulated_sensors.h` the fallback for `settle()` **wrote the expected
  temperature straight into `thermalManager.temp_hotend[0].celsius`**, computed with the same
  conversion the firmware uses, because the ADC pipeline is dormant there. Every temperature
  assertion under that HAL was therefore checking a number the fixture had just placed, through
  the same arithmetic — the self-consistency trap this file warns about, sitting inside the
  harness rather than inside a test.

## Test, coverage, and mutation tooling

```bash
make unit-test-all-local                    # test HAL, all 3 configs in test/
make unit-test-coverage                     # test HAL + gcov/gcovr report
make unit-test-mutation TARGET=<file.cpp>   # mutation-test one source file, test HAL
pio run -t marlin_default -e acceptance_native_test        # acceptance suite alone
pio run -t marlin_default -e acceptance_native_coverage    # ... with coverage
```

**There is only the test HAL now.** `UNIT_TEST_ENV`, `COVERAGE_ENV` and `MUTATION_ENV` all
name `testhal_*` and there is no longer a second suite to name by mistake:

```bash
make unit-test-all-local                                  # test HAL, all three configs
make unit-test-coverage                                   # test HAL + gcov
make unit-test-mutation TARGET=<file.cpp>                 # test HAL
make unit-test-mutation TARGET=<file.cpp> RERUN=.pio/mutation/results.json
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
| `009-parser_consumers` | the last reachable consumers of the parser's global state — five files no other configuration compiles |
| `011-shared_enable` | a board where X and Y share one driver enable pin — the first configuration here with any enable overlap, which makes two thirds of `M17_M18_M84.cpp` reachable at all |
| `010-dwin` | `DWIN_CREALITY_LCD` — the first LCD driver made host-buildable; needs an `LCD_SERIAL` port and a `WString.h` that provides nothing. Output is observable, **input is not** — see below |
| `012-max_endstops` | `Z_HOME_DIR 1` — the first machine here that homes an axis to its *maximum*, which is what makes `home_dir(axis) > 0` reachable and `base_home_pos(axis)` non-zero |
| `013-bogus_temp_grace` | `BOGUS_TEMPERATURE_GRACE_PERIOD` — a temperature error disables the heaters and **returns** instead of calling `kill()`, which is the seam register #19 asks for and it already existed in the firmware |

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
- **A loop that waits by grepping the process table waits for itself, for ever.** Every
  measurement here is long enough to invite one, and the obvious form is broken:

  ```bash
  while pgrep -f "make unit-test-all-local" >/dev/null; do sleep 30; done   # never returns
  ```

  `pgrep -f` matches whole command lines, and this waiter's own command line contains the
  string it is searching for. Start a second and they match each other. On 2026-08-13 **ten
  of them piled up in one session**, every one reporting "still running" about a build that
  had finished — and the reports were believed, so the same measurement was waited on three
  times and a mutation run was polled long after its results were on disk. The same trap
  had already bitten `mutation_test.py` earlier the same day.

  Use `buildroot/share/scripts/wait_for.sh <path>`, which waits for the **artifact**: a
  results file cannot match the thing looking for it, and its existence is the condition
  that actually matters. Better still, do not add a waiter at all — run the job itself in
  the background and let the runner report it. If you must match a process, keep the
  pattern out of your own command line: `pgrep -f "[m]ake unit-test-all-local"`.

  One caveat the script's own header repeats: **the artifact must not already exist** when
  the job starts, or the wait returns at once and reports a job done that has not begun.
  `make unit-test-mutation` writes its results at the end, so wait on a results path you
  have just deleted or have not used before.
- **Never `git add -A` after a coverage or mutation run.** Both rewrite
  `Marlin/Configuration.h`, `Configuration_adv.h` and `config.ini` for the suite they measure
  and leave them rewritten. Cleaning them before the *test* run is not enough if a measurement
  runs afterwards — this has now committed a generated bed-levelling config twice. The tell is
  every configuration reporting the same test count as the one last measured, because
  `restore_configs` restores to whatever is checked in. Re-run `git checkout --` on those four
  paths immediately before `git add`.
- **`make unit-test-all-local` deletes `.pio/build/testhal_native_coverage`.** Confirmed by
  running it with the build present and watching it go. The next mutation run then finds no
  coverage data, **warns, and mutates every line** — `queue.cpp` went from 1044 mutants on 135
  covered lines to 2399 on all lines, and reported 28.2% instead of 77.2%. Nothing else about
  the output looks wrong. The tell is the runner's own wording: "on all lines" where it should
  say "on N covered lines". Rebuild coverage immediately before any measurement that follows a
  full-suite run — which is most of them, since the natural order is measure, write tests, run
  every config, re-measure.
- **`restore_configs` also reverts `Marlin/src/pins/*/pins_*.h`, which is a source tree you
  may legitimately be editing.** Adding encoder pins to `pins_RAMPS_NATIVE.h` and then running
  a test target silently undid the edit, and the symptom was a macro that stayed
  compile-time false with no diagnostic anywhere. The cheap defence is that it runs
  `git checkout <path>`, which restores from the **index** — so `git add` the pins file and it
  survives. Not the same problem as the generated configs, which you want reverted.
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
- **Commands that wait for hardware need a stand-in sensor**, and the sensor is driven at
  the pin, not at the reading. `M109`/`M190` loop until a temperature is reached, so a
  non-zero target would never return without one. Tests say what the sensor reads via
  `Marlin/tests/gcode/simulated_sensors.h`, which drives a raw ADC count on the pin
  `MarlinHAL::adc_value()` samples and lets the firmware's own pipeline turn it into
  degrees. **Two corrections to what this file used to say here**, both of which cost time
  on 2026-08-13:
    - *"The ADC pipeline is dormant in this build"* — it is not, and has not been since the
      test HAL arrived. `Temperature::isr()` runs whenever simulated time advances, so a
      value written into `temp_hotend[0].celsius` survives only until the next conversion.
    - *"`thermalManager.init()` crashes with SIGFPE — do not call it"* — it does not, and
      `SimulatedHardware::ensure_ready()` has been calling it all along. That matters: `init()`
      is what narrows `temp_range[]` from the thermistor table's ends to the configured
      `HEATER_n_MINTEMP`/`MAXTEMP`, so the min/max checks *can* be bracketed against the
      machine's own limits. Believing the note cost a first draft of
      `test_temperature_errors.cpp` written against the table's ends instead.

  What is true: **a changed reading takes about 300 ms of simulated time to arrive**, because
  the ADC is oversampled 16 times and the conversion only lands when a full set is in. A test
  that changes a sensor and asserts 50 ms later sees the old value, which looks exactly like a
  fault that went undetected. The same caution applies to anything calling
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
