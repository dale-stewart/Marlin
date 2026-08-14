# What validating the subagents taught

Each agent was tested before being trusted, control first. The errors they made are worth
more than the passes, and both are recorded in the agent definitions.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

**What validating the agents taught (2026-08-11).** The split skill and its two agents were
tested before being trusted, control first: `harness-validator` was run once against the
correct apparatus and once with `.pio/build/testhal_native_coverage` moved aside — the
documented fault where the runner warns and mutates every line. It returned `TRUSTWORTHY`
and `NOT TRUSTWORTHY` respectively, named the missing coverage build as the cause, and
refused to compare its unrestricted 16.8% against the restricted 71.9%. Its numbers on the
good run reproduced exactly when re-run here (41/16/50 of 528, 21 covered lines).

Two things it got wrong are worth more than the passes:

- **A false equivalence inside the evidence for a pass.** It certified the harness partly on
  a "control" pair of mutants it called equivalent because `target_extruder` is "always 0".
  It is not: `get_target_extruder_from_command()` returns **-1** for a `T` the build does not
  have (`gcode.cpp:138`), so `> 0` is killable and only `!= 0` is genuinely equivalent. No
  existing test took that path, which is why it looked unreachable. The next agent killed it.
- **"The suite passed" meant one config of eight.** `mutant-killer` measured against
  `001-default` as instructed and reported green truthfully; its new test used the literal
  `T1`, which is out of range with `EXTRUDERS` 1 and an ordinary request with `EXTRUDERS` 3.
  It failed immediately under `003-extruders_3_runout`. Fixed by deriving the index from
  `EXTRUDERS` itself, so the test states "the first extruder this build does not have".

Both agent definitions now carry the corresponding rule. The pattern behind both: **an agent
is most dangerous where it is most confident**, and both errors were in claims nothing
downstream would normally re-check.

**`acceptance-author` validated by a refactor drill (2026-08-11).** A grep proves the
scenarios do not *name* the implementation; it cannot prove they do not *depend* on it. So
the check was empirical: it wrote `timing_the_job.feature` for the print job timer, and then
`Stopwatch` → `ElapsedClock` and `print_job_timer` → `job_elapsed_clock` were renamed across
**41 production files**, with the feature file and step definitions untouched.

The acceptance suite built and passed, 34/34. The unit tests did not compile —
`test_stopwatch.cpp`, `test_thermal_limits.cpp`, `test_media_commands.cpp` and
`test_powerloss.cpp` all failed on the renamed symbols. That contrast is the whole argument
for Steps 6-7 in one run: the same rename that a net must survive is the one that edits every
unit test naming it, and a net that moves with the code is not a net.

What made it work is visible in the steps — every one goes through `the_host_sends("M75")`
and `the_reply_to("M31")`. The only non-G-code call is the test HAL's clock, which is the
harness rather than the target, and renaming the target could not reach it. The four
`duration` hits its own grep reported are the English word, in prose and assertion messages.
Acceptance-only coverage verified rather than relayed: `M31.cpp` 100%, `M75-M78.cpp` 100%,
`stopwatch.cpp` 83%, all 0% before.

The drill is the reusable part. `acceptance_native_test` compiles exactly one test source
besides the framework and `tests/support`, so renaming a target's public surface in
`Marlin/src` and rebuilding *that env alone* is a cheap, honest test of whether a scenario
suite is really at the boundary — and `git checkout -- Marlin/src` puts it back.

**`rescue-surveyor` validated against `Marlin/src/feature/` (2026-08-11).** Chosen because
the directory is the four-way classification in concentrated form: 41 of 42 `.cpp` files sit
at 0% and *none* of them wants tests written. It led with the denominator rather than the
percentage — 95 of ~17,785 countable lines are compiled under `001-default`, so the honest
figure for the directory is ~0.07%, not the 13.7% that a coverage report shows — found
`host_actions.cpp` as the one genuine gap, and separated *not compiled in the baseline* from
**not compiled in any of the eight configurations** (mmu, mmu3, leds, resonance, password,
digipot, dac, tmc_util — about 10,800 lines dark everywhere). That last distinction was not
in the answer key and is the more useful one.

It also flagged that `test_runout.o` links under `001-default` while `feature/runout.o` does
not. Benign — `test_runout.cpp` is guarded on `FILAMENT_RUNOUT_SENSOR` and compiles to an
empty translation unit — and it reported the observation without claiming a defect, which is
the right handling of an unresolved lead. Its one real error was a structural count given
approximately (94 files; there are 85), now covered by a rule in its definition.
