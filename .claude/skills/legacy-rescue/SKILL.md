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
- **State a *failing* test leaves behind turns kills into timeouts.** Many frameworks
  abandon a failed test by jumping out of it rather than returning, which skips the
  test's own cleanup and any scope-based teardown it was relying on. Under mutation that
  is not merely untidy: the mutant is detected, the assertion fires, and then the state it
  left hangs something later — so the run records a timeout instead of a kill. The score
  is unchanged and the diagnosis is inverted.

  The signal is a round of new tests where the total moves but the killed count does not.
  If that happens, suspect the teardown before you suspect the tests. Put the reset
  somewhere the jump cannot skip — the harness, after the test returns — rather than in
  the tests themselves, and re-measure: kills and timeouts should trade places.
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
- **Read the diagnostic output the code already produces — for its properties, not its
  layout.** Code that reports on itself often computes exactly the quantity a test needs and
  then prints it: residuals, deviations, coefficients, counts. Those are the terms the
  domain is stated in, and they are already there. Asserting a *property* of them — every
  residual is zero, the reported coefficients equal the inputs the fixture was built from —
  is a derived assertion that costs one test and reaches code no ordinary path does.

  It matters that it is the property and not the format. Pinning the layout of a diagnostic
  freezes something nobody depends on and breaks on every cosmetic change; pinning the
  property survives reformatting and still fails when the numbers are wrong. Here one such
  test raised a target from 37% to 54% on its own, because a whole reporting path had been
  reachable but unasserted.
- **An assertion written to be independent of a convention is also blind to that convention
  being wrong.** Comparing a case with itself — this input against its opposite, this run
  against the same run with one setting changed — is the right way to avoid encoding a
  configuration constant in a test. It also cannot see the whole thing being inverted,
  reversed, or offset consistently, because both sides move together.

  So pair every such comparison with one absolute claim, made against whatever independently
  models the real world — a fixture, a physical quantity, a signed position. "Opposite inputs
  give opposite outputs" and "a positive input gives a positive output" are different
  statements, and mutation testing distinguishes them immediately: the sign-flip mutant
  survives the first and dies to the second.
- **A value can be computed, correct, and then discarded.** Where two limits are combined by
  taking the smaller — a clamp, a `min`, a cap applied on top of another — the losing one has
  no effect at all. Its line is covered, its arithmetic runs, and every mutation of it survives
  because nothing downstream can tell. This is not "unasserted": no assertion can reach it
  while the other limit is lower.

  Test for it directly. Compile the branch out, or force its result to something absurd, and
  re-run the test you wrote for it. If the test still passes, it was never testing that code —
  and the finding is that the value is masked, which is worth recording with the condition that
  would unmask it. A test whose subject is a branch it does not depend on is worse than no test,
  because it reads as coverage of exactly the thing nobody has checked.
- **Some inputs cannot be supplied one at a time.** A survivor that needs the code to be in a
  particular *régime* — a buffer partly drained, a cache warm, a rate high enough to saturate
  something — is not reached by making one call with extreme arguments. Extreme arguments often
  put the code in a different régime instead: ask for a single very fast, very short operation
  and some other limit dominates, so the value under test is computed and then never used.

  What reaches these is a *sequence*: several operations queued together, so the state the
  branch reads is the state the earlier ones left. Budget for that — it is a fixture that
  drives the system into a régime and holds it there, not another parameter on an existing
  helper. Recognising it early saves a round of tests that look reasonable and kill nothing.
- **Equivalence is often a property of the configuration, not of the code.** A comparison whose
  operands can only take two values, a guard on a constant no build actually uses, a branch in
  an arm the current options compile out — all produce mutants that no test can kill *here*,
  while being perfectly killable in another configuration. That is a legitimate stopping point,
  but it is a claim about this build and should be recorded as one.

  Say which configuration made them equivalent. "All twelve comparison mutants collapse because
  every axis homes to its minimum" is a fact a reader can act on: it names the variant that
  would distinguish them, and it stops the next person re-deriving it. "Equivalent" alone does
  not.

  The *type* does this as often as the configuration. Half the relational mutants of a
  comparison against zero are unkillable when the operand cannot be negative, and the same
  goes for a range check on a value the type already bounds. Neither is a gap in the tests,
  and neither is worth a second attempt once it has been named.
- **A source-level mutator edits text; check the text still means something different.** Where
  a language has a preprocessor, macro system, or code generator between the source and what
  runs, a mutant can be erased before it reaches the compiler. Changing an argument that is
  pasted into a token, or a constant that a generator normalises, produces a file that differs
  on disk and compiles to the same program — so it survives every test that could ever exist.

  These arrive in clusters on the generated-looking lines, and they are recognisable by what
  the mutant edits: if every survivor on a line changes only the *inputs to a macro* and none
  changes the operation, suspect expansion rather than a gap in the tests. Settle it by
  expanding one — a throwaway test that runs the mutated expression and prints what it did
  costs a minute. Do not infer it from reading the macro; the paste rules are exactly where
  intuition fails.
- **Ask the build what it compiled, do not read it off the configuration.** Related, and the
  cheaper mistake to make: a conditional block that looks disabled in the settings file may be
  enabled by a default, a dependency, or another option's side effect. Classifying survivors as
  "dead arms of a disabled feature" on the strength of the settings is a claim about the build
  that has not been checked, and it is wrong often enough to matter. Print the resolved constant.
- **A counter the system re-bases cannot measure the movement it re-bases.** Plenty of code
  keeps its own bookkeeping — a position, an offset, a sequence number, a running total — and
  resets or re-references it partway through an operation, precisely so the rest of the
  operation can be expressed in the new frame. Reading that bookkeeping afterwards tells you
  where the system *thinks* it is, which is exactly what you wanted to verify and therefore
  exactly what you cannot use as evidence.

  The tell is an assertion that reports no change when you can see work happening: the counter
  was moved forward by the re-base and back by the operation, and the two cancel. The instrument
  has to be something outside the system's control that only accumulates — an observer counting
  the events themselves rather than reading a total the code is free to rewrite.
- **A survivor may mean the test exists but not in the build you measured.** Where a suite is
  compiled per configuration, a test excluded by a build guard does not fail, does not appear,
  and does not run — so its subject shows up in the report as unasserted. The mutation score is
  a property of *one* build, and every test guarded out of that build is invisible to it.

  Check this before writing anything: search the whole suite for the behaviour, not just the
  files that compiled. If the test turns out to exist elsewhere, you have learned something
  more useful than a new test — the measurement understates the suite, and writing a duplicate
  would have hidden that rather than fixed it. The fix is usually to make the basic case
  testable in the measured build, since a behaviour reachable only under an optional feature is
  a behaviour whose common path nobody is checking.
- **When the only thing a branch changes is the order, the sequence is the assertion.** Some
  branches decide *when* work happens rather than whether or what: do this part first, defer
  that until after, handle these in priority order. Every ordering finishes in the same final
  state, so every assertion about the result passes against all of them — and the ordering is
  usually the whole point, because it is what holds while the operation is only half done.

  That needs an instrument that records the sequence, not the outcome: a log of which
  subsystem acted when, keyed on something each step already emits. Build it once and it
  serves every ordering question in the codebase. The assertion to reach for is a *relation*
  between two spans — "A had finished before B began" — rather than any absolute time, because
  the durations move with unrelated settings and the ordering does not.
- **A negative assertion is satisfied by every cause of nothing.** "No work was done", "nothing
  was sent", "the collection is still empty" — these are true whenever *any* of several
  mechanisms produced that outcome: the guard you meant to test skipped the work, a downstream
  filter discarded it, a precondition was never met, or the operation failed silently. Only one
  of those is your subject, and the assertion cannot tell you which one is holding.

  So before writing "and nothing happened", enumerate what else in the path would swallow the
  work, and check whether the branch under test is even reachable past it. Where something
  downstream discards degenerate input — a zero-length item, an empty batch, a no-op update —
  the guard upstream of it is unobservable by construction, and asserting emptiness pins the
  filter rather than the guard. Say so in the test, or the next reader will trust it.
- **A guard that only avoids redundant work has no wrong answer, only a slow one.** "Skip if
  unchanged", "recompute only when dirty", "return early if already in order" — the block such
  a guard protects is idempotent, so running it when it was not needed produces the same state
  as skipping it. Every mutant that makes the guard *more* permissive is therefore equivalent,
  and they are numerous: widening a comparison, deleting the condition, replacing it with true.
  Only narrowing it changes an answer.

  Recognise these by shape rather than by hunting for the input that separates them — ask what
  the guarded block would do if it ran anyway, and if the answer is "the same thing", stop.
  Killing them would take a test that measured *work done*, not results, and that is a
  different kind of test with a different kind of flakiness.
- **Do not state a precondition in terms of a value the code under test may have rewritten.**
  A test that opens with "this input is only interesting if X" needs X read from something the
  behaviour does not touch. Code that corrects a value often stamps the corrected answer back
  into the field the correction was measured against — to mark the item as handled, to keep an
  invariant, to stop a second pass repeating the work — and a precondition phrased against that
  field then agrees with whatever happened. The failure is not silent, which is the good news:
  it usually shows up as a precondition that cannot be satisfied at all.
- **When a value is folded over a collection, move the deciding element away from the end.**
  "The tightest limit wins", "the earliest deadline wins", "the highest bidder wins" — the
  natural example to reach for tends to put the winner last, because that is the order the
  domain lists things in. A fold that simply keeps the most recent value passes every one of
  those, and so does the correct one. Vary the winner's *position* as deliberately as its
  value: one case with it first, one with it last. Otherwise `min` and `last` are the same
  function as far as the suite is concerned.
- **A shortcut and the exact computation agree wherever the shortcut is valid — so test outside
  its domain.** Code often chooses between a general calculation and a cheaper one that is only
  correct under some condition: a term that can be neglected, a factor already computed
  elsewhere, a case that collapses to a constant. The two arms then agree *by construction* on
  every input the shortcut was written for, which is most of the inputs a test naturally
  reaches for, so the choice between them survives everything.

  What kills it is an input where the neglected quantity actually matters — and **non-zero is
  not enough**. The neglected term has to be comparable in size to the ones that are kept, or
  the two arms still agree to within any tolerance the test can reasonably use. Work out the
  magnitude that moves the result before writing the test, not after watching it pass against
  both arms.
- **A fixture must state every setting the behaviour under test depends on, not only the one
  it is varying.** Configuration that outlives the command that changed it — a scale factor, an
  override switch, a mode — is an *input* to the code being tested, and a test that does not
  set it measures whatever the previous test left. The failure is order-dependent and often
  appears only in one build variant, which makes it look like a difference between variants
  rather than a leak.

  The tell is a test that passes alone and fails in company, or passes in one configuration
  and not another. Set the value in the fixture and restore it, the same way the fixture
  already handles the value it is deliberately varying.
- **A cluster that should have died and did not may mean the test is not there.** The obvious
  reading of an unmoved survivor count is that the new test is too weak. Check the cheaper
  explanation first: that the test is not running. Tests go missing silently — excluded by a
  build guard whose condition is false in this configuration, dropped by an edit that replaced
  a span of a file, filtered out by a name pattern, or never linked at all. None of those
  report anything; the suite just gets smaller and still passes.

  So confirm the test exists and ran before rewriting it, and hand-apply one survivor to see
  the test fail. Used this way the mutation run is a check on the suite's *integrity* as well
  as its strength — it is the only thing that notices a test that quietly stopped existing.
- **A report is a grid, a list, or a table — assert its shape, not its spacing.** For code
  whose output *is* a structure, the properties worth pinning are how many rows it has, how
  many terms are in each, what those terms are, and whatever regularity makes it readable as a
  structure at all — a sign on every term so columns align, a fixed field per record, one entry
  per input. Those are what a reader depends on and what the printing loop can get wrong;
  mutants that change a loop bound, read the wrong element, or drop a separator all fail such
  assertions, while reformatting does not.

  Reach for the degenerate input to test the regularity itself. Alignment rules usually only
  bite on the value that would otherwise be printed short — a zero, an empty field, a missing
  record — so the case that exercises them is the boring one nobody writes a test for.
- **Assert the channel a message came out on, not only its words.** Diagnostic text is
  often produced in more than one place — an error report and a status line, a log record and
  a user-facing notice — and the same wording travels both. An assertion that searches for
  the words alone then passes with the report it was written for **deleted**, because the
  other producer still supplies the string. Mutation testing finds this immediately: the
  mutant that removes the report survives a test that was written to pin it.

  Assert on the severity marker, prefix, or stream that identifies the producer. That is the
  part the caller actually depends on, and it is what makes the test falsifiable.
- **When the only thing a branch changes is speed, time is the assertion.** Optimisations —
  a fast path taken above a threshold, a cache, a shortcut for a common case — are written so
  that the result does not change. Every assertion on the result therefore passes with the
  branch removed, inverted, or its threshold moved anywhere at all, and the whole cluster
  survives. What the branch was for is the cost, so the cost is what has to be asserted.

  Measure a **difference between two inputs either side of the threshold**, not one absolute
  duration. Everything the two runs share — setup, teardown, the work below the threshold —
  cancels in the subtraction, so nothing has to be modelled except the gap between them. A
  threshold shows up as a kink: above it the extra work is cheap, below it expensive, and two
  inputs straddling it differ by one unit of each. That difference *locates* the threshold
  rather than merely noticing one exists, which is what kills mutants that shift it.

  State it as **bounds rather than an equality**. There is usually a fixed per-operation
  overhead you have not modelled and should not have to: assert the difference falls strictly
  between the two pure cases (all-cheap and all-expensive), which is true only if the
  threshold lies between the inputs, and needs no constant at all.
- **A magnitude needs bracketing from both sides.** "Further than before", "faster than
  before", "more than the default" pins no number — every mutant that changes the size of an
  allowance, a retry count, or a margin still satisfies it. Find the input that just succeeds
  and the input that just fails, and assert both. One test, two calls, and the quantity is
  specified instead of merely present.
- **When a virtual call arrives somewhere impossible, dump the dispatch table.** Symptoms
  that no ordinary bug explains — a call landing in another class's method, cleanup emitted
  but never run, an object that is provably intact behaving as though it were not — are
  usually a *definition* problem rather than a runtime one. In languages that emit
  per-class dispatch tables, print the table for the type involved and read the entries.
  It takes one command and it either names the wrong function immediately or rules the
  whole class of cause out.

  The cause worth suspecting first is **two definitions sharing one name**. Test fixtures
  are unusually prone to it: they live in headers, they are written quickly, they are named
  for the thing they stand in for — so two different fixtures for two different aspects of
  the same physical part end up with the same obvious name. Where the language merges such
  definitions silently, one wins and objects of the other get its behaviour, with no
  diagnostic at compile or link time.

  Search the fixture directory for the name before adding a type to it. That is a
  one-second check against a fault that presents as memory corruption and reads, from
  every angle except the dispatch table, as something else entirely.
- **A sanitizer's first report is where the damage surfaced, not always where it began.**
  One fault can produce several, and the tool stops at the first one it meets. If the first
  report describes a *read* of something already dead, look for the *write* that killed it:
  most sanitizers can be told to suppress a category so the run continues to the next
  finding, and the second report is often the cause of the first.

  The tell is a symptom no ordinary bug explains. Cleanup that is emitted but never runs,
  an object constructed and never destroyed while its neighbours in the same frame are, a
  failure that moves when unrelated code changes — those are not lifetime bugs, they are
  what a large stray write looks like from the inside. Do not build a theory that explains
  them as ordinary; find the write.

  And treat any fixture that writes a large buffer from a callback as a hazard in itself,
  separately from whatever aims it. A small stray write corrupts one thing and is hard to
  find; a kilobyte-scale one destroys the evidence, including the frames you would use to
  work out where it came from.
- **Make the harness check its own invariants between tests, not just the code's.** Shared
  state that a test registers and the framework dispatches through — callbacks, listeners,
  handles, anything holding a pointer to a fixture — has to be given back when the fixture
  goes. When it is not, the next dispatch runs against a dead object, and the damage lands
  in whichever unrelated test happens to be running: it fails by corruption rather than by
  assertion, so the failing test is never the one at fault.

  A check that the registry ends each test as it began turns that into a named failure at
  the test that caused it. Compare against the state before the first test rather than
  against empty, so fixtures that legitimately install something for the whole run are not
  reported. Restore what you found, so one fault is reported once instead of by every test
  after it.

  Run the check *outside* the test, and be careful how it reports. A framework whose failure
  path is a non-local jump will jump to a stale target if you call its assertion macros
  after the test has returned — which corrupts the run instead of reporting it. Collect the
  findings and fail one synthetic test at the end.

  It is also a diagnostic, not only a guard: a clean report at every boundary told me a
  dangling pointer I was hunting could not have been left by an earlier test, which is half
  the answer for the cost of running the suite once.
- **Run the suite under a sanitizer once it is worth trusting.** Coverage says a line ran
  and mutation says a test noticed; neither says the suite is reading memory it owns. A
  fixture that outlives the registration pointing at it, or an undersized buffer handed to
  something that writes in place, corrupts quietly — and surfaces later as an unrelated
  test crashing, hanging, or passing for the wrong reason. Chasing that from the symptom is
  expensive; a sanitizer names the write and the frame it came from.

  Make it a separate build rather than a flag on the normal one. It is slower, and it stops
  at the first fault instead of reporting all of them, so it is a fix-and-repeat cycle
  rather than a measurement — a different activity from the one the regular suite serves.

  Expect it to find faults in the **harness** before it finds any in the code under test:
  test fixtures are written quickly, are exempt from review, and are exactly where lifetime
  mistakes live. Expect it also to disagree with the ordinary build about a result or two,
  because it changes the optimisation level; when it does, the assertion that moved was
  pinning the compiler rather than the code, and is worth knowing about either way.
- **A survivor that should obviously have died means the test is wrong, not the tool.**
  When a mutant contradicts an assertion you believe covers it and still survives, stop and
  apply that one mutation to the real source by hand, then run the suite. Either it fails —
  and the discrepancy is in the harness — or it passes, and the test was never testing what
  you thought. It is the cheapest experiment available and it settles the question in one
  build.

  The usual cause is an assertion that cannot fail. Watch particularly for a **tolerance,
  bound, or expected value computed from configuration**: it reads as rigorous, and if the
  configuration is uninitialised in the test build it can evaluate to zero, to infinity, or
  to the very quantity being asserted. A tolerance derived from a setting that is zero is
  not a tight tolerance — it is no assertion at all, and nothing in the report says so.

  So assert the fixture's own preconditions alongside the behaviour: that the machine was
  configured, that the state the test needs was actually reached, that the quantity a
  tolerance is derived from is sane. Those assertions never fail in a healthy run, which is
  exactly why they are worth having — they fail the day the fixture silently stops setting
  something up, instead of letting every test in the file quietly become vacuous.
- **A build option can be what makes a line unobservable.** Where a feature is compiled
  out, the code that *feeds* it often still compiles and still runs: a value is computed
  and then handed to something that discards it, or stored in a field the disabled feature
  was the only reader of. Coverage reports those lines as covered, because they are. No
  assertion can kill their mutants, because nothing downstream can see the result — and
  from the report that is indistinguishable from a missing test.

  So when a survivor cluster sits on a computation whose result you cannot find a reader
  for, look for the reader behind a disabled option before concluding the code is dead or
  the test is missing. The fix is usually to **turn the option on in the pinned
  configuration** rather than to write a cleverer assertion: that is the input class, and
  it costs one configuration line. Where the option cannot be enabled — it needs hardware
  the build cannot have — say so with the count, in the same terms as any other blocked
  cluster.
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

**Stop when the survivor distribution flattens, not when the score hits a number.** Through
most of a rescue the survivors arrive in clusters, and each cluster is one missing input class
— which is what makes the work tractable, because one fixture buys twenty mutants. The signal
that a target is done is that the clusters are gone: the largest remaining group is a handful,
spread across unrelated lines, each needing its own bespoke test for its own single mutant.
Past that point the cost per mutant has jumped by an order of magnitude and the yield has not.

**Report the killable score alongside the raw one.** Once survivors have been triaged, the raw
percentage understates the suite by however many mutants no test could ever kill — and that
share grows as the real gaps close, so the number gets *less* informative exactly as the work
gets better. Quote both, with the equivalent count and the reason categories behind it, so a
reader can see which is which. A raw score falling while the killable score rises is a normal
and healthy thing to happen; a single number cannot show it.

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
