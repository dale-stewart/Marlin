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

**"Equivalence is often a property of the type."** `esteps` is a `uint32_t`, so `esteps >= 0`,
`esteps != 0` and `(1==1)` are all equivalent to `esteps > 0` on sight; `> 1` differs only for a
single 1/500 mm step, which no assertion can separate from zero. That plus the two `ANY()`
argument reorderings and the `IS_CORE` arm this build compiles out accounts for all 8 survivors
left on that line.

**"Pin the build configuration."** Here that is `restore_configs`; see the gotchas below.

**Delegating to subagents in this repo.** `.claude/agents/hal-debugger.md` and
`mutant-killer.md` exist for the two recurring roles. Give any agent that builds its own
worktree — all PlatformIO environments share `.pio/build/<env>`, so two agents building
concurrently overwrite each other's binary and interleave `restore_configs`, and an agent
measuring a flake over hundreds of runs will silently measure someone else's build.

Worktrees here have been created from `d58bae7e9a` (upstream, Sept 2025) rather than from
the branch under test, which has none of `Marlin/src/HAL/TEST`, `Marlin/tests/support`,
or the module tests. Tell the agent the commit its work builds on and give it the exact
baseline test counts, so a mismatch shows up as "the tree is wrong" instead of as a
mysterious build failure. Both agents that hit this reset the worktree branch themselves
and reported it.

**Test counts as of `unit-test-coverage`:** `testhal_native_test` 464,
`linux_native_test` 377, `acceptance_native_test` 18 — each measured with
`pio run -t marlin_default -e <env>`, i.e. against the **default config only**.

Say which of those two axes you mean whenever you quote a count. `make unit-test-all-local`
varies the *config* and holds the env fixed: it runs `testhal_native_test` against all
three configs in `test/`, reporting **464, 465, 465**. The counts above vary the *env* and
hold the config fixed. Give an agent a bare number as a baseline without saying which, and
a correct tree reports a mismatch.

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

`gcovr` is required for coverage reports (`uv tool install gcovr` — `pip install --user`
is blocked by PEP 668 on this machine).

### Gotchas that have cost real time

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
