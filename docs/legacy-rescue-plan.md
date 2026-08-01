# Applying legacy-rescue to the rest of Marlin

A sequencing plan for `.claude/skills/legacy-rescue/`, based on measurements taken while
rescuing `Marlin/src/gcode/parser.cpp`. See `CLAUDE.md` for why this fork exists and for
the ordering rule that governs refactors.

## Two facts that shape everything

**Only 80 of 1,016 `.cpp` files compile into the native test build** — about 4,600 lines.
Everything else is unreachable by any test until new configuration profiles or HAL fakes
pull it in. "The remaining code" is therefore two problems: rescuing what is testable,
and expanding what is testable.

**The measurement loop is too slow to scale.** `parser.cpp` — 62 compiled lines — took
eight mutation runs at roughly 20 minutes each. Extrapolated to 4,600 lines that is on
the order of 150 hours of mutation compute, before any tests are written. This is the
binding constraint, and it is fixable once rather than paid 74 times.

## Phase 0 — Make the loop affordable

Everything else is gated on this.

| Work | Why | Expected gain |
|---|---|---|
| Parallel runner (N workers, isolated build artifacts) | runs are serial today | ~5-8x |
| Pre-filter non-compiling mutants with a syntax-only pass | 248 of 446 mutants (56%) did not compile; each cost a full build, link and run | ~2x |
| Restrict the mutator set to semantic operators | much of that 56% is invalid C++ the text mutator emits | fewer wasted mutants |
| Incremental mode — mutate only lines changed since the last run | re-running survivors should be the default, not a manual step | large on iteration |
| Promote the runner out of scratch into the repo with a `make` target | it holds load-bearing logic (baseline gate, failure classification) | reproducibility |

**Exit gate:** a full measurement of one target in under five minutes, with the runner
committed and subject to the same rules it enforces.

## Phase 1 — Leaf utilities

`libs/numtostr` (270 uncovered lines), `lcd/utf8` (81), `libs/vector_3`, `libs/crc16`,
`libs/stopwatch`, `core/utility`.

Pure functions, one to three includes, no hardware. `numtostr` alone is about 6% of the
uncovered compiled mass and is pure string formatting. These are cheap wins that prove
out the Phase 0 tooling and build fluency with the acceptance-suite idiom.

Roughly six targets, low risk.

## Phase 2 — G-code handlers

The ~30 `gcode/*` files already in the build, then `gcode/queue.cpp` (171 uncovered) and
`gcode/gcode.cpp` (163).

These are `GCodeParser`'s consumers. Under the test-frontier rule, the blocked
global-state correction becomes legal exactly when they are covered. `parser.h` has a
fan-in of 59 files, about 30 of which are in the compiled set, so this phase clears most
of it and Phase 4 finishes the rest.

**Milestone:** the first blocked surface change becomes unblocked.

## Phase 3 — Core modules

| Target | Uncovered lines | Commits in 2 years |
|---|---|---|
| `module/temperature.cpp` | 501 | 53 |
| `module/planner.cpp` | 377 | 53 |
| `module/stepper.cpp` | 230 | 70 |
| `module/motion.cpp` | 219 | 38 |
| `module/settings.cpp` | — | 42 |
| `module/endstops.cpp` | 62 | — |

Highest churn and highest untested mass — the best regression value in the repository,
and the only phase that needs real step-4 seams (clock, ADC, GPIO, stepper ISR). Fan-in
is severe (`motion.h` 162, `planner.h` 151, `temperature.h` 124), so these surfaces stay
frozen for a long time. Budget the most time here; expect seam work to dominate.

## Phase 4 — Expand the frontier

Add configurations beyond `test/001-003` to pull in SD, LCD, bed leveling, probes and
TMC drivers, with HAL fakes as needed. Each configuration is a separate coverage
denominator and a separate mutation run, so cost multiplies — which is why this follows
Phase 0 and the cheap targets.

## Cross-cutting: a blocked-corrections register

`CLAUDE.md` records one blocked correction today. Phases 2 and 3 will generate more
(`MarlinCore` singleton, `Temperature` and `Planner` globals). They need a single list
with the unblock condition stated per entry, or they will be forgotten — the failure
mode the skill explicitly warns about.

## Per-target gates

Unchanged from the skill: line coverage at or above 95%, mutation detection at or above
80% excluding equivalents, acceptance-only parity with the unit suite, and every
survivor killed, documented as equivalent, or logged as an open question.

## Caveats

- Phase 3 may not reach 95% without seams that amount to surface changes, which are
  themselves blocked by fan-in. Expect to discover that some modules cannot be finished
  until their consumers move, and to re-sequence.
- About 936 files remain unreachable after all four phases without substantial HAL
  simulation. A complete rescue is a multi-quarter effort; this plan covers the
  load-bearing code.
- The estimate that matters is not file count. It is whether Phase 0 makes the following
  phases cost 20 hours of compute or 150.
