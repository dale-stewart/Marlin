# Delegating a rescue to subagents

Loaded from `legacy-rescue` when work is handed to an agent.

Two rules from this section are promoted into the orchestrator's guardrails because they
are read every run, not only when delegating: *quote a baseline with the command that
produced it*, and *verify before you relay*. They are repeated here in full.


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
