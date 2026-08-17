# Survivor taxonomy — what a surviving mutant is telling you

Loaded from `legacy-rescue` Step 4. Read this **before** writing a test for a survivor.

A survivor does not mean a missing test. Roughly half the time it means something else,
and writing a test for the other half is wasted work that also pins the implementation.
Classify first. The categories below are each one that has been mistaken for a gap.

Never report an equivalent mutant as killed, and give the reason for every one — a
category, not a shrug. An unreasoned "equivalent" is the only verdict nothing downstream
re-checks.

- **Equivalent mutants** (semantically identical to the original, including anything
  behind a disabled build flag) cannot be killed. Document each one with a reason; do
  not contort tests to chase them, and never enable a feature flag purely to make a
  mutant killable.
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

  **Before recording that, check whether the code already has such a seam behind a build
  option.** Software that halts on a fault very often has a mode in which it does not — a
  grace period at start-up, a dry-run flag, a "report but continue" setting, a maintenance
  mode — because the people who wrote it needed the same escape for their own reasons. Where
  one exists, a configuration turns the whole cluster from blocked into ordinary at the cost
  of one line, with nothing stubbed and no production code touched. Say plainly in the
  configuration what it defers and what therefore stays untested: the detection usually
  becomes assertable while the halting itself does not, and those are different claims.

  Check first whether the halt is the *system under test* behaving correctly or the
  *harness* being unfaithful. If the real system genuinely blocks there — waiting on an
  operator, a reset, a signal that only exists in production — then the substitute is
  being accurate, and the instrument-defect exception does not apply however inconvenient
  that is.
- **A fixture that only builds valid input leaves every validator covered and unexercised.**
  Watch for a survivor cluster sitting inside a single guard-shaped function — a parser's
  well-formedness checks, a loader's header validation, a request's schema test. They run on
  every test, because everything goes through them, so coverage is satisfied; and none of them
  has ever been given something to *reject*, so removing any of them changes nothing.

  The cause is usually one fixture that constructs one perfect example, shared by the whole
  suite. Ours built exactly one valid filesystem volume, and **151 of 290 survivors in that file
  were its boot-sector checks** — more than half the file's entire remaining gap, from one
  missing input class.

  This is the "needs an input, not an assertion" category wearing a disguise, and it is usually
  cheap to close: a fixture that already builds the structure field by field can be asked to
  build a broken one with a parameter rather than a new mechanism. It is also worth doing rather
  than dismissing, because validators exist for input you did not write — a corrupt file, a
  foreign disk, a hostile request — and untested ones fail open.

- **Some mutants are detected by everything and pinned by nothing — safety invariants.**
  Removing a bounds clamp, a null guard, an overflow check or a capacity limit does not change
  what the code *decides*; it changes whether the code stays inside its own memory. Under mutation
  that surfaces as a broad scattered failure: dozens of unrelated tests fail, or the run crashes,
  or it hangs somewhere with no connection to the mutant.

  Read it as a kill and do not hunt for the one test that should have caught it — there usually
  cannot be one. The clamp has no observable behaviour of its own; it exists so that nothing
  *else* observes anything, and a test pinning it would have to assert on memory the program does
  not expose. Ours failed twenty-six tests from one removed line, beside a logic mutant in the
  same function that failed exactly one and printed the off-by-one in its message.

  This matters when triaging because "fails half the suite" reads like poor isolation in the
  tests and is not. Note it as a safety invariant, count it detected, and spend the effort on the
  mutants that failed *nothing*.

- **Settle an initialiser with one exaggerated perturbation, not with a dataflow read.**
  Declarations that seed an accumulator — a running minimum, a timer, a result struct —
  produce large survivor clusters, and the tempting move is to trace the reads and decide.
  Do not: the ordering that matters is often several branches away, and being wrong here is
  cheap to do and expensive to notice, because the conclusion is a claim that *no* test could
  ever help.

  Instead set the initialiser to something far outside anything the mutator generates and run
  the suite once. Identical output means dead, and one build has settled the whole cluster.
  Different output separates two findings the report cannot: a **dead store**, where the value
  never escapes, and a **live line whose mutants are all in range**, where the seed does matter
  but only for values the tool never produces. Those have different follow-ups — the second
  says an input class is missing, the first says nothing is.

  Two cautions from doing it. Perturb **one thing at a time**: seeding several at once and
  seeing failures tells you nothing about which. And an exaggerated value can fail for a reason
  unrelated to the mutants — a sentinel set past a threshold takes an abort path no `±1` mutant
  would — so read *which* tests failed rather than only how many.

- **A report guarded by a once-per-process flag can be observed by at most one test, and which
  one is the linker's choice.** Diagnostics for terminal faults are often written to fire once —
  "the first failure is the one that matters, and we are about to stop anyway" — using a
  function-local static or a module-level flag that nothing resets. On the real system that is
  reasonable. In a suite where every test shares one process it means the message is spent by
  whoever reaches it first, and every later test sees silence.

  The tell is a cluster on reporting code that survives even after you have made the path
  reachable: the *action* is assertable and the *words* are not. Confirm it by printing the
  flag on entry rather than by reasoning about who might have set it — the culprit is usually
  a test that has nothing to do with the subject.

  **Do not fix this by ordering.** A test that passes only when it runs first will pass today,
  fail the day a file is added ahead of it, and present as a defect in the code rather than in
  the arrangement. Delete the test, record the flag as the reason, and note that the fix — a
  resettable flag, or separating the reporting from the once-only action — is a production
  change and belongs behind the frontier.

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
- **A configuration that unblocks a cluster will usually lower the file's headline score — judge
  it on the cluster it was added for.** The new configuration is a different build, so it is a
  different mutant population; and it is nearly always a *smaller* suite, because it enables the
  one feature you wanted and not the several that other configurations bring. Fewer tests reach
  the rest of the file, and everything outside the target cluster gets slightly worse.

  Ours moved the cluster it was for from 11% killed to 34%, while the file total went from 60.4%
  to 57.6%. Both numbers are correct and neither is a before-and-after: different population,
  different suite. Report the cluster, and say explicitly that the file totals are not comparable,
  or the work reads as a regression to anyone scanning the headline.

  Expect some of the newly reachable mutants to be equivalent even so. Making a branch *reachable*
  is not the same as making it *consequential*, and ours turned out to be dead code — the
  configuration proved that, which is worth as much as killing the mutants would have been.

- **A configuration that unblocks a cluster can also change the plant the tests run against.**
  Turning on the option that makes a dead arm reachable is cheap when the option only selects
  code. It is not cheap when the option changes a *control loop* — a different regulator, a
  different scheduler, a different retry policy — because the substitute hardware the suite
  runs against was built and tuned for the old one. The shipped parameters are matched to real
  equipment; the simulation is not that equipment; and the two can be mismatched badly enough
  that the system never reaches the state a waiting test is waiting for.

  Expect the symptom to be a **hang rather than a failure**, since a substitute where time is
  controlled by the test cannot tell "slow" from "never". Budget for the variant to need its
  own tuning stated alongside it, the way the fixture already states its own scale and limits —
  and treat that as a piece of work with a build-and-run per attempt, not as one line of
  configuration. Where that is more than the cluster is worth today, record the count, the
  reason, and what the attempt cost, so the next person starts from the second problem rather
  than the first.

- **Reaching a dead branch is not the same as separating it — look for the second reason before
  building the variant.** When a correction branch cannot run because its guard can never hold,
  the obvious fix is a configuration in which the guard does hold. That is often only half the
  work. The same setting that disables the guard may also *collapse the correction*: where the
  general formula and the special case are written in terms of a quantity the configuration pins
  to a neutral value, the corrected result equals the uncorrected one, and the branch is a no-op
  even when it runs. Every mutant of the guard then survives for the second reason after the
  first is fixed, and nothing in the report tells the two apart.

  So work out what the branch would *compute* if it ran, and check that it differs, before
  deciding what the variant has to change. The one you need is whichever separates the two
  expressions, and that may be a different setting from the one that reaches the line. Where it
  is the same setting, say so — that is why one configuration line bought two things, and the
  next person should not have to re-derive it.
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
- **A value written to a format nothing reads back is unkillable, and there may be a lot of it.**
  Code that serialises a fixed layout — a record, a wire format, a saved document — usually keeps
  writing a slot when the feature behind it is compiled out or switched off, so that the layout
  does not shift and older and newer versions can still read each other's data. The reader skips
  that slot into a throwaway. Every constant in those slots is unobservable *by design*, and in a
  serialiser they can outnumber everything else: they were 58% of one file's survivors here.

  Recognise them by shape — a `const` or literal declared next to the write, with the read
  discarding the same slot — and check one by changing its value wholesale rather than trusting
  the pattern. Then report the score with them excluded, because the raw number will otherwise
  say the file is untested when the part of it that can be tested is fine.
- **In presentation code, most survivors are killable only by transcribing the code, and that is
  a judgement to record rather than a gap to close.** Where a module's job is to render — to place
  things on a screen, lay out a document, emit a drawing — the majority of its lines are literal
  geometry and style: positions, sizes, colours, fonts, and the arithmetic between them. Driving
  the module *executes* all of it, so line coverage climbs freely, while nothing asserts any of
  it. Expect the two numbers to diverge further here than anywhere else: ours came out at 65%
  line and 9% mutation, with four fifths of the survivors on lines that call a drawing primitive.

  The reason not to close it is not that the faults are harmless — a value drawn in the
  background colour, or off the edge, is a screen that lies, and that is the module's whole
  purpose. It is that the only assertion available is a copy of the expression under test, which
  is the self-consistency trap applied to thousands of lines: it would pin the layout so firmly
  that no one could ever change it, and it would still not check that the result is *legible*.

  So write the trade down next to the number, as a decision with a cost, not as a property of the
  file. A bare low score reads as neglect and invites someone to "fix" it by transcription. Three
  assertion shapes stay available and are worth taking: **content rather than position** (the
  words that appeared, not where), **volume rather than content** (a pass with nothing to say
  emits nothing — which pins the change-detection that makes the module usable), and
  **destination rather than appearance** (choosing this control led there). Then separate the
  presentation survivors from the rest and report what remains, because that residue is the part
  a test can still help with.

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
- **A cluster that should have died and did not may mean the test is not there.** The obvious
  reading of an unmoved survivor count is that the new test is too weak. Check the cheaper
  explanation first: that the test is not running. Tests go missing silently — excluded by a
  build guard whose condition is false in this configuration, dropped by an edit that replaced
  a span of a file, filtered out by a name pattern, or never linked at all. None of those
  report anything; the suite just gets smaller and still passes.

  So confirm the test exists and ran before rewriting it, and hand-apply one survivor to see
  the test fail. Used this way the mutation run is a check on the suite's *integrity* as well
  as its strength — it is the only thing that notices a test that quietly stopped existing.
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

**A mutant whose only effect is undefined behaviour is invisible to assertions and visible to a
sanitizer.** Widening a bounds check is the common case: the mutant lets an out-of-range index
through, and the access that follows is out of bounds, so it does not compute a *different*
answer — it computes an *undefined* one, which on any given platform often reads as whatever
happens to be adjacent. Zero, usually. Every assertion agrees with the original and the mutant
survives.

Do not reach for an assertion here. Making that memory hold something recognisable means
asserting on layout, which pins the compiler rather than the code and breaks the moment anything
near it changes. Classify these as equivalent *under the ordinary suite*, and say so with the
qualifier — because a sanitizer build does distinguish them, turning the same mutant into a
named, located failure. That is a different instrument answering a question the first one cannot,
and it is worth one run to confirm rather than assert: apply the mutant, build under the
sanitizer, and read the report.

Two cautions from doing exactly that. A sanitizer aborts at its *first* finding, so a run that
reports nothing about your mutant may simply never have reached it — check which tests ran, not
just what was reported, because **a control that does not run is not a control**. And where the
test runner wraps the sanitizer, expect the report to be swallowed: a runner that summarises
results by parsing output will happily print a passing-looking tally beside an aborted run. Run
the instrumented binary directly and read its stderr.

**Watch for the reachable-domain equivalence hiding among them.** Arithmetic mutants are usually
easy kills, but where the input range is narrow two different operations can agree across all of
it — subtraction and modulo by the same constant coincide exactly when the operand stays within
one multiple of it. That is equivalence by reachable range, not by undefined behaviour, and it
needs the domain written down rather than the operation compared.
