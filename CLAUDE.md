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
move peaks at its midpoint; acceleration and deceleration take equal time; the applied PID
gains are the Ziegler-Nichols relations of the measured `Ku` and `Tu`; the autotune relay
levels are a mirrored pair inside the power limits. Asserting a final step count instead
is what left `stepper.cpp` at 87.8% line coverage and 28.8% mutation detection. See
`Marlin/tests/module/test_step_timing.cpp` and `test_pid_autotune.cpp`.

**"Separate needs-an-assertion from needs-an-input."** `MULTISTEPPING_LIMIT` is 16, so
`stepper.cpp:2442` needs `steps_per_isr >= 16` — sixteen pulses inside one interrupt —
before any assertion can touch it. Roughly 3200 steps/mm at 200 mm/s gets there; the
current tests reach 2× or 4× and leave ~69 mutants alive that no assertion can kill.

**"Watch for an assertion that is self-consistent rather than correct."** An acceptance
test once asserted that the `M105` reply contained the formatted value of
`thermalManager.degHotend(0)` — the same accessor `M105` formats its output from. It
would have passed with the sensor pipeline delivering any value at all. Assert against
`SimulatedSensors::hotend_would_read()`, which predicts independently of the pipeline.

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
varies the *config* and holds the env fixed: it runs `linux_native_test` alone against all
three configs in `test/`, reporting **377, 378, 378**. The counts above vary the *env* and
hold the config fixed. Give an agent "377" as a baseline without saying which, and a
correct tree reports a mismatch.

## Test, coverage, and mutation tooling

```bash
make unit-test-all-local                    # linux_native_test only, over all 3 configs in test/
make unit-test-coverage                     # one suite + gcov/gcovr report
make unit-test-mutation TARGET=<file.cpp>   # mutation-test one source file
pio run -t marlin_default -e acceptance_native_test        # acceptance suite alone
pio run -t marlin_default -e acceptance_native_coverage    # ... with coverage
```

**Coverage and mutation must name the same suite, and neither defaults to the one that
matters.** `COVERAGE_ENV` and `MUTATION_ENV` both default to LINUX, where motion,
blocking commands and the temperature ISR do not run. Measuring anything under `testhal`
means saying so twice:

```bash
make unit-test-coverage COVERAGE_ENV=testhal_native_coverage
make unit-test-mutation TARGET=<file.cpp> MUTATION_ENV=testhal_native_test
make unit-test-mutation TARGET=<file.cpp> MUTATION_ENV=testhal_native_test \
     RERUN=.pio/mutation/results.json      # MUTATION_ENV again, or this reverts to LINUX
```

Getting this wrong is not loud. A LINUX/LINUX pair is self-consistent and produces a
perfectly plausible report — of a suite where most of the interesting lines never
execute, so unasserted lines and unreachable lines become indistinguishable. The headline
figures in `docs/` are all `testhal`.

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
| `linux_native_coverage` | `linux_native_test` + gcov instrumentation |
| `acceptance_native_test` | acceptance suite only, unit tests excluded |
| `acceptance_native_coverage` | the same, with coverage |
| `testhal_native_test` | unit tests against `HAL/TEST` — time advances only on request |
| `testhal_native_coverage` | the same, with coverage; the only env that measures motion and blocking commands |

`gcovr` is required for coverage reports (`uv tool install gcovr` — `pip install --user`
is blocked by PEP 668 on this machine).

### Gotchas that have cost real time

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
- **Object link order matters, and the mutation runner's order is not reproducible.**
  The test binary carries a latent static-initialisation-order dependency, and
  `mutation_test.py` collects objects with `rglob('*.o')` — filesystem order, which
  changes when test files are added. Measured on `linux_native_test`: sorted order
  **segfaults before the first test**, current filesystem order **hangs after 355 of
  377**, and only PlatformIO's own order completes. So **mutation testing under
  `linux_native_test` does not currently run** — the baseline gate refuses rather than
  scoring a broken binary, which is it working as intended. `testhal_native_test` is
  unaffected and is where every current figure comes from. Register entry #20; the fix
  is to remove the dependency, not to chase a lucky order.
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
