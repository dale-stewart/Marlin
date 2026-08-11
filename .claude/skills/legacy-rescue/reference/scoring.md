# Scoring and reporting conventions

Loaded from `legacy-rescue` Steps 3 and 5. Any agent reporting a score reads this.

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
**Report the killable score alongside the raw one.** Once survivors have been triaged, the raw
percentage understates the suite by however many mutants no test could ever kill — and that
share grows as the real gaps close, so the number gets *less* informative exactly as the work
gets better. Quote both, with the equivalent count and the reason categories behind it, so a
reader can see which is which. A raw score falling while the killable score rises is a normal
and healthy thing to happen; a single number cannot show it.
