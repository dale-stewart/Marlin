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

**The skill must stay portable.** `SKILL.md` and its `reference/` files are meant to be lifted
into other projects, so they state the *general* rule and never name Marlin, a G-code command, or
a firmware symbol. When a run here teaches something, split it: the transferable principle goes
to the skill in neutral terms, the concrete instance goes to `docs/rescue-log/`. If an example
only makes sense to someone who knows this firmware, it belongs in the log.

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

## The rescue log

Everything learned from running the skill on a specific file lives in `docs/rescue-log/`, not
here. This file holds only what applies to *every* session; the log holds what applies to one
target, and is meant to be read when that target comes up rather than every time.

Read the relevant one **before** measuring or writing tests against a file it covers — several
entries exist precisely because a figure was quoted wrong, or a wall reported that was not there.

| File | What is in it |
|---|---|
| [assertion-examples.md](docs/rescue-log/assertion-examples.md) | The worked examples behind the skill's abstract assertion rules — what "assert derived relationships" and the rest look like in this firmware |
| [planner-settings.md](docs/rescue-log/planner-settings.md) | The blocked `planner.settings` correction, what it waits on, and the measured state of every consumer. Closures of `motion.cpp` and `planner.cpp` |
| [gcode-parser-and-queue.md](docs/rescue-log/gcode-parser-and-queue.md) | `gcode.cpp`, `queue.cpp`, and the survey sizing the blocked `GCodeParser` correction |
| [temperature.md](docs/rescue-log/temperature.md) | `temperature.cpp`, PID autotune, autotemp, and the three thermal configurations. **Quote its killed-by-assertion figure, not its raw one** |
| [display-drivers.md](docs/rescue-log/display-drivers.md) | The DWIN driver and `marlinui.cpp` — and the mutation number that put a cost on testing a driver whose only observable is a byte stream |
| [core-and-commands.md](docs/rescue-log/core-and-commands.md) | `MarlinCore.cpp`, `kill()`, and the individual command files, several of which needed a configuration rather than a test |
| [agents.md](docs/rescue-log/agents.md) | What validating each subagent taught, including the two errors that mattered more than the passes |
| [embedded-platforms.md](docs/rescue-log/embedded-platforms.md) | What emulating the embedded targets would actually cost, staged. None of it done |
| [pause-and-filament.md](docs/rescue-log/pause-and-filament.md) | `pause.cpp` and `e_parser.cpp` — the filament change and the emergency stop, plus the fixture fault that made every extruder-only move take hours |
| [binary-transfer.md](docs/rescue-log/binary-transfer.md) | `binary_stream.h` — the framed packet protocol behind firmware upload. Opened, not closed |
| [survey-2026-08-14.md](docs/rescue-log/survey-2026-08-14.md) | **What is left, measured.** Only 128 of 799 source files are compiled by any configuration — so every whole-tree figure is over a sixth of the tree. Ranked candidates |

Behaviour found and recorded rather than changed is in `docs/defect-register.md`, and the skill
itself is `.claude/skills/legacy-rescue/`. **Findings belong in one of those three places, not
in a commit message.** Which one: the transferable principle goes to the skill in neutral terms,
the concrete instance to the rescue log, behaviour left unchanged to the register.

## Delegating to subagents

One agent per step of the skill:
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

**Test counts as of `unit-test-coverage`:** `testhal_native_test` 731,
`acceptance_native_test` 37 — each measured with `pio run -t marlin_default -e <env>`, i.e.
against the **default config only**.

Say which of those two axes you mean whenever you quote a count. `make unit-test-all-local`
varies the *config* and holds the env fixed: it runs `testhal_native_test` against all
**fourteen** configs in `test/`, reporting **731, 767, 777, 826, 802, 740, 737, 757, 797, 820, 731, 734, 740, 735**. The counts above vary
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
| `014-pid_bed` | `PIDTEMPBED` — the bed regulated rather than switched, so `M303 E-1` can tune it and `PID_autotune`'s bed arms are reachable. **States its own bed PID gains**, matched to `SimulatedBed` rather than to a real 250 W heater; the shipped ones hang `M190` |

`gcovr` is required for coverage reports (`uv tool install gcovr` — `pip install --user`
is blocked by PEP 668 on this machine).

### Gotchas that have cost real time

- **Mutation cannot reach code in headers, and coverage can — so every killable score here is
  about the `.cpp` alone.** `mutation_test.py` looks its target up in `compile_commands.json`,
  and a header is not compiled directly, so it exits with
  `... is not in compile_commands.json — is it compiled into this env?`. It fails loudly, which
  is the one good thing about this: there are no silently wrong numbers, only unmeasured ones.

  But `gcovr` *does* report headers, with real figures — `core/mstring.h` 73%, `core/serial.h`
  88%, `gcode/parser.h` 81%. So inline logic in a header is coverage-visible and
  mutation-invisible, and **any figure in the rescue log of the form "closed at N% killable"
  covers the `.cpp` and says nothing about its header.** That qualification applies to targets
  already declared closed: `core/serial.cpp` at 100% killable sits beside `serial.h` (96 lines)
  and `serial_base.h` (79); `parser.cpp` at ~83% sits beside `parser.h` (303 lines, 26 branch
  constructs); `mstring.h` is where register #41 lived.

  Nine headers in this tree carry 25 or more branch constructs, and the ones that still matter are
  `runout.h` (368 lines), `parser.h` (303) and `temperature.h` (1159). Before quoting a target as
  closed, check whether its logic is in the `.cpp` at all.

  **`binary_stream.h` was the fourth, and it was moved rather than measured** — the whole protocol
  now lives in `binary_stream.cpp`, and the header is a class declaration plus one line. That is
  the cheaper answer whenever the code is only in a header by habit: relocation changes no
  behaviour and needs no tooling. Keep any generic form as a one-line forward to a single
  out-of-line definition, so call sites keep their compile-time checking and there is one copy to
  measure; `queue.cpp` needed no edit at all. See `docs/rescue-log/binary-transfer.md`, and expect
  the newly-visible one-liners to score badly — the accessors and resets that hide in headers were
  better than a third of that file's survivors.

  Fixing the runner instead is possible but not cheap, and is only worth it for a header whose
  logic genuinely belongs there. Only the *including* translation unit needs rebuilding per
  mutant, but Marlin's headers are included by relative path, so a shadow include directory does
  not override them — which leaves either serial in-place swapping (correct, far too slow) or a
  per-worker copy of the source tree (fast, about a gigabyte across the workers). Neither is done.

- **A loop the firmware bounds by elapsed time never ends under the test HAL, unless a test says
  polling costs something.** `Clock` moves only when `advance()` is called, and nothing inside a
  `while (PENDING(millis(), deadline))` loop calls it — so code that spins polling a port runs for
  ever here and terminates on a board. It presents as a hang rather than a failure, and under
  mutation every mutant that strands such a loop scores as a TIMEOUT, which counts as *detected*.

  `HAL_test_set_idle_poll_nanos(n)` makes `HalSerial::available()` charge an empty poll against
  the clock. Zero by default, so nothing changes for a test that does not ask. Switch it on for a
  whole test file whose subject busy-waits, not just the one test that needs it — well-formed
  input never reaches the spin, and it is the mutants you want to terminate. On `binary_stream.cpp`
  that moved 38 mutants from TIMEOUT to killed-by-assertion while the raw score stayed put.

  It advances time **without firing timer interrupts**, unlike `HAL_test_advance_micros()`. Reset
  to zero in `quiesce_simulated_peripherals()`, because the test most likely to leave it set is
  one that failed while spinning.

- **Anything the per-test teardown prints is written to a port nobody drains, and it hangs the
  suite eventually rather than immediately.** `SerialCapture` is the only thing draining
  `MYSERIAL1`, and it is scoped to a test — so `quiesce_simulated_peripherals()` runs bare. Adding
  a `card.mount()` there (it announces "SD card ok") filled the 128-byte transmit buffer a few
  bytes per test until `write()` spun for ever.

  It is a **slow fuse, not a race**: where it goes off depends on how much earlier tests printed,
  so it moved between builds and looked exactly like a timing bug. `make unit-test-coverage` hung
  for eleven minutes at a test that did nothing unusual while `testhal_native_test` stayed green,
  and the same coverage binary passed six standalone runs — to a file, through a pipe, with stdin
  closed, and under a pty. Two plausible theories (unguarded card I/O; the drainer thread of
  register #57) were both wrong.

  The teardown now marks the port as having no host attached for its own duration and restores
  what it found. **If a hang appears after adding anything to the teardown, comment that one line
  out before theorising** — it turned eleven minutes into eleven seconds and cost one build.

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
- **gcovr fails a whole run on a `negative_hits` parse error, and the cause is now known.**
  First seen as intermittent on 2026-08-13; it became reproducible the moment enough threaded
  tests were in the build, and the file is `HAL/TEST/hardware/Gpio.h:77` — `valid_pin()`, which
  inlines into every `WRITE`. Several fixtures here drive the simulated pins from a second
  thread (the `SerialCapture` drainer, the kill button, the answer to a blocking `M0`), gcov's
  counters are not atomic, and one increment underflows. That is GCC PR68080, and it is a
  permanent property of measuring a threaded suite rather than a fault that comes and goes.

  gcovr then abandons the **entire report** over a branch counter on one line of a HAL header,
  while every line figure in it is unaffected. `COVERAGE_PARSE_ERRORS` in the `Makefile` is
  `--gcov-ignore-parse-errors=negative_hits.warn_once_per_file`, which keeps the diagnostic —
  it names the file, so a new instance is still visible — without discarding the measurement.
  Scoped to that one failure mode deliberately: any other parse error should still be fatal,
  because any other parse error means the coverage data itself is not to be trusted.
- **`make unit-test-coverage UNIT_TEST_CONFIG=` takes the short name, not the file name.** The
  target it runs is `marlin_$(UNIT_TEST_CONFIG)`, so it is `dwin`, not `010-dwin` — the latter
  fails with `Do not know how to make File target 'marlin_010-dwin'` after doing the whole
  build, which is a slow way to find a typo.
- **`pio test -e <env> -f <name>` filters *test names*, not configurations.** It looks exactly
  like the config selector and is not one: `-f` is PlatformIO's test filter, so the build it
  runs uses whatever `Marlin/config.ini` happens to hold. Copying a config by hand first makes
  it *usually* right, which is worse than being obviously wrong — on 2026-08-13 two consecutive
  runs of supposedly different configurations both reported **792 tests**, which is the tell
  this file already records for a config that did not apply. The authoritative check is
  `pio run -t marlin_<config> -e <env>`, which runs `restore_configs` and regenerates before
  building. The same nine tests passed under `-f default` and then failed immediately under
  `marlin_default`.
- **`MAX_MESSAGE_SIZE` is 1 on a machine with no display.** The fallback arm of
  `Conditionals-2-LCD.h`: with no wired LCD and no status message there is nowhere to put a
  message, so the size is one character. Anything that takes it as a buffer length in a test —
  a status string, an expanded template — will assert that "Tool 0" comes out as "T" in the
  default build and something quite different under `010-dwin`. A test about a formatting *rule*
  should state its own length; a test about what fits on a display is a different test.
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
