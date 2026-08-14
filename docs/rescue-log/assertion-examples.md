# Worked examples behind the skill's assertion rules

Each heading here is a rule stated abstractly in `SKILL.md`, followed by the concrete
instance that produced it. Read them as worked examples, not as additional rules.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

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

**"Pin the build configuration."** Here that is `restore_configs` — see "Gotchas that have cost real time" in `CLAUDE.md`.
