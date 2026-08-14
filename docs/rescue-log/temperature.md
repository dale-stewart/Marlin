# `temperature.cpp`, autotune, autotemp, and the thermal configurations

The largest single rescue here, across three configurations. Quote the killed-by-assertion
figure for this file rather than the raw one, and say which configuration you measured in.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

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

**`014-pid_bed` done (2026-08-13), and the tuning was the job.** The 47 bed and chamber
survivors in `PID_autotune` were unreachable because `PIDTEMPBED` and `PIDTEMPCHAMBER` are off
everywhere else, so `isbed`/`ischamber` are compile-time false and every true arm of
`(isbed || ischamber) ? A : B` is dead — thirteen mutants on the `df` line alone, plus the
`PER_CBH(...)` / `PER_WATCH_CBH(...)` selectors.

Turning the option on is one line. Making the suite *work* with it on was the rest of a
session, and it is the transferable part: **a configuration that unblocks a cluster can also
change the plant the tests run against.** Two problems, in order:

- `BED_CHECK_INTERVAL` **does not exist** under `PIDTEMPBED` — the bed is regulated every pass
  rather than every 5 s. Two test files failed to compile. `BED_CONTROL_PERIOD_MS` in
  `simulated_sensors.h` names the property instead of the macro, the same move as `STR_Z_LIMIT`.
- **The suite then hung on the first `M190`, 528 tests in.** Marlin ships `Kp 10, Ki 0.023,
  Kd 305.4` for the bed, from an FOPDT model of a real 250 W silicone heater with `Tp = 405`.
  `SimulatedBed` is 110 C at full power with a 120-second time constant.

**The diagnosis came from tracing the plant, not from reading the gains**, and the finding is
one worth keeping about this firmware: **`Ki` is applied per call, and `Temperature::task()`
returns early at `updateTemperaturesIfReady()` until a full oversampled ADC set is in — so the
PID runs about three times a second here, not once per millisecond.** At `Ki 0.023` the
integral needs ~150 s to wind up to the 41% duty that holds 60 C; the trace showed the bed at
50.2 C after 100 s, gaining 0.1 C/s. And `Kd 305.4` at that call rate collapsed the duty from
full power to 45% the moment the error came inside `PID_FUNCTIONAL_RANGE`. Under a HAL where
the test owns the clock, that is a hang rather than a failure.

`Kp 15, Ki 0.20, Kd 60` reaches 60 C in ~65 s — the plant's own floor, since a 120-second time
constant towards 110 C cannot do better — and settles at 60.4 C with **no overshoot**, which
matters because any excursion past `TEMP_BED_HYSTERESIS` restarts `M190`'s residency timer. A
test configuration stating its own tuning is the same move `SimulatedMachine` already makes for
steps-per-millimetre.

Three tests assert what actually differs about a bed, rather than repeating the hotend's twenty:
the bed's Ziegler-Nichols factors (`0.2*Ku` and `Kp*Tu/3`, not `0.6*Ku` and `Kp*Tu/8`), that the
published gains are the applied ones, and that each relay level is held for the bed's five
seconds rather than the hotend's three.

**Measured: the eleven bed-and-chamber lines went 47 -> 15 survivors, and every one of the 15
is the chamber.** The three lines the tests aimed at are cleared —
`df` 13 -> 1, `pf` 7 -> 1, `relay_delay` 6 -> 1 — and each remaining one is the `ischamber`
operand of a disjunction whose other half now works (`(0==1) || isbed`), plus five on `:808`
that `TERN0(PIDTEMPCHAMBER, ...)` erases outright. So **all of the bed arms are dead and the
residue is exactly the machine this configuration is not**. The autotune region as a whole is
142 -> 111.

Do not compare the whole-file score across these two configurations: `014` reports 68.7%
(1005/1463) and `013` 65.5% (990/1512), but `014` has no grace period, so the MINTEMP/MAXTEMP
lines are back to being detectable only by hanging and reappear at the top of its survivor
list. Different covered sets, different questions.

**And it exposed a latent order dependency in a completely unrelated file.** Two planner corner
tests began reporting the *travel*-corner junction speed. `PlainExtrusion` set the flow
multiplier but not `allow_cold_extrude`, so the tests had been inheriting that allowance from
whichever earlier test last set it — and three new tests earlier in the run changed what was
left behind. `Planner::buffer_line()` drops the E part of a move on a cold nozzle silently,
with `buffer_line()` still returning true, so every assertion about an extruding corner read as
a travel corner. The fixture now states it. A configuration that changes nothing about corners
found this, which is the argument for having more than one.

Still dark: chamber tuning. `PIDTEMPCHAMBER` needs a heated chamber, which is a different
machine rather than a different control mode, so the `ischamber` half of those ternaries remains
unreachable — worth knowing before reading a figure from `014` as though it covered all three.

**And the third bucket was too optimistic — "~56 genuinely unasserted" does not survive
inspection.** Reading the actual mutants rather than the line numbers:

| what | count | why it survives |
|---|---|---|
| the heat-up watchdog: `:834`, `:963`, `:964`, `:966`, `:968`, `:973` | ~21 | both its abort arms call `_TEMP_ERROR` -> `kill()`. Register #19 again, inside autotune. The non-abort arm is reached constantly but its only effect is *when* the abort would fire |
| `print_heater_states(heater_id < 0 ? extruder : heater_id)` at `:954` | 9 | for a hotend tune `heater_id`, `0` and `motion.extruder` are all zero, so every relational mutant agrees. For a bed tune the differing arm passes -1 to `degHotend()`, which indexes `temp_hotend[-1]` — **undefined behaviour**, not different behaviour |
| `diff > 0.001f` at `:914` | 6 | the swing is ~30 C; `>=`, `> 0.0`, `> 0.1`, `(1==1)` all agree. Separating them needs a heater whose whole oscillation is under a tenth of a degree |
| the 20-minute timeout at `:985` | 4 | one is `_MIN(t1,t2)` reordered, which is commutative; the rest need a tune that runs 20 simulated minutes without completing a cycle, and the watchdog aborts first |
| the tuning-style label at `:925`/`:926` | 5 | **this one was a real gap** — see below |

So of the 57, **five were addressable and the rest are blocked or equivalent**. That is the
honest shape, and the earlier "~56 genuinely unasserted" was a count of lines rather than a
reading of mutants.

The five: `if (ischamber || isbed) " No overshoot" else STR_CLASSIC_PID` is the only place the
operator can see *which* set of factors produced the three numbers they are about to paste into
their configuration. `the_report_names_the_tuning_style_it_used` asserts both directions under
`014` — a bed tune says no-overshoot and not classic, a hotend tune the reverse — because
asserting only the bed's would pass against firmware that labelled everything no-overshoot.
**Measured: 5 -> 1, and the survivor is `(0==1) || isbed`**, equivalent because `ischamber` is
already a compile-time false. Whole-file survivors 458 -> 454.

**So `temperature.cpp` is closed.** Every remaining survivor has a category and a reason:
the chamber (needs a heated-chamber machine), the terminal paths behind `kill()` (register #19,
in both `updateTemperaturesFromRawValues()` and the autotune watchdog), initialisers verified
dead by probe, wrap-arithmetic sentinels in the `M109`/`M190` waits (defect #52), and mutants
whose only effect is undefined behaviour. There is no cluster left that a test would help with.

The other two buckets of the 142 are settled below: ~39 initialisers, all equivalent and
verified by probe, and the 15 chamber arms.

**The ~39 initialisers are equivalent, and it took being wrong twice to establish it.** The
first reading was "dead stores, all equivalent" — correct, but asserted from a glance. I then
"corrected" it to "`maxT`/`minT` are read at `:881`/`:882` and printed at `:911`, so the first
reported `T_MIN` carries the initialiser" — which is **wrong**, because `minT = target` at
`:936` runs at the end of *every* cycle and the print at `:911` is guarded on `cycles > 0`, so
the initialiser is always overwritten before any read escapes.

Settled by probe, which is what should have happened first. Perturbing far beyond anything the
mutator produces — `maxT = 999, minT = 0`, then `t_high = 7777, t_low = 8888,
tune_pid = {5,6,7}, next_watch_temp = 1.0` — leaves the `M303` report **byte-identical** and the
suite green. Two of the 39 are equivalent for a different reason worth separating:
`current_temp = 0.0` is genuinely *live* — setting it to 999 aborts the tune at
`:944`'s overshoot check and fails twenty tests — but every mutant the tool generates is
`0 ± 1`, far below `target + MAX_OVERSHOOT_PID_AUTOTUNE`, so the *mutants* are equivalent by
reachable range while the line is not dead. Same for `next_watch_temp`.

One thing the probe did **not** establish, so it is not claimed: whether a tune that never
computes gains can still reach `_set_hotend_pid(tune_pid)` and apply `{0, 0, 0}`. The probe used
`M303 C4`, which completes and overwrites them. If some abort path applies the initialiser, that
is a real defect and the classification above does not cover it.

**The reusable part is the shape of the mistake.** Both wrong answers came from reading dataflow
instead of running the code, and the second was more confident than the first. A one-line
perturbation far outside the mutator's range settles these in a single build and distinguishes
"dead store" from "live line whose mutants are all in range" — which are different findings with
different follow-ups.

**Autotemp rescued (2026-08-13): a whole feature with 27 survivors and no tests.**
`M104 S<min> B<max> F<factor>` makes the nozzle track extrusion speed — the target becomes
`min + speed * factor`, capped at `max` — and the only mention of it anywhere in the suite was
a fixture restoring its `enabled` flag. Nine tests took it **27 -> 2 survivors, both
equivalent**: a `TERN_` body and a `TERN0` argument reorder, each erased by the preprocessor
because `AUTOTEMP_PROPORTIONAL` is off. 100% killable.

The arithmetic is asserted on `calculate()` directly, because it is a public method taking the
speed as an argument, so the *slope* can be stated exactly: equal speed increments give equal
temperature increments. A single temperature would pass with the factor and the floor wrong in
compensating directions. The cap is bracketed either side, and the rise/fall asymmetry — up in
one step, down weighted against the previous value so the nozzle does not chase every dip — is
asserted in both directions from the same starting point.

Three things this cost, all worth keeping:

- **`calculate()` keeps a function-local `static float oldt`**, so it is not a pure function and
  one test's last call is the next test's starting point. Only the upward path is unsmoothed and
  therefore exact, so every test drives the value up to a known point first. The state is real
  behaviour rather than an inconvenience — it is what smooths the descent.
- **`setTargetHotend()` disables autotemp; `_setTargetHotend()` does not.** That asymmetry is how
  an explicit `M104` overrides the feature, and it is why the set-up has to go through the
  command: a test that set `enabled = true` and *then* the target switched the feature off in the
  line after enabling it. The first draft did exactly that and failed against working firmware.
- **`autotemp_task()` is called by `Planner::check_axes_activity()`, not by
  `Temperature::task()`** — via `manage_inactivity()` at 10 Hz. The second draft drove the
  temperature task and also failed against working firmware. Two wrong guesses about *who calls
  this* in one test; both were settled by grepping for the caller rather than by assuming the
  obvious owner.

**And one of the survivors was my own test's fault, which is the most reusable part.** Six
mutants of the `S` and `B` parameter assignments survived because the test used `S210 B250` —
the configured defaults. Deleting either assignment left the field holding exactly the value
being asserted. **A parameter test whose value coincides with the default asserts nothing about
the parameter.**

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
