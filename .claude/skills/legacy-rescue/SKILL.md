---
name: legacy-rescue
description: "Bring untested legacy code under meaningful test cover, then refactor it safely: seed tests → coverage baseline → mutation testing → kill mutants → Gherkin acceptance tests → SOLID/hexagonal refactor"
user-invocable: true
argument-hint: '[target] - Module, package, directory, or file to rescue (e.g. "src/billing" or "parser.cpp")'
---

# LEGACY-RESCUE: Get Legacy Code Under Test, Then Refactor

Language- and stack-agnostic workflow for code that has little or no test cover.
The order is non-negotiable: **cover first, refactor last**. Refactoring untested
code is not refactoring — it is undirected editing.

## When to use

- A module has low or unknown coverage and needs to change.
- Tests exist but pass regardless of whether the code is correct.
- A rewrite is being considered but behavior is not documented anywhere.

## nWave integration

If the nWave skills/agents are installed (look for `nw-*` in the skills list or
`~/.claude/skills/nw-*`), prefer them over ad-hoc work, and run **`/nw-buddy`**
first to confirm which agent applies to the step at hand. Typical mapping —
verify with the buddy rather than assuming:

| Step | nWave skill / agent |
|---|---|
| 3-5 mutation + test generation | `/nw-mutation-test`, `nw-acceptance-designer` |
| 5 test-suite hygiene | `/nw-optimize-tests`, `nw-test-optimizer` |
| 6 Gherkin scenarios | `/nw-distill`, `nw-acceptance-designer` |
| 8 refactoring | `/nw-refactor` (RPP L1-L6), `/nw-mikado` for large moves |
| Investigating opaque behavior | `/nw-root-why`, `nw-troubleshooter` |

If nWave is absent, execute the steps directly with the project's own tooling.

## Step 0 — Scope and inventory (always do this first)

Never run this workflow against a whole repository. Pick a **bounded target**:
one module, package, or file cluster with a describable responsibility.

Record before touching anything:

- Build, test, coverage, and mutation commands for this stack.
- The target's public surface (entry points, exported functions, CLI/HTTP/queue adapters).
- Its dependencies on I/O, clocks, randomness, globals, and hardware.
- Baseline coverage, if already known.

Report the plan and the target, then proceed.

## Step 1 — Establish a seed test suite

Goal: any harness that runs and can fail. Do not aim for good tests yet.

- Reuse the project's existing framework and runner if one exists.
- Write **characterization tests**: assert what the code *currently does*, not what
  it should do. Capture the observed output even if it looks wrong, and mark
  suspicious assertions with a `LEGACY-BEHAVIOR:` comment for later review.
- If nothing is callable without heavy setup, test the outermost entry point first
  and work inward. A slow, ugly end-to-end test beats no test.

Exit gate: the suite runs green from a clean checkout, in CI-equivalent conditions.

## Step 2 — Measure the coverage baseline

- Wire up the stack's standard coverage tooling; commit the configuration so the
  measurement is reproducible.
- Record line, branch, and function coverage. Branch coverage matters most here.
- State the **denominator** explicitly — coverage of the compiled/loaded subset is
  not coverage of the repository. Report both when they differ.

Exit gate: a committed command that regenerates the report, and a recorded number.

## Step 3 — Set up and run mutation testing

Coverage proves execution; mutation proves assertion. Expect the mutation score to
be far below the coverage number, and treat that gap as the real backlog.

- Select a mutation tool for the language; if none is mature, use a
  language-agnostic source mutator, or hand-seed faults in the highest-risk paths.
- Scope mutants to the target only. Set a timeout per mutant of ~2-5x the suite's
  normal runtime to catch infinite loops.
- Record: mutants generated, killed, survived, timed out, and not-covered.

**Validate the harness before trusting any score — every harness, every time.** Hand-inject
an obvious fault and confirm the suite fails with a non-zero exit. A harness that silently
fails to rebuild, or a tool whose mutants never reach the binary, reports a perfect score on
no evidence. Pick a fault the tests must catch — an off-by-one in a value the tests assert
on, not one that is arithmetically inert for the inputs they use. A second runner, a new
environment, or a changed test scope is a **new** harness and needs its own validation.

Build these two checks into the runner itself rather than relying on discipline:

- **A baseline gate.** Run the unmutated suite first and refuse to start unless it is
  green. Without it, anything that breaks the build scores every mutant as killed.
- **Honest failure classification.** Distinguish "the tests failed" from "the build
  failed" by a signal that actually differs between them. Test runners often print a
  summary line on build errors too, so the presence of output is not proof a test ran.

**Pin the build configuration for the whole run.** If the project's test tooling rewrites
config, checks out files, or restores a default (Marlin's `restore_configs` does all
three), a mutation run started afterward silently measures a broken build. Re-apply the
configuration immediately before the run and do not interleave other build targets while
it is in flight.

**Sanity-check the arithmetic.** A score at or above 100%, or a survivor count of zero
where equivalent mutants are known to exist, means the harness is broken — not that the
suite is perfect.

**Timebox tool selection, and abort on toolchain contagion.** Compile-time instrumenters
(IR plugins, coverage-style passes) require *their* compiler across every translation
unit. If adopting one means changing the project's production toolchain — a different
compiler, a different standard library, or edits to production headers to satisfy the
tool — stop and switch to a source-level mutator that rebuilds with the project's own
toolchain. Slower per mutant, but it measures the code that actually ships.

**Scoring conventions**, stated up front so the number means something:

- Timeout = **killed**. A mutant that hangs is a mutant the suite detected.
- Build failure = **not a mutant**. Exclude it; it never produced a testable program.
- **Equivalent mutants belong outside the denominator.** Watch for the systematic
  source: code disabled at build time — `#if`/`#ifdef`, ternaries on compile-time
  constants, feature flags, dead generics. Line-coverage tools mark those lines as
  executed and text-level mutators happily mutate them, but the branch cannot run in
  this configuration, so the mutants are unkillable by construction. Identify them
  from the config, report them as a separate class, and quote both the raw and the
  adjusted score.

Exit gate: a reproducible mutation command, a validated harness, and a survivor list
partitioned into real gaps and equivalent mutants.

## Step 4 — Kill the mutants

For each survivor, in descending order of risk:

- Write a test that fails against the mutant and passes against the original. If
  you cannot express one, the survivor is telling you the behavior is unspecified —
  decide with the user whether it is dead code or a missing requirement.
- **Equivalent mutants** (semantically identical to the original, including anything
  behind a disabled build flag) cannot be killed. Document each one with a reason; do
  not contort tests to chase them, and never enable a feature flag purely to make a
  mutant killable.
- Survivors cluster. One test often kills a whole family — if every mutation of an
  accumulator survives, the cause is usually a single missing input class, not a
  missing assertion per mutant. Fix the input gap, then re-measure before writing more.
- Only now, do the **minimum refactoring needed to make the code testable**:
  - Break hard dependencies by introducing a seam (parameter, interface, factory,
    or injection point) at the call site.
  - Invert dependencies on I/O, time, randomness, network, filesystem, and hardware
    so a test can substitute a fake.
  - Do not rename, reorganize, or "clean up" anything else at this stage.
  - Each seam is its own commit, separate from test commits.

Exit gate: every survivor is killed, documented as equivalent, or logged as an
open question.

## Step 5 — Re-measure and loop

Re-run coverage. If line coverage is below **95%** for the target, or the mutation
score is still weak, return to **step 3** with the newly uncovered regions.

Guard against the failure mode this loop invites: tests that raise the number
without testing behavior. Reject any test that

- asserts only that a call did not throw,
- asserts on mocks rather than results (unless the interaction *is* the contract),
- restates the implementation line by line,
- or would still pass if the function body were replaced with its output constant.

Exit gate: ≥95% line coverage on the target **and** a mutation score the team
accepts, with survivors accounted for.

## Step 6 — Extract Gherkin scenarios and build acceptance tests

Now that behavior is pinned, describe it in the domain's language.

- Derive `Given/When/Then` scenarios from the characterized behavior, one per
  externally meaningful outcome. Write them at the **port boundary** — user- or
  client-visible behavior, not internal function calls.
- Use the business vocabulary, not the code's vocabulary. If a scenario cannot be
  written without naming a private class, it belongs at a lower level.
- Implement them against the public surface with reusable steps.
- Any `LEGACY-BEHAVIOR:` marker from step 1 that survives to here is a question for
  the user: intended behavior or long-lived bug?

Exit gate: scenarios reviewed by the user; acceptance suite green.

## Step 7 — Validate the acceptance suite on its own

Re-run coverage **and** mutation testing with the unit tests excluded, so only the
Gherkin-based suite is exercised.

This is the load-bearing check: it proves the acceptance tests alone describe the
full behavior, which is what makes the step 8 refactor safe. Gaps here mean a
behavior exists that no scenario describes — add the scenario, do not backfill
with a unit test.

Exit gate: acceptance-only coverage and mutation results meet the step 5 bar.

## Step 8 — Refactor

Only once step 7 passes. The acceptance suite is the safety net; keep it green and
unmodified throughout — if a refactor requires changing a scenario, the refactor
changed behavior.

### The test frontier bounds the blast radius

**Refactor freely inside the target. Do not change its public surface until the code
that calls it is itself covered and mutation tested.** Editing hundreds of untested
call sites is refactoring untested code at one remove — the same mistake the whole
workflow exists to prevent, just displaced onto the consumers.

This is what makes cross-cutting corrections — removing global mutable state, undoing
a singleton, replacing a leaky type that appears in a thousand signatures — a
**sequenced migration**, not a refactor:

1. Rescue the target. Refactor its internals behind the existing API.
2. Record the surface change you want as a tracked follow-on, blocked and stated
   plainly, with the reason it is blocked.
3. Rescue each consumer through the same workflow.
4. Only when a consumer is covered may its calls move to the new surface.
5. Change the surface when the last consumer is ready, or introduce the new API
   alongside the old and retire the old as consumers arrive.

Do not let a blocked surface change become an argument for skipping the cover-first
rule "just this once." The ordering is what makes the correction safe; the correction
is still wanted, and saying so in the follow-on note keeps it from being forgotten.

Work in small, individually reverting commits, in this order:

1. **DRY / L1-L2**: remove duplication, dead code, and misleading names.
2. **SOLID**: single responsibility, then dependency inversion on the seams from
   step 4; extract interfaces where more than one implementation genuinely exists.
3. **Patterns**: apply only where a named force is present. A pattern introduced
   without a problem to solve is added coupling.
4. **Hexagonal architecture**: pure domain at the center; push I/O, frameworks, and
   third-party types out to adapters behind ports the domain owns.

Run the acceptance suite after every commit; run mutation testing at the end to
confirm the net still catches faults in the restructured code.

## Guardrails

- **Never** refactor ahead of cover. If a step-8 opportunity appears during step 4,
  write it down and continue.
- Behavior is preserved by default. A discovered bug is reported to the user, not
  silently fixed — fixing it invalidates the characterization tests that protect it.
- Keep test commits separate from production-code commits.
- Report numbers honestly, including the denominator and anything skipped. A
  coverage figure without its scope is a misleading figure.
- Timebox the loop in step 5. If the score plateaus, surface the remaining
  survivors and let the user decide.
