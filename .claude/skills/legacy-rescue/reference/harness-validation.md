# Harness validation — proving the measurement before trusting it

Loaded from `legacy-rescue` Step 3, and by the `harness-validator` agent.

A mutation score is a measurement, and an unvalidated measurement is a rumour. A
self-consistent wrong measurement is indistinguishable from a right one: coverage from
one suite restricting mutants for another produces a perfectly plausible report of a
suite nobody ran. Everything here exists because a number lied first.

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

**The act of testing can destroy the instrument that measures it.** Coverage builds, profiling
data and instrumented binaries are build artifacts, and the ordinary test targets are entitled
to clean them — so a sequence as innocent as *measure, write tests, run the whole suite,
re-measure* can leave the second measurement with no coverage data to restrict itself to. The
failure is silent by construction: the run still completes, still reports a score, and the score
is of a different thing.

Check the artifact exists immediately before each measurement, not once at the start, and read
what the runner says about its own inputs — a line like "on all lines" where you expected "on N
covered lines" is the whole finding. Where a project's own tooling has this property, write down
which command destroys what, because it is invisible in the output of either one.
