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

**A fixture that relies on a destructor is unsafe to fail inside.** Many test frameworks
implement a failed assertion as a non-local jump out of the test, which unwinds no stack: the
tail of the test does not run, and neither does the cleanup of anything it constructed. So any
fixture whose *correctness* depends on being torn down — one that switches a mode on, holds a
lock, redirects a stream, starts a thread — silently stops being torn down the moment a test
fails, which is precisely when you were about to change something.

The list is longer than it first looks, because a plain value can qualify. Anything that shifts
the **frame of reference** later assertions are expressed in — an origin, a base unit, a
timezone, a locale, a current user — leaks the same way and is harder to recognise, because
there is no resource to notice the absence of. The tell is a later test failing against a
constant it never touched.

**Reset every stage of a staged input, not just the one with a name.** Input usually arrives
through a small pipeline — a transport buffer, an accumulator that assembles a unit from
fragments, then a queue of assembled units — and only the last of those is an object anyone
thinks of as "the input". Clearing it feels like clearing the input, and it is not: whatever had
arrived but not yet *become* a unit survives, because it is not in the container you emptied.

The next test's input then arrives behind the leftover fragment and the two are assembled
together. What the code under test receives is a well-formed request with a corrupt prefix, so it
is refused or misread — and nothing in the failure mentions buffering, because by the time
anything is observable the fragments are indistinguishable from one bad input. Ours put a partial
command in front of the next test's command two files later, and the report said only that a
value was wrong.

Find these by following the input backwards from where the code reads it to where the test writes
it, and reset every buffer, index and state flag on the way. The parser's queue is the obvious
one; the half-assembled unit and the raw transport buffer are the ones that get missed. Then
prove it: inject a fault that makes a test fail *mid-unit* and check that exactly one test fails.
A teardown that only handles inputs that completed is untested against the case it exists for.

**Write the teardown from what operations leave behind, not from what tests change.** The obvious
teardown list is the things a test deliberately sets — a flag raised, a value overridden, a fault
injected — and that list is the smaller half. The rest is the *ordinary end state of ordinary
operations*: a completed transaction that closed the connection, a finished job that released the
handle, a parse that consumed half a line and is waiting for the rest. Every one of those is
correct behaviour for the system, which is exactly why nobody lists it: there is nothing to
"restore", because nothing was tampered with.

They are the expensive ones because the failure lands somewhere else. The next test starts against
a system that has legitimately put something away, its unrelated operation fails, and the report
names that operation. We had three in a row — a half-received line, a state machine stranded
mid-packet, and a resource released by a successful close — and each first read as a defect in the
subsystem that reported it, two files away from the cause.

So enumerate by walking the *operations* your tests invoke and asking what each leaves changed
when it succeeds, not only when it fails. Then prove the list: inject a fault that makes a test
fail mid-operation and confirm exactly one test fails. A second failure elsewhere is the teardown
telling you what it still does not know about.

**The teardown runs outside the protections every test has, and it is not exempt from the rules
they exist for.** Fixtures redirect output, silence a channel, stand in for a consumer, hold a
lock — and all of that is scoped to the test. The between-tests hook runs after the last of it
has been undone, so anything it does is done bare. If it *reports* — and teardown that re-mounts,
reconnects, or resets tends to report — it is writing to a channel that no longer has whatever was
consuming it, and a bounded buffer with no reader is a wait that never ends.

Its own output accumulates a little at a time, so the failure is a **slow fuse**: the suite passes
until the total crosses a threshold, and *where* it crosses depends on how much everything before
it happened to print. That is why it presents as a flake — one build hanging where another is
green, at a test that does nothing unusual, having passed the same binary six times in a row. It
is not a race, and hours can go into looking for one.

Give the teardown the same protection the tests have, scoped to itself, and restore what it found
rather than forcing a constant — the teardown should assert nothing about the state between tests
except that it is not itself heard. And when a hang moves between builds, suspect accumulation
before concurrency: bisect by *disabling one thing the teardown does*, which is a cheap experiment
that either turns an eleven-minute hang into an eleven-second run or clears the whole hypothesis.

**Some state cannot be reached from the teardown at all — reset it on the way in.** Where the
state lives somewhere the between-tests hook cannot see (private to a compilation unit, behind an
interface that exposes no reset, owned by a component the harness does not construct), the only
place left that the failure path cannot skip is the *next* test's setup. Have the fixture put
things right when it starts rather than when it ends: begin by cancelling, closing, or aborting
whatever might still be running, chosen so that doing it to an idle system is harmless. It reads
oddly the first time — cleaning up before you have made a mess — and it is the only construction
that survives a jump out of the previous test.

**When the fixture started a thread, the leak is not state — it is memory, and the between-tests
hook cannot fix it.** Everything above says "put the restoration where the jump lands". That
advice fails exactly once, and the case it fails on is the most damaging one. A fixture that
starts a background worker usually gives it a pointer to the fixture — to accumulate into, to
signal through — and the fixture lives on the test's stack. When the jump discards that frame
without joining the worker, the worker keeps writing into memory that is about to be handed to
whatever runs next. The between-tests hook cannot rescue this by calling the fixture's own
cleanup, because reaching the fixture at all means dereferencing the dead frame.

The symptom is a crash *after* the first failing test, taking the rest of the run with it — so
the suite reports a fraction of its tests and every failure after the first is invisible. It is
easy to misread as "the failure cascaded", and easy to leave unfixed for a long time, because it
only fires when a test fails and a suite that is green never shows it. Ours had been live for as
long as the file it affected had existed.

The fix is ownership, not sequencing: give the worker its own reference-counted state so it
never touches the fixture, and keep a registry of live sessions the hook can stop and join
without touching any fixture. A skipped destructor then leaks a small allocation instead of
corrupting the process. Make the registry a list rather than a single slot — these things nest,
because a helper opens one while its caller already holds one.

**Where the system special-cases a dimension, check that your fixture configures it.** Many
systems have one axis, column, channel or tier that the rest of the code treats differently — and
the helper that means "all of them" usually excludes it, because it was written for the ordinary
ones. A fixture that sets limits by iterating that helper leaves the special one at whatever its
storage was zero-initialised to.

The failure mode is what makes this expensive: **an unset limit does not refuse the operation, it
scales it.** Zero throughput, zero acceleration, zero batch size — the code takes the correct path,
produces the correct result, and arrives in the correct state, arbitrarily slowly. Nothing throws
and nothing asserts. What you see is a suite that stopped finishing, usually first noticed in an
unrelated test whose duration depends on accumulated state.

Two habits make it cheap instead. Set the whole configuration in one loop over the widest
enumeration available rather than several loops over narrower ones — the bug here lived in the gap
between two such loops that differed by exactly one element. And when a value is a *limit*, prefer
a fixture that fails loudly on zero to one that inherits it.

**When one operation is slow and a related one is not, look for what the fast path selects that
the slow path does not.** The decisive measurement in the case above was not removing things until
the problem went away — every removal left it unchanged — but *adding* an ordinary operation
alongside the suspect one. The combined operation was 1000× faster than the suspect alone, which
is only possible if the two take different branches, and that named the branch immediately.

Reach for this when narrowing by subtraction has stalled: if A is slow and B is fast, try A+B. A
combination that is faster than one of its parts is a strong signal that a special case exists,
and it points at the exact predicate. Along the way, test whether the cost **scales with the
size of the work**; a cost that is constant across a tenfold change of input is a fixed wait or a
fixed misconfiguration, not a slow algorithm, and that distinction rules out half the candidates
in one run.

**A plausible mechanism that fits every symptom is not a diagnosis.** The failure above had an
obvious explanation available: another fixture in the same test leaked a value that shifts the
frame of reference, which is a documented fault class, was genuinely present, and accounted for
every symptom. Acting on it changed nothing, because it was not the cause. One backtrace named
the real one immediately and named something in a different file.

Where a fault is *reproducible*, spend the ten minutes on the debugger or the sanitizer before
spending an hour on the theory. Reasoning is what you fall back on when the fault will not stand
still; it is not the cheaper option when it will. And when a defensive fix is kept anyway — it
was correct hygiene, just not the fix — say so where it lives, or the next reader takes it for
the explanation and stops looking.

**And the state to reset is not only the state your tests set.** A leak can compose out of
behaviours that are each correct: one test sets a value, the system reacts by entering a mode,
that mode arms a monitor, and the monitor acts on the *next* test that idles — which is where
the damage appears, with nothing in it referring to any of the three. Resetting what the test
touched is not enough, because the test touched the first link only.

Find these by instrumenting the boundary, not by reasoning about which feature could have done
it. Print the offending state after every test and stop at the first one that shows it; that
names the culprit in one run, where working backwards from the symptom names candidates for an
afternoon. Then fix it where the reaction was — the reset belongs next to whatever sets the
first link, because the two are one leak rather than two.

The damage lands in a later, unrelated test, and it is usually not a failure. Expect the suite
to hang or to slow down rather than to report anything: a leaked mode leaves later code waiting
on a condition nothing will now satisfy, and a leaked *scale* — anything that multiplies how
much work every subsequent operation does — makes the run take minutes instead of seconds with
every test still passing. That is worse than a wrong answer, because there is nothing in the
output to read.

Put the restoration where the jump lands: in the framework's between-tests hook, not in the
fixture. Two cautions when you do. Capture the baseline at a moment when the state is real —
capturing before the system under test has initialised records zeros and then *imposes* them
after every test, which is the same fault with the sign flipped, and it looks like a fix.
And this is why an injected-fault control belongs in the harness check: it was injecting a
known fix and watching a green suite become one that had to be killed that exposed both leaks
here, neither of which any passing run could have shown.

**A shared fixture accumulates, so derive quantities from it rather than from what your test
put there.** Where the suite shares one instance of something with state — a database, a
filesystem, a card, a queue — every earlier test's leftovers are in it. A test that writes three
records and then reasons about "the last one", or sizes a loop to the number it created, is
describing a container it does not own. It passes today and stops passing when a file is added
somewhere unrelated, and the failure reads as a defect in the code under test.

Ask the shared thing for its own count and derive everything from that. And read any expected
value *before* performing the action where the query that computes it is the same query the code
uses — otherwise the computation overwrites the evidence, and the assertion compares the result
against itself.

**A flag that looks like a variant selector may be a filter, and the difference is silent.**
Test runners often have an option that *narrows what runs* and a separate mechanism that
*changes what is built*, and their spellings can be almost identical. Reach for the wrong one
and the suite executes against whatever configuration was last generated — usually the one you
wanted, because you set it up by hand a moment earlier, which is exactly what makes the mistake
survive. The tell is two supposedly different variants reporting the **same test count**.

Use the project's own variant target rather than the filter, and prefer one that regenerates
the configuration as part of running. Where a figure matters, quote the command beside it: a
number produced by the wrong invocation is not wrong-looking, it is just about something else.

**A constant that describes the environment is not a constant to test against.** Buffer sizes,
widths, capacities and limits are frequently derived from the presence of hardware — a display,
a network interface, a card slot — and collapse to a degenerate value when it is absent. A test
that inherits one as a default parameter is asserting about the configuration rather than about
the code, and it will say something different in every variant. Where the behaviour under test
is a *rule*, state the size in the test; where it is *what fits*, that is a different test and
should say so.

**Find the caller; do not assume the obvious owner.** A test that drives a feature has to
invoke whatever the production code actually invokes, and the function that *looks* like the
owner is often not it — periodic work gets hung off whichever loop was convenient, so a
temperature adjustment can be driven from the motion subsystem and a cache refresh from the
display. Guessing costs a full build-and-run and, worse, produces a test that fails against
working code, which reads as a defect until it is chased down.

Grep for the call site before writing the test, not after it fails. The same applies to the
*order* of set-up calls: where one setter deliberately clears the state another sets — an
explicit command overriding an automatic mode is the usual reason — doing it in the wrong order
switches off the thing under test in the line after enabling it, and the test then fails
honestly against a system that is fine. Prefer driving set-up through the same public entry
point a user would, which gets the order right by construction.

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
