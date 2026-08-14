# `MarlinCore.cpp`, `kill()`, and the individual G-code commands

The emergency stop and the wall that was never there, plus the smaller command files
rescued one at a time — several of which needed a configuration rather than a test.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

**What the retired `kill()` blocker actually bought, measured (2026-08-13).** Two follow-ons,
and they came out differently:

**`M112` is testable and now tested.** `queue.cpp` recognises it in the serial reader rather
than the queue, so that a machine already stuck — waiting on a temperature, or minutes deep in
buffered moves — can still be stopped. Three tests: it stops the heaters and releases *every*
stepper as the line is read, with the queue never advanced; it names itself as the reason, which
is the only thing distinguishing an emergency stop in the log from a thermal fault; and `M110`
is not mistaken for it. That last one deliberately runs **without** an operator, so a firmware
that took it for `M112` hangs rather than fails — a louder result than a wrong assertion.

**The temperature-error *report* is still unreachable, and the reason is neither `kill()` nor
the grace period.** This is defect **#53** and it is the better answer to the half of register
#19 that was left open. `_temp_error()` opens with `static uint8_t killed = 0` and reports only
while that is 0 — so the first fault of the run prints and every later one shuts the heaters
down silently. The message is a **once-per-process event**: at most one test in the suite can
ever see it, and which one depends on link order. A probe printing `killed` on entry reported
**1** by the time `test_temperature_errors.cpp` ran.

The tests written for it were **deleted rather than kept green by arranging the order**. One
that passes only when it runs first would pass today, fail the day a file is added ahead of it,
and present as a defect in the firmware rather than in the arrangement. Under `013` the same
static blocks it from the other side — the threshold moves to `killed == 2` and nothing waits
out the grace period — so the two configurations fail for the same underlying reason by
opposite routes.

Worth keeping as a shape: **the action was assertable and the words were not**, and the cause
was a flag set by a test with nothing to do with temperature. The rule is in
`survivor-taxonomy.md`.

**`MarlinCore.cpp` rescued, and the wall it was behind was never there (2026-08-13).**
36% -> **76% line**, and the denominator changed for a reason worth reading: `setup()` and
`loop()` moved to `MarlinBoot.cpp`, which is excluded from coverage, so the file went from 132
countable lines to 95 and the figure now means *of the code a test could run*. Whole-tree
84.0% -> 85.3%; platform-agnostic 85.3% -> **86.7%**.

**`kill()` returns.** That is the finding, and it retires a classification that had been shaping
decisions here for months. Register #19 records `minkill()` as ending in
`for (;;) hal.watchdog_refresh()` — but that is the `#else` arm. This board has a `KILL_PIN`, so
it compiles:

    while (kill_state())  hal.watchdog_refresh();   // wait for release
    while (!kill_state()) hal.watchdog_refresh();   // wait for a press
    hal.reboot();

and `MarlinHAL::reboot()` under the test HAL has an empty body. The original measurement —
"a probe driving `TEMP_0_PIN` to raw 1023 hangs the binary" — was accurate; the *explanation*
was not. It hangs waiting for an operator, which is what a halted printer is supposed to do.
Nobody had pressed the button.

`tests/support/kill_button.h` is the operator: another thread, because neither loop advances
simulated time or returns. It presses **and then releases**, because Unity's `longjmp` skips
destructors and a fixture that left the button held would hand the next test a machine whose
250th idle calls `kill()` — and *that* kill hangs in the first loop, waiting for a release
nobody will perform.

Six tests. The ones worth knowing:

- **A plain `kill()` releases the extruder and holds the axes; `M112` releases everything.** The
  difference is the whole point of the `steppers_off` flag — a gantry with nothing holding it up
  drops onto the print — and asserting either alone passes against firmware that always did the
  same thing.
- **The release-then-press sequence is asserted with a flag, not a clock.** `kill()` returns
  while the button is still *down*, so the pin state afterwards says nothing, and a wall-clock
  bound would be a race. The operator sets an atomic between letting go and pressing again;
  firmware that accepted the held button returns before it is ever true.

**`pin_is_protected()` — five tests, and two findings.** It is the list of pins `M42` refuses,
and it is what stops a G-code line turning a heater fully on or dropping a gantry. The analog
half is a separate loop with a separate conversion (`analogInputToDigitalPin`) and is invisible
to a test written around heater outputs. Two things the tests said that reading would not:

- **A main axis contributes ENABLE, the endstops and the microstepping pins — not STEP or DIR.**
  The extruder contributes all three. Defensible (a stray write to STEP injects one pulse; ENABLE
  drops the gantry) and now written down rather than re-derived.
- **`M42` is not compiled in any configuration here** — `DIRECT_PIN_CONTROL` is off. Found because
  `M42_I_overrides_the_protection` **passed** against a firmware with no `M42` at all: it asserted
  the *absence* of an error, and an unknown command produces `echo:Unknown command`. A negative
  assertion is satisfied by every cause of nothing, including the command not existing. Both `M42`
  tests are now guarded and neither runs; the predicate itself is tested directly.

**The extraction, and the rule it is meant to enforce.** `MarlinBoot.cpp` holds `setup()`,
`loop()` and `tmc_standby_setup()` — the code a test can never execute, because `setup()` brings
up real hardware in a fixed order and `loop()` does not return. The split is not tidiness:

- Before it, a third of `MarlinCore.cpp` was unreachable by construction and there was no way to
  tell the untestable part from the untested part. `COVERAGE_EXCLUDES` in the `Makefile` names the
  file, so excluding one is a visible argument rather than a convenience.
- **The file is meant to shrink.** Anything in it that can be named and called belongs back where
  it can be tested. Two pieces already moved: `report_reset_reason(mcu)` and
  `report_firmware_identity()` were straight-line reporting inside `setup()`, and are now members
  of `Marlin` with four tests. The reset reason is the only evidence an operator has of *why* a
  printer restarted mid-print, and the flags are independent — a brown-out that also tripped the
  watchdog reports both, which a chain of `else if` would not.

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
