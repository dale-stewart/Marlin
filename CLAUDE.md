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

### Known blocked correction

`GCodeParser` is a static-only class with global mutable state (`parser.codenum`,
`parser.codebits`, `parser.string_arg`), read directly across the codebase. Removing
that global state is wanted, and is **blocked** until its consumers are rescued.
`Marlin/src/gcode/parser.cpp` itself has been rescued (95% line coverage, ~83% mutation
detection excluding equivalents, 18 acceptance scenarios).

## Test, coverage, and mutation tooling

```bash
make unit-test-all-local                    # all three suites in test/*.ini
make unit-test-coverage                     # one suite + gcov/gcovr report
pio run -t marlin_default -e acceptance_native_test        # acceptance suite alone
pio run -t marlin_default -e acceptance_native_coverage    # ... with coverage
```

Environments added by this fork, in `ini/native.ini`:

| Env | Purpose |
|---|---|
| `linux_native_coverage` | `linux_native_test` + gcov instrumentation |
| `acceptance_native_test` | acceptance suite only, unit tests excluded |
| `acceptance_native_coverage` | the same, with coverage |

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
- **Mull is not viable here.** Its IR plugin forces clang across the whole build, which
  needs `-stdlib=libc++`, which then rejects Marlin's own `types.h` and `temperature.h`.
  Use a source-level mutator (`universalmutator`) that builds with the project's own
  toolchain.
- **Mutation runs take ~20 minutes per target** at this size. That, not test-writing, is
  the binding constraint at codebase scale.
- **`preflight-checks.py` gates env/board compatibility.** Envs whose names end in
  `_native_test` or `_native_coverage` are exempt, because the test targets rewrite the
  board per suite. New measurement envs should follow that naming.
