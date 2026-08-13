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

This file is the orchestrator. It carries the sequence, the exit gates, and the
guardrails — what is needed to *decide*. The detail needed to *execute* a step lives
in `reference/`, read only when that step is reached, and usually read by a subagent
rather than loaded here.

## When to use

- A module has low or unknown coverage and needs to change.
- Tests exist but pass regardless of whether the code is correct.
- A rewrite is being considered but behavior is not documented anywhere.

## How this skill is organised

**Reference files** — read the one the current step names, not the set:

| File | Read it when |
|---|---|
| `reference/harness-validation.md` | Step 3, before trusting any score |
| `reference/survivor-taxonomy.md` | Step 4, before writing a test for a survivor |
| `reference/assertion-patterns.md` | Step 4, once a survivor is confirmed killable |
| `reference/scoring.md` | Any time a number is reported |
| `reference/acceptance-scenarios.md` | Steps 6-7 |
| `reference/refactor-frontier.md` | Step 8, and whenever a surface change is proposed |
| `reference/delegation.md` | Before handing any step to an agent |

**Delegate the loops.** Steps 2-7 are dominated by build and test output that is
worthless once read. Run them in a subagent and keep the conclusion, not the logs.
Each agent reads its own reference files, so that detail never enters this context.

| Step | Agent | Returns |
|---|---|---|
| 0-2 | `rescue-surveyor` | ranked candidates, each classified untested / not-compiled / not-host-buildable / not-linked, and whether that was measured or inferred |
| 3 | `harness-validator` | `TRUSTWORTHY` / `NOT TRUSTWORTHY`, the evidence, the command |
| 4-5 | `mutant-killer` | killed / equivalent-with-reason / surviving-with-what-would-reach-it, largest remaining cluster, tests added |
| 6-7 | `acceptance-author` | scenario titles, the acceptance-only coverage, and the vocabulary grep with its hit count |

An agent that hands back its raw output has saved nothing. Each agent definition
states a narrow return shape, and that contract is what makes delegation pay rather
than merely relocate the tokens. Where no agent exists for the stack at hand, run the
step directly and read the reference file yourself.

**Where a property matters, have the agent show it rather than assess it.** An agent
asked whether its own work has a property will usually say yes, and will believe it. A
grep with a hit count, a re-run with its output, an artifact on disk — these are checkable
by the caller and the self-assessment is not. Prefer a contract that returns evidence over
one that returns a verdict, wherever the difference is affordable.

**An agent is most dangerous where it is most confident.** The claims worth re-checking are
not the hedged ones; they are the ones stated as established fact in passing — an
equivalence used as a control, a suite reported green, a constant asserted from a
configuration file. Each is load-bearing for everything after it and none of them announce
themselves as claims.

**Two things are not delegated.** Step 8 stays with the orchestrator: the migration is
where the judgement is highest and the evidence subtlest, and deciding to *stop* is the
call an agent optimising for task completion gets wrong. And an agent may *propose* that
a mutant is equivalent, but the reason is spot-checked before it is recorded — that is
the one verdict nothing downstream ever re-checks.

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

**Then validate the harness before believing any of it** — every harness, every time.
`reference/harness-validation.md` is that work, and `harness-validator` is the agent
that does it. A score from an unvalidated harness is not a weak result, it is no
result, and it is indistinguishable from a good one.

Exit gate: an explicit `TRUSTWORTHY` verdict, with the evidence that produced it.

## Step 4 — Kill the mutants

For each survivor, in descending order of risk: write a test that fails against the
mutant and passes against the original. If you cannot express one, the survivor is
telling you the behavior is unspecified — decide with the user whether it is dead
code or a missing requirement.

That is the whole instruction, and it is the smallest part of the work. **Classify
before writing.** About half of all survivors are not missing tests, and a test
written for one of those is wasted effort that also pins the implementation:

1. `reference/survivor-taxonomy.md` — is this killable at all?
2. `reference/assertion-patterns.md` — if it is, what would the assertion have to say?

Survivors cluster. Ten survivors in one function is usually one missing input class,
not ten missing assertions, so fix the input gap and re-measure before writing more.

## Step 5 — Re-measure and loop

Re-run coverage. If line coverage is below **95%** for the target, or the mutation
score is still weak, return to **step 3** with the newly uncovered regions.

Guard against the failure mode this loop invites: tests that raise the number
without testing behavior. Reject any test that

- asserts only that a call did not throw,
- asserts on mocks rather than results (unless the interaction *is* the contract),
- restates the implementation line by line,
- or would still pass if the function body were replaced with its output constant.

**Stop when the survivor distribution flattens, not when the score hits a number.** Through
most of a rescue the survivors arrive in clusters, and each cluster is one missing input class
— which is what makes the work tractable, because one fixture buys twenty mutants. The signal
that a target is done is that the clusters are gone: the largest remaining group is a handful,
spread across unrelated lines, each needing its own bespoke test for its own single mutant.
Past that point the cost per mutant has jumped by an order of magnitude and the yield has not.

Report the killable score alongside the raw one — see `reference/scoring.md`.

Exit gate: ≥95% line coverage on the target **and** a mutation score the team
accepts, with survivors accounted for.

## Steps 6 and 7 — Acceptance scenarios, then validate them alone

Write scenarios at the system boundary in the user's vocabulary, then measure what
they reach **with the unit tests excluded**. See `reference/acceptance-scenarios.md`.

Exit gate: the acceptance suite reaches the target's main paths on its own, and no
step definition names an implementation symbol.

## Step 8 — Refactor

Only now, and only within the frontier the tests have reached.
`reference/refactor-frontier.md` has what may change, what must wait, and why a
cross-cutting correction is a **sequenced migration** rather than a refactor.

The evidence that a refactor preserved behaviour is that the tests did not have to
change. A test edited during the refactor has stopped being evidence.

## Guardrails

- **Never** refactor ahead of cover. If a step-8 opportunity appears during step 4,
  write it down and continue.
- Behavior is preserved by default. A discovered bug is reported to the user, not
  silently fixed — fixing it invalidates the characterization tests that protect it.
  The exception is a defect in the **measuring instrument**: a harness that lies is not
  protecting behaviour, it is manufacturing false results, and it is fixed on sight.
- Keep test commits separate from production-code commits.
- Report numbers honestly, including the denominator and anything skipped. A
  coverage figure without its scope is a misleading figure.
- Timebox the loop in step 5. If the score plateaus, surface the remaining
  survivors and let the user decide.

**Wait on the artifact, never on the process.** Long runs invite a polling loop, and the
obvious one is broken in a way that is invisible until it has cost an afternoon: a loop
that greps the process table for the job it is waiting on **matches itself**, because its
own command line contains the text it is searching for. The first waiter then blocks
forever, and every later one joins the pile — each waiting on the others, all of them
reporting "still running" about a job that finished long ago. Any conclusion drawn from
that report is wrong, and the natural response, adding another waiter, makes it worse.

Two rules, in order of preference:

1. **Do not add a waiter at all.** Run the job itself in the background and let the
   runner tell you it is done. A separate poller is redundant when the job's own
   completion is already observable, and redundancy here is what creates the deadlock.
2. **Where you must poll, poll for the output, not the producer** — `until [ -f
   results.json ]` rather than `while pgrep -f "the-command"`. The artifact cannot
   accidentally match the thing looking for it, and its existence is the condition you
   actually care about. If you truly must match a process, keep the pattern out of your
   own command line (`pgrep -f "[t]he-command"`).

The general shape is worth recognising beyond shells: **an observer that is visible to
its own query will observe itself.** The same trap turns up in log tails that match their
own grep, in monitors that alert on their own noise, and in a test that counts objects of
a kind it has just constructed.

**Quote a baseline with the command that produced it, never as a bare number.** That
turns any ambiguity in the number into a false alarm rather than a silent divergence,
and suites usually have more than one axis to vary — the environment, the
configuration, the target selection. A count that is exact along one axis and silently
different along another will read as a corrupt tree to anyone who reached it by a
reasonable route. Write the invocation, not just the total, and say which axis it
holds fixed.

**Verify before you relay.** Read the diff, run the suites yourself, and check that no
assertion was weakened and no forbidden file was touched. An agent's report is a claim.
Relaying it unverified launders a claim into a fact, and a rescue's only product is
trustworthy measurement. Watch specifically for an assertion that has become
**self-consistent rather than correct** — checking a result against the same accessor
the code under test used to produce it, or against another of the code's own readings
(before-and-after, one state versus the other). It passes, it looks like a real
assertion, and it constrains nothing: a systematic fault moves both sides together.
The tell is a mutation score that does not move when a test is added. See
`reference/assertion-patterns.md`.

**Re-run the agent's evidence, control first.** When an agent claims to have fixed an
intermittent fault, build the unfixed version too and confirm *your* harness reproduces
it before trusting a clean run of the fixed one. A green run only means something once
you know the test can go red. This is step 3's harness-validation rule applied to
someone else's result, and it is where a plausible non-fix gets caught.
