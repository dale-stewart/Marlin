# Scoring and reporting conventions

Loaded from `legacy-rescue` Steps 3 and 5. Any agent reporting a score reads this.

**Scoring conventions**, stated up front so the number means something:

- **How far the mutation score sits below the coverage number is a property of the code, not of
  the coverage.** Three files measured together, all well covered and none previously mutated:
  a pure formatter at 98% coverage scored 96.5%; a decoder at 100% scored 90.7%; a stateful
  allocator at 75% scored 55.1%. The order does not follow the coverage at all — the 100% file
  scored below the 98% one.

  What it follows is **how directly the code's decisions are observed**. Pure functions asserted
  on their exact output have nowhere for a wrong decision to hide. Code whose effects are seen
  several layers away, through some later operation, can execute completely while almost nothing
  it decides ever reaches an assertion.

  Use it to schedule: mutation runs are expensive, and they buy least on pure code with exact
  assertions and most on stateful code observed indirectly. And use it to set expectations before
  the run, so a high score is not read as "the tool found nothing useful" — on the pure file it
  is the correct answer.

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
- **Check what your mutation tool can actually see, and whether it matches what coverage sees.**
  The two instruments usually work at different granularities: coverage is attributed to source
  *lines* wherever they end up, while mutation is applied to a *compiled unit*. Anything that is
  compiled indirectly — code in headers, templates, generated sources, macro-expanded bodies,
  anything inlined from elsewhere — can therefore show a coverage figure and be entirely beyond
  the mutation run.

  That makes a score of the form "this component is N% killable" narrower than it sounds: it
  covers the unit that was mutated, not the component. Where a language or project keeps
  substantial logic outside the primary unit, say which part the number is about, and count how
  much sits outside it before treating a target as closed.

  Verify it rather than assume either way. Point the tool at a header (or equivalent) and see what
  happens: exiting with a clear message is the good case, because then absence of measurement is
  visible. Silent success on a target that was never really mutated is the bad one, and the tell
  is a suspiciously perfect score over a suspiciously small mutant population.

- **The mutant population is small edits to code that exists — so a whole class of guarantee
  cannot score.** A mutator deletes and alters text that is already there. It does not *add*
  anything, and it does not restructure. So a test defending against a plausible future
  rewrite — state reset where it should have persisted, a call moved inside a loop, a resource
  reacquired per item instead of held — has no corresponding mutant, and killing zero is the only
  possible outcome no matter how good the test is.

  The tell is that the probe you used to justify the test was an *insertion* rather than an edit.
  When that happens, say so and keep the test: the injection is the evidence, and the score was
  never able to be. Ours pinned that a decoder's state survives across input chunks, proved it by
  adding a reset that failed exactly one test in the suite, and killed four mutants out of 238.

  Keep this apart from the case below where the code lives elsewhere — they look identical in a
  report and have different causes. There the *code* is out of the target; here the *fault* is out
  of the population.

- **A test can add real coverage and real assertions and move the target's score not at all.**
  A mutation score is about one *file*; a guarantee is usually about a *path through several*.
  Where the behaviour you just pinned is implemented in a dependency — a decoder, a formatter, a
  driver the target delegates to — the target's own lines are unchanged, and the survivor list
  does not move. Ours added a test that took a library from 52% to 81% and killed zero of its
  target's survivors.

  That reads as failure and is not. Report it as what it is: the coverage it bought, the file it
  bought it in, and the fact that the target's score was not the thing being moved. Otherwise the
  obvious inference — "the test was worthless, revert it" — is available and wrong. It is also
  the signal that the dependency has become worth measuring in its own right, which is usually a
  better next target than squeezing the original.

- **When the target is generic, the test chooses which copy it measures — and it will not choose
  the production one by accident.** Anything instantiated per type or per parameter (templates,
  generics, macro-generated code) is compiled once per instantiation, and coverage counts each
  separately. A test that supplies its own parameter because nothing forced it to supplies one the
  production code never uses, so it exercises a copy of its own: every assertion holds, the tests
  pass, and the report shows the copy the product actually builds still at zero.

  It is quiet in both directions. The denominator inflates, because the file's lines are counted
  once per instantiation, so the percentage understates by a factor nobody thinks to look for. And
  the *uncovered* line list names lines the tests plainly do exercise — which reads as a broken
  coverage build rather than as what it is. Ours reported the reply path uncovered in a test that
  asserted on the reply.

  The rule is to take the instantiation from production rather than pick one: use the same type,
  the same constant, the same buffer the real caller passes, and say in the test why that value and
  not another. The tell, if you have already got it wrong, is a total line count for one file that
  is a clean multiple of what the file actually contains.

- **Do not estimate a run's duration from its first minute.** A parallel mutation run opens with
  every worker doing a cold compile at once, so the early rate is several times below the steady
  state and extrapolating it overstates the total badly — ours read as six hours during warm-up
  and finished in one. That matters because the number gets used to decide whether to abandon the
  run. Sample the rate twice, a couple of minutes apart, once workers are saturated, and quote
  the second; and if you have already given a figure from the first sample, correct it explicitly
  rather than letting it stand.

  Budget for the target rather than for the tool. Per-mutant cost is dominated by rebuilding the
  file *and* relinking the whole suite, so a large module in a large binary can cost an order of
  magnitude more per mutant than a small one — and the generated mutants are a full copy of the
  source each, which for a big file is gigabytes. Put them somewhere with room, and check the
  runner reports the *restricted* population ("N on M covered lines") rather than the whole file,
  which is the signal that the coverage build it needed was actually found.
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
- **A controllable clock turns bounded waits into unbounded ones, and the timeouts that follow
  are not detections.** Where the harness advances time only on request — the usual arrangement
  for testing anything that waits — a loop the product bounds by elapsed time has no bound at
  all: nothing inside it moves the clock. Mutants that strand such a loop hang for ever, score as
  timeouts, and are counted as detected, when the same mutant on real hardware would leave the
  loop on schedule and carry on misbehaving quietly. The score is inflated by however many of
  those there are, and they are concentrated in exactly the state machines worth measuring.

  Report killed-by-assertion separately whenever the target contains a time-bounded loop, and say
  which figure is which. Confirm the mechanism rather than assume it: build one such mutant in
  directly and watch it hang, then check whether anything in that loop advances the clock.

  The same property makes the product's real behaviour there — *leaving the loop when the budget
  expires* — unreachable, so it cannot be pinned by a test either. That is the mirror of the
  problem a real-time harness has, where waiting cannot be tested because the test cannot skip
  ahead. Neither harness covers both; know which blind spot yours has.

  **This one is worth closing rather than reporting around.** A loop that waits on time must poll
  *something* — a port, a queue, a flag — and a poll that finds nothing is precisely where real
  hardware burns cycles. So let the fake charge an empty poll against the simulated clock, at a
  rate a test declares and zero by default. The loop then ends for the same reason it ends in
  production, and the mutants that used to hang run to completion and are judged on what they did.

  Keep it opt-in: a clock that moves when read surprises every test that did not ask for it, and
  the default must leave the existing suite untouched. Turn it on for the whole file whose subject
  contains the loop rather than for the single test that needs it — well-formed input never
  reaches the spin, so nothing about the passing tests changes, and it is the *mutants* you are
  trying to let terminate. Say in the fake whether advancing this way also fires whatever
  interrupts a real advance would, because usually it should not, and a test that needs them needs
  something else. Ours moved 38 mutants from "timed out" to "killed by an assertion" while the raw
  score stayed put — which is the point: the number did not improve, it started being true.
- Build failure = **not a mutant**. Exclude it; it never produced a testable program.
- **Equivalent mutants belong outside the denominator.** Watch for the systematic
  source: code disabled at build time — `#if`/`#ifdef`, ternaries on compile-time
  constants, feature flags, dead generics. Line-coverage tools mark those lines as
  executed and text-level mutators happily mutate them, but the branch cannot run in
  this configuration, so the mutants are unkillable by construction. Identify them
  from the config, report them as a separate class, and quote both the raw and the
  adjusted score.

  **A third-party library is the extreme case, and worth expecting before you measure one.**
  A library ships configurable, and your configuration is what makes most of its options dead —
  so the proportion of structurally unkillable mutants is far higher than in code written for one
  purpose. Three sources dominate, and all three look like ordinary logic in a diff:

  - **Diagnostic macros that compile to nothing.** An assertion or trace macro defined as empty
    in a release configuration leaves its argument uncompiled, so mutating that argument changes
    a statement that is not there. The line still reads as covered, because the surrounding block
    executed. Ours contributed 39 unkillable mutants from two lines.
  - **Comparisons against constants the configuration fixes** — a width, a buffer size, a
    capability check — where one arm can never be taken.
  - **Code the vendor already disabled**, up to and including a literal `if (0)` left in the
    source.

  Together those were 27% of one library's survivors, several times what the same project's own
  code produced. Grep for the macro definitions before triaging line by line: one `#define` often
  explains the largest cluster on the list, and it is faster to read the configuration than to
  read a hundred mutants.

Exit gate: a reproducible mutation command, a validated harness, and a survivor list
partitioned into real gaps and equivalent mutants.
**Report the killable score alongside the raw one.** Once survivors have been triaged, the raw
percentage understates the suite by however many mutants no test could ever kill — and that
share grows as the real gaps close, so the number gets *less* informative exactly as the work
gets better. Quote both, with the equivalent count and the reason categories behind it, so a
reader can see which is which. A raw score falling while the killable score rises is a normal
and healthy thing to happen; a single number cannot show it.

**A score is comparable only to another over the same mutant population.** Killing mutants
means writing tests, and tests cover lines that were not covered before — so the next run
restricts mutants to a *larger* set and generates ones that never existed in the baseline. The
difference between two runs' kill counts is therefore not the number of survivors killed: it
also includes new mutants that were born dead. Reporting it as work done overstates the work,
sometimes badly.

Two numbers, and they answer different questions. **How many survivors did this pass kill?** —
re-run the previous run's survivor list; that population is fixed by definition. **How good is
the suite now?** — a fresh full run, quoted with its covered-line count, because that count *is*
the population. Give both, and never subtract one run's total from another's.

**Count a remaining-work bucket by reading mutants, not by counting lines.** A survivor summary
groups by line, and it is tempting to total the lines you have not yet explained and call the
result the work left. That number is almost always too big, because the reasons a cluster
survives do not respect line boundaries: within one function you will typically find a few
lines that are genuinely unasserted sitting among others that are blocked behind a terminal
call, equivalent because the reachable range collapses them, or different only in that they
invoke undefined behaviour.

The difference matters because it changes what you do next, and the ratio is often stark — one
pass here turned "about fifty-six genuinely unasserted" into five addressable and the rest
already explained. Read the mutant text for each line in the bucket before you promise the
bucket, and say which category each line landed in. An estimate built from line counts is a
claim you have not checked.
