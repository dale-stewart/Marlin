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
- **Keep a defect register from the first target, not from the tenth.** A marker in a
  test file records the behavior; it does not put the decision in front of anyone. Add
  a row the moment you write the marker — symptom, location, who it affects, and a
  status (open, by-design, blocked) — and separate genuine defects from deliberate
  design limits, since a long list of "quirks" trains the reader to skip all of it. The
  register is also where blocked design corrections from step 8 belong, so there is one
  place to look rather than three.
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

**Validate a replacement harness by reproducing a known result, not by passing its own
checks.** When you rewrite or speed up the runner, re-measure a target you have already
measured and compare the **survivor sets**, not the headline score. A baseline gate
cannot catch a misclassification, because a green baseline looks identical whether the
classifier is right or wrong — one runner rewrite reported a score off by more than
seventy points, and only set-equality against a prior run exposed it. Scores can agree by
coincidence; survivor sets cannot.

Build these checks into the runner itself rather than relying on discipline:

- **A baseline gate.** Run the unmutated suite first and refuse to start unless it is
  green. Without it, anything that breaks the build scores every mutant as killed.
- **Honest failure classification.** Distinguish "the tests failed" from "the build
  failed" by a signal that actually differs between them. Test runners often print a
  summary line on build errors too, so the presence of output is not proof a test ran.

**A flaky test makes the whole run meaningless — hunt it before you measure.** Mutation
scoring assumes the suite is a deterministic function of the code: a mutant is killed
because it changed behaviour, not because a test failed today. One test failing at random
scores mutants by coin flip, and because each mutant runs once, the noise is invisible in
the result — no re-run disagrees with anything. A baseline gate does not catch this
either: a 1-in-40 flake passes the gate 39 times out of 40.

So treat a known intermittent failure as blocking, not as background annoyance, and note
that scores measured before you found it were taken against a baseline you now know was
unreliable. Say so when you report them rather than quietly reusing the numbers. If the
flake turns out to be a genuine defect in code the tests depend on — a shared buffer, a
clock, the harness itself — that is the one case where fixing beats recording, since a
characterization test cannot pin behaviour that is not deterministic.

**Pin the build configuration for the whole run.** If the project's test tooling rewrites
config, checks out files, or restores a default, a mutation run started afterward
silently measures a broken build. Find out whether it does before the first run — this is
a common and well-hidden property of test targets. Re-apply the configuration immediately
before the run, and do not interleave other build targets while it is in flight.

**Sanity-check the arithmetic.** A score at or above 100%, or a survivor count of zero
where equivalent mutants are known to exist, means the harness is broken — not that the
suite is perfect.

**If a custom runner rebuilds the suite itself, make its build reproducible — and treat
"it worked when we captured it" as a countdown.** A runner that assembles its own binary
usually collects inputs by walking the filesystem, and that order is neither stable nor
the order the project's own build uses. Where the suite has any latent dependency on it —
initialisation order is the classic one — the runner is measuring whichever arrangement
the filesystem happened to hand it, and adding a file can change that at any time.

The failure is at least loud if a baseline gate stands in front of it: it refuses instead
of scoring a binary that no longer works. Without one, the same change quietly converts
every mutant into a kill. Either way, the fix is to remove the ordering dependency from
the suite, not to keep searching for an arrangement that happens to run — a lucky order
is a result you cannot reproduce, and reproducing results is the whole job. Then fix the
order in place, and **prove the independence** by running the suite under sorted,
reversed and several shuffled orders; "it passes now" is the claim that decays.

Expect more than one cause, and expect the loudest not to be the real one. Two classic
kinds turn up together:

- *Initialisation order.* Anything registering itself into a container defined in another
  compilation unit, at initialisation time, is a coin toss — the container may not exist
  yet. Constructing shared state on first use instead removes the question entirely.
  Baselines captured at startup (a clock's zero point, a seed) have the same flaw and the
  same remedy.
- *State a test leaves running.* A whole suite usually shares one process, so anything a
  test starts and does not stop is still running during every later test. Where the
  simulated peripheral is backed by a real operating-system facility — a timer, a thread,
  a signal — this stops being untidiness and becomes interference: a periodic signal
  interrupts blocking calls elsewhere, and a delay that is retried on interruption may
  never complete. Have the framework quiesce shared facilities after every test rather
  than trusting each test to tidy up.

Two traps in fixing the second kind. *Disabling is not stopping* — masking a signal or
clearing an enable flag often leaves the underlying facility running. And **quiesce in
the safe order**: silence delivery first, then tear down, because an event already queued
is still delivered afterwards, and a handler whose last act is to reschedule itself will
restart the very thing you just stopped. That one presents as a teardown that provably
runs and provably does nothing.

**Make the coverage run and the mutation run name the same suite, and check that the
default is the one you want.** Mutants are normally restricted to lines a coverage build
marked as covered, so these are two measurements that must agree about what they are
measuring. Where a project has several ways to execute its tests — several harnesses,
several configurations, several target selections — those two settings usually default
independently, and the default is often the oldest and least capable option rather than
the one the current work depends on.

A mismatched pair is not loud. Both halves run, the report has the right shape, and the
score is plausible; it just describes a suite in which the interesting code never
executes, and there *unasserted* and *unreachable* look identical. That is the one
distinction the whole exercise exists to make.

Prefer tooling that cannot express the mismatch — derive one setting from the other, or
have the run print the suite it measured next to the score. Wherever a headline figure
comes from a narrower filter than the default report, have the tool emit that figure too,
rather than leaving someone to reconstruct the filter by hand later.

**Timebox tool selection, and abort on toolchain contagion.** Compile-time instrumenters
(IR plugins, coverage-style passes) require *their* compiler across every translation
unit. If adopting one means changing the project's production toolchain — a different
compiler, a different standard library, or edits to production headers to satisfy the
tool — stop and switch to a source-level mutator that rebuilds with the project's own
toolchain. Slower per mutant, but it measures the code that actually ships.

**Scoring conventions**, stated up front so the number means something:

- Timeout = **killed**. A mutant that hangs is a mutant the suite detected.
- **Derive the timeout from the measured suite runtime, and record it beside the score.**
  Because a timeout counts as detected, one that is too tight converts survivors into
  false kills — and how tight it is depends on machine load, so a fixed constant makes
  the score depend on what else was running. Measure the unmutated suite at the start of
  every run and scale from that. Verify a suspected false timeout by building the mutant
  in directly and running it once: a mutant that completes in seconds and survives, but
  scores TIMEOUT under a parallel run, proves the threshold rather than the code.

  Two runs at different timeouts are no more comparable than two runs over different
  covered-line sets. Store both in the results, and print both next to the score.
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
- **Assert derived relationships, not recorded outputs.** The highest-yield tests state
  something that follows from the domain and would be hard to satisfy by accident: a
  known invariant, a conservation law, a scaling relation ("doubling the input doubles
  the elapsed time"), a round-trip that must return the original, an ordering that must
  hold. A test that records what the code currently returns passes for *any*
  implementation returning the same thing — including a wrong one — so it kills almost
  nothing.

  The symptom is **high line coverage with a low mutation score**, with survivors
  concentrated in code that computes *how* rather than *what*: intermediate values,
  timing, iteration counts, and the steps between a call and its result. That code runs —
  hence the coverage — but only its endpoint is observed. Ask what the code computes on
  the way, and assert a relationship the domain guarantees about it.
- **Separate "needs an assertion" from "needs an input" before writing anything.** A
  survivor on a line that never executes cannot be killed by any assertion, and no amount
  of rewriting will change that — coverage may still report the line as covered because
  the enclosing function ran, or because the tool attributes it to a covered region.
  Check which of the two you are looking at; guessing wrong costs a whole round. Branches
  guarded by a threshold no test input reaches are the usual case.
- Survivors cluster. One test often kills a whole family — if every mutation of an
  accumulator survives, the cause is usually a single missing input class, not a
  missing assertion per mutant. Fix the input gap, then re-measure before writing more.
- **There is a third category: code with no observable outcome at all.** Some code's only
  effect is to end the process, halt the machine, or hand control somewhere that never
  comes back. A test can reach it, but not survive it and assert afterwards, so *no*
  assertion kills those mutants — they are neither unasserted nor unreached. Suspect this
  wherever a probe **hangs or terminates the run** instead of failing.

  Prove it by probing rather than reasoning about it, then record the whole cluster with
  its measured cause and a count. It is a blocked seam, not a gap: what is wanted is a way
  for the terminal path to be observed and returned from under test, which is a production
  surface change and belongs behind the frontier.

  Check first whether the halt is the *system under test* behaving correctly or the
  *harness* being unfaithful. If the real system genuinely blocks there — waiting on an
  operator, a reset, a signal that only exists in production — then the substitute is
  being accurate, and the instrument-defect exception does not apply however inconvenient
  that is.
- **Do not convert survivors into timeouts to move the number.** Where a mutant can be
  detected only by hanging, a test that makes it hang is real but nearly worthless: it
  adds no killed-by-assertion and costs a full timeout on every future run of that target,
  slowing the loop permanently. Prefer leaving the survivor recorded and classified.
- **Look for an existing seam before adding one.** Platform or hardware abstraction
  layers, simulation and test builds, public state, configuration switches, and
  dependency-injection points already present for other reasons often provide what a test
  needs. A seam added where one already exists is production risk bought for nothing, and
  it is easy to add without noticing — looking takes minutes. Check any abstraction layer
  the project already has, check what is already public, and check whether the
  collaborator you are trying to fake is even active in the test build: code that never
  runs cannot overwrite what a test writes.
- Only when none exists, do the **minimum refactoring needed to make the code testable**:
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

**Scenarios belong to user-facing features, not to modules.** Write them at the system
boundary, about outcomes a user or client would recognise. Do **not** create a feature
file per rescued file: a leaf utility — a formatter, a parser helper, a maths routine —
earns no feature of its own and is exercised *through* the feature that uses it. If the
only way to describe a scenario is in terms of the module you just rescued, you are
writing a unit test in Gherkin syntax; keep it as a unit test.

**There is no one-to-one correspondence between scenarios and unit tests, and there
should not be.** The two suites answer different questions and are sized differently:

| | Unit tests | Scenarios |
|---|---|---|
| Pin | boundaries, edge cases, error paths, exact formats | outcomes a user would notice |
| Number | many per module | few per feature |
| Read by | whoever changes that module | anyone deciding what the system does |

What they owe jointly is **coverage of all the code, directly or indirectly**. A line
reached only through a scenario is covered; a line reached only by a unit test is
covered. Neither suite has to reach everything on its own, but between them nothing
should be left unexercised — that union is what step 7 measures.

**Write the outcome, not the mechanism.** The scenario text says what the user gets;
the step definitions do the translating. If a step reads like a function call with the
parentheses removed, push the detail down into the step.

    # Mechanism — belongs in a step definition, not in the feature
    When formatAmount is called with 12.345
    Then the result is "  12.35"

    # Outcome — what a user would actually observe
    When the balance reaches 12.345
    Then the statement shows the amount to two decimal places

- Use the domain's vocabulary. If a scenario cannot be written without naming a private
  class, it belongs at a lower level.
- Implement scenarios against the public surface with reusable steps; a step is the
  single place where domain language becomes an API call.
- Any `LEGACY-BEHAVIOR:` marker from step 1 that survives to here is a question for
  the user: intended behavior or long-lived bug?

Exit gate: scenarios reviewed by the user; acceptance suite green.

## Step 7 — Validate the acceptance suite on its own

Re-run coverage **and** mutation testing with the unit tests excluded, so only the
Gherkin-based suite is exercised.

This is the load-bearing check for anything you intend to restructure in step 8: it
shows how much of the behavior the scenarios describe by themselves, which is what
makes a refactor safe. **Measure the rescued code's coverage under the acceptance suite
even when no scenario mentions it** — a utility reached indirectly through a feature is
still protected by that feature, and that indirect reach is exactly what you need to
know before restructuring it.

Read the result by what it protects, not against a fixed number:

- **A gap in behavior a user would notice** is a missing scenario. Add it; do not
  backfill with a unit test.
- **A gap in an internal edge case** — an overflow format, a defensive branch — is
  legitimately unit-test territory. Leave it there and record that the scenarios do
  not cover it, because that is precisely the part step 8 cannot lean on.
- Code that no scenario reaches even indirectly is either dead, or a feature nobody
  has described yet. Find out which.

Exit gate: the union of both suites meets the step 5 bar, and you know which parts of
the target the scenarios protect on their own.

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

## Delegating work to subagents

A rescue runs long enough to exhaust a context window, and the parts that exhaust it
fastest — chasing a hang, killing a batch of survivors — are the parts that benefit
least from carrying the whole history. Hand those out.

**Delegate when the work is bounded and verifiable by running something.** Debugging a
hang, killing the survivors of one target, writing tests for one behaviour. Do not
delegate the judgement calls: which target is next, whether a survivor is equivalent,
whether a surface change is safe to make.

### Write the brief so a wrong answer is cheap

- **Give evidence, not conclusions.** State the observations — the exact commands, the
  values seen, the state the system was in — and mark any diagnosis as *to be verified,
  not trusted*. A confident wrong theory in a brief is worse than no theory: the agent
  spends its budget defending yours instead of looking. Expect this to happen; a recorded
  diagnosis that has never been tested is a hypothesis wearing a fact's clothing, and the
  agents that ignore one are often the ones that find the real cause.
- **List what has been ruled out**, so the agent does not re-run your dead ends.
- **Make the definition of done a set of values**, not "it stops failing" — exact
  expected results, an exact test total in each environment. "The suites are green"
  invites an agent to weaken an assertion until they are.
- **State the harness gotchas up front.** Buffered output hiding where a hang really is,
  a runner that prints a summary line even when the build failed, an exit code that lies
  about test failures. Each of these costs hours to rediscover; each is one sentence in a
  brief. Keep the project's list somewhere durable and paste the relevant ones in.
- **Say what must not change** — the file the fix must not reach for, the suite that
  must not move, and "do not commit" so you can review the diff.

### Isolate any agent that builds

**Concurrent agents that build need their own worktree.** Dividing the files between
them is not enough: the build directory is a shared mutable resource, and two agents
that never touch the same source still overwrite each other's binary, interleave their
config-restoring build hooks, and break each other's builds in ways that look like their
own last edit.

The damage is worst where it is hardest to see. An agent measuring an intermittent fault
over hundreds of runs, while another rebuilds the binary underneath it, produces a number
that is not about anything — and it will not know. This is the same failure as a flaky
mutation baseline, one level up: a measurement whose denominator moved during the count.

If an agent must share a tree, have it copy the built binary somewhere private and
measure the copy. Prefer separate worktrees; only agents that purely read are safe to
share one.

**A private worktree is not isolation.** Concurrent agents still share the CPU, the disk
and the system temp directory, and each of those has produced a wrong answer here:

- *The CPU.* Any measurement with a wall-clock threshold in it — a timeout, a
  benchmark, a flake count — changes value under load. Two agents each running a correct
  measurement will each get a different number, and neither will know why. Take timing
  measurements on a quiet machine, or derive the threshold from a baseline measured in
  the same conditions.
- *The disk.* Tooling that generates a file per case can be several GB per run, and per
  worktree. Filling the volume fails whatever is running, not whoever caused it.
- *The temp directory.* An agent tidying up `/tmp/<tool-prefix>*` will delete another
  agent's in-flight scratch space. The victim sees inexplicable build failures in
  unrelated code.

So tell agents that others are running, that shared scratch space is not theirs to clean,
and to report — not silently absorb — a result that moved for no reason they can name.
Where a measurement must be trustworthy, re-take it once alone at the end; that final run
is the number to publish.

**Check what base an isolated agent actually started from, and say so in the brief.** An
isolation mechanism may branch from a default or upstream commit rather than the work in
progress, which drops the agent into a tree without the harness, fixtures, or tests the
task depends on. Two agents in one session each landed on a base months behind and had to
reset before starting. State the commit the work builds on, and have the agent confirm
its baseline numbers match yours *before* it changes anything — a baseline that disagrees
with the brief means the tree is wrong, not that the brief is.

**Quote a baseline with the command that produced it, never as a bare number.** That last
rule turns any ambiguity in the number into a false alarm, and suites usually have more
than one axis to vary — the environment, the configuration, the target selection. A count
that is exact along one axis and silently different along another will read as a corrupt
tree to an agent that reached it by a reasonable route. Write the invocation, not just the
total, and say which axis it holds fixed.

### Verify before you relay

Read the diff, run the suites yourself, and check that no assertion was weakened and no
forbidden file was touched. An agent's report is a claim. Relaying it unverified launders
a claim into a fact, and a rescue's only product is trustworthy measurement.

Watch specifically for an assertion that has become **self-consistent rather than
correct** — checking a result against the same accessor the code under test used to
produce it. It passes, it looks like a real assertion, and it constrains nothing.

**Re-run the agent's evidence, control first.** When an agent claims to have fixed an
intermittent fault, build the unfixed version too and confirm *your* harness reproduces
it before trusting a clean run of the fixed one. A green run only means something once
you know the test can go red. This is step 3's harness-validation rule applied to
someone else's result, and it is where a plausible non-fix gets caught.

### Keep an agent definition, not just a prompt

Recurring roles belong in a file (`.claude/agents/*.md`) so the working rules — reproduce
before theorising, suspect your own scaffolding first, change one thing per build,
timeouts short enough that a failure is cheap information — are stated once. Two roles
recur in almost every rescue and are worth defining early: a **harness debugger** for
hangs and environment differences, and a **mutant killer** for survivor batches. Note
that agent definitions typically load at session start, so a newly written one may not be
selectable until the session restarts; inline the rules that first time.

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
