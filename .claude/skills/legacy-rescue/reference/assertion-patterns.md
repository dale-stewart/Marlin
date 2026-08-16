# Assertion patterns — how to make a test say something

Loaded from `legacy-rescue` Step 4. Read this once the survivor is confirmed killable
(see `survivor-taxonomy.md`).

The governing idea is the first rule below: assert what the domain guarantees, not what
the code happened to return. Everything after it is a way that an assertion can look
substantial and say nothing.

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
- **Where the outcome is blocked, assert the state the outcome will be computed from.** Some
  decisions are only visible when they fire, and firing is what a test cannot survive — the
  process halts, the machine stops, control leaves and does not come back. It is tempting to
  conclude that everything leading up to it is untestable too, and to settle for asserting that
  the system is still in its ordinary state. Do not: that assertion reads the same on both sides
  of every boundary, so it distinguishes nothing, and a wholly different implementation passes it.

  Look instead for the accumulator the decision will be taken on — a deadline, a counter, a
  running total, a flag. It is usually reachable, it changes on *every* pass rather than only at
  the end, and asserting it turns one unreachable outcome into many observable steps. Assert both
  that it moved and *how far*: "the allowance was extended" is satisfied by any amount at all,
  including one that would let the fault run for an hour.

  The tell that you need this is a file of tests whose assertions are all the same value. If every
  case — inside the boundary and outside it, before the limit and after — asserts that the system
  is still normal, then none of them is asserting anything about the boundary.

- **Compare against the values the system defines, never against another of its own
  readings.** The tempting shape, when a thing has two states, is to record it in one and
  then in the other and assert that they differ. It reads as a proper bracketed test — both
  sides exercised, no magic constants — and it is satisfied by exactly the fault it looks
  like it would catch: anything that *inverts* or *offsets* the reading changes both
  observations equally, so they still differ and the test still passes.

  The same applies to any assertion phrased as a relation between two of the code's own
  outputs: before-and-after, this-call-versus-that-call, one member of a collection against
  another. What it constrains is internal consistency, which is the one property a
  systematic fault preserves.

  Assert the *named* values instead — the constants, strings or enumerators the code
  publishes for those states. That is what a consumer reads, and it is what a fault has to
  get right. Where the domain genuinely has no absolute to check against, say so in the
  test rather than substituting a differential assertion that looks stronger than it is.

  This one is hard to see by reading, because the differential version is often the more
  elegant code. The tell is a mutation run that does not move: write the test, measure, and
  if the score is unchanged, inject the obvious fault by hand before concluding the mutants
  were equivalent. Two tests written this way in one session here each killed nothing, and
  neither was noticed until the fault was injected deliberately.
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
- **Some inputs cannot be supplied one at a time.** A survivor that needs the code to be in a
  particular *régime* — a buffer partly drained, a cache warm, a rate high enough to saturate
  something — is not reached by making one call with extreme arguments. Extreme arguments often
  put the code in a different régime instead: ask for a single very fast, very short operation
  and some other limit dominates, so the value under test is computed and then never used.

  What reaches these is a *sequence*: several operations queued together, so the state the
  branch reads is the state the earlier ones left. Budget for that — it is a fixture that
  drives the system into a régime and holds it there, not another parameter on an existing
  helper. Recognising it early saves a round of tests that look reasonable and kill nothing.
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

  **The most common cause of nothing is the fixture.** Where the test says "the thing did not
  happen", check that the machinery which would have made it happen was actually in place —
  and state that as an assertion in the arrangement, not as a comment. Two of three tests here
  asserted that a print had not been aborted, and passed for months' worth of edits against a
  machine with no print running at all: the abort flag is derived from "is a file open", so no
  file meant no abort meant a green test. One line asserting the file was open turned three
  passing tests into three failing ones, which is what they should have been saying all along.

  **Where the subject is a refusal, there is usually a positive assertion available beside the
  negative one.** Code that declines to act for a reason almost always says so — raises, logs,
  warns, returns a status, puts words in front of somebody. Asserting the announcement as well
  as the absence gives the test one claim that no other cause of nothing can satisfy, and it
  pins the half that actually reaches a person. A refusal delivered silently is
  indistinguishable from a control that does not work, and the next thing anyone does with a
  control that does not work is operate it harder. Assert the words, not only the inaction.
  **When the test builds the input, "rejected" may be about the builder rather than the code.**
  Testing a rejection means constructing something invalid, which means the test knows how to
  construct the valid version — and if it does that wrongly, *every* input it makes is invalid.
  The rejection test then passes for a reason that has nothing to do with the defect it names,
  and no amount of injection into the code under test will show it, because the check it is
  provoking really is reachable and really does fire.

  This is the self-consistency trap wearing a different hat: the test and the code agree about
  nothing, and the assertion cannot tell the difference. Ours built a packet whose payload
  checksum was computed over the wrong range, so the reader called every packet corrupt; the
  test asserting that a corrupted one is refused had been green from the day it was written.

  The cure is the same pair as above, and it is cheap: assert the *valid* case with the same
  builder. One test says the intact input is accepted, the other says the damaged one is not,
  and neither is worth much alone — a builder that is wrong fails the first, and a check that
  never fires fails the second. Verify by forcing the check both ways and confirming each
  direction fails exactly one of the two.

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
- **A fixture must state every setting the behaviour under test depends on, not only the one
  it is varying.** Configuration that outlives the command that changed it — a scale factor, an
  override switch, a mode — is an *input* to the code being tested, and a test that does not
  set it measures whatever the previous test left. The failure is order-dependent and often
  appears only in one build variant, which makes it look like a difference between variants
  rather than a leak.

  The tell is a test that passes alone and fails in company, or passes in one configuration
  and not another. Set the value in the fixture and restore it, the same way the fixture
  already handles the value it is deliberately varying.
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
- **A parameter test whose value coincides with the default asserts nothing about the
  parameter.** Setting a field to the value it already holds and then checking the field is
  satisfied by code that never performed the assignment at all — the mutant that deletes it,
  the one that guards it out, and the one that writes it to a *different* field all pass.
  These survive in clusters on option-parsing code, which is exactly where the natural test
  value is the documented default because that is the number in front of you.

  Pick values no default could supply, and make each parameter's value distinct from the
  others' so a write to the wrong field is visible too. The same trap applies to any "set it
  and read it back" test: choose the one value that could not have got there by accident.

- **When a comparison passes first time, check whether the strong form was available.**
  `after > before`, `result is non-empty`, `it took longer` — these are easy to write and easy
  to satisfy. Print the actual values once. Very often the real relationship is exact: the
  quiet case is *zero*, not merely smaller; the retry count is *three*, not merely more than
  one. An inequality that happens to hold looks identical to a specification in a green run,
  and only one of them fails when the code gets worse.

  This is the cheap counterpart to bracketing a magnitude: one probe, one build, and the
  assertion goes from "something happened" to "this happened".

- **Where the output is a protocol you do not want to pin, assert its volume instead of its
  content.** Code that talks to a device emits a format full of things that are not the
  behaviour under test — addresses, coordinates, opcodes, framing. Decoding it makes the test a
  second implementation of the protocol and breaks it whenever the layout changes for reasons
  nobody cares about. But *how much* was said is often exactly the contract: a cache that only
  emits on change sends nothing when nothing changed, a batching layer sends one message for
  many inputs, a throttle sends fewer than it was given. Count the bytes, or the messages, and
  assert the relationship.

- **Do not encode the harness's own conventions in an assertion — assert the invariant that
  survives them.** Driving a device through a simulated input has two properties that feel like
  facts and are not: which way its directions map, and how much one call of your fixture moves
  it. Both are agreements between your stand-in and the code, neither is a claim about the
  system's behaviour, and writing tests against them produces failures that look like defects.

  Ask what would still be true if the device were wired the other way round. Usually it is an
  *ordering* or a *set*: the destinations are adjacent in the order they are drawn, no two
  inputs lead to the same place, the extremes clamp rather than wrap. Reach a known state by
  driving hard against a limit rather than by counting steps from an assumed start, then walk
  and record what you see. The result is longer to write and does not need rewriting when
  somebody inverts a pin.

  The same caution applies to *rate limits*, which are invisible until they bite: input handlers
  frequently ignore events for a period after accepting one. Where the clock is under the test's
  control, a burst of input arrives inside a single instant and all but the first is discarded —
  and the symptom is a device that appears not to respond at all, which reads as a broken
  fixture rather than a debounce. Let time pass between steps, the way the hand that would
  really be doing it does.

- **A helper that resets by default cannot test what persists.** Test helpers for stateful code
  usually take the convenient form: start from the beginning, run the input, return. That is right
  for most cases and silently wrong for the ones that matter most — anything asserting that state
  *carries* from one unit of input to the next. Feeding the second half from a fresh start asserts
  only that the second half works on its own, which it does, so the test passes and says nothing.

  Two tests here were written specifically to kill two mutants, passed, and killed neither: one
  about a value surviving to the end of an input, one about a machine releasing state after acting
  on it. Both fed their second stage from a clean start. Nothing in a green run distinguishes that
  from a working test — the mutation score is what said so, which is the clearest case for
  measuring rather than trusting a test you have just written.

  Give the helper an optional starting state and *return* the ending one, so continuing is
  possible and visible at the call site. Then any test whose subject is persistence has to thread
  it explicitly, and one that does not is obvious on the page.

- **A shared probe has a question it answers; where that question does not apply, the probe
  weakens the claim instead of failing.** Once a family of call sites has one reusable helper —
  the walk, the round trip, the sweep — the temptation is to point it at every member. But a
  helper is built around an observable, and a member whose behaviour shows up somewhere else will
  be reported by that helper as *nothing happening*. Two such members in a row collapse into one
  observation, the result is compared against an expectation derived the same way, and everything
  agrees. The test is green, shorter than it should be, and says almost nothing.

  The tell is a member whose entry in the collected results is indistinguishable from its
  neighbour's, or a total that is smaller than the number of things you drove. Before reusing a
  helper, ask what each member changes and whether the helper can see it; where it cannot, that
  member needs a different observable, not a looser assertion. Recording an effect — state before
  and after, keyed on a marker value the subject itself cannot produce — is usually the
  substitute, and it turns "this one did nothing" from an absence into a positive reading.

- **Where one member of a family is suspect, pin the ones that are correct.** A defect recorded
  against a single call site reads as "this component does not do X". Asserting its siblings —
  the two neighbouring functions that *do* refresh the cache, validate the input, take the lock
  — turns it into "this component does X everywhere except here", which is a much stronger
  claim and a different diagnosis. It is the difference between a design decision and a bug, and
  it is usually two short tests.

  It also protects the fix. Whoever eventually corrects the odd one out will be editing code
  that looks exactly like its working neighbours, and the neighbours' tests are what notice if
  the correction is applied in the wrong place or breaks them on the way past.

  Assert the *derived* thing, not the stored one: the stored value is typically right in both
  the working and the broken version, and the whole difference is whether what depends on it was
  brought back into step.

- **A magnitude needs bracketing from both sides.** "Further than before", "faster than
  before", "more than the default" pins no number — every mutant that changes the size of an
  allowance, a retry count, or a margin still satisfies it. Find the input that just succeeds
  and the input that just fails, and assert both. One test, two calls, and the quantity is
  specified instead of merely present.

  **A bound is only as tight as the part of the measurement it constrains.** Where the
  quantity you care about is one component of what the test can actually observe — a
  timeout inside a total elapsed time, a payload inside a whole response — the other
  components set a floor on how tight the bound can be, and a bound loose enough to
  accommodate them will not catch a wrong value for the part you meant. Say which of the
  two you have. "This catches a wait that runs away, not a residency of the wrong size" is
  a useful sentence; presenting the same assertion as a bracket on the residency is not.
  The strong version usually needs a *difference* between two runs that vary only in the
  component of interest.
- **A test is a claim about every configuration, not the one you measured in.** Mutation
  work is done against one build, and it is easy to write an input that is out of range
  *there* — an index the build does not have, a feature it does not compile, a limit it
  does not reach. In another configuration that same input is ordinary, and the test then
  asserts the system ignores a perfectly valid request. It will pass where it was written
  and fail where it was not, which reads as a flaky test rather than a wrong one.

  Derive such an input from the build's own constants rather than writing a literal — the
  first index this build does *not* have, rather than a number that happens to be out of
  range today. That keeps the test true wherever it is compiled, and it states the
  intent, which a literal does not. Run every configuration before believing a green
  suite, and say which ones you ran.
